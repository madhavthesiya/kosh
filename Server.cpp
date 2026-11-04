#include "Server.h"
#include "Commands.h"
#include <iostream>
#include <algorithm>
#include <atomic>
#include <csignal>
#ifndef _WIN32
#include <signal.h>
#include <sys/resource.h>
#endif

namespace {

// Set when Ctrl+C or SIGTERM asks the server to stop; the event loop checks it.
// A signal handler can interrupt the program anywhere, even inside malloc or
// iostream code, so it must not allocate, lock or do I/O. Setting a lock-free
// atomic is safe; the loop does the actual shutdown.
std::atomic<bool> stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free, "signal handler needs a lock-free flag");

#ifdef _WIN32
// Windows calls console handlers on a separate thread
BOOL WINAPI onConsoleEvent(DWORD type) {
    if(type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        stop_requested = true;
        return TRUE;    // handled: don't terminate the process
    }
    return FALSE;
}
#else
void onStopSignal(int) {
    stop_requested = true;
}
#endif

const char kServerFull[] = "-ERR max number of clients reached\r\n";

// Protected mode's reply, which Redis also starts with -DENIED
const char kDenied[] =
    "-DENIED Kosh is running in protected mode: no password is set, so only clients on "
    "this machine are served. To serve this one, start Kosh with --protected-mode no, "
    "but only on a network you trust.\r\n";

// Sends a new client one error reply, as Redis does, then closes its socket.
// The new socket's send buffer is empty, so the short reply fits. (A client
// that sent a command first may lose the reply: closing a socket with unread
// data resets the connection. Redis's refusals behave the same.)
template<size_t N>
void refuseClient(socket_t fd, const char (&reply)[N]) {
    sendSome(fd, reply, N - 1);
    CLOSE_SOCKET(fd);
}

#ifndef _WIN32
// Raises the soft limit on open files to `wanted` if the hard limit allows.
void raiseOpenFileLimit(rlim_t wanted) {
    struct rlimit limit;
    if(getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur >= wanted) return;
    limit.rlim_cur = std::min(wanted, limit.rlim_max);
    setrlimit(RLIMIT_NOFILE, &limit);
    if(getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < wanted) {
        std::cerr << "Warning: the open file limit is " << limit.rlim_cur << ", so about "
                  << (limit.rlim_cur > 32 ? limit.rlim_cur - 32 : 0)
                  << " clients can connect at once\n";
    }
}
#endif

}  // namespace

// The server batches AOF writes (group commit, see run()): the logger only
// buffers records, and the server writes them before sending any reply.
Server::Server(int port, InMemoryStorage& storage, AOFLogger* aof,
               std::unique_ptr<Poller> poller, size_t max_clients)
    : port_(port), storage_(storage), aof_(aof), poller_(std::move(poller)),
      max_clients_(max_clients) {
    if(!poller_) poller_ = makePoller(defaultPollerName());
    if(!poller_) poller_ = makePoller("select");    // epoll_create1() failed: select() always works
    if(aof_) aof_->setDeferred(true);
}

Server::~Server() {
    stop();
    if(aof_) aof_->setDeferred(false);      // writes whatever is still buffered
}

// ==================== Lifecycle ====================

bool Server::start() {
#ifdef _WIN32
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return false;
    }
    SetConsoleCtrlHandler(onConsoleEvent, TRUE);
#else
    // Ctrl+C (SIGINT) or SIGTERM: stop cleanly instead of being killed outright.
    // No SA_RESTART, so the signal also cuts the poller's wait short (EINTR).
    struct sigaction action{};
    action.sa_handler = onStopSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    // send() on a socket whose client has already disconnected raises SIGPIPE,
    // and its default action terminates the whole process: one client closing
    // early would take the server down for everyone. Ignoring it makes send()
    // fail with EPIPE instead, and the server removes that client.
    // Redis does the same at startup. (Windows has no SIGPIPE.)
    std::signal(SIGPIPE, SIG_IGN);

    // A write past the file size limit (RLIMIT_FSIZE) raises SIGXFSZ, which
    // also terminates the process by default. Ignored, the write fails with
    // EFBIG instead, and the AOF reports it like any other write error.
    std::signal(SIGXFSZ, SIG_IGN);

    // Room for every client the server will serve, plus a few files of its own
    // and the socket of one client too many, which must be opened to be refused
    // with an error reply (with a lower limit, accept() would fail with EMFILE
    // instead). That is max_clients_, or with select() its FD_SETSIZE. Redis
    // raises its limit to maxclients + 32 at startup the same way.
    raiseOpenFileLimit(static_cast<rlim_t>(std::min(max_clients_, poller_->capacity())) + 32);
    reserve_fd_ = open("/dev/null", O_RDONLY);
#endif

    // Create TCP socket
    listen_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if(listen_socket_ == INVALID_SOCKET) {
        std::cerr << "socket() failed\n";
        return false;
    }

    // Non-blocking, so accept() can never stall the loop: a client may give up
    // between the poller reporting its connection and the accept() call.
    if(!setNonBlocking(listen_socket_)) {
        std::cerr << "Could not make the listening socket non-blocking\n";
        CLOSE_SOCKET(listen_socket_);
        return false;
    }

    // Allow port reuse (avoid "address already in use" on restart)
    int opt = 1;
    setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    // Listen on one address: 127.0.0.1 unless --bind says otherwise. Only this
    // machine can reach a loopback address, so a server nobody configured
    // can't be reached from the network.
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if(!parseIPv4(bind_address_.c_str(), addr.sin_addr)) {
        std::cerr << "Cannot listen on '" << bind_address_ << "': not an IPv4 address\n";
        CLOSE_SOCKET(listen_socket_);
        return false;
    }
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if(bind(listen_socket_, reinterpret_cast<struct sockaddr*>(&addr),
            sizeof(addr)) == SOCKET_ERROR) {
        // e.g. the port is in use, or the address isn't one of this machine's.
        // Read before printing: on Windows, writing the message's first part
        // resets the error code to 0.
        int error = lastSocketError();
        std::cerr << "Cannot listen on " << bind_address_ << ":" << port_ << ": "
                  << socketErrorText(error) << "\n";
        CLOSE_SOCKET(listen_socket_);
        return false;
    }

    // Mark as passive (server), with room for kListenBacklog pending connections
    if(listen(listen_socket_, kListenBacklog) == SOCKET_ERROR) {
        std::cerr << "listen() failed\n";
        CLOSE_SOCKET(listen_socket_);
        return false;
    }

    poller_->add(listen_socket_);
    running_ = true;
    std::cout << "Kosh server listening on " << bind_address_ << ":" << port_ << "\n";
    std::cout << "Connect with: redis-cli -p " << port_ << "\n";
    if(isLoopback(addr.sin_addr)) {
        std::cout << "Access: this machine only (--bind 0.0.0.0 listens on every interface)\n";
    } else if(protected_mode_) {
        std::cout << "Access: protected mode: clients on other machines are refused, "
                     "since no password is set\n";
    } else {
        std::cout << "WARNING: protected mode is off and no password is set: anyone who can "
                     "reach port " << port_ << " can read and change every key\n";
    }
    std::cout << "I/O multiplexing: " << poller_->name();
    if(poller_->capacity() != SIZE_MAX) std::cout << " (FD_SETSIZE " << poller_->capacity() << ")";
    std::cout << ", single-threaded | maxclients " << max_clients_ << "\n";
    return true;
}

void Server::stop() {
    running_ = false;
    // Close all client sockets
    for(auto& [fd, client] : clients_) {
        poller_->remove(fd);
        CLOSE_SOCKET(fd);
    }
    clients_.clear();
    if(listen_socket_ != INVALID_SOCKET) {
        poller_->remove(listen_socket_);
        CLOSE_SOCKET(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
    }
#ifdef _WIN32
    WSACleanup();
#else
    if(reserve_fd_ >= 0) {
        close(reserve_fd_);
        reserve_fd_ = -1;
    }
#endif
}

// ==================== I/O Multiplexing Event Loop ====================
//
// How the loop works:
//   1. The listening socket and every client socket are registered with the
//      poller (epoll on Linux, select() elsewhere; see Poller.h)
//   2. wait() blocks until at least one of them is ready
//   3. Handle only the ready ones:
//      - listen socket ready → new client connecting → accept()
//      - client readable → recv(), run its complete commands, send the replies
//      - client writable → send more of the replies it still has waiting
//   4. Repeat
//
// Every socket is non-blocking, so no call ever waits for one client: if a
// reply can't be sent yet, it stays in that client's buffer and the loop asks
// the poller to report when the socket can take more.
//
// Why single-threaded?
//   Commands run one at a time, so no two commands can race each other.
//   Redis's main thread uses the same model, also with epoll on Linux, which
//   returns only the ready sockets; select() scans every socket each call.

void Server::run() {
    std::vector<Poller::Event> ready;
    while(running_ && !stop_requested) {
        // On POSIX a stop signal cuts this wait short (EINTR). It can still land
        // just after the check above and before the wait starts; the 1-second
        // timeout bounds that delay (pselect() or a self-pipe would remove it).
        // On Windows the console handler runs on another thread, so the timeout
        // is what lets the loop notice.
        if(poller_->wait(1000, ready) < 0) {
            if(running_) std::cerr << poller_->name() << " error " << lastSocketError() << "\n";
            continue;
        }

        // 1. Read from every ready client and run its commands. Their replies
        //    wait in each client's output buffer, their AOF records in the AOF's.
        //    (`ready` is a separate list, so removing a client while going
        //    through it is safe.)
        for(const auto& event : ready) {
            if(event.fd == listen_socket_) {
                acceptNewClients();
                continue;
            }
            // Each check also skips a client removed earlier in this pass
            if(event.readable && clients_.count(event.fd)) handleReadable(event.fd);
            if(event.writable && clients_.count(event.fd)) queueToServe(event.fd, clients_.at(event.fd));
        }

        // 2. Group commit: one write() hands the OS the AOF records of every
        //    command this pass ran, where each command used to make its own
        //    (with --appendfsync always, one sync puts them on the disk).
        //    Redis does the same in beforeSleep().
        if(!flushAof()) break;

        // 3. Only now send the replies, so none leaves before its record is
        //    with the OS. (serveClient() flushes again before sending replies to
        //    commands it runs itself.)
        for(socket_t fd : to_serve_) {
            auto it = clients_.find(fd);
            if(it == clients_.end()) continue;      // removed earlier in this pass
            it->second.queued = false;
            serveClient(fd);
        }
        to_serve_.clear();
    }
    // Connections and the AOF are closed by stop() and the destructors, which
    // run normally now instead of the process being killed by the signal.
    if(stop_requested) std::cout << "Received a stop signal: shutting down\n";
}

// ==================== Connection Management ====================

// Accepts every waiting connection rather than one per pass of the loop, up to
// kMaxAcceptsPerEvent.
void Server::acceptNewClients() {
    for(int i = 0; i < kMaxAcceptsPerEvent; i++) {
        struct sockaddr_in client_addr{};
        int addr_len = sizeof(client_addr);

        socket_t client = accept(listen_socket_,
            reinterpret_cast<struct sockaddr*>(&client_addr),
            reinterpret_cast<socklen_t*>(&addr_len));

        if(client == INVALID_SOCKET) {
            int error = lastSocketError();
#ifndef _WIN32
            // Out of file descriptors: the connection stays queued, so the poller
            // would report it again at once and the loop would spin at 100% CPU.
            // Free the spare descriptor, accept the connection with it, refuse
            // it properly, then take the spare back (as nginx and libev do).
            if((error == EMFILE || error == ENFILE) && reserve_fd_ >= 0) {
                close(reserve_fd_);
                socket_t extra = accept(listen_socket_, nullptr, nullptr);
                if(extra != INVALID_SOCKET) {
                    refuseClient(extra, kServerFull);
                    std::cout << "Client refused: the server is out of file descriptors\n";
                }
                reserve_fd_ = open("/dev/null", O_RDONLY);
                if(extra != INVALID_SOCKET) continue;
                return;
            }
#endif
            // "Would block": no more connections waiting. Not an error.
            if(!wouldBlock(error) && !isInterrupted(error)) {
                std::cerr << "accept() failed (error " << error << ")\n";
            }
            return;
        }

        // Linux sockets don't inherit O_NONBLOCK from the listening socket
        if(!setNonBlocking(client)) {
            std::cerr << "Could not make a client socket non-blocking\n";
            CLOSE_SOCKET(client);
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

        // Protected mode, as Redis's: with no password, only clients on this
        // machine. Checked first, since it is the lasting reason: a client told
        // "server full" would only retry.
        if(protected_mode_ && !isLoopback(client_addr.sin_addr)) {
            refuseClient(client, kDenied);
            std::cout << "Client refused: " << ip << " is not on this machine (protected mode)\n";
            continue;
        }
        // As Redis does at maxclients
        if(clients_.size() >= max_clients_) {
            refuseClient(client, kServerFull);
            std::cout << "Client refused: maxclients (" << max_clients_ << ") reached\n";
            continue;
        }
        // Watching a socket select() can't hold would write past the end of
        // fd_set (Linux) or silently never serve it (Windows)
        if(!poller_->canWatch(client)) {
            refuseClient(client, kServerFull);
            std::cout << "Client refused: the server is at its " << poller_->name() << " limit ("
                      << clients_.size() << " clients)\n";
            continue;
        }

        // Disable Nagle's algorithm — send responses immediately, don't batch.
        // Without this, TCP waits up to 40ms to coalesce small packets.
        // For a request-response protocol like Redis, that delay is pure waste.
        int flag = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&flag), sizeof(flag));

        // Register client with empty buffers, watched for readability
        clients_[client];
        poller_->add(client);

        std::cout << "Client connected: " << ip << ":"
                  << ntohs(client_addr.sin_port)
                  << " (total: " << clients_.size() << ")\n";
    }
}

void Server::removeClient(socket_t fd) {
    poller_->remove(fd);
    CLOSE_SOCKET(fd);
    clients_.erase(fd);
    std::cout << "Client disconnected (remaining: " << clients_.size() << ")\n";
}

// ==================== Client Data Handling ====================

void Server::handleReadable(socket_t fd) {
    Client& client = clients_.at(fd);

    // Read into a scratch buffer and append only what arrived. Reading straight
    // into the client's string would need resize() first, which zero-fills all
    // 16 KB every time, while a pipelined batch is often under 1 KB: the
    // profile showed that memset at 2.4% of CPU. (C++23's
    // resize_and_overwrite would avoid the zero-fill.)
    char buffer[kReadChunk];
    long long n = recvSome(fd, buffer, sizeof(buffer));

    if(n == 0) {            // the client closed the connection
        removeClient(fd);
        return;
    }
    if(n < 0) {
        int error = lastSocketError();
        if(wouldBlock(error) || isInterrupted(error)) return;   // nothing to read after all
        removeClient(fd);   // connection error, e.g. reset by the client
        return;
    }
    client.in.append(buffer, static_cast<size_t>(n));

    // The parser limits each part of a command; this caps the total a client can
    // make the server hold (Redis's client-query-buffer-limit default).
    if(client.in.size() > kMaxQueryBuffer) {
        std::cerr << "Closing client: input exceeds the "
                  << (kMaxQueryBuffer >> 20) << " MB query buffer limit\n";
        removeClient(fd);
        return;
    }
    // Run its commands now; the replies are sent once the pass has written
    // the AOF (see run())
    runCommands(client);
    if(!client.out.empty()) queueToServe(fd, client);
}

// Hands the AOF records of the commands run so far to the OS, and with
// --appendfsync always to the disk. False if that has failed for good: with
// always, a reply must mean the write is on the disk, which can no longer be
// promised. Then no reply is sent and the server stops, as Redis exits.
bool Server::flushAof() {
    if(!aof_) return true;
    aof_->flush();
    if(!aof_->failedForGood()) return true;
    if(running_) {
        std::cerr << "Stopping: the AOF can't be written or synced (" << aof_->lastWriteError()
                  << "), and with --appendfsync always a reply must mean the write is on disk\n";
    }
    running_ = false;
    exit_code_ = 1;
    return false;
}

void Server::queueToServe(socket_t fd, Client& client) {
    if(client.queued) return;
    client.queued = true;
    to_serve_.push_back(fd);
}

// Sends what the client has waiting, then runs its next commands, until it has
// no complete command left or its socket can't take more. New commands run only
// once earlier replies are all sent, so a client that doesn't read gets no
// further work done for it: its replies stay bounded and TCP flow control slows
// it down.
void Server::serveClient(socket_t fd) {
    Client& client = clients_.at(fd);
    while(true) {
        if(client.out_sent < client.out.size()) {
            // No reply leaves before the AOF records of its commands are with the
            // OS. Usually run() has written them already and this does nothing.
            if(!flushAof()) return;
            SendStatus status = sendReplies(fd, client);
            if(status == SendStatus::Failed) {     // the client has gone
                removeClient(fd);
                return;
            }
            if(status == SendStatus::Blocked) break;   // the poller will say when there is room
        }
        if(client.close_after_reply) {             // QUIT or protocol error, reply sent
            removeClient(fd);
            return;
        }
        // Commands may be waiting in `in` even if no new data arrives: the client
        // may have sent everything and now waits for these replies.
        runCommands(client);
        if(client.out.empty()) break;              // no complete command left
    }
    updateInterest(fd, client);
}

// Runs whole commands from the client's input and appends their replies, until
// the input has no complete command left or kOutputHighWater bytes of replies
// are waiting.
void Server::runCommands(Client& client) {
    // Parse from a moving position and drop the used bytes once at the end.
    // Erasing each command from the front would move all the bytes behind it,
    // once per command: quadratic in the number of commands in one read.
    size_t pos = 0;
    CommandContext ctx{storage_, aof_, clients_.size()};
    while(client.out.size() < kOutputHighWater && !client.close_after_reply) {
        // Into args_, whose strings are reused from command to command
        auto parsed = RespParser::parseInto(std::string_view(client.in).substr(pos), args_);
        if(parsed.status == RespParser::ParseStatus::NeedMore) break;  // wait for more data
        if(parsed.status == RespParser::ParseStatus::Error) {
            // As Redis does: say why, then disconnect. Nothing after the bad bytes
            // can be trusted, because where the bad command ends is unknown.
            client.out += RespParser::error("ERR Protocol error: " + parsed.error);
            client.close_after_reply = true;
            client.in.clear();
            return;
        }
        pos += parsed.consumed;
        if(parsed.argc == 0) continue;

        // The reply goes straight into the client's buffer
        executeCommand(ctx, Args(args_.data(), parsed.argc), client.out);
        releaseLargeArgs(parsed.argc);

        // QUIT: send its reply, then disconnect; commands after it don't run
        if(ctx.close_connection) {
            client.close_after_reply = true;
            client.in.clear();
            return;
        }
    }
    client.in.erase(0, pos);
}

// args_ keeps its memory so that parsing allocates nothing, but one command
// with a huge argument (up to 512 MB) or very many would leave it that large
// for good. Give such memory back, as Redis shrinks a client's query buffer.
void Server::releaseLargeArgs(size_t argc) {
    for(size_t i = 0; i < argc; i++) {
        if(args_[i].capacity() > kKeepArgBytes) std::string().swap(args_[i]);
    }
    if(args_.size() > kKeepArgs) {
        args_.resize(kKeepArgs);
        args_.shrink_to_fit();
    }
}

// Sends as much of the waiting replies as the socket takes. A partial send is
// normal: the rest goes when the poller reports the socket writable again.
Server::SendStatus Server::sendReplies(socket_t fd, Client& client) {
    while(client.out_sent < client.out.size()) {
        long long n = sendSome(fd, client.out.data() + client.out_sent,
                               client.out.size() - client.out_sent);
        if(n > 0) {
            client.out_sent += static_cast<size_t>(n);
            continue;
        }
        if(n < 0) {
            int error = lastSocketError();
            if(isInterrupted(error)) continue;
            if(wouldBlock(error)) return SendStatus::Blocked;   // socket buffer full
        }
        return SendStatus::Failed;   // e.g. EPIPE or ECONNRESET
    }
    client.out.clear();
    client.out_sent = 0;
    return SendStatus::Done;
}

// Reads while nothing waits to be sent; waits for writability while something does.
void Server::updateInterest(socket_t fd, Client& client) {
    bool pending = client.out_sent < client.out.size();
    bool want_read = !pending && !client.close_after_reply;
    bool want_write = pending;
    if(want_read != client.want_read || want_write != client.want_write) {
        poller_->setInterest(fd, want_read, want_write);
        client.want_read = want_read;
        client.want_write = want_write;
    }
}

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

// Tells a client the server is full, as Redis does at maxclients, then closes
// its socket. The new socket's buffer is empty, so the short reply fits.
void refuseClient(socket_t fd) {
    static const char kReply[] = "-ERR max number of clients reached\r\n";
    sendSome(fd, kReply, sizeof(kReply) - 1);
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
        std::cerr << "Warning: open file limit is " << limit.rlim_cur
                  << ", so fewer clients than select() allows can connect\n";
    }
}
#endif

}  // namespace

// The server batches AOF writes (group commit, see run()): the logger only
// buffers records, and the server writes them before sending any reply.
Server::Server(int port, InMemoryStorage& storage, AOFLogger* aof)
    : port_(port), storage_(storage), aof_(aof) {
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

    // select() watches sockets numbered below FD_SETSIZE. A client beyond that
    // gets socket FD_SETSIZE or higher and is refused with an error reply; the
    // server must be allowed to open that socket to reply at all. With a lower
    // limit, accept() would fail with EMFILE instead. (Redis also raises its
    // limit at startup.)
    raiseOpenFileLimit(FD_SETSIZE + 32);
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

    // Bind to port
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if(bind(listen_socket_, reinterpret_cast<struct sockaddr*>(&addr),
            sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "bind() failed — is port " << port_ << " already in use?\n";
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
    std::cout << "Mini-Redis server listening on port " << port_ << "\n";
    std::cout << "Connect with: redis-cli -p " << port_ << "\n";
    std::cout << "I/O multiplexing: select() — single-threaded, concurrent clients\n";
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
//      poller (select() today; see Poller.h)
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
//   Redis's main thread uses the same model, but waits with epoll/kqueue,
//   which return only the ready sockets; select() scans every socket each call.

void Server::run() {
    std::vector<Poller::Event> ready;
    while(running_ && !stop_requested) {
        // On POSIX a stop signal cuts this wait short (EINTR). It can still land
        // just after the check above and before the wait starts; the 1-second
        // timeout bounds that delay (pselect() or a self-pipe would remove it).
        // On Windows the console handler runs on another thread, so the timeout
        // is what lets the loop notice.
        if(poller_->wait(1000, ready) < 0) {
            if(running_) std::cerr << "select() error\n";
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
        //    command this pass ran, where each command used to make its own.
        //    Redis does the same in beforeSleep().
        if(aof_) aof_->flush();

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
            // Out of file descriptors: the connection stays queued, so select()
            // would report it again at once and the loop would spin at 100% CPU.
            // Free the spare descriptor, accept the connection with it, refuse
            // it properly, then take the spare back (as nginx and libev do).
            if((error == EMFILE || error == ENFILE) && reserve_fd_ >= 0) {
                close(reserve_fd_);
                socket_t extra = accept(listen_socket_, nullptr, nullptr);
                if(extra != INVALID_SOCKET) {
                    refuseClient(extra);
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

        // Watching a socket select() can't hold would write past the end of
        // fd_set (Linux) or silently never serve it (Windows)
        if(!poller_->canWatch(client)) {
            refuseClient(client);
            std::cout << "Client refused: the server is at its select() limit ("
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

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
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
            if(aof_) aof_->flush();
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
        auto parsed = RespParser::parse(std::string_view(client.in).substr(pos));
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
        const auto& args = parsed.args;
        if(args.empty()) continue;

        client.out += executeCommand(ctx, args);

        // QUIT: send its reply, then disconnect; commands after it don't run
        if(ctx.close_connection) {
            client.close_after_reply = true;
            client.in.clear();
            return;
        }
    }
    client.in.erase(0, pos);
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

#include "Server.h"
#include "Commands.h"
#include "Version.h"
#include <iostream>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <thread>
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
    "this machine are served. To serve this one, set a password with --requirepass-file "
    "or --requirepass, or start Kosh with --protected-mode no, but only on a network "
    "you trust.\r\n";

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

Server::Server(int port, const std::vector<InMemoryStorage*>& shards, const std::vector<AOFLogger*>& aofs,
               const Router& router, std::unique_ptr<Poller> poller, size_t max_clients)
    : port_(port), max_clients_(max_clients), router_(router) {
    if(!poller) poller = makePoller(defaultPollerName());
    if(!poller) poller = makePoller("select");      // epoll_create1() failed: select() always works
    const std::string poller_name = poller->name();
    for(size_t i = 0; i < shards.size(); i++) {
        loops_.push_back(std::make_unique<EventLoop>(
            *this, i, shards.size(), *shards[i], aofs.empty() ? nullptr : aofs[i],
            i == 0 ? std::move(poller) : makePoller(poller_name)));
        loops_.back()->stats().tcp_port = port;
        loops_.back()->stats().max_clients = max_clients;
    }
}

Server::~Server() {
    stop();     // the loop's destructor then writes whatever the AOF still has buffered
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
    raiseOpenFileLimit(static_cast<rlim_t>(std::min(max_clients_, loops_[0]->poller().capacity())) + 32);
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

    // The same option name means different things on the two systems.
    // POSIX: SO_REUSEADDR lets a restarted server bind its port while
    // connections the old one closed are still in TIME_WAIT; a port another
    // socket is listening on stays refused.
    // Windows: SO_REUSEADDR lets a socket bind a port another one is already
    // listening on, so a second server would start and serve no one, and any
    // program could bind beside Kosh. SO_EXCLUSIVEADDRUSE refuses that, as
    // Microsoft advises for servers.
    int opt = 1;
#ifdef _WIN32
    setsockopt(listen_socket_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
#endif

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

    loops_[0]->poller().add(listen_socket_);
    loops_[0]->setListenSocket(listen_socket_);
    running_ = true;
    const auto started = std::chrono::steady_clock::now();
    for(auto& loop : loops_) loop->stats().started = started;
    std::cout << "Kosh " << kKoshVersion << " listening on " << bind_address_ << ":" << port_ << "\n";
    std::cout << "Connect with: redis-cli -p " << port_ << "\n";
    // Who can connect, and whether they need the password (never printed)
    const char* need = password_ ? ", with the password" : "";
    if(isLoopback(addr.sin_addr)) {
        std::cout << "Access: this machine only" << need
                  << " (--bind 0.0.0.0 listens on every interface)\n";
    } else if(password_) {
        std::cout << "Access: any machine that can reach port " << port_ << need << "\n";
    } else if(protected_mode_) {
        std::cout << "Access: protected mode: clients on other machines are refused, "
                     "since no password is set\n";
    } else {
        std::cout << "WARNING: protected mode is off and no password is set: anyone who can "
                     "reach port " << port_ << " can read and change every key\n";
    }
    std::cout << "I/O multiplexing: " << loops_[0]->poller().name();
    if(loops_[0]->poller().capacity() != SIZE_MAX) std::cout << " (FD_SETSIZE " << loops_[0]->poller().capacity() << ")";
    if(loops_.size() == 1) std::cout << ", single-threaded";
    else std::cout << ", " << loops_.size() << " threads";
    std::cout << " | maxclients " << max_clients_ << "\n";
    return true;
}

void Server::stop() {
    running_ = false;
    for(auto& loop : loops_) loop->closeAllClients();
    if(listen_socket_ != INVALID_SOCKET) {
        loops_[0]->poller().remove(listen_socket_);
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

void Server::run() {
    std::vector<std::thread> threads;
    for(size_t i = 1; i < loops_.size(); i++) {
        threads.emplace_back([this, i] { loops_[i]->run(); });
    }
    loops_[0]->run();
    for(auto& thread : threads) thread.join();
    // Connections and the AOF are closed by stop() and the destructors, which
    // run normally now instead of the process being killed by the signal.
    if(stop_requested) std::cout << "Received a stop signal: shutting down\n";
}

bool Server::keepRunning() const {
    return running_ && !stop_requested;
}

void Server::wakeAllLoops() {
    for(auto& loop : loops_) loop->wake();
}

void Server::stopWithError(const std::string& why) {
    if(running_.exchange(false)) std::cerr << why << "\n";
    exit_code_ = 1;
}

// ==================== Connection Management ====================

// Accepts every waiting connection rather than one per pass of the loop, up to
// kMaxAcceptsPerEvent.
void Server::acceptNewClients() {
    ServerStats& stats = loops_[0]->stats();
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
                    stats.rejected_connections++;
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
        if(protected_mode_ && !password_ && !isLoopback(client_addr.sin_addr)) {
            refuseClient(client, kDenied);
            stats.rejected_connections++;
            std::cout << "Client refused: " << ip << " is not on this machine (protected mode)\n";
            continue;
        }
        // As Redis does at maxclients
        if(connected_ >= max_clients_) {
            refuseClient(client, kServerFull);
            stats.rejected_connections++;
            std::cout << "Client refused: maxclients (" << max_clients_ << ") reached\n";
            continue;
        }
        // Watching a socket select() can't hold would write past the end of
        // fd_set (Linux) or silently never serve it (Windows)
        if(!loops_[0]->poller().canWatch(client)) {
            refuseClient(client, kServerFull);
            stats.rejected_connections++;
            std::cout << "Client refused: the server is at its " << loops_[0]->poller().name() << " limit ("
                      << connected_ << " clients)\n";
            continue;
        }

        // Disable Nagle's algorithm — send responses immediately, don't batch.
        // Without this, TCP waits up to 40ms to coalesce small packets.
        // For a request-response protocol like Redis, that delay is pure waste.
        int flag = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&flag), sizeof(flag));

        connected_++;
        if(!dealToLoop({client, ++last_client_id_})) {
            connected_--;
            refuseClient(client, kServerFull);
            stats.rejected_connections++;
            std::cout << "Client refused: every thread's queue of new clients is full\n";
            continue;
        }

        stats.connections_received++;
        // One string, so that lines from different threads can't interleave
        std::cout << ("Client connected: " + std::string(ip) + ":" +
                      std::to_string(ntohs(client_addr.sin_port)) +
                      " (total: " + std::to_string(connected_) + ")\n");
    }
}

// Round-robin: the loops end up with the same number of clients, to within
// one, however few there are. A loop whose queue of new clients is full (it
// hasn't run for a long while) is passed over.
bool Server::dealToLoop(const EventLoop::NewClient& client) {
    for(size_t tries = 0; tries < loops_.size(); tries++) {
        const size_t index = next_loop_;
        next_loop_ = (next_loop_ + 1) % loops_.size();
        if(index == 0) {                    // this thread's own loop
            loops_[0]->addClient(client.fd, client.id);
            return true;
        }
        if(loops_[index]->postNewClient(client)) return true;
    }
    return false;
}

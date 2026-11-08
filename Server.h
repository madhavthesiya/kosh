#pragma once
#include "Socket.h"     // first: on Windows it must set FD_SETSIZE before winsock2.h is included
#include "Poller.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "EventLoop.h"
#include "InMemoryStorage.h"
#include "Router.h"

// The server as a whole: the listening socket, the settings, starting and
// stopping. The work of serving clients is in EventLoop.
class Server {
public:
    // Clients served at once, as Redis's maxclients default
    static constexpr size_t kDefaultMaxClients = 10000;

    // `poller`: how to wait for sockets; the system's best (epoll on Linux,
    // select() elsewhere) if none is given. Beyond `max_clients` clients, or
    // what the poller can watch, a new client gets "-ERR max number of clients
    // reached" and is disconnected. `shards`: one storage per thread; each
    // thread owns the keys of its shard (above 1 needs epoll).
    Server(int port, const std::vector<InMemoryStorage*>& shards, AOFLogger* aof = nullptr,
           std::unique_ptr<Poller> poller = nullptr, size_t max_clients = kDefaultMaxClients);
    ~Server();

    // Set these before start().
    // The IPv4 address to listen on: 127.0.0.1 by default, so only this
    // machine can connect; 0.0.0.0 listens on every interface.
    void setBindAddress(std::string address) { bind_address_ = std::move(address); }
    // Protected mode, as Redis's (on by default): with no password set, a
    // client that isn't on this machine gets -DENIED and is disconnected.
    void setProtectedMode(bool on) { protected_mode_ = on; }
    // A password (at most kMaxPasswordLength bytes): every client must AUTH
    // before any other command, and protected mode no longer applies
    void setPassword(std::string password) { password_ = std::move(password); }

    bool start();       // Create socket, bind, listen
    void run();         // Event loop — handles multiple clients concurrently
    void stop();

    // 0, or 1 if the server stopped because the AOF failed for good
    int exitCode() const { return exit_code_; }

private:
    friend class EventLoop;

    void acceptNewClients();                         // New connections on listen socket
    bool dealToLoop(const EventLoop::NewClient& client);  // False if no loop can take it
    void wakeAllLoops();                             // So that each notices the server stopping
    bool keepRunning() const;                        // False once stopped or asked to stop
    size_t connectedClients() const { return connected_; }
    EventLoop& loop(size_t index) { return *loops_[index]; }
    const Router& router() const { return router_; }
    size_t clientRemoved() { return --connected_; }  // How many remain
    bool running() const { return running_; }
    void stopWithError(const std::string& why);      // Says why; the exit code becomes 1
    // The password, or nullptr if none is set
    const std::string* password() const { return password_ ? &*password_ : nullptr; }

    // Connections the kernel queues before accept() (Redis's tcp-backlog default;
    // Linux caps it at net.core.somaxconn). With too small a queue, a burst of
    // connections overflows it and the dropped ones retry a second later.
    static constexpr int kListenBacklog = 511;

    // Connections accepted per wakeup, so a flood of them can't starve existing
    // clients (Redis's MAX_ACCEPTS_PER_CALL)
    static constexpr int kMaxAcceptsPerEvent = 1000;

    int port_;
    std::string bind_address_ = "127.0.0.1";
    bool protected_mode_ = true;
    std::optional<std::string> password_;
    socket_t listen_socket_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::atomic<int> exit_code_{0};
    size_t max_clients_;

    // A spare file descriptor (POSIX), given up for a moment when the process
    // runs out, so a waiting connection can still be accepted and refused
    int reserve_fd_ = -1;

    uint64_t last_client_id_ = 0;       // never reused, as in Redis

    // Clients connected, across all loops: the accepting loop adds, every
    // loop subtracts. One counter, touched only when a client comes or goes.
    std::atomic<size_t> connected_{0};

    Router router_;                     // which thread owns a key

    // One per thread. The first runs on the thread that calls run() and accepts
    // connections; the others get them dealt to them in turn.
    std::vector<std::unique_ptr<EventLoop>> loops_;
    size_t next_loop_ = 0;              // the accepting thread's only
};

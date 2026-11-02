#pragma once
#include "Socket.h"     // first: on Windows it must set FD_SETSIZE before winsock2.h is included
#include "Poller.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include "InMemoryStorage.h"
#include "RespParser.h"

class Server {
public:
    Server(int port, InMemoryStorage& storage, AOFLogger* aof = nullptr);
    ~Server();

    bool start();       // Create socket, bind, listen
    void run();         // Event loop — handles multiple clients concurrently
    void stop();

private:
    // One connected client. Received bytes wait in `in` until they form whole
    // commands; replies wait in `out` until the socket takes them. The server
    // never waits for a single client, so one that is slow or stalled cannot
    // hold up the others.
    struct Client {
        std::string in;                  // received, not yet executed
        std::string out;                 // replies; the first out_sent bytes are already sent
        size_t out_sent = 0;
        bool close_after_reply = false;  // QUIT or a protocol error: close once `out` is sent
        bool want_read = true;           // what the poller is currently asked to report
        bool want_write = false;
        bool queued = false;             // in to_serve_ for this pass of the loop
    };

    enum class SendStatus { Done, Blocked, Failed };

    // I/O multiplexing handlers
    void acceptNewClients();                         // New connections on listen socket
    void handleReadable(socket_t fd);                // Data or end of stream on a client socket
    void queueToServe(socket_t fd, Client& client);  // Serve it once this pass has written the AOF
    void serveClient(socket_t fd);                   // Send replies, run commands, until blocked
    void runCommands(Client& client);                // Execute whole commands from in into out
    SendStatus sendReplies(socket_t fd, Client& client);
    void updateInterest(socket_t fd, Client& client);
    void removeClient(socket_t fd);                  // Cleanup on disconnect

    // Most a client's incomplete command may occupy (Redis's client-query-buffer-limit)
    static constexpr size_t kMaxQueryBuffer = size_t(1) << 30;

    // Connections the kernel queues before accept() (Redis's tcp-backlog default;
    // Linux caps it at net.core.somaxconn). With too small a queue, a burst of
    // connections overflows it and the dropped ones retry a second later.
    static constexpr int kListenBacklog = 511;

    // Connections accepted per wakeup, so a flood of them can't starve existing
    // clients (Redis's MAX_ACCEPTS_PER_CALL)
    static constexpr int kMaxAcceptsPerEvent = 1000;

    // Bytes read per recv() (Redis reads 16 KB as well)
    static constexpr size_t kReadChunk = 16 * 1024;

    // Stop running a client's commands once this many bytes of its replies are
    // waiting to be sent, and stop reading from it until they have gone. Memory
    // per client stays bounded, and a client that doesn't read its replies is
    // slowed down by TCP flow control instead of being cut off.
    static constexpr size_t kOutputHighWater = size_t(1) << 20;

    int port_;
    socket_t listen_socket_ = INVALID_SOCKET;
    InMemoryStorage& storage_;
    bool running_ = false;

    AOFLogger* aof_ = nullptr;  // optional: flushed before replies are sent, and for BGREWRITEAOF

    // Tells the event loop which sockets are ready
    std::unique_ptr<Poller> poller_ = std::make_unique<SelectPoller>();

    // A spare file descriptor (POSIX), given up for a moment when the process
    // runs out, so a waiting connection can still be accepted and refused
    int reserve_fd_ = -1;

    // Connected clients, by socket
    std::unordered_map<socket_t, Client> clients_;

    // Clients with replies to send in this pass of the loop, in order. They are
    // served after the pass's AOF records are written (group commit).
    std::vector<socket_t> to_serve_;
};

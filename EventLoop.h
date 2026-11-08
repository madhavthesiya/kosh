#pragma once
#include "Socket.h"     // first: on Windows it must set FD_SETSIZE before winsock2.h is included
#include "Poller.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Commands.h"         // Transaction, ServerStats
#include "InMemoryStorage.h"
#include "RespParser.h"
#include "SpscRing.h"
#include "Wakeup.h"

class Server;

// One thread's share of the server: its clients, the poller that watches their
// sockets, and the loop that reads commands, runs them and sends the replies.
// Server owns what belongs to the whole process: the listening socket, the
// settings, and starting and stopping.
class EventLoop {
public:
    // `threaded`: other threads will send this loop work, so it needs a way to
    // receive it and to be woken
    EventLoop(Server& server, InMemoryStorage& storage, AOFLogger* aof,
              std::unique_ptr<Poller> poller, bool threaded);
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void run();                                 // Until the server stops

    // This loop reports the listening socket's connections to Server. The
    // caller has already added the socket to poller().
    void setListenSocket(socket_t fd) { listen_socket_ = fd; }

    void addClient(socket_t fd, uint64_t id);   // An accepted client, watched from now on
    void closeAllClients();

    struct NewClient {
        socket_t fd;
        uint64_t id;
    };
    // From the accepting thread: gives this loop a client. False if too many
    // are waiting for it to pick them up.
    bool postNewClient(const NewClient& client);

    // From any thread: makes this loop look at the world again if it is asleep
    void wake();
    size_t clientCount() const { return clients_.size(); }

    Poller& poller() { return *poller_; }
    ServerStats& stats() { return stats_; }

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
        bool authenticated = false;      // AUTH succeeded, or no password is set
        int protocol = 2;                // RESP2 until HELLO 3
        uint64_t id = 0;                 // CLIENT ID: 1, 2, 3... in order of connection
        std::string name;                // CLIENT SETNAME; empty: none
        Transaction transaction;         // MULTI ... EXEC
    };

    enum class SendStatus { Done, Blocked, Failed };

    // I/O multiplexing handlers
    int prepareToWait();                             // Returns how long the poller may wait
    bool adoptNewClients();                          // True if any arrived
    void handleReadable(socket_t fd);                // Data or end of stream on a client socket
    void queueToServe(socket_t fd, Client& client);  // Serve it once this pass has written the AOF
    bool flushAof();                                 // Before replies go out; false: stop
    void serveClient(socket_t fd);                   // Send replies, run commands, until blocked
    void runCommands(Client& client);                // Execute whole commands from in into out
    void releaseLargeArgs(size_t argc);              // Bound the memory args_ keeps
    SendStatus sendReplies(socket_t fd, Client& client);
    void updateInterest(socket_t fd, Client& client);
    void removeClient(socket_t fd);                  // Cleanup on disconnect

    // Most a client's incomplete command may occupy (Redis's client-query-buffer-limit)
    static constexpr size_t kMaxQueryBuffer = size_t(1) << 30;

    // How long the poller waits with nothing to do, in milliseconds. The loop
    // wakes this often even when idle, to sample the rate and notice a stop.
    static constexpr int kIdleWaitMs = 1000;

    // New clients that can wait to be picked up, per loop
    static constexpr size_t kNewClientQueue = 4096;

    // Bytes read per recv() (Redis reads 16 KB as well)
    static constexpr size_t kReadChunk = size_t{16} * 1024;

    // Stop running a client's commands once this many bytes of its replies are
    // waiting to be sent, and stop reading from it until they have gone. Memory
    // per client stays bounded, and a client that doesn't read its replies is
    // slowed down by TCP flow control instead of being cut off.
    static constexpr size_t kOutputHighWater = size_t(1) << 20;

    // The parsed arguments keep their memory between commands, up to this much
    // per argument and this many arguments; more is given back after use
    static constexpr size_t kKeepArgBytes = size_t{64} * 1024;
    static constexpr size_t kKeepArgs = 1024;

    Server& server_;
    InMemoryStorage& storage_;
    AOFLogger* aof_ = nullptr;  // optional: flushed before replies are sent, and for BGREWRITEAOF

    // Tells the event loop which sockets are ready
    std::unique_ptr<Poller> poller_;
    socket_t listen_socket_ = INVALID_SOCKET;   // INVALID_SOCKET: another loop accepts

    // Only with more than one thread; a single loop has no one to hear from
    std::unique_ptr<Wakeup> wakeup_;
    std::unique_ptr<SpscRing<NewClient>> new_clients_;

    // Connected clients, by socket
    std::unordered_map<socket_t, Client> clients_;

    // What INFO reports. instantaneous_ops_per_sec comes from samples of the
    // command count, taken at most every 100 ms: the rate between the oldest
    // and newest of the last kOpSamples (as Redis does, over 16 samples)
    ServerStats stats_;
    struct OpSample {
        std::chrono::steady_clock::time_point time;
        uint64_t commands = 0;
    };
    static constexpr size_t kOpSamples = 16;
    OpSample op_samples_[kOpSamples];
    size_t op_sample_next_ = 0;
    size_t op_samples_taken_ = 0;
    void sampleOpsPerSec();

    // Clients with replies to send in this pass of the loop, in order. They are
    // served after the pass's AOF records are written (group commit).
    std::vector<socket_t> to_serve_;

    // Every command is parsed into these strings, which keep their memory, so
    // parsing allocates nothing once they are large enough. One set serves all
    // of this loop's clients: commands run one at a time, and none keeps its
    // arguments.
    std::vector<std::string> args_;
};

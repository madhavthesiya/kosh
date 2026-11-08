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
    // This loop is number `index` of `loops`. With more than one, other threads
    // send it work, so it needs a way to receive it and to be woken.
    EventLoop(Server& server, size_t index, size_t loops, InMemoryStorage& storage, AOFLogger* aof,
              std::unique_ptr<Poller> poller);
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
    // A reply that can't go in the client's output yet, because a reply before
    // it is still on another thread. Slots are used in order and reused, so
    // once a client has enough of them they allocate nothing.
    struct ReplySlot {
        bool ready = false;     // false: the thread running the command hasn't answered
        std::string bytes;
    };

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

        // Replies in the order the commands arrived, numbered: slots_head is
        // the oldest not yet in `out`, slots_tail the next number to give out.
        // Empty unless commands are on other threads. The slot of number n is
        // slots[n & (slots.size() - 1)]; the size is a power of two.
        socket_t fd = INVALID_SOCKET;
        std::vector<ReplySlot> slots;
        uint64_t slots_head = 0;
        uint64_t slots_tail = 0;
        size_t slot_bytes = 0;           // replies waiting in slots

        size_t pendingReplies() const { return static_cast<size_t>(slots_tail - slots_head); }
        ReplySlot& slot(uint64_t number) { return slots[number & (slots.size() - 1)]; }
    };

    // Commands sent to another thread in one go, and their replies. The thread
    // that fills a batch (its home) owns it: the batch goes to the other
    // thread, comes back with the replies in it, and is used again. So once
    // batches have grown to their usual size, nothing is allocated per command.
    struct Item {
        socket_t fd;            // whose reply, checked against client_id: the
        uint64_t client_id;     // fd may be reused if the client has gone
        uint64_t number;        // its reply slot
        size_t reply_end;       // where its reply ends in Batch::replies
        int protocol;           // RESP version to answer in
    };
    struct Batch {
        std::string requests;   // the commands, as the client sent them
        std::string replies;
        std::vector<Item> items;
        size_t home = 0;        // the thread that fills it and owns it
        size_t owner = 0;       // the thread it was sent to
        bool answered = false;
    };

    // Where a command runs, besides a thread's number
    static constexpr size_t kRunHere = SIZE_MAX;       // no keys, or it will be refused
    static constexpr size_t kCrossThread = SIZE_MAX - 1;
    static constexpr size_t kWholeServer = SIZE_MAX - 2;

    enum class SendStatus { Done, Blocked, Failed };

    // I/O multiplexing handlers
    int prepareToWait();                             // Returns how long the poller may wait
    bool adoptNewClients();                          // True if any arrived
    bool collectBatches();                           // From other threads; true if any arrived
    void processBatches();                           // Run requests, deliver replies
    void runBatch(Batch& batch);
    void deliverReplies(Batch& batch);
    void deliverReply(Client& client, uint64_t number, std::string_view reply);
    void publishReplies();                           // Once their AOF records are written
    void publishRequests();
    void sendBatch(size_t to, Batch* batch);
    size_t routeOf(const CommandContext& ctx, const Args& args) const;
    void forward(Client& client, size_t owner, std::string_view command, int protocol);
    Batch* takeBatch();
    void recycleBatch(Batch* batch);
    // Space for the client's next reply; call replyDone() once it is written
    std::string& replyBuffer(Client& client);
    void replyDone(Client& client);
    ReplySlot& reserveSlot(Client& client);
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

    // Commands of one client that can be on other threads at once. With its
    // replies still unknown, the client's memory is bounded by this count. A
    // longer pipeline waits, and TCP flow control slows the client down.
    static constexpr size_t kMaxPendingReplies = 256;

    // Batches a thread can have out at another thread at once. A ring from A
    // to B holds at most A's batches for B and B's batches on their way home,
    // so with this limit a ring has room by construction and a push can't fail.
    static constexpr size_t kMaxBatchesOut = 32;
    static constexpr size_t kBatchRingSize = 2 * kMaxBatchesOut;

    // A batch that grew past this gives its memory back when it returns
    static constexpr size_t kKeepBatchBytes = size_t{1} << 20;

    // A reply slot keeps at most this much memory for reuse
    static constexpr size_t kKeepSlotBytes = size_t{64} * 1024;

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
    size_t index_;              // this loop's number
    size_t loops_;              // how many there are
    InMemoryStorage& storage_;
    AOFLogger* aof_ = nullptr;  // optional: flushed before replies are sent, and for BGREWRITEAOF

    // Tells the event loop which sockets are ready
    std::unique_ptr<Poller> poller_;
    socket_t listen_socket_ = INVALID_SOCKET;   // INVALID_SOCKET: another loop accepts

    // Only with more than one thread; a single loop has no one to hear from
    std::unique_ptr<Wakeup> wakeup_;
    std::unique_ptr<SpscRing<NewClient>> new_clients_;

    // One ring from each other thread to this one, by the sender's number
    std::vector<std::unique_ptr<SpscRing<Batch*>>> inbound_;
    std::vector<Batch*> inbox_;             // taken from the rings, not processed yet
    std::vector<Batch*> replies_ready_;     // run here, waiting for the AOF write
    std::vector<Batch*> open_batches_;      // per thread: being filled, or nullptr
    std::vector<size_t> batches_out_;       // per thread: sent and not returned yet
    std::vector<Batch*> free_batches_;
    std::vector<std::unique_ptr<Batch>> all_batches_;

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

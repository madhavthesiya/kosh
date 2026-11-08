#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Append-Only File logger: every write command is appended to a file in RESP
// format (binary-safe), so the state can be rebuilt by replaying it at startup.
//
// Why RESP format instead of plain text?
//   Plain text: "SET greeting hello world" → on replay, "world" is lost
//   RESP:       "*3\r\n$3\r\nSET\r\n$8\r\ngreeting\r\n$11\r\nhello world\r\n"
//               → length-prefixed, so spaces in values are handled correctly.
//   This is exactly what real Redis does for its AOF file.
//
// Why a separate class?
//   Single Responsibility: AOFLogger handles only file I/O.
//   InMemoryStorage handles only in-memory data operations.
//   They are composed together, not tightly coupled.
//
// Why a file descriptor, not std::ofstream?
//   Durability needs fsync(), truncation needs ftruncate(), and handling a
//   failed write needs to know how many bytes reached the file. A stream gives
//   access to none of these.
//
// Group commit (deferred mode, used by the server):
//   Records are only buffered as commands run, and flush() writes everything
//   buffered with one write(). The server flushes once per pass of its event
//   loop, and always before sending a reply, so no client ever sees a reply
//   whose record isn't with the OS yet: the same guarantee as one write() per
//   command, for far fewer system calls. Redis does the same (it writes its AOF
//   buffer in beforeSleep(), before sending replies).

class AOFLogger {
public:
    explicit AOFLogger(const std::string& filename);
    ~AOFLogger();       // writes what is still buffered

    AOFLogger(const AOFLogger&) = delete;
    AOFLogger& operator=(const AOFLogger&) = delete;

    // Appends a complete RESP record to the AOF (thread-safe). No trailing \n is
    // added: RESP has its own terminators (\r\n).
    //
    // Immediate mode (the default): written at once, with one write(). It then
    // survives a process crash, but without fsync a power cut can still lose it.
    // Deferred mode: buffered until flush().
    //
    // If a write fails (a full disk), the bytes it didn't write stay buffered,
    // and later records go after them, so nothing is lost or reordered; they are
    // written as soon as the file takes them. Returns false while writes are
    // failing. The command that hit the error has already changed the data in
    // memory, so it still succeeds; the server refuses the writes after it (see
    // flushPending()), as Redis does.
    bool log(std::string_view data);

    // The same, but `append(buffer)` appends the record to the buffer itself,
    // so the record is never built as a separate string.
    template<typename Append>
    bool logWith(Append&& append) {
        std::lock_guard<std::mutex> lock(mutex_);
        append(buffer_);
        if(deferred_) return !failing_;
        return writeBuffer();
    }

    // Deferred mode on or off. Turning it off writes what is buffered.
    void setDeferred(bool deferred);

    // When the AOF is synced to the disk with fdatasync() (Redis's appendfsync):
    //   Always       after every write, so before any reply is sent: a reply
    //                means the write survives a power cut. With group commit
    //                that is one sync per pass of the event loop, not per
    //                command. A write or sync that fails is fatal (failedForGood()).
    //   EverySecond  by a background thread, once a second if anything was
    //                written: a power cut loses at most about a second of
    //                writes. A sync that fails refuses writes, as a failed write
    //                does (flushPending()), until a sync succeeds.
    //   No           left to the OS (Linux writes dirty pages back within ~30 s).
    enum class FsyncPolicy { Always, EverySecond, No };
    void setFsyncPolicy(FsyncPolicy policy);

    // With Always, a write or sync failed: a reply can no longer mean that the
    // write is on the disk, so the server must stop, as Redis does
    bool failedForGood() const { return failed_for_good_; }

    // Syncs done so far, by any policy (for tests)
    size_t syncs() const { return syncs_; }

    // Writes everything buffered, with one write(). True if nothing is left.
    bool flush();

    // Tries again to write what failed writes left buffered. True if writes
    // and syncs aren't failing: then a write command may run (its record can
    // be saved).
    bool flushPending();

    // Why writing or syncing failed, while it is failing
    std::string lastWriteError() const;

    // Whether writes or syncs are failing now, without retrying them (INFO's
    // aof_last_write_status)
    bool failing() const { return failing_ || sync_failing_; }

    // False if the file could not be opened; log() would then silently drop writes.
    bool isOpen() const { return fd_ >= 0; }

    // Read the entire AOF file as a raw byte string.
    // The caller uses RespParser to extract individual commands.
    std::string readRaw() const;

    // Replaces the AOF with a compacted set of RESP-formatted commands (each
    // string a complete RESP array). Crash-safe: the new contents are written
    // to "<file>.tmp", synced to disk, and renamed over the AOF, so a failure
    // or crash at any point leaves either the old file or the new one, whole.
    // Returns false, with the AOF untouched, if that fails (see lastError()).
    [[nodiscard]] bool rewrite(const std::vector<std::string>& commands);

    // Cuts the file to its first `size` bytes; false if that fails (see lastError())
    [[nodiscard]] bool truncate(size_t size);

    // Why the last rewrite or truncate failed
    std::string lastError() const;

    std::string getFilename() const { return filename_; }

private:
    // With mutex_ held. writeBuffer() writes what is buffered (and syncs it,
    // with Always); setWriteError() records why it can't, and returns false.
    bool writeBuffer();
    bool setWriteError(const std::string& error);

    // EverySecond's background thread, and stopping it
    void syncEverySecond();
    void stopSyncThread();

    std::string filename_;
    int fd_ = -1;           // opened for appending; -1 if it couldn't be opened
    std::string last_error_;        // why the last rewrite or truncate failed
    std::string buffer_;            // records not written yet, in order
    bool deferred_ = false;         // buffer until flush(), instead of writing at once
    std::string write_error_;       // why, while writes are failing
    std::atomic<bool> failing_{false};  // a write failed; read without the lock
    mutable std::mutex mutex_;      // separate mutex from storage — no contention

    FsyncPolicy policy_ = FsyncPolicy::No;
    bool written_since_sync_ = false;           // data the disk may not have yet
    std::string sync_error_;                    // why, while syncs are failing
    std::atomic<bool> sync_failing_{false};     // EverySecond: the last sync failed
    std::atomic<bool> failed_for_good_{false};  // Always: a write or sync failed
    std::atomic<size_t> syncs_{0};
    std::thread sync_thread_;                   // EverySecond only
    std::condition_variable sync_wake_;         // wakes it early, to stop
    bool stop_sync_thread_ = false;
};

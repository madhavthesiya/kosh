#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
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

class AOFLogger {
public:
    explicit AOFLogger(const std::string& filename);
    ~AOFLogger();       // writes what is still buffered

    AOFLogger(const AOFLogger&) = delete;
    AOFLogger& operator=(const AOFLogger&) = delete;

    // Appends a complete RESP record to the AOF (thread-safe). No trailing \n is
    // added: RESP has its own terminators (\r\n). It is written at once, with
    // one write(): it then survives a process crash, but without fsync a power
    // cut can still lose it.
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
        return writeBuffer();
    }

    // Tries again to write what failed writes left buffered. True if writes
    // aren't failing: then a write command may run (its record can be saved).
    bool flushPending();

    // Why writing failed, while it is failing
    std::string lastWriteError() const;

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
    // With mutex_ held. writeBuffer() writes what is buffered; setWriteError()
    // records why it can't, and returns false.
    bool writeBuffer();
    bool setWriteError(const std::string& error);

    std::string filename_;
    int fd_ = -1;           // opened for appending; -1 if it couldn't be opened
    std::string last_error_;        // why the last rewrite or truncate failed
    std::string buffer_;            // records not written yet, in order
    std::string write_error_;       // why, while writes are failing
    std::atomic<bool> failing_{false};  // a write failed; read without the lock
    mutable std::mutex mutex_;      // separate mutex from storage — no contention
};

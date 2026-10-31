#pragma once
#include <atomic>
#include <string>
#include <vector>
#include <mutex>

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
    ~AOFLogger();

    AOFLogger(const AOFLogger&) = delete;
    AOFLogger& operator=(const AOFLogger&) = delete;

    // Append raw RESP data to the AOF file (thread-safe).
    // The caller is responsible for formatting the data as RESP.
    // No trailing \n is added — RESP already has its own terminators (\r\n).
    // Each call is one write() to the OS: the data survives a process crash,
    // but without fsync a power cut can still lose recent writes.
    //
    // If the write fails (a full disk), the bytes it didn't write wait in
    // memory, and later calls append after them, so nothing is lost or
    // reordered; they are written as soon as the file takes them. Returns false
    // while bytes are waiting. The command that hit the error has already
    // changed the data in memory, so it still succeeds; the server refuses the
    // writes after it (see flushPending()), as Redis does.
    bool log(const std::string& data);

    // Tries again to write what failed writes left waiting. True if nothing
    // is waiting: then a write command may run (its record can be saved).
    bool flushPending();

    // Why writing failed, while bytes are waiting
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
    // With mutex_ held. writePending() writes what is waiting; setWriteError()
    // records why it can't, and returns false.
    bool writePending();
    bool setWriteError(const std::string& error);

    std::string filename_;
    int fd_ = -1;           // opened for appending; -1 if it couldn't be opened
    std::string last_error_;        // why the last rewrite or truncate failed
    std::string pending_;           // bytes a failed write left unwritten, in order
    std::string write_error_;       // why, while pending_ isn't empty
    std::atomic<bool> failing_{false};  // pending_ isn't empty (read without the lock)
    mutable std::mutex mutex_;      // separate mutex from storage — no contention
};

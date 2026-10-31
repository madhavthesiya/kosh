#pragma once
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
    void log(const std::string& data);

    // False if the file could not be opened; log() would then silently drop writes.
    bool isOpen() const { return fd_ >= 0; }

    // Read the entire AOF file as a raw byte string.
    // The caller uses RespParser to extract individual commands.
    std::string readRaw() const;

    // Rewrite the AOF file with a compacted set of RESP-formatted commands.
    // Each string in the vector is a complete RESP array (already formatted).
    void rewrite(const std::vector<std::string>& commands);

    std::string getFilename() const { return filename_; }

private:
    std::string filename_;
    int fd_ = -1;           // opened for appending; -1 if it couldn't be opened
    std::mutex mutex_;      // separate mutex from storage — no contention
};

#include "AOFLogger.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <system_error>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

namespace {

// The few file calls the AOF needs, on POSIX and Windows (the CRT's
// underscore names, which also avoid MSVC's deprecation warnings)
#ifdef _WIN32
int openFile(const std::string& path, int flags) {
    return _open(path.c_str(), flags | _O_BINARY, _S_IREAD | _S_IWRITE);
}
constexpr int kAppendFlags = _O_WRONLY | _O_APPEND | _O_CREAT;
constexpr int kTruncateFlags = _O_WRONLY | _O_TRUNC | _O_CREAT;
long long writeSome(int fd, const char* data, size_t size) {
    return _write(fd, data, static_cast<unsigned>(std::min<size_t>(size, INT_MAX)));
}
void closeFile(int fd) { _close(fd); }
bool syncFile(int fd) { return _commit(fd) == 0; }
bool truncateFile(int fd, size_t size) { return _chsize_s(fd, static_cast<long long>(size)) == 0; }
// NTFS journals its metadata, and Windows offers no portable way to sync a directory
bool syncDirectory(const std::string&) { return true; }
#else
int openFile(const std::string& path, int flags) {
    return open(path.c_str(), flags | O_CLOEXEC, 0644);
}
constexpr int kAppendFlags = O_WRONLY | O_APPEND | O_CREAT;
constexpr int kTruncateFlags = O_WRONLY | O_TRUNC | O_CREAT;
long long writeSome(int fd, const char* data, size_t size) {
    return write(fd, data, size);
}
void closeFile(int fd) { close(fd); }
bool syncFile(int fd) { return fsync(fd) == 0; }
bool truncateFile(int fd, size_t size) { return ftruncate(fd, static_cast<off_t>(size)) == 0; }
// A rename is a change to the directory, not to the file: it is on disk only
// once the directory itself is synced
bool syncDirectory(const std::string& file) {
    std::string dir = std::filesystem::path(file).parent_path().string();
    int fd = open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(fd < 0) return false;
    bool synced = fsync(fd) == 0;
    close(fd);
    return synced;
}
#endif

// Writes as much of `data` as the file takes. write() may write less than asked
// (a signal, a full disk), so it continues from where the last call stopped.
// Returns the bytes written: fewer than `size` means an error, in errno.
size_t writeAll(int fd, const char* data, size_t size) {
    size_t done = 0;
    while(done < size) {
        long long n = writeSome(fd, data + done, size - done);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) {
            if(n == 0) errno = EIO;     // no progress and no error: don't spin
            break;
        }
        done += static_cast<size_t>(n);
    }
    return done;
}

// After a write, the buffer keeps its memory for the next records, up to this
// much; a larger one, grown by a burst, is given back.
constexpr size_t kKeepBufferBytes = size_t(1) << 20;

}  // namespace

AOFLogger::AOFLogger(const std::string& filename)
    : filename_(filename), fd_(openFile(filename, kAppendFlags)) {}

AOFLogger::~AOFLogger() {
    // Last chance for buffered records (no other thread can use the logger any
    // more, so no lock)
    if(!buffer_.empty() && !writeBuffer()) {
        std::cerr << "Warning: " << buffer_.size()
                  << " bytes could not be written to the AOF and are lost\n";
    }
    if(fd_ >= 0) closeFile(fd_);
}

bool AOFLogger::log(std::string_view data) {
    return logWith([data](std::string& buffer) { buffer.append(data); });
}

bool AOFLogger::flushPending() {
    if(!failing_) return true;      // checked without the lock: one load per write command
    std::lock_guard<std::mutex> lock(mutex_);
    return writeBuffer();
}

std::string AOFLogger::lastWriteError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return write_error_;
}

bool AOFLogger::writeBuffer() {
    if(fd_ < 0) return setWriteError("the AOF is not open");
    size_t written = writeAll(fd_, buffer_.data(), buffer_.size());
    int error = errno;
    if(written < buffer_.size()) {
        // Keep what wasn't written, in order, for the next attempt
        buffer_.erase(0, written);
        return setWriteError(std::generic_category().message(error));
    }
    // clear() keeps the capacity, so the next records need no allocation. A
    // burst (a large pipeline) can grow the buffer to megabytes, though, so
    // give that memory back, as Redis does with its AOF buffer.
    if(buffer_.capacity() > kKeepBufferBytes) std::string().swap(buffer_);
    else buffer_.clear();
    if(failing_) {
        failing_ = false;
        write_error_.clear();
        std::cerr << "AOF writes work again\n";
    }
    return true;
}

bool AOFLogger::setWriteError(const std::string& error) {
    if(!failing_) {
        std::cerr << "Error writing to the AOF (" << error
                  << "): write commands are refused until it can be written\n";
    }
    failing_ = true;
    write_error_ = error;
    return false;
}

std::string AOFLogger::readRaw() const {
    std::ifstream in(filename_, std::ios::binary);
    if(!in.is_open()) return "";
    return std::string(std::istreambuf_iterator<char>(in), {});
}

bool AOFLogger::rewrite(const std::vector<std::string>& commands) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string temp = filename_ + ".tmp";
    std::error_code ignored;

    // 1. Write the new contents to a temporary file; the AOF stays untouched.
    // 2. Sync it: otherwise, after a power cut, the rename below could be on
    //    disk while the data it names is not, leaving an empty or partial AOF.
    int out = openFile(temp, kTruncateFlags);
    if(out < 0) {
        last_error_ = "cannot create " + temp + ": " + std::generic_category().message(errno);
        return false;
    }
    bool written = true;
    for(const auto& cmd : commands) {
        if(!(written = writeAll(out, cmd.data(), cmd.size()) == cmd.size())) break;
    }
    if(written) written = syncFile(out);
    int error = errno;
    closeFile(out);
    if(!written) {
        last_error_ = "cannot write " + temp + ": " + std::generic_category().message(error);
        std::filesystem::remove(temp, ignored);
        return false;
    }

    // 3. Rename it over the AOF. The rename replaces the file atomically, so a
    //    crash leaves either the old AOF or the new one. The append handle is
    //    closed first: Windows can't replace an open file, and on POSIX it would
    //    go on writing to the replaced one. (std::filesystem::rename replaces an
    //    existing file on Windows too; std::rename fails there.)
    if(fd_ >= 0) closeFile(fd_);
    std::error_code renamed;
    std::filesystem::rename(temp, filename_, renamed);
    fd_ = openFile(filename_, kAppendFlags);    // the new file, or the old one
    if(renamed) {
        last_error_ = "cannot replace " + filename_ + ": " + renamed.message();
        std::filesystem::remove(temp, ignored);
        return false;
    }
    // The new file holds all the data, including the effect of every record
    // still buffered (left by failed writes). Those records belong at the end
    // of the old file; appending them to the new one would apply them twice.
    buffer_.clear();
    if(failing_ && fd_ >= 0) {
        failing_ = false;
        write_error_.clear();
        std::cerr << "AOF writes work again (rewritten)\n";
    }

    // 4. Sync the directory, so the rename itself survives a power cut. If that
    //    fails, either file is still a complete copy of the data.
    if(!syncDirectory(filename_)) {
        std::cerr << "Warning: could not sync the directory of " << filename_
                  << " after rewriting it\n";
    }
    return true;
}

bool AOFLogger::truncate(size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(fd_ < 0) {
        last_error_ = "the AOF is not open";
        return false;
    }
    if(!truncateFile(fd_, size)) {
        last_error_ = std::generic_category().message(errno);
        return false;
    }
    return true;      // the descriptor appends, so the next write lands at the new end
}

std::string AOFLogger::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

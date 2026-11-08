#include "AOFLogger.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
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
bool syncData(int fd) { return _commit(fd) == 0; }
int duplicateFile(int fd) { return _dup(fd); }
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
// The data, and the metadata needed to read it back (the size of a file
// that grew), but not, say, its modification time: what an AOF needs
bool syncData(int fd) {
#ifdef __APPLE__
    return fsync(fd) == 0;      // macOS has no fdatasync()
#else
    return fdatasync(fd) == 0;
#endif
}
int duplicateFile(int fd) { return fcntl(fd, F_DUPFD_CLOEXEC, 0); }
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
    stopSyncThread();
    // Last chance for buffered records (no other thread can use the logger any
    // more, so no lock)
    if(!buffer_.empty() && !writeBuffer()) {
        std::cerr << "Warning: " << buffer_.size()
                  << " bytes could not be written to the AOF and are lost\n";
    }
    // On a clean shutdown everything reaches the disk, whatever the policy
    // promised while running (Redis syncs at shutdown too)
    if(policy_ != FsyncPolicy::No && written_since_sync_ && fd_ >= 0) syncData(fd_);
    if(fd_ >= 0) closeFile(fd_);
}

bool AOFLogger::log(std::string_view data) {
    return logWith([data](std::string& buffer) { buffer.append(data); });
}

void AOFLogger::setDeferred(bool deferred) {
    std::lock_guard<std::mutex> lock(mutex_);
    deferred_ = deferred;
    if(!deferred_ && !buffer_.empty()) writeBuffer();
}

bool AOFLogger::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_.empty() || writeBuffer();
}

void AOFLogger::setFsyncPolicy(FsyncPolicy policy) {
    stopSyncThread();
    std::lock_guard<std::mutex> lock(mutex_);
    policy_ = policy;
    if(policy_ == FsyncPolicy::EverySecond) {
        stop_sync_thread_ = false;
        sync_thread_ = std::thread([this] { syncEverySecond(); });
    }
}

void AOFLogger::stopSyncThread() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_sync_thread_ = true;
    }
    sync_wake_.notify_all();
    if(sync_thread_.joinable()) sync_thread_.join();
}

// Once a second, if anything was written since the last sync, sync the file.
// The sync itself runs without the lock: it can take tens of milliseconds on a
// busy disk, and the event loop must not wait for it. It syncs a duplicate of
// the descriptor, taken under the lock: the same open file, which stays valid
// even if a rewrite closes fd_ meanwhile (the rewrite syncs its new file
// itself, so syncing the replaced one then is merely unnecessary).
void AOFLogger::syncEverySecond() {
    std::unique_lock<std::mutex> lock(mutex_);
    while(!stop_sync_thread_) {
        sync_wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_sync_thread_; });
        if(stop_sync_thread_ || !written_since_sync_ || fd_ < 0) continue;
        int fd = duplicateFile(fd_);
        written_since_sync_ = false;
        lock.unlock();
        bool synced = fd >= 0 && syncData(fd);
        int error = errno;
        if(fd >= 0) closeFile(fd);
        lock.lock();
        if(synced) {
            syncs_++;
            if(sync_failing_) {
                sync_failing_ = false;
                sync_error_.clear();
                std::cerr << "AOF fsync works again\n";
            }
        } else {
            written_since_sync_ = true;     // still not on the disk: next second
            if(!sync_failing_) {
                std::cerr << "Error syncing the AOF (" << std::generic_category().message(error)
                          << "): write commands are refused until it can be synced\n";
            }
            sync_failing_ = true;
            sync_error_ = "fsync: " + std::generic_category().message(error);
        }
    }
}

bool AOFLogger::flushPending() {
    // Checked without the lock: two loads per write command
    if(!failing_ && !sync_failing_) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    if(failing_ && !writeBuffer()) return false;
    return !sync_failing_;      // cleared by the background thread's next good sync
}

std::string AOFLogger::lastWriteError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failing_ ? write_error_ : sync_error_;
}

bool AOFLogger::writeBuffer() {
    if(fd_ < 0) return setWriteError("the AOF is not open");
    size_t written = writeAll(fd_, buffer_.data(), buffer_.size());
    int error = errno;
    if(written < buffer_.size()) {
        // Keep what wasn't written, in order, for the next attempt
        buffer_.erase(0, written);
        if(policy_ == FsyncPolicy::Always) failed_for_good_ = true;
        return setWriteError(std::generic_category().message(error));
    }
    // Everything is in the file now, so empty the buffer before anything else:
    // whatever happens next, these bytes must never be written twice.
    // clear() keeps the capacity, so the next records need no allocation. A
    // burst (a large pipeline) can grow the buffer to megabytes, though, so
    // give that memory back, as Redis does with its AOF buffer.
    if(buffer_.capacity() > kKeepBufferBytes) std::string().swap(buffer_);
    else buffer_.clear();

    if(policy_ == FsyncPolicy::Always) {
        // Before any reply goes out (the server flushes first). After a failed
        // sync the kernel may already have dropped the dirty pages, so a retry
        // that succeeds wouldn't prove the data is on the disk: give up, as
        // Redis does.
        if(!syncData(fd_)) {
            failed_for_good_ = true;
            return setWriteError("fsync: " + std::generic_category().message(errno));
        }
        syncs_++;
    } else {
        written_since_sync_ = true;
    }
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
        written = writeAll(out, cmd.data(), cmd.size()) == cmd.size();
        if(!written) break;
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
    // still buffered (not flushed yet, or left by failed writes). Those records
    // belong at the end of the old file; appending them to the new one would
    // apply them twice.
    buffer_.clear();
    written_since_sync_ = false;        // the new file was synced above
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

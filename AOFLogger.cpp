#include "AOFLogger.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <fstream>
#include <iterator>
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
#endif

// Writes all of `data`. write() may write less than asked (a signal, a full
// disk), so it continues from where the last call stopped. False on error.
bool writeAll(int fd, const char* data, size_t size) {
    while(size > 0) {
        long long n = writeSome(fd, data, size);
        if(n < 0) {
            if(errno == EINTR) continue;
            return false;
        }
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

AOFLogger::AOFLogger(const std::string& filename)
    : filename_(filename), fd_(openFile(filename, kAppendFlags)) {}

AOFLogger::~AOFLogger() {
    if(fd_ >= 0) closeFile(fd_);
}

void AOFLogger::log(const std::string& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(fd_ >= 0) writeAll(fd_, data.data(), data.size());
}

std::string AOFLogger::readRaw() const {
    std::ifstream in(filename_, std::ios::binary);
    if(!in.is_open()) return "";
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void AOFLogger::rewrite(const std::vector<std::string>& commands) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(fd_ >= 0) closeFile(fd_);
    int out = openFile(filename_, kTruncateFlags);
    if(out >= 0) {
        for(const auto& cmd : commands) writeAll(out, cmd.data(), cmd.size());
        closeFile(out);
    }
    fd_ = openFile(filename_, kAppendFlags);
}

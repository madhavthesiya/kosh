#pragma once
#include "Socket.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef __linux__
#include <sys/epoll.h>
#endif

// Readiness notification for many sockets, which lets one thread serve many
// clients: wait until some sockets are ready, handle just those, repeat.
//
// Sockets are registered once, with the events of interest. That suits both
// select(), which rebuilds its sets from the registrations on every wait, and
// epoll/kqueue, which keep them in the kernel, so the event loop does not need
// to know which one it is using.
class Poller {
public:
    struct Event {
        socket_t fd;
        bool readable;   // data, a connection to accept, end of stream or an error
        bool writable;   // send() can take more data
    };

    virtual ~Poller() = default;

    // "select" or "epoll", for the startup message
    virtual const char* name() const = 0;

    // How many sockets it can watch at once: FD_SETSIZE for select(), no limit
    // (SIZE_MAX) for epoll
    virtual size_t capacity() const = 0;

    // False if this socket can't be watched (a select() limit: see SelectPoller).
    virtual bool canWatch(socket_t fd) const = 0;

    virtual void add(socket_t fd) = 0;      // start watching, for readability
    virtual void remove(socket_t fd) = 0;   // stop watching; call before closing the socket

    // Which events to report for a watched socket. Ask for writability only while
    // there is something to send: a socket is writable almost all the time, so
    // watching an idle one would wake the loop for nothing.
    virtual void setInterest(socket_t fd, bool read, bool write) = 0;

    // Waits up to timeout_ms for watched sockets to become ready and fills `ready`
    // with them. Returns how many are ready: 0 on timeout or when a signal cut the
    // wait short, -1 on any other error.
    virtual int wait(int timeout_ms, std::vector<Event>& ready) = 0;
};

// select(): available everywhere. Each wait costs O(number of watched sockets),
// and fd_set has a fixed capacity, FD_SETSIZE, with a different meaning per OS:
//   POSIX:   a bit array indexed by socket number, so only sockets numbered
//            below FD_SETSIZE fit; FD_SET on a higher one writes out of bounds.
//   Windows: a list of up to FD_SETSIZE sockets of any value; FD_SET silently
//            ignores sockets once it is full.
class SelectPoller final : public Poller {
public:
    const char* name() const override { return "select"; }
    size_t capacity() const override { return FD_SETSIZE; }
    bool canWatch(socket_t fd) const override;
    void add(socket_t fd) override;
    void remove(socket_t fd) override;
    void setInterest(socket_t fd, bool read, bool write) override;
    int wait(int timeout_ms, std::vector<Event>& ready) override;

private:
    struct Interest {
        bool read = true;
        bool write = false;
    };
    std::unordered_map<socket_t, Interest> watched_;
};

#ifdef __linux__
// epoll (Linux): the kernel keeps the set of watched sockets, so a wait costs
// O(sockets ready), where select() scans every watched socket on every call,
// and any socket number can be watched. Level-triggered, like select(): a ready
// socket is reported until it is drained, so the event loop is the same for
// both. Redis uses epoll the same way.
class EpollPoller final : public Poller {
public:
    EpollPoller();
    ~EpollPoller() override;
    EpollPoller(const EpollPoller&) = delete;
    EpollPoller& operator=(const EpollPoller&) = delete;

    bool isOpen() const { return epfd_ >= 0; }

    const char* name() const override { return "epoll"; }
    size_t capacity() const override { return SIZE_MAX; }
    bool canWatch(socket_t fd) const override;
    void add(socket_t fd) override;
    void remove(socket_t fd) override;
    void setInterest(socket_t fd, bool read, bool write) override;
    int wait(int timeout_ms, std::vector<Event>& ready) override;

private:
    int epfd_;
    size_t watched_ = 0;
    std::vector<epoll_event> events_;   // filled by epoll_wait()
};
#endif

// The poller called `name` ("epoll" or "select"), or nullptr if this system
// has none by that name (no epoll outside Linux)
std::unique_ptr<Poller> makePoller(const std::string& name);

// The best one this system has: epoll on Linux, select() elsewhere
const char* defaultPollerName();

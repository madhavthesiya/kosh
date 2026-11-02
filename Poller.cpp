#include "Poller.h"
#include <algorithm>
#ifdef __linux__
#include <cerrno>
#include <unistd.h>
#endif

bool SelectPoller::canWatch(socket_t fd) const {
#ifdef _WIN32
    (void)fd;
    return watched_.size() < FD_SETSIZE;
#else
    return fd < FD_SETSIZE;
#endif
}

void SelectPoller::add(socket_t fd) {
    watched_[fd] = Interest{};
}

void SelectPoller::remove(socket_t fd) {
    watched_.erase(fd);
}

void SelectPoller::setInterest(socket_t fd, bool read, bool write) {
    auto it = watched_.find(fd);
    if(it != watched_.end()) it->second = Interest{read, write};
}

int SelectPoller::wait(int timeout_ms, std::vector<Event>& ready) {
    ready.clear();

    // select() takes the sets afresh on every call and overwrites them with the
    // result, so they are rebuilt from the registrations each time.
    fd_set readfds, writefds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    socket_t max_fd = 0;
    for(const auto& [fd, interest] : watched_) {
        if(interest.read) FD_SET(fd, &readfds);
        if(interest.write) FD_SET(fd, &writefds);
        if(fd > max_fd) max_fd = fd;
    }

    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    // The first argument is the highest socket number + 1 (Windows ignores it).
    int n = select(static_cast<int>(max_fd + 1), &readfds, &writefds, nullptr, &timeout);
    if(n < 0) return isInterrupted(lastSocketError()) ? 0 : -1;

    for(const auto& [fd, interest] : watched_) {
        bool readable = interest.read && FD_ISSET(fd, &readfds);
        bool writable = interest.write && FD_ISSET(fd, &writefds);
        if(readable || writable) ready.push_back({fd, readable, writable});
    }
    return static_cast<int>(ready.size());
}

#ifdef __linux__
EpollPoller::EpollPoller() : epfd_(epoll_create1(EPOLL_CLOEXEC)) {}

EpollPoller::~EpollPoller() {
    if(epfd_ >= 0) close(epfd_);
}

bool EpollPoller::canWatch(socket_t) const {
    return true;        // any socket number; the open-file limit is the only bound
}

void EpollPoller::add(socket_t fd) {
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = fd;
    if(epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &event) == 0) watched_++;
}

void EpollPoller::remove(socket_t fd) {
    if(epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) == 0) watched_--;
}

void EpollPoller::setInterest(socket_t fd, bool read, bool write) {
    epoll_event event{};
    if(read) event.events |= EPOLLIN;
    if(write) event.events |= EPOLLOUT;
    event.data.fd = fd;
    epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &event);
}

int EpollPoller::wait(int timeout_ms, std::vector<Event>& ready) {
    ready.clear();
    // Room for every watched socket to be ready at once
    size_t room = std::max<size_t>(watched_, 64);
    if(events_.size() < room) events_.resize(room);
    int n = epoll_wait(epfd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
    if(n < 0) return errno == EINTR ? 0 : -1;
    for(int i = 0; i < n; i++) {
        const epoll_event& event = events_[static_cast<size_t>(i)];
        // An error or a hang-up is reported as both readable and writable, as
        // Redis does: the next recv() or send() finds out what happened. (epoll
        // reports these even for sockets watched for neither.)
        bool failed = (event.events & (EPOLLERR | EPOLLHUP)) != 0;
        ready.push_back({event.data.fd, (event.events & EPOLLIN) != 0 || failed,
                         (event.events & EPOLLOUT) != 0 || failed});
    }
    return n;
}
#endif

std::unique_ptr<Poller> makePoller(const std::string& name) {
    if(name == "select") return std::make_unique<SelectPoller>();
#ifdef __linux__
    if(name == "epoll") {
        auto poller = std::make_unique<EpollPoller>();
        if(poller->isOpen()) return poller;
    }
#endif
    return nullptr;
}

const char* defaultPollerName() {
#ifdef __linux__
    return "epoll";
#else
    return "select";
#endif
}

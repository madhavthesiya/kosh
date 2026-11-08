#pragma once
#include <atomic>

// How one thread wakes another that may be asleep in its poller.
//
// A thread that has run out of work blocks in epoll_wait(). Work sent to it
// through a ring (SpscRing.h) doesn't make any socket readable, so the sender
// has to wake it, by writing to an eventfd the sleeper's poller watches. A
// write is a system call, too slow to make on every message, so the sender
// makes it only if the receiver says it is going to sleep.
//
// The danger is a lost wakeup: the receiver checks its rings, finds nothing,
// and goes to sleep, while a message arrives just after the check and the
// sender sees that the receiver "isn't asleep" just before the flag is set.
// The message then sits there until something else wakes the receiver.
//
// Each side does the same two things in opposite roles:
//   receiver:  sets asleep_           then checks the rings
//   sender:    publishes the message  then checks asleep_
// If either side's first step always became visible to the other side's
// second step, at least one of them would see the other, and nothing could be
// lost: either the receiver finds the message, or the sender writes the
// eventfd. But release and acquire don't promise that. Even x86 lets a load
// overtake an earlier store to a different address (the store sits in the
// store buffer), so both sides could read the old value. This is the one
// reordering x86 performs, and it is exactly the one that matters here.
//
// A sequentially consistent fence between the two steps, on both sides, rules
// it out. All such fences have one global order; whichever side's fence comes
// first, the other side's check is after it and sees that side's store.
//
// ThreadSanitizer can't see this bug: there is no data race, only a wakeup
// that never happens. tests/test_wakeup.cpp detects it directly.
#ifdef __linux__
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstdint>
#include <stdexcept>

class Wakeup {
public:
    Wakeup() : fd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
        if(fd_ < 0) throw std::runtime_error("eventfd() failed");
    }
    ~Wakeup() { close(fd_); }
    Wakeup(const Wakeup&) = delete;
    Wakeup& operator=(const Wakeup&) = delete;

    // For the receiver's poller to watch, for readability
    int fd() const { return fd_; }

    // The receiver, before its last check for work. If it finds none, it may
    // then sleep.
    void aboutToSleep() {
        asleep_.store(true, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    // The receiver, once awake
    void awake() { asleep_.store(false, std::memory_order_relaxed); }

    // The receiver, when its poller reports fd(): empties the eventfd, so that
    // a level-triggered poller stops reporting it
    void consume() {
        uint64_t count;
        if(read(fd_, &count, sizeof(count)) < 0) return;    // EAGAIN: nothing to consume
    }

    // The sender, after publishing work to the receiver
    void notify() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if(!asleep_.load(std::memory_order_relaxed)) return;
        const uint64_t one = 1;
        // Can only fail with EAGAIN, when the counter is full: then it is
        // readable and the receiver will wake anyway
        if(write(fd_, &one, sizeof(one)) < 0) return;
    }

private:
    // asleep_ is written by the receiver every pass and read by senders on
    // every message, so it gets a line of its own: on a line with the fd, or
    // with whatever follows this object, it would slow those down too
    alignas(64) std::atomic<bool> asleep_{false};
    alignas(64) const int fd_;
};

#else   // Several threads need epoll, so elsewhere there is nothing to wake

class Wakeup {
public:
    int fd() const { return -1; }
    void aboutToSleep() {}
    void awake() {}
    void consume() {}
    void notify() {}
};

#endif

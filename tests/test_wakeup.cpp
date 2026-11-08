// Wakeup: no message may ever be left waiting while its receiver sleeps.
//
// ThreadSanitizer can't find a lost wakeup (nothing races), and a timing test
// would blame a slow machine. So this checks the guarantee itself. Two threads
// pass a message back and forth, each sleeping in poll() between messages,
// with a short timeout. When a poll() times out and a message turns out to be
// waiting, one of two things happened:
//   - the sender has published but hasn't reached notify() yet: not a bug;
//   - the sender finished notify() and the eventfd still isn't readable: the
//     wakeup was lost.
// So the receiver waits until the sender says it finished notify(), and then
// looks at the eventfd. Timing can't produce a false alarm: while the receiver
// stays marked asleep, a correct sender must have written the eventfd.
//
// With either fence in Wakeup.h removed, this reported lost wakeups on x86
// within 100,000 messages, because a load can overtake an earlier store there.

#include "../SpscRing.h"
#include "../Wakeup.h"

#include <poll.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

namespace {

struct Side {
    SpscRing<uint64_t> inbox{16};       // messages to this side
    Wakeup wakeup;                      // wakes this side
    std::atomic<uint64_t> notified{0};  // the last message to this side whose notify() returned
};

bool readable(int fd, int timeout_ms) {
    struct pollfd p{fd, POLLIN, 0};
    return poll(&p, 1, timeout_ms) > 0;
}

struct Counts {
    uint64_t timeouts = 0;
    uint64_t lost = 0;
    uint64_t out_of_order = 0;
};

// Blocks for the next message, the way the event loop does
uint64_t receive(Side& self, Counts& counts) {
    uint64_t message = 0;
    while(true) {
        if(self.inbox.tryPop(message)) return message;
        self.wakeup.aboutToSleep();
        if(self.inbox.tryPop(message)) {
            self.wakeup.awake();
            return message;
        }
        if(readable(self.wakeup.fd(), 2)) {
            self.wakeup.awake();
            self.wakeup.consume();
            continue;
        }
        // Timed out, still marked asleep
        counts.timeouts++;
        if(self.inbox.tryPop(message)) {
            while(self.notified.load() < message) std::this_thread::yield();
            if(readable(self.wakeup.fd(), 0)) self.wakeup.consume();
            else counts.lost++;
            self.wakeup.awake();
            return message;
        }
        self.wakeup.awake();
    }
}

void send(Side& to, uint64_t message) {
    while(!to.inbox.tryPush(message)) std::this_thread::yield();
    to.wakeup.notify();
    to.notified.store(message);
}

// `self` sends odd numbers and gets even ones, or the other way round
void play(Side& self, Side& peer, uint64_t last, bool first, Counts& counts) {
    uint64_t next = first ? 1 : 2;
    while(next <= last) {
        if(first) send(peer, next);
        const uint64_t expected = first ? next + 1 : next - 1;
        if(!first || next + 1 <= last) {
            if(receive(self, counts) != expected) counts.out_of_order++;
        }
        if(!first) send(peer, next);
        next += 2;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    const uint64_t last = argc > 1 ? std::stoull(argv[1]) : 100000;
    auto a = std::make_unique<Side>();
    auto b = std::make_unique<Side>();
    Counts counts_a, counts_b;
    std::thread other([&] { play(*b, *a, last, false, counts_b); });
    play(*a, *b, last, true, counts_a);
    other.join();

    const uint64_t lost = counts_a.lost + counts_b.lost;
    const uint64_t wrong = counts_a.out_of_order + counts_b.out_of_order;
    std::cout << last << " messages, " << counts_a.timeouts + counts_b.timeouts << " timeouts, "
              << lost << " lost wakeups, " << wrong << " out of order\n";
    if(lost != 0 || wrong != 0) {
        std::cout << "Wakeup: FAILED\n";
        return 1;
    }
    std::cout << "Wakeup: all checks passed\n";
    return 0;
}

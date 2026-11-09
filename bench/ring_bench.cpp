// What a cross-thread queue costs, and whether each part of SpscRing's design
// pays for itself. Compares, with threads pinned to CPUs:
//
//   shared-line     a ring with head and tail side by side, no remembered counters
//   padded          head and tail on separate cache lines, no remembered counters
//   cached-only     remembers the other side's counter, head and tail side by side
//   padded+cached   SpscRing itself: both
//   padded128       the same with 128-byte spacing (Intel's prefetcher fetches
//                   cache lines in pairs, so 64 may not be far enough)
//   mutex-inbox     one locked vector per receiving thread, emptied with a swap:
//                   the simple alternative to a ring per pair of threads
//
// Three workloads:
//   one-way    one thread sends as fast as it can, one receives
//   ping-pong  one message goes there and back: the cost when the queue is
//              almost always empty, and the receiver is waiting for it
//   all-to-all every thread sends to every other and receives: the server's shape
//
// Rounds are interleaved, with the order rotated each round, so a machine that
// changes speed partway affects every variant alike. Each cell is the median of
// the rounds, with the fastest and slowest beside it.
//
// Linux only (CPU pinning). Usage:
//   ring_bench [--rounds 9] [--ms 300] [--cpus 0,2,4,6,8,10,12,14]

#include "../SpscRing.h"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr size_t kPairCapacity = 1024;

void pinTo(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if(pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        std::fprintf(stderr, "could not pin to CPU %d\n", cpu);
        std::exit(1);
    }
}

// The ring with its spacing and its remembered counters as parameters. Kept
// here rather than in SpscRing.h: the server needs one version, not a family.
// Align 8 puts every field next to the others.
template<size_t Align, bool Cached>
class ExperimentRing {
public:
    explicit ExperimentRing(size_t capacity) : capacity_(capacity), mask_(capacity - 1), slots_(capacity) {}

    bool tryPush(uint64_t value) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if(Cached) {
            if(tail - cached_head_ == capacity_) {
                cached_head_ = head_.load(std::memory_order_acquire);
                if(tail - cached_head_ == capacity_) return false;
            }
        } else if(tail - head_.load(std::memory_order_acquire) == capacity_) {
            return false;
        }
        slots_[tail & mask_] = value;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool tryPop(uint64_t& out) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if(Cached) {
            if(head == cached_tail_) {
                cached_tail_ = tail_.load(std::memory_order_acquire);
                if(head == cached_tail_) return false;
            }
        } else if(head == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        out = slots_[head & mask_];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

private:
    alignas(Align) std::atomic<size_t> tail_{0};
    size_t cached_head_ = 0;
    alignas(Align) std::atomic<size_t> head_{0};
    size_t cached_tail_ = 0;
    alignas(Align) size_t capacity_;
    size_t mask_;
    std::vector<uint64_t> slots_;
};

// One ring per ordered pair of threads
template<typename Ring>
class RingMesh {
public:
    explicit RingMesh(size_t threads) : threads_(threads) {
        for(size_t i = 0; i < threads * threads; i++) rings_.push_back(std::make_unique<Ring>(kPairCapacity));
    }
    bool send(size_t from, size_t to, uint64_t value) { return rings_[from * threads_ + to]->tryPush(value); }
    // Everything waiting for `self`, from every other thread; how many
    size_t drain(size_t self, uint64_t& last) {
        size_t received = 0;
        for(size_t from = 0; from < threads_; from++) {
            if(from == self) continue;
            Ring& ring = *rings_[from * threads_ + self];
            while(ring.tryPop(last)) received++;
        }
        return received;
    }

private:
    size_t threads_;
    std::vector<std::unique_ptr<Ring>> rings_;
};

// One locked vector per receiver. drain() swaps it out, so the lock is held
// for a few instructions and the items are processed outside it.
class MutexMesh {
public:
    explicit MutexMesh(size_t threads) : limit_(kPairCapacity * (threads > 1 ? threads - 1 : 1)) {
        for(size_t i = 0; i < threads; i++) inboxes_.push_back(std::make_unique<Inbox>());
    }
    bool send(size_t, size_t to, uint64_t value) {
        Inbox& inbox = *inboxes_[to];
        std::lock_guard<std::mutex> lock(inbox.mutex);
        if(inbox.items.size() >= limit_) return false;
        inbox.items.push_back(value);
        return true;
    }
    size_t drain(size_t self, uint64_t& last) {
        Inbox& inbox = *inboxes_[self];
        {
            std::lock_guard<std::mutex> lock(inbox.mutex);
            inbox.spare.swap(inbox.items);
        }
        size_t received = 0;
        for(uint64_t value : inbox.spare) {
            last = value;
            received++;
        }
        inbox.spare.clear();
        return received;
    }

private:
    struct Inbox {
        alignas(64) std::mutex mutex;
        std::vector<uint64_t> items;
        alignas(64) std::vector<uint64_t> spare;    // the receiver's alone
    };
    size_t limit_;
    std::vector<std::unique_ptr<Inbox>> inboxes_;
};

// Runs body(index, stop) on one pinned thread per CPU for `ms` milliseconds.
// body returns how many messages it counts. Returns ns per message.
template<typename Body>
double timed(const std::vector<int>& cpus, int ms, Body body) {
    std::atomic<size_t> ready{0};
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::vector<uint64_t> counts(cpus.size());
    std::vector<std::thread> threads;
    for(size_t i = 0; i < cpus.size(); i++) {
        threads.emplace_back([&, i] {
            pinTo(cpus[i]);
            ready++;
            while(!go.load()) {}
            counts[i] = body(i, stop);
        });
    }
    while(ready.load() < cpus.size()) std::this_thread::yield();
    const auto start = Clock::now();
    go.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    stop.store(true);
    for(auto& thread : threads) thread.join();
    const double elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    uint64_t total = 0;
    for(uint64_t count : counts) total += count;
    return total ? elapsed / static_cast<double>(total) : 0;
}

template<typename Mesh>
double oneWay(const std::vector<int>& cpus, int ms) {
    Mesh mesh(2);
    return timed({cpus[0], cpus[1]}, ms, [&](size_t self, std::atomic<bool>& stop) -> uint64_t {
        uint64_t received = 0, last = 0, sent = 0;
        while(!stop.load(std::memory_order_relaxed)) {
            if(self == 0) sent += mesh.send(0, 1, sent) ? 1 : 0;
            else received += mesh.drain(1, last);
        }
        return received;
    });
}

// Round trips, not messages: `timed` divides by the count the first thread returns
template<typename Mesh>
double pingPong(const std::vector<int>& cpus, int ms) {
    Mesh mesh(2);
    return timed({cpus[0], cpus[1]}, ms, [&](size_t self, std::atomic<bool>& stop) -> uint64_t {
        uint64_t trips = 0, last = 0;
        const size_t peer = 1 - self;
        if(self == 0) {
            while(!stop.load(std::memory_order_relaxed)) {
                while(!mesh.send(0, 1, trips) && !stop.load(std::memory_order_relaxed)) {}
                while(mesh.drain(0, last) == 0 && !stop.load(std::memory_order_relaxed)) {}
                trips++;
            }
            return trips;
        }
        while(!stop.load(std::memory_order_relaxed)) {
            if(mesh.drain(1, last) == 0) continue;
            while(!mesh.send(1, peer, last) && !stop.load(std::memory_order_relaxed)) {}
        }
        return 0;
    });
}

template<typename Mesh>
double allToAll(const std::vector<int>& cpus, int ms) {
    Mesh mesh(cpus.size());
    const size_t threads = cpus.size();
    return timed(cpus, ms, [&](size_t self, std::atomic<bool>& stop) -> uint64_t {
        uint64_t received = 0, last = 0, value = 0;
        while(!stop.load(std::memory_order_relaxed)) {
            for(size_t to = 0; to < threads; to++) {
                if(to != self && mesh.send(self, to, value)) value++;
            }
            received += mesh.drain(self, last);
        }
        return received;
    });
}

struct Variant {
    const char* name;
    double (*oneWay)(const std::vector<int>&, int);
    double (*pingPong)(const std::vector<int>&, int);
    double (*allToAll)(const std::vector<int>&, int);
};

template<typename Mesh>
Variant variant(const char* name) {
    return {name, oneWay<Mesh>, pingPong<Mesh>, allToAll<Mesh>};
}

struct Cell {
    double median = 0, low = 0, high = 0;
};

Cell summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return {values[values.size() / 2], values.front(), values.back()};
}

void printRow(const char* name, const Cell& cell) {
    std::printf("  %-14s %10.1f ns   (%.1f - %.1f)\n", name, cell.median, cell.low, cell.high);
}

}  // namespace

int main(int argc, char* argv[]) {
    int rounds = 9, ms = 300;
    std::vector<int> cpus = {0, 2, 4, 6, 8, 10, 12, 14};
    for(int i = 1; i + 1 < argc; i += 2) {
        if(!std::strcmp(argv[i], "--rounds")) rounds = std::atoi(argv[i + 1]);
        else if(!std::strcmp(argv[i], "--ms")) ms = std::atoi(argv[i + 1]);
        else if(!std::strcmp(argv[i], "--cpus")) {
            cpus.clear();
            for(const char* p = argv[i + 1]; *p;) {
                cpus.push_back(std::atoi(p));
                const char* comma = std::strchr(p, ',');
                if(!comma) break;
                p = comma + 1;
            }
        }
    }
    if(cpus.size() < 2) {
        std::fprintf(stderr, "need at least 2 CPUs\n");
        return 2;
    }

    const std::vector<Variant> variants = {
        variant<RingMesh<ExperimentRing<8, false>>>("shared-line"),
        variant<RingMesh<ExperimentRing<64, false>>>("padded"),
        variant<RingMesh<ExperimentRing<8, true>>>("cached-only"),
        variant<RingMesh<SpscRing<uint64_t>>>("padded+cached"),
        variant<RingMesh<ExperimentRing<128, true>>>("padded128"),
        variant<MutexMesh>("mutex-inbox"),
    };

    // Which thread counts all-to-all runs, limited by the CPUs given
    std::vector<size_t> sizes;
    for(size_t n : {size_t{2}, size_t{4}, size_t{8}}) if(n <= cpus.size()) sizes.push_back(n);

    const size_t workloads = 2 + sizes.size();
    std::vector<std::vector<std::vector<double>>> samples(
        workloads, std::vector<std::vector<double>>(variants.size()));

    for(int round = 0; round < rounds; round++) {
        for(size_t w = 0; w < workloads; w++) {
            for(size_t k = 0; k < variants.size(); k++) {
                const size_t index = (k + static_cast<size_t>(round)) % variants.size();
                const Variant& v = variants[index];
                double result = 0;
                if(w == 0) result = v.oneWay(cpus, ms);
                else if(w == 1) result = v.pingPong(cpus, ms);
                else result = v.allToAll(std::vector<int>(cpus.begin(), cpus.begin() + static_cast<long>(sizes[w - 2])), ms);
                samples[w][index].push_back(result);
            }
        }
    }

    std::printf("%d rounds of %d ms each, median of the rounds (fastest - slowest), pinned to CPUs", rounds, ms);
    for(int cpu : cpus) std::printf(" %d", cpu);
    std::printf("\n\none-way, ns per message\n");
    for(size_t k = 0; k < variants.size(); k++) printRow(variants[k].name, summarize(samples[0][k]));
    std::printf("\nping-pong, ns per round trip\n");
    for(size_t k = 0; k < variants.size(); k++) printRow(variants[k].name, summarize(samples[1][k]));
    for(size_t s = 0; s < sizes.size(); s++) {
        std::printf("\nall-to-all, %zu threads, ns per message delivered (across all threads)\n", sizes[s]);
        for(size_t k = 0; k < variants.size(); k++) printRow(variants[k].name, summarize(samples[2 + s][k]));
    }
    return 0;
}

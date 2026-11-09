// SpscRing: its exact behaviour on one thread, then two threads at full speed.
//
// The two-thread tests are written to be run under ThreadSanitizer as well. On
// x86 a missing release or acquire can't produce a wrong value here, because
// the processor doesn't reorder those accesses, so a plain run would pass.
// TSan checks the ordering the C++ code actually asks for: the items are
// pointers to data the producer wrote without any atomics, so if the ring's
// ordering didn't carry that data across, TSan reports a race on it.

#include "../src/SpscRing.h"

#include <cstdint>
#include <deque>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

// Declared a friend by SpscRing, to read where its fields are
struct SpscRingLayout {
    template<typename T>
    static std::string problem(const SpscRing<T>& ring) {
        auto line = [](const void* p) { return reinterpret_cast<uintptr_t>(p) / SpscRing<T>::kCacheLine; };
        if(line(&ring.tail_) == line(&ring.head_)) return "head and tail share a cache line";
        if(line(&ring.tail_) == line(&ring.capacity_)) return "tail shares a line with the read-only fields";
        if(line(&ring.head_) == line(&ring.capacity_)) return "head shares a line with the read-only fields";
        if(line(&ring.tail_) != line(&ring.cached_head_)) return "the producer's fields are on two lines";
        if(line(&ring.head_) != line(&ring.cached_tail_)) return "the consumer's fields are on two lines";
        if(line(&ring.capacity_) != line(&ring.slots_)) return "the read-only fields are on two lines";
        return "";
    }
};

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if(!ok) {
        std::cout << "FAILED: " << what << "\n";
        failures++;
    }
}

void testCapacity() {
    check(SpscRing<int>(1).capacity() == 1, "1 stays 1");
    check(SpscRing<int>(2).capacity() == 2, "2 stays 2");
    check(SpscRing<int>(3).capacity() == 4, "3 rounds up to 4");
    check(SpscRing<int>(1000).capacity() == 1024, "1000 rounds up to 1024");
    check(SpscRing<int>(1024).capacity() == 1024, "1024 stays 1024");
    bool threw = false;
    try { SpscRing<int> ring(0); } catch(const std::invalid_argument&) { threw = true; }
    check(threw, "capacity 0 is refused");
    threw = false;
    try { SpscRing<int> ring(SIZE_MAX); } catch(const std::invalid_argument&) { threw = true; }
    check(threw, "a capacity that can't be rounded up is refused");
}

void testEmptyAndFull() {
    SpscRing<int> ring(4);
    int value = -1;
    check(!ring.tryPop(value), "a new ring is empty");
    check(value == -1, "a failed pop leaves the output alone");
    for(int i = 0; i < 4; i++) check(ring.tryPush(i), "room for item " + std::to_string(i));
    check(!ring.tryPush(99), "a full ring refuses a push");
    check(ring.tryPop(value) && value == 0, "the first item comes out first");
    check(ring.tryPush(4), "a pop makes room for one");
    check(!ring.tryPush(99), "and only one");
    for(int want = 1; want <= 4; want++) {
        check(ring.tryPop(value) && value == want, "in order after the refused push: " + std::to_string(want));
    }
    check(!ring.tryPop(value), "empty again");
}

void testCapacityOne() {
    SpscRing<int> ring(1);
    int value = 0;
    for(int i = 0; i < 100; i++) {
        check(ring.tryPush(i), "the one slot is free");
        check(!ring.tryPush(-1), "the one slot is taken");
        check(ring.tryPop(value) && value == i, "the one item comes back");
        check(!ring.tryPop(value), "nothing else is there");
    }
}

// Random pushes and pops against a deque, wrapping round the ring many times
void testAgainstModel() {
    std::mt19937_64 rng(11);
    for(size_t capacity : {1u, 2u, 3u, 8u, 64u}) {
        SpscRing<uint64_t> ring(capacity);
        std::deque<uint64_t> model;
        uint64_t next = 1;
        for(int step = 0; step < 300000; step++) {
            // Long runs of one kind reach full and empty often
            if((rng() >> 8) % 2 == 0) {
                const bool pushed = ring.tryPush(next);
                const bool room = model.size() < ring.capacity();
                check(pushed == room, "push succeeds exactly when there is room");
                if(room) model.push_back(next);
                next++;
            } else {
                uint64_t value = 0;
                const bool popped = ring.tryPop(value);
                check(popped == !model.empty(), "pop succeeds exactly when an item is waiting");
                if(popped && !model.empty()) {
                    check(value == model.front(), "items come out in the order they went in");
                    model.pop_front();
                }
            }
            if(failures > 20) return;
        }
    }
}

void testLayout() {
    SpscRing<int> ring(8);
    const std::string problem = SpscRingLayout::problem(ring);
    check(problem.empty(), "layout: " + problem);
    auto boxed = std::make_unique<SpscRing<uint64_t>>(8);     // over-aligned new
    check(SpscRingLayout::problem(*boxed).empty(), "layout on the heap");
}

struct Message {
    uint64_t sequence = 0;
    uint64_t check = 0;
    uint64_t data[6] = {0};
};
uint64_t scramble(uint64_t sequence) { return sequence * 0x9E3779B97F4A7C15ull; }

// One thread sends numbered messages as fast as it can; the other checks that
// each arrives once, in order, and with the contents the sender wrote. A
// small ring forces full and empty over and over.
void testTwoThreads(size_t capacity, uint64_t messages) {
    SpscRing<Message*> ring(capacity);
    std::thread producer([&] {
        for(uint64_t i = 1; i <= messages; i++) {
            auto* message = new Message;
            message->sequence = i;
            message->check = scramble(i);
            for(auto& word : message->data) word = i;
            while(!ring.tryPush(message)) std::this_thread::yield();
        }
    });
    uint64_t wrong = 0;
    for(uint64_t expected = 1; expected <= messages; expected++) {
        Message* message = nullptr;
        while(!ring.tryPop(message)) std::this_thread::yield();
        if(message->sequence != expected || message->check != scramble(expected)) wrong++;
        for(auto word : message->data) if(word != expected) wrong++;
        delete message;
    }
    producer.join();
    check(wrong == 0, "two threads, capacity " + std::to_string(capacity) + ": " +
                          std::to_string(wrong) + " wrong fields");
    Message* extra = nullptr;
    check(!ring.tryPop(extra), "nothing is left after every message arrived");
}

// The shape the server uses: buffers go one way full and come back through a
// second ring to be used again, so the thread that wrote a buffer is the one
// that writes it next. Each buffer is written and read by both threads in
// turn with no atomics of its own: only the two rings order those accesses,
// in both directions.
//
// Each ring has one thread per end: this thread pops `returned` and pushes
// `outbound`, the consumer does the opposite. `returned` is filled here before
// the consumer starts, and starting a thread orders that fill before it runs.
void testBuffersComeBack(size_t buffers, uint64_t rounds) {
    struct Buffer {
        uint64_t round = 0;
        std::vector<uint64_t> words = std::vector<uint64_t>(32);
    };
    std::vector<Buffer> pool(buffers);
    SpscRing<Buffer*> outbound(buffers);
    SpscRing<Buffer*> returned(buffers);
    for(auto& buffer : pool) check(returned.tryPush(&buffer), "the pool fits the return ring");

    uint64_t wrong = 0;     // the consumer's count, read after join
    std::thread consumer([&] {
        for(uint64_t expected = 1; expected <= rounds; expected++) {
            Buffer* buffer = nullptr;
            while(!outbound.tryPop(buffer)) std::this_thread::yield();
            if(buffer->round != expected) wrong++;
            for(auto word : buffer->words) if(word != expected) wrong++;
            buffer->round = 0;      // the consumer writes it too: the producer must see this
            while(!returned.tryPush(buffer)) std::this_thread::yield();
        }
    });
    for(uint64_t round = 1; round <= rounds; round++) {
        Buffer* buffer = nullptr;
        while(!returned.tryPop(buffer)) std::this_thread::yield();
        if(buffer->round != 0) wrong++;
        buffer->round = round;
        for(auto& word : buffer->words) word = round;
        while(!outbound.tryPush(buffer)) std::this_thread::yield();
    }
    consumer.join();
    check(wrong == 0, "buffers coming back: " + std::to_string(wrong) + " wrong fields");
}

}  // namespace

int main() {
    testCapacity();
    testEmptyAndFull();
    testCapacityOne();
    testAgainstModel();
    testLayout();
    testTwoThreads(1, 50000);
    testTwoThreads(4, 200000);
    testTwoThreads(1024, 500000);
    testBuffersComeBack(1, 50000);
    testBuffersComeBack(8, 200000);
    if(failures == 0) {
        std::cout << "SpscRing: all checks passed\n";
        return 0;
    }
    std::cout << "SpscRing: " << failures << " checks FAILED\n";
    return 1;
}

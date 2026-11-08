#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>

// A bounded queue between exactly two threads: one only pushes, the other only
// pops. With more threads, each ordered pair of threads gets its own ring, so
// items from one thread to another stay in the order they were sent.
//
// Neither side ever waits. A push to a full ring and a pop from an empty one
// return false at once, and the caller decides what to do. A thread that waited
// for room could wait forever, if the thread it waits for were waiting for room
// in a ring going the other way.
//
// How it works:
//   Two counters only ever grow: tail_ counts pushes, head_ counts pops. An
//   item's slot is its count modulo the capacity, which is a power of two so
//   that the modulo is a mask. tail_ - head_ is how many items are waiting;
//   using counters rather than wrapped positions tells full (== capacity) from
//   empty (== 0) with no slot left unused. A 64-bit counter can't overflow in
//   practice (centuries at a billion pushes a second), and the unsigned
//   subtraction would stay right even if it did.
//
// Why no lock and no compare-and-swap:
//   Each counter has one writer, so neither side ever has to win a race for it.
//   An increment is a plain store.
//
// The memory ordering, which is the whole of the correctness argument:
//   The producer writes the slot, then stores tail_ with release. The consumer
//   loads tail_ with acquire. So a consumer that sees the new count also sees
//   the slot, and everything the producer wrote before pushing, such as the
//   contents of a buffer the item points to.
//   The same holds in the other direction: the consumer reads the slot, then
//   stores head_ with release, and the producer loads head_ with acquire. So
//   the producer can only overwrite a slot once the consumer has finished
//   reading it.
//   Each thread reads its own counter relaxed: nobody else writes it.
//
// What is kept apart, and why (bench/ring_bench.cpp measures this):
//   A write to a cache line takes it away from every other core. tail_ is
//   written on every push and head_ on every pop, so if they shared a line,
//   the two threads would pull that line back and forth on every operation
//   even though they write different variables. That is false sharing.
//   So each thread's fields get a line of their own, and the fields nobody
//   writes after construction get a third.
//
//   Each side also remembers the last value it saw of the other side's
//   counter, and reads the real one only when that value says full or empty.
//   Most operations then touch only the thread's own line. A stale value is
//   always safe: the consumer only removes items, so an old head_ shows less
//   room than there is; the producer only adds, so an old tail_ shows fewer
//   items than there are.
//
// T must be trivially copyable: slots hold plain values (in the server, a
// pointer to a batch), so a slot needs no construction or destruction.
template<typename T>
class SpscRing {
    static_assert(std::is_trivially_copyable<T>::value, "SpscRing holds plain values");

public:
    static constexpr size_t kCacheLine = 64;

    // Holds at least min_capacity items (rounded up to a power of two)
    explicit SpscRing(size_t min_capacity)
        : capacity_(roundUpToPowerOfTwo(min_capacity)),
          mask_(capacity_ - 1),
          slots_(std::make_unique<T[]>(capacity_)) {}

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    size_t capacity() const { return capacity_; }

    // Producer thread only. False if the ring is full.
    bool tryPush(const T& value) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if(tail - cached_head_ == capacity_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if(tail - cached_head_ == capacity_) return false;
        }
        slots_[tail & mask_] = value;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only. False if the ring is empty.
    bool tryPop(T& out) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if(head == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if(head == cached_tail_) return false;
        }
        out = slots_[head & mask_];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

private:
    friend struct SpscRingLayout;       // tests/test_spsc_ring.cpp checks the layout

    static size_t roundUpToPowerOfTwo(size_t n) {
        if(n == 0) throw std::invalid_argument("SpscRing needs room for at least one item");
        size_t capacity = 1;
        while(capacity < n) {
            if(capacity > SIZE_MAX / 2) throw std::invalid_argument("SpscRing capacity too large");
            capacity *= 2;
        }
        return capacity;
    }

    // The producer's line: it writes both, the consumer only reads tail_
    alignas(kCacheLine) std::atomic<size_t> tail_{0};
    size_t cached_head_ = 0;

    // The consumer's line: it writes both, the producer only reads head_
    alignas(kCacheLine) std::atomic<size_t> head_{0};
    size_t cached_tail_ = 0;

    // Never written after construction, so both threads can keep this line
    alignas(kCacheLine) const size_t capacity_;
    const size_t mask_;
    const std::unique_ptr<T[]> slots_;
};

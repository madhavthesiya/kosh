// The parts of kosh-loadgen that decide what number gets printed, kept apart
// from the sockets so tests/test_loadgen.cpp can check each one exactly.
//
// Histogram     log-linear latency histogram, at most 0.78% above the true value
// ReplyScanner  counts RESP replies in a byte stream, across any read boundary
// intendedNs    when the n-th request of the schedule should be sent
// computeQuantile  a percentile, or the reason it can't be stated

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace loadgen {

// Latency histogram in the style of HdrHistogram. Values below 2^kSubBits have
// a bucket each; above that, every power of two is split into 2^kSubBits equal
// buckets, so a bucket's width is at most 1/128 of its lower edge. Recording is
// an array increment: no allocation and no sorting while the test runs.
class Histogram {
public:
    static constexpr unsigned kSubBits = 7;
    static constexpr unsigned kMaxBits = 46;   // 2^46 ns is about 19.5 hours
    static constexpr size_t kBuckets = size_t{kMaxBits - kSubBits + 1} << kSubBits;

    Histogram() : counts_(kBuckets, 0) {}

    static size_t indexOf(uint64_t value) {
        if(value < (uint64_t{1} << kSubBits)) return static_cast<size_t>(value);
        const unsigned msb = 63u - static_cast<unsigned>(__builtin_clzll(value));
        const unsigned shift = msb - kSubBits;
        const uint64_t mantissa = (value >> shift) - (uint64_t{1} << kSubBits);
        return (size_t{shift + 1} << kSubBits) + static_cast<size_t>(mantissa);
    }

    // The largest value that lands in this bucket
    static uint64_t upperEdge(size_t index) {
        if(index < (size_t{1} << kSubBits)) return index;
        const unsigned shift = static_cast<unsigned>(index >> kSubBits) - 1;
        const uint64_t mantissa = index & ((size_t{1} << kSubBits) - 1);
        const uint64_t lower = ((uint64_t{1} << kSubBits) + mantissa) << shift;
        return lower + (uint64_t{1} << shift) - 1;
    }

    void add(uint64_t value) {
        if(value >> kMaxBits) {
            saturated_++;
            value = (uint64_t{1} << kMaxBits) - 1;
        }
        counts_[indexOf(value)]++;
        total_++;
        max_ = std::max(max_, value);
    }

    void merge(const Histogram& other) {
        for(size_t i = 0; i < kBuckets; i++) counts_[i] += other.counts_[i];
        total_ += other.total_;
        saturated_ += other.saturated_;
        max_ = std::max(max_, other.max_);
    }

    // The value at 1-based `rank` in sorted order. Reports the bucket's upper
    // edge, so it can overstate by up to 0.78% and never understates, except
    // that it is capped at the largest value actually recorded.
    uint64_t valueAtRank(uint64_t rank) const {
        if(total_ == 0) return 0;
        uint64_t seen = 0;
        for(size_t i = 0; i < kBuckets; i++) {
            seen += counts_[i];
            if(seen >= rank) return std::min(upperEdge(i), max_);
        }
        return max_;
    }

    uint64_t count() const { return total_; }
    uint64_t max() const { return max_; }
    uint64_t saturated() const { return saturated_; }

private:
    std::vector<uint64_t> counts_;
    uint64_t total_ = 0;
    uint64_t saturated_ = 0;
    uint64_t max_ = 0;
};

// Counts complete RESP replies as bytes arrive. It keeps its state between
// calls, so a reply may be cut anywhere by the network. Only the reply types
// GET and SET produce are accepted; anything else returns false, which means
// the stream can no longer be matched to requests.
class ReplyScanner {
public:
    template<typename OnReply>
    bool feed(const char* data, size_t size, OnReply&& onReply) {
        size_t i = 0;
        while(i < size) {
            switch(state_) {
            case State::Type: {
                const char c = data[i++];
                if(c == '+' || c == ':') { isError_ = false; state_ = State::Line; }
                else if(c == '-') { isError_ = true; state_ = State::Line; }
                else if(c == '$') { length_ = 0; digits_ = 0; negative_ = false; state_ = State::Length; }
                else return false;
                break;
            }
            case State::Line: {
                const void* newline = std::memchr(data + i, '\n', size - i);
                if(newline == nullptr) { i = size; break; }
                i = static_cast<size_t>(static_cast<const char*>(newline) - data) + 1;
                state_ = State::Type;
                onReply(isError_);
                break;
            }
            case State::Length: {
                const char c = data[i++];
                if(c == '-' && digits_ == 0 && !negative_) { negative_ = true; }
                else if(c >= '0' && c <= '9' && digits_ < 18) {
                    length_ = length_ * 10 + static_cast<uint64_t>(c - '0');
                    digits_++;
                }
                else if(c == '\r' && digits_ > 0) { state_ = State::LengthEnd; }
                else return false;
                break;
            }
            case State::LengthEnd: {
                if(data[i++] != '\n') return false;
                if(negative_) {
                    if(length_ != 1) return false;     // only $-1, a missing key
                    state_ = State::Type;
                    onReply(false);
                } else {
                    remaining_ = length_ + 2;          // the value and its \r\n
                    state_ = State::Body;
                }
                break;
            }
            case State::Body: {
                const uint64_t available = size - i;
                const uint64_t take = std::min(remaining_, available);
                i += static_cast<size_t>(take);
                remaining_ -= take;
                if(remaining_ == 0) { state_ = State::Type; onReply(false); }
                break;
            }
            }
        }
        return true;
    }

private:
    enum class State { Type, Line, Length, LengthEnd, Body };
    State state_ = State::Type;
    bool isError_ = false;
    bool negative_ = false;
    unsigned digits_ = 0;
    uint64_t length_ = 0;
    uint64_t remaining_ = 0;
};

// When the n-th request (0-based, across all connections) is meant to be sent,
// in ns after the start. Computed from n every time, not by adding a period
// again and again, so rounding never accumulates into a drift in the rate.
inline uint64_t intendedNs(uint64_t n, uint64_t rate) {
    return static_cast<uint64_t>(static_cast<unsigned __int128>(n) * 1000000000u / rate);
}

// The rules for when a percentile may be printed

// At least this many samples must lie above a percentile. With fewer, the
// normal approximation behind its confidence interval doesn't hold, and the
// percentile is in effect one of the last few samples: p99.9 needs 10,000.
constexpr uint64_t kMinSamplesAbove = 10;

// Latency from the intended time includes the generator's own lateness. If that
// lateness is over 1/10 of a percentile, the number says more about this client
// than about the server.
constexpr uint64_t kMaxClientLagDivisor = 10;

enum class QuantileStatus { Ok, TooFewSamples, Unfinished, ClientLate };

struct Quantile {
    uint32_t ppm = 0;                  // 999000 is p99.9
    QuantileStatus status = QuantileStatus::TooFewSamples;
    uint64_t rank = 0;
    uint64_t value = 0;
    uint64_t ciLow = 0;                // 95% confidence interval for the value
    uint64_t ciHigh = 0;
    uint64_t clientLag = 0;            // the generator's lateness at this percentile
    uint64_t samplesAbove = 0;
    unsigned windowsAbove = 0;         // how many 1 s windows reached the value
};

// Nearest-rank percentile: the smallest rank with at least ppm/1e6 of the
// samples at or below it. Integer arithmetic, because 0.999 * 1000 in floating
// point is not 999.
inline uint64_t rankOf(uint32_t ppm, uint64_t n) {
    if(n == 0) return 0;
    const auto rank = (static_cast<unsigned __int128>(n) * ppm + 999999u) / 1000000u;
    return std::max<uint64_t>(static_cast<uint64_t>(rank), 1);
}

// `unfinished` requests were scheduled and never answered. They count toward
// the total as slower than everything recorded, the only assumption that can't
// make the tail look better than it was.
//
// The interval comes from order statistics: the number of samples below the
// true percentile is binomial. That assumes independent samples, which a
// single stall breaks, so windowsAbove reports how many separate seconds the
// tail came from.
inline Quantile computeQuantile(const Histogram& latency, uint64_t unfinished, uint32_t ppm,
                                const Histogram* clientLag,
                                const std::vector<uint64_t>& windowMax) {
    Quantile q;
    q.ppm = ppm;
    const uint64_t answered = latency.count();
    const uint64_t n = answered + unfinished;
    if(n == 0) return q;

    q.rank = rankOf(ppm, n);
    q.samplesAbove = n - q.rank;
    if(q.samplesAbove < kMinSamplesAbove) return q;

    const double p = ppm / 1e6;
    const double spread = 1.96 * std::sqrt(static_cast<double>(n) * p * (1.0 - p));
    const double lowRank = std::floor(static_cast<double>(q.rank) - spread);
    const double highRank = std::ceil(static_cast<double>(q.rank) + spread);
    const uint64_t low = lowRank < 1.0 ? 1 : static_cast<uint64_t>(lowRank);
    const uint64_t high = std::min(n, static_cast<uint64_t>(highRank));
    if(high > answered) {
        q.status = QuantileStatus::Unfinished;
        return q;
    }

    q.value = latency.valueAtRank(q.rank);
    q.ciLow = latency.valueAtRank(low);
    q.ciHigh = latency.valueAtRank(high);
    for(uint64_t windowPeak : windowMax) {
        if(windowPeak >= q.value) q.windowsAbove++;
    }
    q.status = QuantileStatus::Ok;

    if(clientLag != nullptr && clientLag->count() > 0) {
        q.clientLag = clientLag->valueAtRank(rankOf(ppm, clientLag->count()));
        if(q.clientLag * kMaxClientLagDivisor > q.value) q.status = QuantileStatus::ClientLate;
    }
    return q;
}

} // namespace loadgen

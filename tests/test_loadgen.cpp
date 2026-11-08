// Exact checks of what kosh-loadgen prints: the histogram, the RESP reply
// scanner, the schedule, and the rules for refusing a percentile.

#include "../bench/LoadgenCore.h"

#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace loadgen;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if(!ok) {
        std::cout << "FAILED: " << what << "\n";
        failures++;
    }
}

void testHistogramBuckets() {
    for(uint64_t v = 0; v < 128; v++) {
        check(Histogram::upperEdge(Histogram::indexOf(v)) == v, "small values are exact: " + std::to_string(v));
    }
    // Buckets must be contiguous: no value falls between two of them
    for(size_t i = 1; i < Histogram::kBuckets; i++) {
        check(Histogram::indexOf(Histogram::upperEdge(i - 1)) == i - 1, "upper edge maps back: " + std::to_string(i));
        check(Histogram::indexOf(Histogram::upperEdge(i - 1) + 1) == i, "next value is the next bucket: " + std::to_string(i));
    }
    check(Histogram::indexOf((uint64_t{1} << Histogram::kMaxBits) - 1) == Histogram::kBuckets - 1, "last bucket");
}

void testHistogramErrorBound() {
    std::mt19937_64 rng(7);
    for(int i = 0; i < 200000; i++) {
        const unsigned bits = 1 + static_cast<unsigned>(rng() % (Histogram::kMaxBits - 1));
        const uint64_t v = rng() & ((uint64_t{1} << bits) - 1);
        const uint64_t edge = Histogram::upperEdge(Histogram::indexOf(v));
        check(edge >= v, "upper edge is not below the value");
        check(static_cast<double>(edge - v) <= static_cast<double>(v) / 128.0, "within 1/128: " + std::to_string(v));
    }
}

void testRanksAndMerge() {
    Histogram h, a, b;
    for(uint64_t v = 1; v <= 1000; v++) {
        h.add(v);
        (v % 2 ? a : b).add(v);
    }
    check(h.count() == 1000 && h.max() == 1000, "count and max");
    for(uint64_t rank = 1; rank <= 1000; rank++) {
        const uint64_t got = h.valueAtRank(rank);
        check(got >= rank && static_cast<double>(got - rank) <= static_cast<double>(rank) / 128.0,
              "rank " + std::to_string(rank) + " gave " + std::to_string(got));
    }
    check(h.valueAtRank(1000) == 1000, "the top rank is the exact maximum");
    a.merge(b);
    check(a.count() == 1000 && a.max() == 1000, "merged count and max");
    for(uint64_t rank = 1; rank <= 1000; rank += 7) {
        check(a.valueAtRank(rank) == h.valueAtRank(rank), "merging equals adding to one");
    }

    Histogram top;
    top.add(uint64_t{1} << 50);
    check(top.saturated() == 1 && top.max() == (uint64_t{1} << Histogram::kMaxBits) - 1, "too large a value is clamped and counted");
}

void testRankOf() {
    check(rankOf(999000, 1000) == 999, "p99.9 of 1000 is rank 999");
    check(rankOf(999000, 10000) == 9990, "p99.9 of 10000");
    check(rankOf(500000, 1) == 1 && rankOf(500000, 2) == 1 && rankOf(500000, 3) == 2, "p50 nearest rank");
    check(rankOf(990000, 101) == 100, "rounds up");
    check(rankOf(999900, 10000000000000ull) == 9999000000000ull, "large n stays exact");
}

void testSchedule() {
    check(intendedNs(0, 1000) == 0, "starts at zero");
    check(intendedNs(333, 333) == 1000000000, "a second of a rate with no exact period");
    check(intendedNs(3000000000ull, 1000000) == 3000000000000ull, "no overflow at billions of requests");
    check(intendedNs(1000000000000ull, 1000000000) == 1000000000000ull, "n * 1e9 past 64 bits");
    uint64_t previous = 0;
    for(uint64_t n = 1; n < 100000; n++) {
        const uint64_t t = intendedNs(n, 7919);
        check(t > previous, "strictly increasing");
        previous = t;
    }
}

struct Scanned { size_t replies = 0; size_t errors = 0; bool ok = true; };

Scanned scanInPieces(const std::string& bytes, const std::vector<size_t>& cuts) {
    ReplyScanner scanner;
    Scanned out;
    size_t from = 0;
    auto onReply = [&](bool isError) { out.replies++; if(isError) out.errors++; };
    for(size_t cut : cuts) {
        out.ok = scanner.feed(bytes.data() + from, cut - from, onReply) && out.ok;
        from = cut;
    }
    out.ok = scanner.feed(bytes.data() + from, bytes.size() - from, onReply) && out.ok;
    return out;
}

void testReplyScanner() {
    // A value may hold anything, including what looks like the end of a line
    const std::string stream =
        "+OK\r\n:42\r\n-ERR no\r\n$-1\r\n$0\r\n\r\n$5\r\nhello\r\n$9\r\n$3\r\n+OK\r\n\r\n-WRONGTYPE x\r\n+OK\r\n";
    const size_t replies = 9, errors = 2;

    Scanned whole = scanInPieces(stream, {});
    check(whole.ok && whole.replies == replies && whole.errors == errors, "the whole stream at once");
    for(size_t cut = 0; cut <= stream.size(); cut++) {
        Scanned s = scanInPieces(stream, {cut});
        check(s.ok && s.replies == replies && s.errors == errors, "cut at byte " + std::to_string(cut));
    }
    std::vector<size_t> everyByte;
    for(size_t i = 1; i < stream.size(); i++) everyByte.push_back(i);
    Scanned bytes = scanInPieces(stream, everyByte);
    check(bytes.ok && bytes.replies == replies && bytes.errors == errors, "one byte at a time");

    for(const char* bad : {"*1\r\n", "?\r\n", "$x\r\n", "$-2\r\n", "$-\r\n", "$\r\n", "$5\rX", "$--1\r\n", "$1234567890123456789\r\n"}) {
        check(!scanInPieces(bad, {}).ok, std::string("rejected: ") + bad);
    }
    Scanned partial = scanInPieces("$5\r\nhel", {});
    check(partial.ok && partial.replies == 0, "an unfinished value isn't a reply yet");
}

Histogram uniformHistogram(uint64_t n) {
    Histogram h;
    for(uint64_t v = 1; v <= n; v++) h.add(v * 1000);
    return h;
}

void testRefusals() {
    const std::vector<uint64_t> noWindows;

    // 10 samples above is the line
    Histogram exactly = uniformHistogram(10000);
    Quantile ok = computeQuantile(exactly, 0, 999000, nullptr, noWindows);
    check(ok.status == QuantileStatus::Ok && ok.samplesAbove == 10, "p99.9 of 10000 is allowed");
    Histogram short_ = uniformHistogram(9999);
    Quantile few = computeQuantile(short_, 0, 999000, nullptr, noWindows);
    check(few.status == QuantileStatus::TooFewSamples, "p99.9 of 9999 is refused");
    check(computeQuantile(Histogram(), 0, 500000, nullptr, noWindows).status == QuantileStatus::TooFewSamples, "no data");

    // The interval brackets the value and holds real values
    Quantile p50 = computeQuantile(exactly, 0, 500000, nullptr, noWindows);
    check(p50.ciLow <= p50.value && p50.value <= p50.ciHigh && p50.ciLow < p50.ciHigh, "interval brackets the value");
    // Ranks 5000 +- 98, and a bucket reports up to 1/128 above the true value
    check(p50.ciLow >= 4902000 && p50.ciHigh <= 5098000 + 5098000 / 128, "p50 interval has the expected width");

    // Unanswered requests count as slower than everything seen
    Quantile partly = computeQuantile(exactly, 100, 990000, nullptr, noWindows);
    check(partly.status == QuantileStatus::Unfinished, "p99 falls among the unanswered");
    Quantile lower = computeQuantile(exactly, 100, 500000, nullptr, noWindows);
    check(lower.status == QuantileStatus::Ok && lower.rank == 5050, "p50 is still known, ranked over all requests");

    // The generator's own lateness
    Histogram quietLag, lateLag;
    for(int i = 0; i < 10000; i++) { quietLag.add(100); lateLag.add(2000000); }
    check(computeQuantile(exactly, 0, 990000, &quietLag, noWindows).status == QuantileStatus::Ok, "a punctual generator passes");
    Quantile late = computeQuantile(exactly, 0, 990000, &lateLag, noWindows);
    check(late.status == QuantileStatus::ClientLate && late.clientLag >= 2000000, "a late generator is refused");

    // Which seconds the tail came from
    std::vector<uint64_t> oneSpike{100, 100, 99999999, 100};
    check(computeQuantile(exactly, 0, 500000, nullptr, oneSpike).windowsAbove == 1, "one second reached it");
    std::vector<uint64_t> everySecond(5, 99999999);
    check(computeQuantile(exactly, 0, 500000, nullptr, everySecond).windowsAbove == 5, "every second reached it");
}

} // namespace

int main() {
    testHistogramBuckets();
    testHistogramErrorBound();
    testRanksAndMerge();
    testRankOf();
    testSchedule();
    testReplyScanner();
    testRefusals();
    std::cout << (failures == 0 ? "ALL PASSED\n" : "SOME FAILED\n");
    return failures == 0 ? 0 : 1;
}

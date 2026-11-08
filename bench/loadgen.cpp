// kosh-loadgen: an open-loop load generator for Redis-compatible servers.
//
// redis-benchmark sends a request, waits for the reply, then sends the next.
// If the server stalls for a second, the client stops sending for a second too,
// so the requests that a real stream of users would have sent in that second
// are never made and never measured. That is coordinated omission, and it makes
// the tail look far better than it was.
//
// This tool uses wrk2's method over RESP: every request has a time it should be
// sent, fixed before the run starts, and latency counts from that time. Requests
// go out on schedule whether or not earlier replies have arrived. YCSB (with
// measurement.interval=intended) and dfly_bench's negative --qps got here first.
//
// Each percentile is printed only if the run can support it (LoadgenCore.h).
// Taken from Lancet (ATC 2019): say N/A rather than print a number that isn't
// backed by the data.
//
// Each thread spins instead of sleeping, so it needs a core of its own: waking
// from a sleep takes tens of microseconds, which would be counted as server
// latency. Linux only (epoll).
//
// Build: cmake --build build --target kosh-loadgen
// Run:   ./build/kosh-loadgen --port 6380 --rate 50000 --duration 30

#include "LoadgenCore.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace loadgen;

namespace {

const uint64_t kNsPerSec = 1000000000;
const int kSendsPerPoll = 64;          // sends before replies are read again
const int kKeyDigits = 12;

struct Config {
    std::string host = "127.0.0.1";
    std::string port = "6379";
    uint64_t rate = 0;
    uint64_t connections = 8;
    uint64_t threads = 1;
    uint64_t duration = 30;
    uint64_t warmup = 2;
    uint64_t drain = 10;
    uint64_t keyspace = 100000;
    uint64_t valueSize = 64;
    double setRatio = 0.1;
    uint64_t seed = 1;
    bool prefill = true;
    std::string jsonPath;
};

uint64_t nowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// In-flight send times for one connection, in order. Grows to the deepest the
// socket buffers ever hold, then stops allocating.
class Fifo {
public:
    Fifo() : items_(64) {}
    void push(uint64_t value) {
        if(size_ == items_.size()) grow();
        items_[(head_ + size_) & (items_.size() - 1)] = value;
        size_++;
    }
    bool empty() const { return size_ == 0; }
    uint64_t pop() {
        const uint64_t value = items_[head_];
        head_ = (head_ + 1) & (items_.size() - 1);
        size_--;
        return value;
    }
private:
    void grow() {
        std::vector<uint64_t> bigger(items_.size() * 2);
        for(size_t i = 0; i < size_; i++) bigger[i] = items_[(head_ + i) & (items_.size() - 1)];
        items_.swap(bigger);
        head_ = 0;
    }
    std::vector<uint64_t> items_;
    size_t head_ = 0;
    size_t size_ = 0;
};

struct Conn {
    int fd = -1;
    uint64_t index = 0;        // its place in the schedule's round-robin
    uint64_t scheduled = 0;    // requests that have come due
    uint64_t sent = 0;         // requests started on the wire
    uint64_t replied = 0;
    bool blocked = false;      // the socket is full; waiting for EPOLLOUT
    bool dead = false;
    std::string out;           // the request being written
    size_t outOffset = 0;
    Fifo sentAt;
    ReplyScanner scanner;
};

struct Result {
    Histogram intended;        // latency from the scheduled time
    Histogram actual;          // latency from when the kernel took the request
    Histogram lag;             // how late the generator itself was
    std::vector<uint64_t> windowMax;
    uint64_t recorded = 0;
    uint64_t scheduledMeasured = 0;
    uint64_t errors = 0;
    uint64_t lost = 0;
    uint64_t protocolErrors = 0;
};

class RequestBuilder {
public:
    RequestBuilder(const Config& config, uint64_t seed)
        : keyspace_(config.keyspace), setRatio_(config.setRatio), rng_(seed),
          value_(config.valueSize, 'x'),
          lengthPrefix_("$" + std::to_string(4 + kKeyDigits) + "\r\n"),
          valueHeader_("$" + std::to_string(config.valueSize) + "\r\n") {}

    void buildRandom(std::string& out) {
        const uint64_t key = rng_() % keyspace_;
        const bool isSet = setRatio_ > 0.0 && coin_(rng_) < setRatio_;
        if(isSet) buildSet(out, key);
        else buildGet(out, key);
    }

    void buildSet(std::string& out, uint64_t key) {
        out.assign("*3\r\n$3\r\nSET\r\n");
        appendKey(out, key);
        out.append(valueHeader_).append(value_).append("\r\n");
    }

private:
    void buildGet(std::string& out, uint64_t key) {
        out.assign("*2\r\n$3\r\nGET\r\n");
        appendKey(out, key);
    }

    void appendKey(std::string& out, uint64_t key) {
        out.append(lengthPrefix_).append("key:");
        char digits[kKeyDigits];
        for(int i = kKeyDigits - 1; i >= 0; i--) {
            digits[i] = static_cast<char>('0' + key % 10);
            key /= 10;
        }
        out.append(digits, kKeyDigits).append("\r\n");
    }

    uint64_t keyspace_;
    double setRatio_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> coin_{0.0, 1.0};
    std::string value_;
    std::string lengthPrefix_;
    std::string valueHeader_;
};

// One thread's connections and its pass through the schedule. Connection i
// takes requests i, i+C, i+2C, ... of the schedule (C connections in all), so
// the arrivals are evenly spaced across the connections and across the threads.
class Worker {
public:
    Worker(const Config& config, std::vector<Conn>& conns, uint64_t startNs, uint64_t seed)
        : config_(config), conns_(conns), startNs_(startNs), requests_(config, seed),
          total_(config.rate * (config.warmup + config.duration)),
          measuredFrom_(config.rate * config.warmup) {
        result_.windowMax.assign(config.duration, 0);
    }

    void run() {
        epoll_ = epoll_create1(0);
        for(Conn& c : conns_) control(c, EPOLL_CTL_ADD, EPOLLIN);

        while(nowNs() < startNs_) {}
        const uint64_t lastDue = startNs_ + intendedNs(total_ - 1, config_.rate);
        const uint64_t giveUpAt = lastDue + config_.drain * kNsPerSec;

        std::vector<char> buffer(64 * 1024);
        epoll_event events[64];
        while(true) {
            sendDue();
            const int ready = epoll_wait(epoll_, events, 64, 0);
            for(int i = 0; i < ready; i++) {
                handleEvent(*static_cast<Conn*>(events[i].data.ptr), events[i].events, buffer);
            }
            const bool scheduleDone = nextRequest() >= total_;
            if(scheduleDone && outstanding_ == 0) break;
            if(scheduleDone && nowNs() >= giveUpAt) break;
        }
        close(epoll_);
    }

    Result& result() { return result_; }

private:
    uint64_t nextRequest() const {
        return cycle_ * config_.connections + conns_[cursor_].index;
    }

    // Everything due goes out now, a bounded number at a time so a generator
    // that has fallen behind still reads replies.
    void sendDue() {
        for(int i = 0; i < kSendsPerPoll; i++) {
            const uint64_t n = nextRequest();
            if(n >= total_) return;
            const uint64_t due = startNs_ + intendedNs(n, config_.rate);
            const uint64_t now = nowNs();
            if(due > now) return;

            Conn& c = conns_[cursor_];
            if(++cursor_ == conns_.size()) { cursor_ = 0; cycle_++; }
            if(n >= measuredFrom_) result_.scheduledMeasured++;
            if(c.dead) continue;
            c.scheduled++;
            outstanding_++;
            // A full socket means the server isn't reading: that wait belongs
            // to the server, and isn't the generator being late.
            if(!c.blocked) {
                if(n >= measuredFrom_) result_.lag.add(now - due);
                flush(c);
            }
        }
    }

    void flush(Conn& c) {
        while(!c.blocked && !c.dead) {
            if(c.outOffset == c.out.size()) {
                if(c.sent == c.scheduled) return;
                requests_.buildRandom(c.out);
                c.outOffset = 0;
                c.sent++;
            }
            const ssize_t written = send(c.fd, c.out.data() + c.outOffset,
                                         c.out.size() - c.outOffset, MSG_NOSIGNAL);
            if(written > 0) {
                c.outOffset += static_cast<size_t>(written);
                if(c.outOffset == c.out.size()) c.sentAt.push(nowNs());
            } else if(written < 0 && errno == EINTR) {
                continue;
            } else if(written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                c.blocked = true;
                control(c, EPOLL_CTL_MOD, EPOLLIN | EPOLLOUT);
            } else {
                markDead(c);
            }
        }
    }

    void handleEvent(Conn& c, uint32_t events, std::vector<char>& buffer) {
        if(c.dead) return;
        if(events & EPOLLOUT) {
            c.blocked = false;
            control(c, EPOLL_CTL_MOD, EPOLLIN);
            flush(c);
        }
        if(events & (EPOLLIN | EPOLLHUP | EPOLLERR)) readReplies(c, buffer);
    }

    void readReplies(Conn& c, std::vector<char>& buffer) {
        // Stamped before the read: the bytes were already waiting when
        // epoll reported them, so this is the nearest time to their arrival
        const uint64_t now = nowNs();
        const ssize_t got = recv(c.fd, buffer.data(), buffer.size(), 0);
        if(got > 0) {
            const bool ok = c.scanner.feed(buffer.data(), static_cast<size_t>(got), [&](bool isError) {
                recordReply(c, isError, now);
            });
            if(!ok) {
                result_.protocolErrors++;
                markDead(c);
            }
        } else if(got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            markDead(c);
        }
    }

    void recordReply(Conn& c, bool isError, uint64_t now) {
        if(c.dead) return;
        if(c.sentAt.empty()) {         // a reply to a request not yet fully sent
            result_.protocolErrors++;
            markDead(c);
            return;
        }
        const uint64_t sentAt = c.sentAt.pop();
        const uint64_t n = c.replied * config_.connections + c.index;
        c.replied++;
        outstanding_--;
        if(isError) result_.errors++;
        if(n < measuredFrom_) return;

        const uint64_t intended = intendedNs(n, config_.rate);
        const uint64_t due = startNs_ + intended;
        const uint64_t latency = now > due ? now - due : 0;
        result_.intended.add(latency);
        result_.actual.add(now > sentAt ? now - sentAt : 0);
        result_.recorded++;

        const size_t window = std::min<size_t>(
            static_cast<size_t>((intended - config_.warmup * kNsPerSec) / kNsPerSec),
            result_.windowMax.size() - 1);
        result_.windowMax[window] = std::max(result_.windowMax[window], latency);
    }

    void markDead(Conn& c) {
        c.dead = true;
        result_.lost++;
        outstanding_ -= c.scheduled - c.replied;
        epoll_ctl(epoll_, EPOLL_CTL_DEL, c.fd, nullptr);
    }

    void control(Conn& c, int operation, uint32_t events) {
        epoll_event ev{};
        ev.events = events;
        ev.data.ptr = &c;
        epoll_ctl(epoll_, operation, c.fd, &ev);
    }

    const Config& config_;
    std::vector<Conn>& conns_;
    uint64_t startNs_;
    RequestBuilder requests_;
    uint64_t total_;
    uint64_t measuredFrom_;
    size_t cursor_ = 0;
    uint64_t cycle_ = 0;
    uint64_t outstanding_ = 0;
    int epoll_ = -1;
    Result result_;
};

int connectTo(const Config& config) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if(getaddrinfo(config.host.c_str(), config.port.c_str(), &hints, &found) != 0) return -1;
    int fd = -1;
    for(addrinfo* a = found; a != nullptr; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if(fd < 0) continue;
        if(connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(found);
    if(fd >= 0) {
        int on = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    }
    return fd;
}

// Writes every key once, in batches, so that GETs find values of the real size.
bool prefill(const Config& config) {
    const int fd = connectTo(config);
    if(fd < 0) return false;
    RequestBuilder builder(config, config.seed);
    ReplyScanner scanner;
    std::string one, batch;
    std::vector<char> buffer(64 * 1024);
    bool ok = true;
    for(uint64_t key = 0; key < config.keyspace && ok;) {
        batch.clear();
        const uint64_t count = std::min<uint64_t>(1000, config.keyspace - key);
        for(uint64_t i = 0; i < count; i++) {
            builder.buildSet(one, key + i);
            batch += one;
        }
        size_t offset = 0;
        while(offset < batch.size()) {
            const ssize_t w = send(fd, batch.data() + offset, batch.size() - offset, MSG_NOSIGNAL);
            if(w <= 0) { ok = false; break; }
            offset += static_cast<size_t>(w);
        }
        uint64_t replies = 0;
        while(ok && replies < count) {
            const ssize_t got = recv(fd, buffer.data(), buffer.size(), 0);
            if(got <= 0) { ok = false; break; }
            ok = scanner.feed(buffer.data(), static_cast<size_t>(got), [&](bool isError) {
                replies++;
                if(isError) ok = false;
            }) && ok;
        }
        key += count;
    }
    close(fd);
    return ok;
}

std::string formatNs(uint64_t ns) {
    char text[32];
    if(ns < 1000) std::snprintf(text, sizeof(text), "%llu ns", static_cast<unsigned long long>(ns));
    else if(ns < 1000000) std::snprintf(text, sizeof(text), "%.1f us", static_cast<double>(ns) / 1e3);
    else if(ns < 1000000000) std::snprintf(text, sizeof(text), "%.2f ms", static_cast<double>(ns) / 1e6);
    else std::snprintf(text, sizeof(text), "%.3f s", static_cast<double>(ns) / 1e9);
    return text;
}

std::string percentileName(uint32_t ppm) {
    char text[16];
    if(ppm % 10000 == 0) std::snprintf(text, sizeof(text), "p%u", ppm / 10000);
    else if(ppm % 1000 == 0) std::snprintf(text, sizeof(text), "p%.1f", ppm / 10000.0);
    else std::snprintf(text, sizeof(text), "p%.2f", ppm / 10000.0);
    return text;
}

const char* statusName(QuantileStatus status) {
    switch(status) {
    case QuantileStatus::Ok: return "ok";
    case QuantileStatus::TooFewSamples: return "too_few_samples";
    case QuantileStatus::Unfinished: return "unfinished";
    case QuantileStatus::ClientLate: return "client_late";
    }
    return "?";
}

std::string describeRefusal(const Quantile& q, uint64_t n) {
    char text[120];
    switch(q.status) {
    case QuantileStatus::TooFewSamples: {
        const uint64_t needed = kMinSamplesAbove * 1000000 / (1000000 - q.ppm);
        std::snprintf(text, sizeof(text), "N/A: needs %llu samples, run has %llu",
                      static_cast<unsigned long long>(needed), static_cast<unsigned long long>(n));
        break;
    }
    case QuantileStatus::Unfinished:
        std::snprintf(text, sizeof(text), "N/A: falls among unanswered requests");
        break;
    case QuantileStatus::ClientLate:
        std::snprintf(text, sizeof(text), "N/A: generator was %s late here, over 1/%llu of the %s measured",
                      formatNs(q.clientLag).c_str(), static_cast<unsigned long long>(kMaxClientLagDivisor),
                      formatNs(q.value).c_str());
        break;
    case QuantileStatus::Ok:
        text[0] = '\0';
        break;
    }
    return text;
}

void writeQuantileJson(FILE* f, const Quantile& q) {
    std::fprintf(f, "{\"name\":\"%s\",\"status\":\"%s\",\"value_ns\":%llu,\"ci_low_ns\":%llu,"
                    "\"ci_high_ns\":%llu,\"client_lag_ns\":%llu,\"samples_above\":%llu,\"windows_above\":%u}",
                 percentileName(q.ppm).c_str(), statusName(q.status),
                 static_cast<unsigned long long>(q.value), static_cast<unsigned long long>(q.ciLow),
                 static_cast<unsigned long long>(q.ciHigh), static_cast<unsigned long long>(q.clientLag),
                 static_cast<unsigned long long>(q.samplesAbove), q.windowsAbove);
}

void usage() {
    std::fputs(
        "Usage: kosh-loadgen --rate REQ_PER_SEC [options]\n"
        "  --host H         server address (default 127.0.0.1)\n"
        "  --port P         server port (default 6379)\n"
        "  --connections N  connections in all (default 8)\n"
        "  --threads N      threads, each spinning on a core (default 1)\n"
        "  --duration S     seconds measured (default 30)\n"
        "  --warmup S       seconds sent but not measured, first (default 2)\n"
        "  --drain S        seconds to wait for late replies (default 10)\n"
        "  --keyspace N     distinct keys (default 100000)\n"
        "  --value-size N   bytes per value (default 64)\n"
        "  --set-ratio R    fraction of requests that are SET (default 0.1)\n"
        "  --seed N         seed for keys and the GET/SET mix (default 1)\n"
        "  --no-prefill     don't write every key before starting\n"
        "  --json PATH      also write the results as JSON\n"
        "Exit status: 0 ok, 1 bad arguments, 2 can't connect or protocol error,\n"
        "             3 finished but with error replies or unanswered requests\n", stderr);
}

bool parseUint(const char* text, uint64_t& out) {
    char* end = nullptr;
    errno = 0;
    if(*text == '-' || *text == '\0') return false;
    out = std::strtoull(text, &end, 10);
    return errno == 0 && *end == '\0';
}

bool parseArgs(int argc, char** argv, Config& config) {
    for(int i = 1; i < argc; i++) {
        const std::string name = argv[i];
        if(name == "--no-prefill") { config.prefill = false; continue; }
        if(i + 1 >= argc) return false;
        const char* value = argv[++i];
        bool ok = true;
        if(name == "--host") config.host = value;
        else if(name == "--port") config.port = value;
        else if(name == "--rate") ok = parseUint(value, config.rate);
        else if(name == "--connections") ok = parseUint(value, config.connections);
        else if(name == "--threads") ok = parseUint(value, config.threads);
        else if(name == "--duration") ok = parseUint(value, config.duration);
        else if(name == "--warmup") ok = parseUint(value, config.warmup);
        else if(name == "--drain") ok = parseUint(value, config.drain);
        else if(name == "--keyspace") ok = parseUint(value, config.keyspace);
        else if(name == "--value-size") ok = parseUint(value, config.valueSize);
        else if(name == "--seed") ok = parseUint(value, config.seed);
        else if(name == "--json") config.jsonPath = value;
        else if(name == "--set-ratio") {
            char* end = nullptr;
            config.setRatio = std::strtod(value, &end);
            ok = *end == '\0' && config.setRatio >= 0.0 && config.setRatio <= 1.0;
        }
        else ok = false;
        if(!ok) return false;
    }
    return config.rate >= 1 && config.rate <= 1000000000 && config.connections >= 1 &&
           config.threads >= 1 && config.threads <= config.connections &&
           config.duration >= 1 && config.duration <= 86400 && config.warmup <= 86400 &&
           config.keyspace >= 1;
}

} // namespace

int main(int argc, char** argv) {
    Config config;
    if(!parseArgs(argc, argv, config)) {
        usage();
        return 1;
    }
    std::signal(SIGPIPE, SIG_IGN);

    if(config.setRatio < 1.0 && config.prefill && !prefill(config)) {
        std::fprintf(stderr, "kosh-loadgen: prefill failed at %s:%s\n", config.host.c_str(), config.port.c_str());
        return 2;
    }

    // Connections are dealt to threads one by one, so each thread's requests
    // stay evenly spread through the schedule
    std::vector<std::vector<Conn>> perThread(config.threads);
    for(uint64_t i = 0; i < config.connections; i++) {
        perThread[i % config.threads].emplace_back();
    }
    uint64_t index = 0;
    for(uint64_t round = 0; index < config.connections; round++) {
        for(uint64_t t = 0; t < config.threads && index < config.connections; t++) {
            if(round >= perThread[t].size()) continue;
            Conn& c = perThread[t][round];
            c.index = index++;
            c.fd = connectTo(config);
            if(c.fd < 0 || fcntl(c.fd, F_SETFL, O_NONBLOCK) < 0) {
                std::fprintf(stderr, "kosh-loadgen: can't connect to %s:%s\n", config.host.c_str(), config.port.c_str());
                return 2;
            }
        }
    }

    const uint64_t startNs = nowNs() + 100000000;    // 100 ms for the threads to start
    std::vector<Worker> workers;
    workers.reserve(config.threads);
    for(uint64_t t = 0; t < config.threads; t++) {
        workers.emplace_back(config, perThread[t], startNs, config.seed + t);
    }
    std::vector<std::thread> threads;
    for(Worker& w : workers) threads.emplace_back([&w] { w.run(); });
    for(std::thread& t : threads) t.join();

    Result all;
    all.windowMax.assign(config.duration, 0);
    for(Worker& w : workers) {
        Result& r = w.result();
        all.intended.merge(r.intended);
        all.actual.merge(r.actual);
        all.lag.merge(r.lag);
        all.recorded += r.recorded;
        all.scheduledMeasured += r.scheduledMeasured;
        all.errors += r.errors;
        all.lost += r.lost;
        all.protocolErrors += r.protocolErrors;
        for(size_t i = 0; i < all.windowMax.size(); i++) {
            all.windowMax[i] = std::max(all.windowMax[i], r.windowMax[i]);
        }
    }
    for(auto& conns : perThread) {
        for(Conn& c : conns) close(c.fd);
    }

    const uint64_t unfinished = all.scheduledMeasured - all.recorded;
    const uint64_t n = all.scheduledMeasured;
    static const uint32_t kPercentiles[] = {500000, 900000, 990000, 999000, 999900};

    std::printf("kosh-loadgen  %s:%s  target %llu req/s, %llu connections, %llu thread(s), "
                "%llu s measured after %llu s warm-up, %.0f%% SET\n",
                config.host.c_str(), config.port.c_str(), static_cast<unsigned long long>(config.rate),
                static_cast<unsigned long long>(config.connections), static_cast<unsigned long long>(config.threads),
                static_cast<unsigned long long>(config.duration), static_cast<unsigned long long>(config.warmup),
                config.setRatio * 100.0);
    std::printf("requests: %llu scheduled, %llu answered, %llu error replies, %llu unanswered, "
                "%llu connections lost\n",
                static_cast<unsigned long long>(n), static_cast<unsigned long long>(all.recorded),
                static_cast<unsigned long long>(all.errors), static_cast<unsigned long long>(unfinished),
                static_cast<unsigned long long>(all.lost));
    std::printf("\nlatency from the intended send time (what a client waited):\n");

    std::vector<Quantile> intendedRows, actualRows;
    for(uint32_t ppm : kPercentiles) {
        intendedRows.push_back(computeQuantile(all.intended, unfinished, ppm, &all.lag, all.windowMax));
        actualRows.push_back(computeQuantile(all.actual, unfinished, ppm, nullptr, {}));
    }
    bool oneBurst = false;
    for(const Quantile& q : intendedRows) {
        if(q.status == QuantileStatus::Ok) {
            std::printf("  %-7s %-11s 95%% CI [%s, %s]  in %u of %llu s%s\n",
                        percentileName(q.ppm).c_str(), formatNs(q.value).c_str(),
                        formatNs(q.ciLow).c_str(), formatNs(q.ciHigh).c_str(), q.windowsAbove,
                        static_cast<unsigned long long>(config.duration), q.windowsAbove == 1 ? "  *" : "");
            oneBurst = oneBurst || q.windowsAbove == 1;
        } else {
            std::printf("  %-7s %s\n", percentileName(q.ppm).c_str(), describeRefusal(q, n).c_str());
        }
    }
    std::printf("  %-7s %s%s\n", "max", unfinished > 0 ? "over " : "", formatNs(all.intended.max()).c_str());
    if(oneBurst) std::printf("  * one second produced all of this tail: a single event, so the run can't say how often it recurs\n");

    std::printf("\nlatency from the moment the kernel took the request (what a send-timed tool reports):\n");
    for(const Quantile& q : actualRows) {
        std::printf("  %-7s %s\n", percentileName(q.ppm).c_str(),
                    q.status == QuantileStatus::Ok ? formatNs(q.value).c_str() : "N/A");
    }
    std::printf("  %-7s %s\n", "max", formatNs(all.actual.max()).c_str());

    std::printf("\ngenerator's own lateness: p50 %s, p99 %s, max %s\n",
                formatNs(all.lag.valueAtRank(rankOf(500000, all.lag.count()))).c_str(),
                formatNs(all.lag.valueAtRank(rankOf(990000, all.lag.count()))).c_str(),
                formatNs(all.lag.max()).c_str());
    if(all.protocolErrors > 0) std::printf("protocol errors: %llu\n", static_cast<unsigned long long>(all.protocolErrors));

    if(!config.jsonPath.empty()) {
        FILE* f = std::fopen(config.jsonPath.c_str(), "w");
        if(f == nullptr) {
            std::fprintf(stderr, "kosh-loadgen: can't write %s\n", config.jsonPath.c_str());
            return 1;
        }
        std::fprintf(f, "{\"rate\":%llu,\"duration_s\":%llu,\"scheduled\":%llu,\"answered\":%llu,"
                        "\"errors\":%llu,\"unanswered\":%llu,\"connections_lost\":%llu,\"protocol_errors\":%llu,"
                        "\"max_ns\":%llu,\"actual_max_ns\":%llu,\"lag_max_ns\":%llu,\"intended\":[",
                     static_cast<unsigned long long>(config.rate), static_cast<unsigned long long>(config.duration),
                     static_cast<unsigned long long>(n), static_cast<unsigned long long>(all.recorded),
                     static_cast<unsigned long long>(all.errors), static_cast<unsigned long long>(unfinished),
                     static_cast<unsigned long long>(all.lost), static_cast<unsigned long long>(all.protocolErrors),
                     static_cast<unsigned long long>(all.intended.max()), static_cast<unsigned long long>(all.actual.max()),
                     static_cast<unsigned long long>(all.lag.max()));
        for(size_t i = 0; i < intendedRows.size(); i++) {
            if(i > 0) std::fputc(',', f);
            writeQuantileJson(f, intendedRows[i]);
        }
        std::fputs("],\"actual\":[", f);
        for(size_t i = 0; i < actualRows.size(); i++) {
            if(i > 0) std::fputc(',', f);
            writeQuantileJson(f, actualRows[i]);
        }
        std::fputs("]}\n", f);
        std::fclose(f);
    }

    if(all.protocolErrors > 0) return 2;
    if(all.errors > 0 || unfinished > 0 || all.lost > 0) return 3;
    return 0;
}

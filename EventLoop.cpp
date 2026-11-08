#include "EventLoop.h"
#include "Server.h"
#include <iostream>
#include <cmath>

// The server batches AOF writes (group commit, see run()): the logger only
// buffers records, and the loop writes them before sending any reply.
EventLoop::EventLoop(Server& server, InMemoryStorage& storage, AOFLogger* aof,
                     std::unique_ptr<Poller> poller)
    : server_(server), storage_(storage), aof_(aof), poller_(std::move(poller)) {
    if(aof_) aof_->setDeferred(true);
}

EventLoop::~EventLoop() {
    if(aof_) aof_->setDeferred(false);      // writes whatever is still buffered
}

// ==================== I/O Multiplexing Event Loop ====================
//
// How the loop works:
//   1. The listening socket and every client socket are registered with the
//      poller (epoll on Linux, select() elsewhere; see Poller.h)
//   2. wait() blocks until at least one of them is ready
//   3. Handle only the ready ones:
//      - listen socket ready → new client connecting → accept()
//      - client readable → recv(), run its complete commands, send the replies
//      - client writable → send more of the replies it still has waiting
//   4. Repeat
//
// Every socket is non-blocking, so no call ever waits for one client: if a
// reply can't be sent yet, it stays in that client's buffer and the loop asks
// the poller to report when the socket can take more.
//
// Why single-threaded?
//   Commands run one at a time, so no two commands can race each other.
//   Redis's main thread uses the same model, also with epoll on Linux, which
//   returns only the ready sockets; select() scans every socket each call.

void EventLoop::run() {
    std::vector<Poller::Event> ready;
    while(server_.keepRunning()) {
        // On POSIX a stop signal cuts this wait short (EINTR). It can still land
        // just after the check above and before the wait starts; the 1-second
        // timeout bounds that delay (pselect() or a self-pipe would remove it).
        // On Windows the console handler runs on another thread, so the timeout
        // is what lets the loop notice.
        if(poller_->wait(1000, ready) < 0) {
            int error = lastSocketError();      // before printing, which can reset it (Windows)
            if(server_.running()) std::cerr << poller_->name() << " error: " << socketErrorText(error) << "\n";
            continue;
        }
        sampleOpsPerSec();

        // 1. Read from every ready client and run its commands. Their replies
        //    wait in each client's output buffer, their AOF records in the AOF's.
        //    (`ready` is a separate list, so removing a client while going
        //    through it is safe.)
        for(const auto& event : ready) {
            if(event.fd == listen_socket_) {
                server_.acceptNewClients();
                continue;
            }
            // Each check also skips a client removed earlier in this pass
            if(event.readable && clients_.count(event.fd)) handleReadable(event.fd);
            if(event.writable && clients_.count(event.fd)) queueToServe(event.fd, clients_.at(event.fd));
        }

        // 2. Group commit: one write() hands the OS the AOF records of every
        //    command this pass ran, where each command used to make its own
        //    (with --appendfsync always, one sync puts them on the disk).
        //    Redis does the same in beforeSleep().
        if(!flushAof()) break;

        // 3. Only now send the replies, so none leaves before its record is
        //    with the OS. (serveClient() flushes again before sending replies to
        //    commands it runs itself.)
        for(socket_t fd : to_serve_) {
            auto it = clients_.find(fd);
            if(it == clients_.end()) continue;      // removed earlier in this pass
            it->second.queued = false;
            serveClient(fd);
        }
        to_serve_.clear();
    }
}

// After each wakeup of the event loop, at most every 100 ms. While the
// server is idle, the loop still wakes once a second (the poller's timeout),
// so the rate falls back to 0 within the 16 samples.
void EventLoop::sampleOpsPerSec() {
    using namespace std::chrono;
    auto now = steady_clock::now();
    if(op_samples_taken_ > 0) {
        const OpSample& last = op_samples_[(op_sample_next_ + kOpSamples - 1) % kOpSamples];
        if(now - last.time < milliseconds(100)) return;
    }
    op_samples_[op_sample_next_] = {now, stats_.commands_processed};
    op_sample_next_ = (op_sample_next_ + 1) % kOpSamples;
    if(op_samples_taken_ < kOpSamples) op_samples_taken_++;
    // The oldest sample kept: the first while the ring fills, then the next to be overwritten
    const OpSample& oldest = op_samples_[op_samples_taken_ < kOpSamples ? 0 : op_sample_next_];
    double seconds = duration<double>(now - oldest.time).count();
    stats_.ops_per_sec = seconds > 0
        ? static_cast<uint64_t>(std::llround(static_cast<double>(stats_.commands_processed - oldest.commands) / seconds))
        : 0;
}

// ==================== Connection Management ====================

// Registers the client with empty buffers, watched for readability. With a
// password set, it must AUTH first.
void EventLoop::addClient(socket_t fd, uint64_t id) {
    Client& added = clients_[fd];
    added.authenticated = !server_.password();
    added.id = id;
    poller_->add(fd);
}

void EventLoop::removeClient(socket_t fd) {
    poller_->remove(fd);
    CLOSE_SOCKET(fd);
    clients_.erase(fd);
    std::cout << "Client disconnected (remaining: " << clients_.size() << ")\n";
}

void EventLoop::closeAllClients() {
    for(auto& [fd, client] : clients_) {
        poller_->remove(fd);
        CLOSE_SOCKET(fd);
    }
    clients_.clear();
}

// ==================== Client Data Handling ====================

void EventLoop::handleReadable(socket_t fd) {
    Client& client = clients_.at(fd);

    // Read into a scratch buffer and append only what arrived. Reading straight
    // into the client's string would need resize() first, which zero-fills all
    // 16 KB every time, while a pipelined batch is often under 1 KB: the
    // profile showed that memset at 2.4% of CPU. (C++23's
    // resize_and_overwrite would avoid the zero-fill.)
    char buffer[kReadChunk];
    long long n = recvSome(fd, buffer, sizeof(buffer));

    if(n == 0) {            // the client closed the connection
        removeClient(fd);
        return;
    }
    if(n < 0) {
        int error = lastSocketError();
        if(wouldBlock(error) || isInterrupted(error)) return;   // nothing to read after all
        removeClient(fd);   // connection error, e.g. reset by the client
        return;
    }
    client.in.append(buffer, static_cast<size_t>(n));

    // The parser limits each part of a command; this caps the total a client can
    // make the server hold (Redis's client-query-buffer-limit default).
    if(client.in.size() > kMaxQueryBuffer) {
        std::cerr << "Closing client: input exceeds the "
                  << (kMaxQueryBuffer >> 20) << " MB query buffer limit\n";
        removeClient(fd);
        return;
    }
    // Run its commands now; the replies are sent once the pass has written
    // the AOF (see run())
    runCommands(client);
    if(!client.out.empty()) queueToServe(fd, client);
}

// Hands the AOF records of the commands run so far to the OS, and with
// --appendfsync always to the disk. False if that has failed for good: with
// always, a reply must mean the write is on the disk, which can no longer be
// promised. Then no reply is sent and the server stops, as Redis exits.
bool EventLoop::flushAof() {
    if(!aof_) return true;
    aof_->flush();
    if(!aof_->failedForGood()) return true;
    server_.stopWithError("Stopping: the AOF can't be written or synced (" + aof_->lastWriteError() +
                          "), and with --appendfsync always a reply must mean the write is on disk");
    return false;
}

void EventLoop::queueToServe(socket_t fd, Client& client) {
    if(client.queued) return;
    client.queued = true;
    to_serve_.push_back(fd);
}

// Sends what the client has waiting, then runs its next commands, until it has
// no complete command left or its socket can't take more. New commands run only
// once earlier replies are all sent, so a client that doesn't read gets no
// further work done for it: its replies stay bounded and TCP flow control slows
// it down.
void EventLoop::serveClient(socket_t fd) {
    Client& client = clients_.at(fd);
    while(true) {
        if(client.out_sent < client.out.size()) {
            // No reply leaves before the AOF records of its commands are with the
            // OS. Usually run() has written them already and this does nothing.
            if(!flushAof()) return;
            SendStatus status = sendReplies(fd, client);
            if(status == SendStatus::Failed) {     // the client has gone
                removeClient(fd);
                return;
            }
            if(status == SendStatus::Blocked) break;   // the poller will say when there is room
        }
        if(client.close_after_reply) {             // QUIT or protocol error, reply sent
            removeClient(fd);
            return;
        }
        // Commands may be waiting in `in` even if no new data arrives: the client
        // may have sent everything and now waits for these replies.
        runCommands(client);
        if(client.out.empty()) break;              // no complete command left
    }
    updateInterest(fd, client);
}

// Runs whole commands from the client's input and appends their replies, until
// the input has no complete command left or kOutputHighWater bytes of replies
// are waiting.
void EventLoop::runCommands(Client& client) {
    // Parse from a moving position and drop the used bytes once at the end.
    // Erasing each command from the front would move all the bytes behind it,
    // once per command: quadratic in the number of commands in one read.
    size_t pos = 0;
    CommandContext ctx{storage_, aof_, clients_.size()};
    ctx.password = server_.password();
    ctx.authenticated = client.authenticated;
    ctx.protocol = client.protocol;
    ctx.client_id = client.id;
    ctx.client_name = &client.name;
    ctx.transaction = &client.transaction;
    ctx.server = &stats_;
    while(client.out.size() < kOutputHighWater && !client.close_after_reply) {
        // Into args_, whose strings are reused from command to command. Until
        // the client authenticates, smaller limits apply (see RespParser); an
        // AUTH lifts them for the next command in the same read.
        auto parsed = RespParser::parseInto(std::string_view(client.in).substr(pos), args_,
                                            ctx.authenticated);
        if(parsed.status == RespParser::ParseStatus::NeedMore) break;  // wait for more data
        if(parsed.status == RespParser::ParseStatus::Error) {
            // As Redis does: say why, then disconnect. Nothing after the bad bytes
            // can be trusted, because where the bad command ends is unknown.
            client.out += RespParser::error("ERR Protocol error: " + parsed.error);
            client.close_after_reply = true;
            client.in.clear();
            return;
        }
        pos += parsed.consumed;
        if(parsed.argc == 0) continue;

        // The reply goes straight into the client's buffer
        executeCommand(ctx, Args(args_.data(), parsed.argc), client.out);
        client.authenticated = ctx.authenticated;   // AUTH or HELLO may have succeeded
        client.protocol = ctx.protocol;             // HELLO may have switched it
        releaseLargeArgs(parsed.argc);

        // QUIT: send its reply, then disconnect; commands after it don't run
        if(ctx.close_connection) {
            client.close_after_reply = true;
            client.in.clear();
            return;
        }
    }
    client.in.erase(0, pos);
}

// args_ keeps its memory so that parsing allocates nothing, but one command
// with a huge argument (up to 512 MB) or very many would leave it that large
// for good. Give such memory back, as Redis shrinks a client's query buffer.
void EventLoop::releaseLargeArgs(size_t argc) {
    for(size_t i = 0; i < argc; i++) {
        if(args_[i].capacity() > kKeepArgBytes) std::string().swap(args_[i]);
    }
    if(args_.size() > kKeepArgs) {
        args_.resize(kKeepArgs);
        args_.shrink_to_fit();
    }
}

// Sends as much of the waiting replies as the socket takes. A partial send is
// normal: the rest goes when the poller reports the socket writable again.
EventLoop::SendStatus EventLoop::sendReplies(socket_t fd, Client& client) {
    while(client.out_sent < client.out.size()) {
        long long n = sendSome(fd, client.out.data() + client.out_sent,
                               client.out.size() - client.out_sent);
        if(n > 0) {
            client.out_sent += static_cast<size_t>(n);
            continue;
        }
        if(n < 0) {
            int error = lastSocketError();
            if(isInterrupted(error)) continue;
            if(wouldBlock(error)) return SendStatus::Blocked;   // socket buffer full
        }
        return SendStatus::Failed;   // e.g. EPIPE or ECONNRESET
    }
    client.out.clear();
    client.out_sent = 0;
    return SendStatus::Done;
}

// Reads while nothing waits to be sent; waits for writability while something does.
void EventLoop::updateInterest(socket_t fd, Client& client) {
    bool pending = client.out_sent < client.out.size();
    bool want_read = !pending && !client.close_after_reply;
    bool want_write = pending;
    if(want_read != client.want_read || want_write != client.want_write) {
        poller_->setInterest(fd, want_read, want_write);
        client.want_read = want_read;
        client.want_write = want_write;
    }
}

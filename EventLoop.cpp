#include "EventLoop.h"
#include "Server.h"
#include <iostream>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cstdlib>

// The server batches AOF writes (group commit, see run()): the logger only
// buffers records, and the loop writes them before sending any reply.
EventLoop::EventLoop(Server& server, size_t index, size_t loops, InMemoryStorage& storage, AOFLogger* aof,
                     std::unique_ptr<Poller> poller)
    : server_(server), index_(index), loops_(loops), storage_(storage), aof_(aof), poller_(std::move(poller)) {
    if(aof_) aof_->setDeferred(true);
    if(loops_ > 1) {
        wakeup_ = std::make_unique<Wakeup>();
        new_clients_ = std::make_unique<SpscRing<NewClient>>(kNewClientQueue);
        poller_->add(static_cast<socket_t>(wakeup_->fd()));
        inbound_.resize(loops_);
        for(size_t from = 0; from < loops_; from++) {
            if(from != index_) inbound_[from] = std::make_unique<SpscRing<Batch*>>(kBatchRingSize);
        }
        open_batches_.resize(loops_, nullptr);
        batches_out_.resize(loops_, 0);
    }
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
        const int wait_ms = prepareToWait();
        if(poller_->wait(wait_ms, ready) < 0) {
            int error = lastSocketError();      // before printing, which can reset it (Windows)
            if(server_.running()) std::cerr << poller_->name() << " error: " << socketErrorText(error) << "\n";
            continue;
        }
        if(wakeup_) wakeup_->awake();
        sampleOpsPerSec();

        // 0. What other threads sent: their commands to run, and the replies
        //    to commands this thread sent them.
        processBatches();

        // 1. Read from every ready client and run its commands. Their replies
        //    wait in each client's output buffer, their AOF records in the AOF's.
        //    (`ready` is a separate list, so removing a client while going
        //    through it is safe.)
        for(const auto& event : ready) {
            if(event.fd == listen_socket_) {
                server_.acceptNewClients();
                continue;
            }
            if(wakeup_ && event.fd == static_cast<socket_t>(wakeup_->fd())) {
                wakeup_->consume();
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

        // Commands for other threads go out, and so do the replies to theirs,
        // now that those commands' AOF records are written: a thread's reply
        // carries the same promise as a reply to a client.
        publishReplies();
        publishRequests();

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

        // Serving a client can run more of its commands
        publishRequests();
    }
    // A signal reaches only one thread, and an error stops only the loop that
    // met it: the others may be asleep
    server_.wakeAllLoops();
}

// The receiving half of the protocol in Wakeup.h: say this loop is going to
// sleep, and only then make the last check for work. The stop check is part of
// that: the server stops by setting a flag and then waking the loops.
int EventLoop::prepareToWait() {
    if(!wakeup_) return kIdleWaitMs;
    const bool work = wakeup_->goingToSleep([this] {
        const bool adopted = adoptNewClients();
        const bool batches = collectBatches();
        return adopted || batches || !server_.keepRunning();
    });
    return work ? 0 : kIdleWaitMs;
}

bool EventLoop::collectBatches() {
    Batch* batch = nullptr;
    bool any = false;
    for(size_t from = 0; from < loops_; from++) {
        if(from == index_) continue;
        while(inbound_[from]->tryPop(batch)) {
            inbox_.push_back(batch);
            any = true;
        }
    }
    return any;
}

void EventLoop::processBatches() {
    for(Batch* batch : inbox_) {
        if(batch->answered) {
            deliverReplies(*batch);
            batches_out_[batch->owner]--;
            recycleBatch(batch);
        } else {
            runBatch(*batch);
            batch->answered = true;
            replies_ready_.push_back(batch);
        }
    }
    inbox_.clear();
}

// Runs another thread's commands on this thread's keys, through the same
// command table as any other. The home thread already checked that each is
// allowed for its client, and it parsed these same bytes, so they parse here.
void EventLoop::runBatch(Batch& batch) {
    CommandContext ctx{storage_, aof_, server_.connectedClients()};
    batch.replies.clear();
    size_t pos = 0;
    const auto nextCommand = [&] {
        auto parsed = RespParser::parseInto(std::string_view(batch.requests).substr(pos), args_);
        pos += parsed.consumed;
        return parsed.argc;
    };
    static const std::vector<std::string> kExec{"EXEC"};
    for(Item& item : batch.items) {
        ctx.protocol = item.protocol;
        // A part of a split command is counted once, at home, when it finishes
        ctx.server = item.gather ? nullptr : &stats_;
        if(item.kind == ItemKind::Info) {
            CommandContext own = ctx;
            own.server = &stats_;
            const InfoData info = localInfo(own);
            batch.replies.append(reinterpret_cast<const char*>(&info), sizeof(info));
        } else if(item.kind == ItemKind::Transaction) {
            // The home thread queued and checked it. Running it here through
            // EXEC makes it one task on this thread, and logs it between
            // MULTI and EXEC in this thread's file, as with one thread.
            Transaction transaction;
            transaction.active = true;
            transaction.writes = item.writes;
            for(uint32_t i = 0; i < item.commands; i++) {
                const size_t argc = nextCommand();
                transaction.queued.emplace_back(args_.begin(), args_.begin() + static_cast<long>(argc));
                releaseLargeArgs(argc);
            }
            ctx.transaction = &transaction;
            executeCommand(ctx, kExec, batch.replies);
            ctx.transaction = nullptr;
        } else {
            const size_t argc = nextCommand();
            executeCommand(ctx, Args(args_.data(), argc), batch.replies);
            releaseLargeArgs(argc);
        }
        item.reply_end = batch.replies.size();
    }
}

void EventLoop::deliverReplies(Batch& batch) {
    const std::string_view replies(batch.replies);
    size_t start = 0;
    for(const Item& item : batch.items) {
        const std::string_view reply = replies.substr(start, item.reply_end - start);
        start = item.reply_end;
        if(item.gather) {
            applyPart(*item.gather, item.part, reply);
            partDone(*item.gather);
            continue;
        }
        auto it = clients_.find(item.fd);
        // It may have left, and the socket number been given to a new client
        if(it != clients_.end() && it->second.id == item.client_id) {
            deliverReply(it->second, item.number, reply);
            queueToServe(item.fd, it->second);
        }
    }
}

// ==================== Commands that need several threads ====================

// Reserves the client's reply slot now, so the combined reply takes the place
// of the command in its pipeline however long the parts take
EventLoop::Gather* EventLoop::newGather(Client& client, Merge merge, int protocol, size_t waiting) {
    gathers_.emplace_back();
    Gather& gather = gathers_.back();
    gather.self = std::prev(gathers_.end());
    gather.merge = merge;
    gather.fd = client.fd;
    gather.client_id = client.id;
    gather.number = client.slots_tail;
    gather.protocol = protocol;
    gather.waiting = waiting;
    reserveSlot(client);
    return &gather;
}

EventLoop::Batch& EventLoop::openBatch(size_t owner) {
    if(!open_batches_[owner]) open_batches_[owner] = takeBatch();
    return *open_batches_[owner];
}

void EventLoop::sendPart(size_t owner, Gather* gather, uint32_t part, const std::vector<std::string>& command,
                         ItemKind kind) {
    Batch& batch = openBatch(owner);
    if(kind == ItemKind::Command) RespParser::appendArray(batch.requests, command);
    Item item;
    item.kind = kind;
    item.gather = gather;
    item.part = part;
    item.protocol = gather->protocol;
    batch.items.push_back(item);
}

void EventLoop::runPartHere(Gather* gather, uint32_t part, CommandContext& ctx,
                            const std::vector<std::string>& command) {
    std::string reply;
    ServerStats* server = ctx.server;
    ctx.server = nullptr;       // counted once, when the whole command finishes
    executeCommand(ctx, command, reply);
    ctx.server = server;
    applyPart(*gather, part, reply);
}

// Takes in one thread's answer. These replies come from this server, in the
// format the merge expects, unless the part failed.
void EventLoop::applyPart(Gather& gather, uint32_t part, std::string_view reply) {
    if(gather.merge == Merge::Info) {
        InfoData info;
        std::memcpy(static_cast<void*>(&info), reply.data(), sizeof(info));
        mergeInfo(gather.info, info);
        return;
    }
    if(!reply.empty() && reply[0] == '-') {
        if(gather.error.empty()) gather.error.assign(reply);
        return;
    }
    switch(gather.merge) {
        case Merge::Sum: {
            long long value = 0;
            std::from_chars(reply.data() + 1, reply.data() + reply.size(), value);
            gather.sum += value;
            break;
        }
        case Merge::Values: {
            // An array of one value per key of this part: put each back in
            // the position its key had in the command
            size_t pos = reply.find("\r\n") + 2;
            for(uint32_t key : gather.keys_of_part[part]) {
                const size_t start = pos;
                if(reply[pos] == '_') {             // RESP3's null
                    pos += 3;
                } else {                            // $<length>, or $-1
                    const size_t end_of_line = reply.find("\r\n", pos);
                    long long length = 0;
                    std::from_chars(reply.data() + pos + 1, reply.data() + end_of_line, length);
                    pos = end_of_line + 2;
                    if(length >= 0) pos += static_cast<size_t>(length) + 2;
                }
                gather.values[key].assign(reply.substr(start, pos - start));
            }
            break;
        }
        case Merge::ScanStep: {
            // *2, then the thread's next cursor, then the keys. A cursor of 0
            // means this thread is done, so the next one starts.
            const size_t bulk_header = reply.find("\r\n") + 2;
            const size_t cursor_start = reply.find("\r\n", bulk_header) + 2;
            const size_t cursor_end = reply.find("\r\n", cursor_start);
            uint64_t cursor = 0;
            std::from_chars(reply.data() + cursor_start, reply.data() + cursor_end, cursor);
            uint64_t next = 0;
            if(cursor != 0) next = (uint64_t{gather.thread} << kScanThreadShift) | cursor;
            else if(gather.thread + 1 < loops_) next = uint64_t{gather.thread + 1} << kScanThreadShift;
            RespParser::appendLength(gather.scan_reply, '*', 2);
            RespParser::appendBulk(gather.scan_reply, std::to_string(next));
            gather.scan_reply.append(reply.substr(cursor_end + 2));
            break;
        }
        default:    // AllOk: nothing to keep
            break;
    }
}

// When the last part is in: the one reply, into the slot kept for it
void EventLoop::partDone(Gather& gather) {
    if(--gather.waiting != 0) return;
    stats_.commands_processed++;
    std::string reply;
    if(gather.merge == Merge::Info) {
        appendInfo(gather.info, Args(gather.args), gather.protocol, reply);
    } else if(!gather.error.empty()) {
        reply = gather.error;
    } else if(gather.merge == Merge::Sum) {
        RespParser::appendInteger(reply, gather.sum);
    } else if(gather.merge == Merge::AllOk) {
        RespParser::appendSimpleString(reply, "OK");
    } else if(gather.merge == Merge::Values) {
        RespParser::appendLength(reply, '*', gather.values.size());
        for(const std::string& value : gather.values) reply += value;
    } else {
        reply = std::move(gather.scan_reply);
    }
    auto it = clients_.find(gather.fd);
    if(it != clients_.end() && it->second.id == gather.client_id) {
        deliverReply(it->second, gather.number, reply);
        queueToServe(gather.fd, it->second);
    }
    gathers_.erase(gather.self);
}

// MGET, DEL or EXISTS with keys on several threads: one command per thread, with
// that thread's keys. Each part is atomic on its thread; the whole is not.
void EventLoop::splitCommand(Client& client, CommandContext& ctx, const Command& cmd, const Args& args) {
    const Router& router = server_.router();
    std::vector<std::vector<uint32_t>> keys_of_thread(loops_);
    for(size_t i = 1; i < args.size(); i++) keys_of_thread[router.shardOf(args[i])].push_back(static_cast<uint32_t>(i - 1));
    size_t parts = 0;
    for(const auto& keys : keys_of_thread) parts += keys.empty() ? 0 : 1;

    Gather* gather = newGather(client, cmd.merge, ctx.protocol, parts);
    gather->values.resize(cmd.merge == Merge::Values ? args.size() - 1 : 0);
    gather->keys_of_part.reserve(parts);
    std::vector<std::string> own_part;
    uint32_t own_number = 0;
    for(size_t thread = 0; thread < loops_; thread++) {
        if(keys_of_thread[thread].empty()) continue;
        std::vector<std::string> part{args[0]};
        for(uint32_t key : keys_of_thread[thread]) part.push_back(args[key + 1]);
        const uint32_t number = static_cast<uint32_t>(gather->keys_of_part.size());
        gather->keys_of_part.push_back(std::move(keys_of_thread[thread]));
        if(thread == index_) {
            own_part = std::move(part);
            own_number = number;
        } else {
            sendPart(thread, gather, number, part, ItemKind::Command);
        }
    }
    if(!own_part.empty()) {
        runPartHere(gather, own_number, ctx, own_part);
        partDone(*gather);
    }
}

// The same command on every thread: DBSIZE, FLUSHALL, BGREWRITEAOF, INFO
void EventLoop::fanOut(Client& client, CommandContext& ctx, const Command& cmd, const Args& args) {
    Gather* gather = newGather(client, cmd.merge, ctx.protocol, loops_);
    const std::vector<std::string> command(args.begin(), args.end());
    const ItemKind kind = cmd.merge == Merge::Info ? ItemKind::Info : ItemKind::Command;
    for(size_t thread = 0; thread < loops_; thread++) {
        if(thread != index_) sendPart(thread, gather, static_cast<uint32_t>(thread), command, kind);
    }
    if(cmd.merge == Merge::Info) {
        gather->args = command;
        gather->info = localInfo(ctx);
    } else {
        runPartHere(gather, 0, ctx, command);
    }
    partDone(*gather);
}

// One call of SCAN covers part of one thread's table. The cursor says which
// thread, and when a thread is finished the cursor moves to the next, so
// every key present for the whole scan is returned, as each table guarantees.
EventLoop::Routed EventLoop::scanStep(Client& client, CommandContext& ctx, const Args& args) {
    uint64_t cursor = 0;
    if(!parseScanCursor(args[1], cursor)) return Routed::RunHere;       // it says why
    const size_t thread = static_cast<size_t>(cursor >> kScanThreadShift);
    if(thread >= loops_) {      // no such thread: nothing left to scan
        std::string& out = replyBuffer(client);
        RespParser::appendLength(out, '*', 2);
        RespParser::appendBulk(out, "0");
        RespParser::appendLength(out, '*', 0);
        replyDone(client);
        return Routed::Handled;
    }
    std::vector<std::string> command(args.begin(), args.end());
    command[1] = std::to_string(cursor & kScanCursorMask);
    Gather* gather = newGather(client, Merge::ScanStep, ctx.protocol, 1);
    gather->thread = thread;
    if(thread == index_) {
        runPartHere(gather, 0, ctx, command);
        partDone(*gather);
    } else {
        sendPart(thread, gather, 0, command, ItemKind::Command);
    }
    return Routed::Handled;
}

void EventLoop::refuse(Client& client, std::string_view error) {
    RespParser::appendError(replyBuffer(client), error);
    replyDone(client);
}

// Decides where a command runs, with more than one thread. Anything that will
// be refused is refused on the home thread, which alone knows the client's state.
EventLoop::Routed EventLoop::routeCommand(Client& client, CommandContext& ctx, const Args& args, std::string_view raw) {
    const Command* cmd = lookupCommand(args[0]);
    if(!cmd || !arityMatches(*cmd, args.size()) || (!ctx.authenticated && !(cmd->flags & kCmdNoAuth))) return Routed::RunHere;
    if(client.transaction.active) return routeInTransaction(client, ctx, *cmd, args);

    if(cmd->flags & kCmdWholeServer) {
        if(cmd->merge == Merge::ScanStep) return scanStep(client, ctx, args);
        // No need to wait for this client's earlier commands. Every thread
        // gets them in the order they were sent, by one first-in first-out
        // ring, and this thread ran its own as they were parsed. So on each
        // thread DBSIZE or FLUSHALL lands after those commands and before the
        // later ones, and the client's replies are in order by their slots.
        fanOut(client, ctx, *cmd, args);
        return Routed::Handled;
    }
    if(cmd->first_key == 0) return Routed::RunHere;
    const size_t owner = ownerOf(*cmd, args);
    if(owner == kRunHere) return Routed::RunHere;
    if(owner == kCrossThread) {
        if(cmd->merge == Merge::None) refuse(client, "CROSSSLOT Keys in request don't hash to the same slot");
        else splitCommand(client, ctx, *cmd, args);
        return Routed::Handled;
    }
    if(owner == index_) return Routed::RunHere;
    forward(client, owner, raw, ctx.protocol);
    return Routed::Handled;
}

// A transaction runs as one task on one thread, so that nothing can come between
// its commands. So every key must be on that thread; anything else is refused
// as it is queued, which aborts the transaction, and EXEC then says so.
EventLoop::Routed EventLoop::routeInTransaction(Client& client, CommandContext& ctx, const Command& cmd, const Args& args) {
    Transaction& transaction = client.transaction;
    if(cmd.flags & kCmdNotQueued) {             // EXEC, DISCARD, QUIT, a nested MULTI
        const bool exec = std::strcmp(cmd.name, "exec") == 0;
        if(!exec || transaction.aborted || transaction.owner == SIZE_MAX || transaction.owner == index_) return Routed::RunHere;
        forwardTransaction(client, ctx.protocol);
        return Routed::Handled;
    }
    if(cmd.flags & (kCmdWholeServer | kCmdConnection)) {
        transaction.aborted = true;
        refuse(client, "ERR Command not allowed inside a transaction with more than one thread");
        return Routed::Handled;
    }
    if(cmd.first_key != 0) {
        const size_t owner = ownerOf(cmd, args);
        const bool other_thread = owner == kCrossThread ||
                                  (owner < loops_ && transaction.owner != SIZE_MAX && owner != transaction.owner);
        if(other_thread) {
            transaction.aborted = true;
            refuse(client, "CROSSSLOT Keys in request don't hash to the same slot");
            return Routed::Handled;
        }
        if(owner < loops_) transaction.owner = owner;
    }
    // Queued here. Whether the AOF can be written is asked at EXEC, of the
    // thread that will write it, not of this one.
    ctx.aof = nullptr;
    executeCommand(ctx, args, replyBuffer(client));
    replyDone(client);
    ctx.aof = aof_;
    return Routed::Handled;
}

void EventLoop::forwardTransaction(Client& client, int protocol) {
    Transaction& transaction = client.transaction;
    Batch& batch = openBatch(transaction.owner);
    for(const auto& command : transaction.queued) RespParser::appendArray(batch.requests, command);
    Item item;
    item.fd = client.fd;
    item.client_id = client.id;
    item.number = client.slots_tail;
    item.protocol = protocol;
    item.kind = ItemKind::Transaction;
    item.commands = static_cast<uint32_t>(transaction.queued.size());
    item.writes = transaction.writes;
    batch.items.push_back(item);
    reserveSlot(client);
    transaction = Transaction{};
}

// The usual case is the oldest reply arriving, which goes straight to the
// output, followed by any later ones that had been waiting for it.
void EventLoop::deliverReply(Client& client, uint64_t number, std::string_view reply) {
    if(number != client.slots_head) {
        ReplySlot& slot = client.slot(number);
        slot.bytes.assign(reply);
        slot.ready = true;
        client.slot_bytes += reply.size();
        return;
    }
    client.out.append(reply);
    client.slots_head++;
    while(client.slots_head != client.slots_tail) {
        ReplySlot& slot = client.slot(client.slots_head);
        if(!slot.ready) break;
        client.out += slot.bytes;
        client.slot_bytes -= slot.bytes.size();
        slot.ready = false;
        if(slot.bytes.capacity() > kKeepSlotBytes) std::string().swap(slot.bytes);
        else slot.bytes.clear();
        client.slots_head++;
    }
}

void EventLoop::publishReplies() {
    for(Batch* batch : replies_ready_) sendBatch(batch->home, batch);
    replies_ready_.clear();
}

// A batch waits if too many are already out at that thread, and more commands
// join it meanwhile; it goes when one of them returns, which wakes this loop.
void EventLoop::publishRequests() {
    for(size_t to = 0; to < open_batches_.size(); to++) {
        if(!open_batches_[to] || batches_out_[to] >= kMaxBatchesOut) continue;
        open_batches_[to]->owner = to;
        sendBatch(to, open_batches_[to]);
        open_batches_[to] = nullptr;
        batches_out_[to]++;
    }
}

void EventLoop::sendBatch(size_t to, Batch* batch) {
    EventLoop& target = server_.loop(to);
    // Can't fail: see kMaxBatchesOut. If it did, a batch would be lost and
    // its client would wait for ever, so stop instead.
    if(!target.inbound_[index_]->tryPush(batch)) {
        std::cerr << "A ring between threads is full, which the batch limit should prevent\n";
        std::abort();
    }
    target.wakeup_->notify();
}

EventLoop::Batch* EventLoop::takeBatch() {
    if(free_batches_.empty()) {
        all_batches_.push_back(std::make_unique<Batch>());
        all_batches_.back()->home = index_;
        return all_batches_.back().get();
    }
    Batch* batch = free_batches_.back();
    free_batches_.pop_back();
    return batch;
}

void EventLoop::recycleBatch(Batch* batch) {
    if(batch->requests.capacity() > kKeepBatchBytes) std::string().swap(batch->requests);
    else batch->requests.clear();
    if(batch->replies.capacity() > kKeepBatchBytes) std::string().swap(batch->replies);
    else batch->replies.clear();
    batch->items.clear();
    batch->answered = false;
    free_batches_.push_back(batch);
}

// The thread that owns this command's keys, or kCrossThread if they are on
// several
size_t EventLoop::ownerOf(const Command& cmd, const Args& args) const {
    const Router& router = server_.router();
    const size_t argc = args.size();
    const size_t first = static_cast<size_t>(cmd.first_key);
    const size_t step = static_cast<size_t>(cmd.key_step);
    const size_t last = cmd.last_key < 0 ? argc - 1 : static_cast<size_t>(cmd.last_key);
    // Key and value pairs that don't pair up: the command itself says so
    if(step > 1 && (argc - first) % step != 0) return kRunHere;
    const size_t owner = router.shardOf(args[first]);
    for(size_t i = first + step; i <= last; i += step) {
        if(router.shardOf(args[i]) != owner) return kCrossThread;
    }
    return owner;
}

// Sends one command to the thread that owns its key, and keeps its place in
// the client's replies. Commands to one thread stay in order: they go in one
// batch, batches go through one ring, and a ring is first-in, first-out. So
// two commands on one key run in the order they were sent. Commands on
// different keys may run in either order, which no client can observe.
void EventLoop::forward(Client& client, size_t owner, std::string_view command, int protocol) {
    Batch& batch = openBatch(owner);
    batch.requests.append(command);
    Item item;
    item.fd = client.fd;
    item.client_id = client.id;
    item.number = client.slots_tail;
    item.protocol = protocol;
    batch.items.push_back(item);
    reserveSlot(client);
}

EventLoop::ReplySlot& EventLoop::reserveSlot(Client& client) {
    if(client.pendingReplies() == client.slots.size()) {
        std::vector<ReplySlot> bigger(client.slots.empty() ? 8 : client.slots.size() * 2);
        for(uint64_t number = client.slots_head; number != client.slots_tail; number++) {
            bigger[number & (bigger.size() - 1)] = std::move(client.slot(number));
        }
        client.slots = std::move(bigger);
    }
    ReplySlot& slot = client.slot(client.slots_tail++);
    stats_.peak_pending_replies = std::max<uint64_t>(stats_.peak_pending_replies, client.pendingReplies());
    return slot;
}

// With nothing outstanding, a reply goes straight to the output. Otherwise it
// must wait its turn behind the replies still on other threads.
std::string& EventLoop::replyBuffer(Client& client) {
    if(client.pendingReplies() == 0) return client.out;
    return reserveSlot(client).bytes;
}

void EventLoop::replyDone(Client& client) {
    if(client.pendingReplies() == 0) return;
    ReplySlot& slot = client.slot(client.slots_tail - 1);
    slot.ready = true;
    client.slot_bytes += slot.bytes.size();
}

bool EventLoop::adoptNewClients() {
    NewClient client;
    bool any = false;
    while(new_clients_->tryPop(client)) {
        addClient(client.fd, client.id);
        any = true;
    }
    return any;
}

bool EventLoop::postNewClient(const NewClient& client) {
    if(!new_clients_->tryPush(client)) return false;
    wakeup_->notify();
    return true;
}

void EventLoop::wake() {
    if(wakeup_) wakeup_->notify();
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
    added.fd = fd;
    added.authenticated = !server_.password();
    added.id = id;
    poller_->add(fd);
}

void EventLoop::removeClient(socket_t fd) {
    poller_->remove(fd);
    CLOSE_SOCKET(fd);
    clients_.erase(fd);
    // One string, so that lines from different threads can't interleave
    std::cout << ("Client disconnected (remaining: " + std::to_string(server_.clientRemoved()) + ")\n");
}

void EventLoop::closeAllClients() {
    // Dealt to this loop but never picked up: the server stopped first
    NewClient waiting;
    while(new_clients_ && new_clients_->tryPop(waiting)) CLOSE_SOCKET(waiting.fd);
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
        if(client.pendingReplies() == 0) {
            removeClient(fd);
            return;
        }
        // It may only have closed its sending side and still be reading, so
        // finish what other threads are running for it first
        client.close_after_reply = true;
        client.in.clear();
        queueToServe(fd, client);
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
    if(!client.out.empty() || client.pendingReplies() >= kMaxPendingReplies) queueToServe(fd, client);
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
        // QUIT or protocol error, reply sent, and nothing still to come
        if(client.close_after_reply && client.pendingReplies() == 0) {
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
    CommandContext ctx{storage_, aof_, server_.connectedClients()};
    ctx.password = server_.password();
    ctx.authenticated = client.authenticated;
    ctx.protocol = client.protocol;
    ctx.client_id = client.id;
    ctx.client_name = &client.name;
    ctx.transaction = &client.transaction;
    ctx.server = &stats_;
    while(client.out.size() + client.slot_bytes < kOutputHighWater &&
          client.pendingReplies() < kMaxPendingReplies && !client.close_after_reply) {
        // Into args_, whose strings are reused from command to command. Until
        // the client authenticates, smaller limits apply (see RespParser); an
        // AUTH lifts them for the next command in the same read.
        auto parsed = RespParser::parseInto(std::string_view(client.in).substr(pos), args_,
                                            ctx.authenticated);
        if(parsed.status == RespParser::ParseStatus::NeedMore) break;  // wait for more data
        if(parsed.status == RespParser::ParseStatus::Error) {
            // As Redis does: say why, then disconnect. Nothing after the bad bytes
            // can be trusted, because where the bad command ends is unknown.
            replyBuffer(client) += RespParser::error("ERR Protocol error: " + parsed.error);
            replyDone(client);
            client.close_after_reply = true;
            client.in.clear();
            return;
        }
        if(parsed.argc == 0) {
            pos += parsed.consumed;
            continue;
        }

        const Args args(args_.data(), parsed.argc);
        if(loops_ > 1) {
            const std::string_view raw = std::string_view(client.in).substr(pos, parsed.consumed);
            const Routed routed = routeCommand(client, ctx, args, raw);
            if(routed == Routed::Handled) {
                pos += parsed.consumed;
                releaseLargeArgs(parsed.argc);
                continue;
            }
        }
        pos += parsed.consumed;

        // Straight into the client's buffer, unless earlier replies are still
        // on other threads
        executeCommand(ctx, args, replyBuffer(client));
        replyDone(client);
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
    bool want_read = !pending && !client.close_after_reply && client.pendingReplies() < kMaxPendingReplies;
    bool want_write = pending;
    if(want_read != client.want_read || want_write != client.want_write) {
        poller_->setInterest(fd, want_read, want_write);
        client.want_read = want_read;
        client.want_write = want_write;
    }
}

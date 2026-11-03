#pragma once
// A hash table of pointers to entries that hold their own keys, with buckets
// of one cache line each and incremental rehashing. After Valkey 8.1's table
// (src/hashtable.c, Viktor Söderqvist), itself inspired by Swiss tables.
//
// - A bucket is 64 bytes, one cache line: a byte of metadata (7 bits saying
//   which slots hold an entry, 1 saying the last slot holds a child bucket),
//   a byte of each entry's hash (its "tag"), and 7 slots. A lookup reads one
//   bucket and compares the tags; only an entry whose tag matches (1 in 256
//   by chance) has its key compared. So finding a key costs two memory reads,
//   the bucket and the entry, where std::unordered_map makes three or four
//   (the bucket array, the node before the first in that bucket, the node,
//   the key's heap block).
// - A full bucket's last slot becomes a pointer to a child bucket.
// - The table grows by allocating a table twice the size and moving one
//   bucket per insert or erase, as Redis's dict does, so no operation pays
//   for moving every key: no stall at any size. Lookups check both tables
//   while a move is in progress.
// - Entries never move: the table holds pointers, so pointers to entries
//   stay valid. It doesn't own the entries; the caller allocates and frees
//   them. It never shrinks.
//
// Entry is the caller's type; KeyOf::key(const Entry&) returns its key.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <new>
#include <string_view>

template<typename Entry, typename KeyOf, typename Hash = std::hash<std::string_view>>
class KeyTable {
    public:
        KeyTable() = default;
        ~KeyTable() { release(t0_); release(t1_); }
        KeyTable(const KeyTable&) = delete;
        KeyTable& operator=(const KeyTable&) = delete;

        size_t size() const { return size_; }
        // Buckets in the table, plus the new table's while a rehash runs
        size_t bucketCount() const { return bucketsOf(t0_) + bucketsOf(t1_); }
        size_t childBuckets() const { return children_; }
        bool rehashing() const { return t1_.buckets != nullptr; }

        // The entry with this key, or nullptr. Changes nothing (no rehash
        // step), so it is safe under a shared lock.
        Entry* find(std::string_view key) const {
            size_t h = Hash{}(key);
            uint8_t tag = tagOf(h);
            if(rehashing()) {
                // A bucket the rehash hasn't reached yet can still hold the
                // key; entries added since the rehash began are in the new table
                size_t i = h & t0_.mask;
                if(i >= rehash_index_) {
                    if(Entry* e = findInChain(&t0_.buckets[i], key, tag)) return e;
                }
                return findInChain(&t1_.buckets[h & t1_.mask], key, tag);
            }
            if(!t0_.buckets) return nullptr;
            return findInChain(&t0_.buckets[h & t0_.mask], key, tag);
        }

        // Adds an entry whose key isn't in the table yet. If allocating a
        // table or a child bucket throws, the table is unchanged.
        void insert(Entry* entry) {
            growIfFull();
            rehashStep();
            size_t h = Hash{}(KeyOf::key(*entry));
            Table& t = rehashing() ? t1_ : t0_;
            insertInChain(&t.buckets[h & t.mask], entry, tagOf(h));
            size_++;
        }

        // Removes this entry (found by its key, matched by its address).
        // Returns false if it isn't in the table.
        bool erase(const Entry* entry) {
            rehashStep();
            size_t h = Hash{}(KeyOf::key(*entry));
            bool removed = false;
            if(rehashing()) {
                size_t i = h & t0_.mask;
                removed = i >= rehash_index_ && removeFromChain(&t0_.buckets[i], entry);
                if(!removed) removed = removeFromChain(&t1_.buckets[h & t1_.mask], entry);
            } else if(t0_.buckets) {
                removed = removeFromChain(&t0_.buckets[h & t0_.mask], entry);
            }
            if(removed) size_--;
            return removed;
        }

        // Forgets every entry (doesn't free them) and frees every bucket
        void clear() {
            release(t0_);
            release(t1_);
            rehash_index_ = 0;
            size_ = 0;
        }

        // Calls f(Entry&) once for every entry. The table must not change
        // meanwhile.
        template<typename F>
        void forEach(F&& f) const {
            for(const Table* t : {&t0_, &t1_}) {
                for(size_t i = 0; t->buckets && i <= t->mask; i++) {
                    for(const Bucket* b = &t->buckets[i]; b; b = childOf(b)) {
                        for(unsigned s = 0; s < kSlots; s++) {
                            if(b->meta >> s & 1) f(*static_cast<Entry*>(b->slots[s]));
                        }
                    }
                }
            }
        }

    private:
        static constexpr unsigned kSlots = 7;
        static constexpr uint8_t kChained = 0x80;       // slot 6 holds a child bucket
        static constexpr uint8_t kSlotBits = 0x7F;      // bit s: slot s holds an entry
        static constexpr size_t kFirstBuckets = 4;      // 28 slots, allocated with the first key
        static constexpr int kEmptyBucketsPerStep = 10; // as Redis: bounds a step's work

        // Zero-filled memory is a valid empty bucket: no slot used, not chained
        struct alignas(64) Bucket {
            uint8_t meta;
            uint8_t tags[kSlots];
            void* slots[kSlots];       // Entry*, or Bucket* in the last slot when chained
        };
        static_assert(sizeof(Bucket) == 64, "a bucket is one cache line");

        struct Table {
            Bucket* buckets = nullptr; // 64-byte aligned, inside `block`
            void* block = nullptr;     // what calloc returned, to free
            size_t mask = 0;           // buckets - 1 (a power of two)
        };

        Table t0_;                     // the table
        Table t1_;                     // the rehash target, while a rehash runs
        size_t rehash_index_ = 0;      // t0_'s buckets below this have moved to t1_
        size_t size_ = 0;
        size_t children_ = 0;

        static uint8_t tagOf(size_t h) { return static_cast<uint8_t>(h >> (8 * sizeof(size_t) - 8)); }
        static size_t bucketsOf(const Table& t) { return t.buckets ? t.mask + 1 : 0; }
        static Bucket* childOf(const Bucket* b) {
            return (b->meta & kChained) ? static_cast<Bucket*>(b->slots[kSlots - 1]) : nullptr;
        }

        // calloc, because its large blocks come straight from the OS as
        // zeroed pages, faulted in as they are first touched: even a table of
        // tens of megabytes costs no time to clear when it is allocated.
        // Aligned by hand, so no bucket straddles two cache lines.
        static Table allocate(size_t buckets) {
            void* block = std::calloc(1, buckets * sizeof(Bucket) + alignof(Bucket));
            if(!block) throw std::bad_alloc();
            auto addr = reinterpret_cast<uintptr_t>(block);
            addr = (addr + alignof(Bucket) - 1) & ~uintptr_t{alignof(Bucket) - 1};
            return {reinterpret_cast<Bucket*>(addr), block, buckets - 1};
        }

        void release(Table& t) {
            for(size_t i = 0; t.buckets && i <= t.mask; i++) {
                for(Bucket* c = childOf(&t.buckets[i]); c; ) {
                    Bucket* next = childOf(c);
                    delete c;
                    children_--;
                    c = next;
                }
            }
            std::free(t.block);
            t = Table{};
        }

        static Entry* findInChain(const Bucket* b, std::string_view key, uint8_t tag) {
            for(; b; b = childOf(b)) {
                for(unsigned s = 0; s < kSlots; s++) {
                    if((b->meta >> s & 1) && b->tags[s] == tag) {
                        Entry* e = static_cast<Entry*>(b->slots[s]);
                        if(KeyOf::key(*e) == key) return e;
                    }
                }
            }
            return nullptr;
        }

        // A free slot in the chain, or a new child bucket. The child is
        // allocated before anything changes, so a throw leaves the chain as it was.
        void insertInChain(Bucket* b, Entry* entry, uint8_t tag) {
            for(;;) {
                unsigned usable = (b->meta & kChained) ? kSlots - 1 : kSlots;
                for(unsigned s = 0; s < usable; s++) {
                    if(!(b->meta >> s & 1)) {
                        b->slots[s] = entry;
                        b->tags[s] = tag;
                        b->meta |= static_cast<uint8_t>(1u << s);
                        return;
                    }
                }
                if(Bucket* child = childOf(b)) {
                    b = child;
                    continue;
                }
                // Full and not chained: the last entry moves to a new child,
                // and the last slot points at it
                Bucket* child = new Bucket();
                children_++;
                child->slots[0] = b->slots[kSlots - 1];
                child->tags[0] = b->tags[kSlots - 1];
                child->meta = 1;
                b->slots[kSlots - 1] = child;
                b->meta = static_cast<uint8_t>((b->meta & ~(1u << (kSlots - 1))) | kChained);
                b = child;
            }
        }

        // An emptied child bucket is freed: its parent then points past it,
        // or, if it was the last in the chain, gets its last slot back
        bool removeFromChain(Bucket* head, const Entry* entry) {
            Bucket* parent = nullptr;
            for(Bucket* b = head; b; parent = b, b = childOf(b)) {
                for(unsigned s = 0; s < kSlots; s++) {
                    if((b->meta >> s & 1) && b->slots[s] == entry) {
                        b->meta = static_cast<uint8_t>(b->meta & ~(1u << s));
                        if(parent && (b->meta & kSlotBits) == 0) {
                            if(Bucket* next = childOf(b)) {
                                parent->slots[kSlots - 1] = next;
                            } else {
                                parent->meta = static_cast<uint8_t>(parent->meta & ~kChained);
                                parent->slots[kSlots - 1] = nullptr;
                            }
                            delete b;
                            children_--;
                        }
                        return true;
                    }
                }
            }
            return false;
        }

        // Starts a rehash once every slot is in use on average (Valkey's
        // limit too); one rehash at a time
        void growIfFull() {
            if(rehashing()) return;
            if(!t0_.buckets) {
                t0_ = allocate(kFirstBuckets);
                return;
            }
            size_t buckets = t0_.mask + 1;
            if(size_ < buckets * kSlots) return;
            t1_ = allocate(buckets * 2);
            rehash_index_ = 0;
        }

        // Moves one bucket chain to the new table (or passes up to 10 empty
        // buckets). Each entry leaves the old chain only once it is in the new
        // table, so if a child bucket can't be allocated, every entry is still
        // in exactly one table, and lookups check both.
        void rehashStep() {
            if(!rehashing()) return;
            int empty = kEmptyBucketsPerStep;
            while(rehash_index_ <= t0_.mask) {
                Bucket* head = &t0_.buckets[rehash_index_];
                bool moved_any = false;
                for(Bucket* b = head; b; b = childOf(b)) {
                    for(unsigned s = 0; s < kSlots; s++) {
                        if(!(b->meta >> s & 1)) continue;
                        Entry* e = static_cast<Entry*>(b->slots[s]);
                        size_t h = Hash{}(KeyOf::key(*e));
                        insertInChain(&t1_.buckets[h & t1_.mask], e, b->tags[s]);
                        b->meta = static_cast<uint8_t>(b->meta & ~(1u << s));
                        moved_any = true;
                    }
                }
                for(Bucket* c = childOf(head); c; ) {
                    Bucket* next = childOf(c);
                    delete c;
                    children_--;
                    c = next;
                }
                head->meta = 0;
                rehash_index_++;
                if(moved_any || --empty == 0) break;
            }
            if(rehash_index_ > t0_.mask) {
                release(t0_);
                t0_ = t1_;
                t1_ = Table{};
                rehash_index_ = 0;
            }
        }
};

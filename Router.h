#pragma once
#include "SipHash.h"
#include <cstddef>
#include <string_view>

// Which shard owns a key: its hash modulo the number of shards.
//
// The hash has its own key, separate from each shard's table. A table picks
// its bucket from the hash's low bits, so if routing used the same hash, all
// the keys of shard 3 of 4 would have the same two low bits, and each table
// would only ever use a quarter of its buckets. Under a different key the two
// hashes are unrelated. It is keyed at all for the reason the tables are
// (SipHash.h): clients choose the keys, and must not be able to aim them all
// at one shard.
//
// A key containing a non-empty {tag} is routed by the tag alone, with Redis
// Cluster's rule, so keys that must be used together can be put on one shard:
// {user7}:name and {user7}:age.
class Router {
public:
    explicit Router(size_t shards, SipKey key = SipKey::random()) : shards_(shards), key_(key) {}

    size_t shards() const { return shards_; }

    size_t shardOf(std::string_view key) const {
        if(shards_ == 1) return 0;
        const std::string_view part = routingPart(key);
        return static_cast<size_t>(sipHash<1, 3>(key_.k0, key_.k1, part.data(), part.size()) % shards_);
    }

    // The part of the key that is hashed: what is between the first { and the
    // first } after it, if that isn't empty; otherwise the whole key
    static std::string_view routingPart(std::string_view key) {
        const size_t open = key.find('{');
        if(open == std::string_view::npos) return key;
        const size_t close = key.find('}', open + 1);
        if(close == std::string_view::npos || close == open + 1) return key;
        return key.substr(open + 1, close - open - 1);
    }

private:
    size_t shards_;
    SipKey key_;
};

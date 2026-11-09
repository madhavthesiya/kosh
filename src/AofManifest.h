#pragma once
#include "AOFLogger.h"
#include "SipHash.h"

#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// With several threads, each thread logs its keys to a file of its own, so
// that no thread waits for another to write. The manifest records what those
// files mean: how many there are, and the key of the hash that decides which
// thread owns a key.
//
// Why the routing key must be kept: a key's records are all in one file only
// if the key is routed to the same thread every time. With a new random key
// after a restart, a key would move to another thread and its next records
// would go to another file. Replay reads the files one by one, so an old value
// could then overwrite a newer one.
//
// For the same reason the number of threads can't change across a restart, and
// neither can a single-threaded server's file be loaded by several threads:
// moving keys between files would need a rewrite of all of them with a new
// manifest as the commit point, which this doesn't do. Starting is refused.
//
// The manifest is written last, atomically, once every file exists. So if it
// is there, the files must be too, and a missing one means data was lost. A
// crash before that point leaves only empty files, which are safe to start from.
//
// It holds the routing key, which is as sensitive as the data files beside it.

inline std::string manifestPath(const std::string& aof) { return aof + ".manifest"; }
inline std::string shardAofPath(const std::string& aof, size_t shard) { return aof + ".shard-" + std::to_string(shard); }

constexpr size_t kMaxAofShards = 256;      // as --threads

inline std::string formatManifest(size_t shards, const SipKey& key) {
    char text[128];
    std::snprintf(text, sizeof(text), "kosh-aof 1\nshards %zu\nrouting-key %016" PRIx64 " %016" PRIx64 "\n",
                  shards, key.k0, key.k1);
    return text;
}

// Strict: anything but exactly what formatManifest() writes is refused
inline bool parseManifest(const std::string& text, size_t& shards, SipKey& key) {
    size_t parsed_shards = 0;
    uint64_t k0 = 0, k1 = 0;
    if(std::sscanf(text.c_str(), "kosh-aof 1\nshards %zu\nrouting-key %" SCNx64 " %" SCNx64 "\n",
                   &parsed_shards, &k0, &k1) != 3) {
        return false;
    }
    if(parsed_shards < 2 || parsed_shards > kMaxAofShards) return false;
    SipKey parsed;
    parsed.k0 = k0;
    parsed.k1 = k1;
    if(formatManifest(parsed_shards, parsed) != text) return false;
    shards = parsed_shards;
    key = parsed;
    return true;
}

struct AofLayout {
    std::string error;          // why the server must not start; empty if it may
    bool fresh = false;         // nothing on disk yet: write a manifest once the files exist
    SipKey routing_key;         // from the manifest, or a new one if fresh
};

// What the files on disk allow a server with this many threads to do. Reads only.
inline AofLayout checkAofLayout(const std::string& aof, size_t threads) {
    namespace fs = std::filesystem;
    AofLayout layout;
    std::error_code ec;
    const auto hasData = [&](const std::string& path) { return fs::file_size(path, ec) > 0 && !ec; };
    const bool manifest = fs::exists(manifestPath(aof), ec);

    if(threads == 1) {
        if(manifest) {
            layout.error = manifestPath(aof) + " says this data was written by several threads, so one "
                           "thread can't load it: start with the --threads it was written with";
        }
        return layout;
    }
    if(!manifest) {
        if(hasData(aof)) {
            layout.error = aof + " was written by a single thread, so several threads can't load it: "
                           "start with --threads 1, or use another --appendfilename";
            return layout;
        }
        for(size_t i = 0; i < kMaxAofShards; i++) {
            if(hasData(shardAofPath(aof, i))) {
                layout.error = shardAofPath(aof, i) + " holds data but " + manifestPath(aof) +
                               " is missing, and without it the keys can't be routed";
                return layout;
            }
        }
        layout.fresh = true;
        layout.routing_key = SipKey::random();
        return layout;
    }

    std::ifstream in(manifestPath(aof), std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    size_t shards = 0;
    if(!in || !parseManifest(text, shards, layout.routing_key)) {
        layout.error = manifestPath(aof) + " is damaged";
        return layout;
    }
    if(shards != threads) {
        layout.error = "this data was written by " + std::to_string(shards) +
                       " threads, and a key's thread depends on that number: start with --threads " +
                       std::to_string(shards);
        return layout;
    }
    for(size_t i = 0; i < threads; i++) {
        if(!fs::exists(shardAofPath(aof, i), ec)) {
            layout.error = shardAofPath(aof, i) + " is missing, so the keys of that thread are lost";
            return layout;
        }
    }
    return layout;
}

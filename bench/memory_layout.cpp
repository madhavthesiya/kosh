// Prints the size of the storage engine's types on this compiler and standard
// library, and measures how much heap one key really costs.
//
// The README quotes these numbers. They differ between libstdc++, libc++ and
// MSVC, so they should come from running this on the build being described.
//
// Usage: memory_layout [keys]    (default 1000000)

#include "../InMemoryStorage.h"
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
#include <malloc.h>
#define HAVE_MALLINFO2 1
#endif

// A friend of InMemoryStorage, which keeps CacheEntry private.
struct MemoryLayoutReport {
    static void row(const char* type, size_t bytes) {
        std::printf("| %s | %zu |\n", type, bytes);
    }

    static void printSizes() {
        using Entry = InMemoryStorage::CacheEntry;
        std::printf("| Type | sizeof (bytes) |\n|---|---:|\n");
        row("`std::string`", sizeof(std::string));
        row("`std::deque<std::string>` (List)", sizeof(std::deque<std::string>));
        row("`std::unordered_set<std::string>` (Set)", sizeof(std::unordered_set<std::string>));
        row("`std::unordered_map<std::string, std::string>` (Hash)",
            sizeof(std::unordered_map<std::string, std::string>));
        row("`ValueType` (`std::variant` of the four)", sizeof(ValueType));
        row("`CacheEntry` (value, expiry, eviction queue links, key pointer)", sizeof(Entry));
        row("Map element `std::pair<const std::string, CacheEntry>`",
            sizeof(std::pair<const std::string, Entry>));
        std::printf("\nStrings up to %zu characters are stored inside the `std::string` "
                    "object (small-string optimisation); longer ones allocate.\n",
                    std::string().capacity());
    }
};

static void printToolchain() {
#if defined(__clang__)
    std::printf("Compiler: Clang %s", __clang_version__);
#elif defined(__GNUC__)
    std::printf("Compiler: GCC %s", __VERSION__);
#elif defined(_MSC_VER)
    std::printf("Compiler: MSVC %d", _MSC_VER);
#endif
#if defined(_LIBCPP_VERSION)
    std::printf(", libc++ %d\n", _LIBCPP_VERSION);
#elif defined(__GLIBCXX__)
    std::printf(", libstdc++ %d\n", __GLIBCXX__);
#else
    std::printf("\n");
#endif
    std::printf("Pointer size: %zu bytes\n\n", sizeof(void*));
}

#ifdef HAVE_MALLINFO2
// Bytes the program has allocated and not freed. uordblks covers ordinary
// chunks (including malloc's per-chunk header); hblkhd covers large blocks
// malloc gets straight from mmap, such as a big hash table bucket array.
static size_t heapInUse() {
    struct mallinfo2 info = mallinfo2();
    return info.uordblks + info.hblkhd;
}

// Inserts keys shaped like redis-benchmark's (16-character "key:000000012345",
// 3-byte value) and reports the heap growth per key. No AOF, and the capacity
// equals the key count, so nothing is evicted.
static void measureHeapPerKey(size_t keys) {
    size_t before = heapInUse();
    InMemoryStorage storage(keys);
    char key[32];
    for(size_t i = 0; i < keys; i++) {
        std::snprintf(key, sizeof(key), "key:%012zu", i);
        storage.set(key, "xxx");
    }
    size_t used = heapInUse() - before;
    std::printf("Heap per key (glibc mallinfo2), %zu keys of 16 bytes with 3-byte values:\n", keys);
    std::printf("  total %.1f MiB, %.0f bytes per key, for 19 bytes of key and value\n",
                static_cast<double>(used) / (1024 * 1024),
                static_cast<double>(used) / static_cast<double>(keys));
}
#endif

int main(int argc, char* argv[]) {
    size_t keys = 1000000;
    if(argc > 1) {
        char* end = nullptr;
        unsigned long long n = std::strtoull(argv[1], &end, 10);
        if(*argv[1] == '\0' || *end != '\0' || n == 0) {
            std::fprintf(stderr, "Usage: %s [keys]   (keys > 0, default 1000000)\n", argv[0]);
            return 2;
        }
        keys = static_cast<size_t>(n);
    }

    printToolchain();
    MemoryLayoutReport::printSizes();
#ifdef HAVE_MALLINFO2
    std::printf("\n");
    measureHeapPerKey(keys);
#else
    (void)keys;
    std::printf("\nHeap per key: not measured (needs glibc 2.33+ for mallinfo2).\n");
#endif
    return 0;
}

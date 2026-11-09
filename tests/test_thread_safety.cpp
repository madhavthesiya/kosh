#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include "../src/InMemoryStorage.h"

std::atomic<int> errors{0};   // reset at the start of each test
int failed_tests = 0;           // across all tests — decides the exit code

// Prints one test's result. errors is reset per test, so failures must be
// counted here; otherwise only the last test would decide the exit code.
void report() {
    bool ok = (errors == 0);
    if(!ok) failed_tests++;
    std::cout << (ok ? " PASSED" : " FAILED") << std::endl;
}

void writer_thread(InMemoryStorage& storage, int id, int ops) {
    for(int i = 0; i < ops; i++)
    {
        std::string key = "key_" + std::to_string(id) + "_" + std::to_string(i);
        std::string value = "val_" + std::to_string(i);
        Result r = storage.set(key, value);
        if(!r.success)
        {
            errors++;
        }
    }
}

void reader_thread(InMemoryStorage& storage, int id, int ops) {
    for(int i = 0; i < ops; i++)
    {
        // Read a key that may or may not exist — both outcomes are valid
        std::string key = "key_" + std::to_string(id % 4) + "_" + std::to_string(i % 50);
        Result r = storage.get(key);
        // Result must be either success or KEY_NOT_FOUND — never a crash
        if(!r.success && r.error != ErrorCode::KEY_NOT_FOUND)
        {
            errors++;
        }
    }
}

void mixed_thread(InMemoryStorage& storage, int /*id*/, int ops) {
    for(int i = 0; i < ops; i++)
    {
        std::string key = "shared_" + std::to_string(i % 20);
        if(i % 3 == 0)
        {
            storage.set(key, "v" + std::to_string(i));
        }
        else if(i % 3 == 1)
        {
            storage.get(key);
        }
        else
        {
            storage.del(key);
        }
    }
}

void ttl_thread(InMemoryStorage& storage, int id, int ops) {
    for(int i = 0; i < ops; i++)
    {
        std::string key = "ttl_" + std::to_string(id) + "_" + std::to_string(i % 10);
        if(i % 2 == 0)
        {
            storage.set(key, "temp", 2);    // 2 second TTL
        }
        else
        {
            storage.get(key);               // may or may not be expired
        }
    }
}

int main() {
    std::cout << "\n========== Thread Safety Stress Test ==========\n\n";

    const int NUM_THREADS = 8;
    const int OPS_PER_THREAD = 500;

    // Test 1: Concurrent writers
    {
        std::cout << "  Test 1: " << NUM_THREADS << " concurrent writers (" << NUM_THREADS * OPS_PER_THREAD << " total ops)...";
        InMemoryStorage storage(200);
        std::vector<std::thread> threads;
        errors = 0;

        for(int i = 0; i < NUM_THREADS; i++)
        {
            threads.emplace_back(writer_thread, std::ref(storage), i, OPS_PER_THREAD);
        }
        for(auto& t : threads) t.join();

        report();
    }

    // Test 2: Concurrent readers + writers
    {
        std::cout << "  Test 2: " << NUM_THREADS << " mixed reader/writer threads...";
        InMemoryStorage storage(100);
        // Pre-populate some data
        for(int i = 0; i < 50; i++)
        {
            storage.set("key_0_" + std::to_string(i), "init");
        }

        std::vector<std::thread> threads;
        errors = 0;

        for(int i = 0; i < NUM_THREADS / 2; i++)
        {
            threads.emplace_back(writer_thread, std::ref(storage), i, OPS_PER_THREAD);
        }
        for(int i = 0; i < NUM_THREADS / 2; i++)
        {
            threads.emplace_back(reader_thread, std::ref(storage), i, OPS_PER_THREAD);
        }
        for(auto& t : threads) t.join();

        report();
    }

    // Test 3: Mixed operations (SET + GET + DEL)
    {
        std::cout << "  Test 3: " << NUM_THREADS << " threads doing mixed SET/GET/DEL...";
        InMemoryStorage storage(50);
        std::vector<std::thread> threads;
        errors = 0;

        for(int i = 0; i < NUM_THREADS; i++)
        {
            threads.emplace_back(mixed_thread, std::ref(storage), i, OPS_PER_THREAD);
        }
        for(auto& t : threads) t.join();

        report();
    }

    // Test 4: TTL operations under contention
    {
        std::cout << "  Test 4: " << NUM_THREADS << " threads with TTL operations...";
        InMemoryStorage storage(100);
        std::vector<std::thread> threads;
        errors = 0;

        for(int i = 0; i < NUM_THREADS; i++)
        {
            threads.emplace_back(ttl_thread, std::ref(storage), i, OPS_PER_THREAD);
        }
        for(auto& t : threads) t.join();

        report();
    }

    // Test 5: High contention on same keys
    {
        std::cout << "  Test 5: " << NUM_THREADS << " threads fighting over same keys...";
        InMemoryStorage storage(10);    // small capacity = more eviction pressure
        std::vector<std::thread> threads;
        errors = 0;

        for(int i = 0; i < NUM_THREADS; i++)
        {
            threads.emplace_back(mixed_thread, std::ref(storage), i, OPS_PER_THREAD);
        }
        for(auto& t : threads) t.join();

        report();
    }

    int total_ops = NUM_THREADS * OPS_PER_THREAD * 5;
    std::cout << "\n===============================================\n";
    std::cout << "Total operations: " << total_ops << " (5 tests x " << NUM_THREADS << " concurrent threads)\n";
    std::cout << "Failed tests: " << failed_tests << "\n";
    std::cout << "Result: " << (failed_tests == 0 ? "ALL PASSED" : "SOME FAILED") << "\n";
    std::cout << "===============================================\n\n";

    return failed_tests > 0 ? 1 : 0;
}

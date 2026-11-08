cmake_minimum_required(VERSION 3.16)
project(kosh LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Without a build type, CMake passes no optimisation flag at all (-O0), so every
# benchmark would measure an unoptimised binary. Default to an optimised build
# that keeps debug symbols for perf and gdb (-O2 -g on GCC/Clang).
# Multi-config generators (Visual Studio, Xcode) choose the config at build time instead.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Sanitizers (GCC/Clang only), e.g.:
#   cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=address,undefined
#   cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=thread
# -fno-sanitize-recover makes UBSan abort on the first error instead of printing
# a warning and carrying on, so a test that triggers undefined behaviour fails.
set(SANITIZE "" CACHE STRING "Comma-separated sanitizers to enable (GCC/Clang only)")
if(SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "SANITIZE is only supported with GCC or Clang")
    endif()
    add_compile_options(-fsanitize=${SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${SANITIZE})
    # GCC warns that TSan doesn't model atomic_thread_fence. Wakeup.h's fences
    # exist to prevent lost wakeups, which TSan can't see in any case; every
    # ordering of the data itself comes from acquire and release, which it does
    # model. tests/test_wakeup.cpp tests the fences.
    if(SANITIZE MATCHES "thread" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-Wno-tsan)
    endif()
endif()

find_package(Threads REQUIRED)

# Main executable (CLI mode)
add_executable(kosh-cli
    main.cpp
    Commands.cpp
    InMemoryStorage.cpp
    AOFLogger.cpp
    RedisLite.cpp
)
target_link_libraries(kosh-cli Threads::Threads)

# Server executable (TCP + RESP mode)
add_executable(kosh-server
    server_main.cpp
    Server.cpp
    EventLoop.cpp
    Poller.cpp
    Commands.cpp
    InMemoryStorage.cpp
    AOFLogger.cpp
    RedisLite.cpp
)
target_link_libraries(kosh-server Threads::Threads)
if(WIN32)
    target_link_libraries(kosh-server ws2_32)
endif()

# Unit tests
add_executable(tests
    tests/test_main.cpp
    Poller.cpp
    Commands.cpp
    InMemoryStorage.cpp
    AOFLogger.cpp
    RedisLite.cpp
)
target_link_libraries(tests Threads::Threads)
if(WIN32)
    target_link_libraries(tests ws2_32)
endif()

# Thread safety stress test
add_executable(test_threads
    tests/test_thread_safety.cpp
    InMemoryStorage.cpp
    AOFLogger.cpp
    RedisLite.cpp
)
target_link_libraries(test_threads Threads::Threads)

# The queue between threads: exact behaviour, then two threads at full speed
add_executable(test_spsc_ring tests/test_spsc_ring.cpp)
target_link_libraries(test_spsc_ring Threads::Threads)

# Which thread owns a key, and where the command table says the keys are
add_executable(test_router tests/test_router.cpp Commands.cpp InMemoryStorage.cpp AOFLogger.cpp)
target_link_libraries(test_router Threads::Threads)

# Prints sizeof() of the storage types and the heap cost of one key (see bench/README.md)
add_executable(memory_layout
    bench/memory_layout.cpp
    InMemoryStorage.cpp
    AOFLogger.cpp
)
target_link_libraries(memory_layout Threads::Threads)

# Times every insert of a new key, to find the storage engine's worst case (see bench/README.md)
add_executable(insert_latency
    bench/insert_latency.cpp
    InMemoryStorage.cpp
    AOFLogger.cpp
)
target_link_libraries(insert_latency Threads::Threads)

# Times lookups in the storage engine as the table outgrows the caches (see bench/README.md)
add_executable(lookup_latency
    bench/lookup_latency.cpp
    InMemoryStorage.cpp
    AOFLogger.cpp
)
target_link_libraries(lookup_latency Threads::Threads)

# Miss ratio of the engine's eviction on cache traces (see bench/README.md)
add_executable(eviction_sim
    bench/eviction_sim.cpp
    InMemoryStorage.cpp
    AOFLogger.cpp
)
target_link_libraries(eviction_sim Threads::Threads)

# Counts heap allocations per request, layer by layer (see bench/README.md)
add_executable(alloc_count
    bench/alloc_count.cpp
    Commands.cpp
    InMemoryStorage.cpp
    AOFLogger.cpp
)
target_link_libraries(alloc_count Threads::Threads)

# Hash flooding against std::hash and KeyedHash, and SipHash's cost (see bench/README.md)
add_executable(hash_flood bench/hash_flood.cpp)

# Times a password check by where the guess goes wrong (see bench/README.md)
add_executable(auth_compare
    bench/auth_compare.cpp
    Commands.cpp
    InMemoryStorage.cpp
    AOFLogger.cpp
)
target_link_libraries(auth_compare Threads::Threads)

# Open-loop load generator for any Redis-compatible server (see bench/README.md).
# Linux only: it is built on epoll.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(kosh-loadgen bench/loadgen.cpp)
    target_link_libraries(kosh-loadgen Threads::Threads)
    add_executable(test_loadgen tests/test_loadgen.cpp)
    # No wakeup between threads may be lost (eventfd, so Linux only)
    add_executable(test_wakeup tests/test_wakeup.cpp)
    target_link_libraries(test_wakeup Threads::Threads)
    # What a cross-thread queue costs, and what each part of SpscRing buys (see bench/README.md)
    add_executable(ring_bench bench/ring_bench.cpp)
    target_link_libraries(ring_bench Threads::Threads)
endif()

# Fuzz target: client bytes through the parser and the command table
# (tests/fuzz). With GCC it gets a standalone driver that replays the corpus
# (a ctest) or mutates it at random; with clang and -DKOSH_LIBFUZZER=ON it
# is coverage-guided libFuzzer instead.
option(KOSH_LIBFUZZER "Build fuzz_commands with libFuzzer (clang only)" OFF)
if(KOSH_LIBFUZZER)
    add_executable(fuzz_commands tests/fuzz/fuzz_commands.cpp
        Commands.cpp InMemoryStorage.cpp AOFLogger.cpp)
    target_compile_options(fuzz_commands PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_commands PRIVATE -fsanitize=fuzzer)
else()
    add_executable(fuzz_commands tests/fuzz/fuzz_commands.cpp tests/fuzz/standalone_main.cpp
        Commands.cpp InMemoryStorage.cpp AOFLogger.cpp)
endif()
target_link_libraries(fuzz_commands Threads::Threads)

enable_testing()
add_test(NAME unit_tests COMMAND tests)
add_test(NAME thread_safety_tests COMMAND test_threads)
add_test(NAME spsc_ring_tests COMMAND test_spsc_ring)
add_test(NAME router_tests COMMAND test_router)
if(TARGET test_loadgen)
    add_test(NAME loadgen_unit_tests COMMAND test_loadgen)
    add_test(NAME wakeup_tests COMMAND test_wakeup)
endif()
if(NOT KOSH_LIBFUZZER)
    add_test(NAME fuzz_corpus COMMAND fuzz_commands ${CMAKE_SOURCE_DIR}/tests/fuzz/corpus)
endif()

# Integration tests start the real server binary and talk to it over TCP.
# They need Python 3 (standard library only); skipped if it isn't installed.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME integration_tests
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_server.py
                     $<TARGET_FILE:kosh-server>)
    # Linux serves with epoll by default; run everything with select() as well,
    # so both pollers stay tested
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME integration_tests_select
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_server.py
                         $<TARGET_FILE:kosh-server>)
        set_tests_properties(integration_tests_select PROPERTIES ENVIRONMENT "KOSH_POLLER=select")
        add_test(NAME shard_aof_tests
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_shard_aof.py
                         $<TARGET_FILE:kosh-server>)
        add_test(NAME threads_integration_tests
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_threads.py
                         $<TARGET_FILE:kosh-server>)
        add_test(NAME loadgen_integration_tests
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_loadgen.py
                         $<TARGET_FILE:kosh-loadgen> $<TARGET_FILE:kosh-server>)
    endif()
    # redis-py and node-redis against the server. Each is skipped, saying so,
    # if it isn't installed; KOSH_REQUIRE_CLIENTS=1 makes that a failure (CI).
    add_test(NAME client_library_tests
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_clients.py
                     $<TARGET_FILE:kosh-server>)
    # The same commands to real Redis and to Kosh, replies compared byte for
    # byte; registered only where redis-server is installed
    find_program(REDIS_SERVER redis-server)
    if(REDIS_SERVER)
        add_test(NAME redis_compatibility
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/compat/compare_with_redis.py
                         --kosh $<TARGET_FILE:kosh-server> --redis ${REDIS_SERVER})
    endif()
else()
    message(STATUS "Python 3 not found: integration tests will not be registered")
endif()

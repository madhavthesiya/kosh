cmake_minimum_required(VERSION 3.16)
project(mini-redis LANGUAGES CXX)
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
endif()

find_package(Threads REQUIRED)

# Main executable (CLI mode)
add_executable(mini_redis
    main.cpp
    Commands.cpp
    InMemoryStorage.cpp
    AOFLogger.cpp
    RedisLite.cpp
)
target_link_libraries(mini_redis Threads::Threads)

# Server executable (TCP + RESP mode)
add_executable(mini_redis_server
    server_main.cpp
    Server.cpp
    Poller.cpp
    Commands.cpp
    InMemoryStorage.cpp
    AOFLogger.cpp
    RedisLite.cpp
)
target_link_libraries(mini_redis_server Threads::Threads)
if(WIN32)
    target_link_libraries(mini_redis_server ws2_32)
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

# Counts heap allocations per request, layer by layer (see bench/README.md)
add_executable(alloc_count
    bench/alloc_count.cpp
    Commands.cpp
    InMemoryStorage.cpp
    AOFLogger.cpp
)
target_link_libraries(alloc_count Threads::Threads)

enable_testing()
add_test(NAME unit_tests COMMAND tests)
add_test(NAME thread_safety_tests COMMAND test_threads)

# Integration tests start the real server binary and talk to it over TCP.
# They need Python 3 (standard library only); skipped if it isn't installed.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME integration_tests
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_server.py
                     $<TARGET_FILE:mini_redis_server>)
    # Linux serves with epoll by default; run everything with select() as well,
    # so both pollers stay tested
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME integration_tests_select
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_server.py
                         $<TARGET_FILE:mini_redis_server>)
        set_tests_properties(integration_tests_select PROPERTIES ENVIRONMENT "MINI_REDIS_POLLER=select")
    endif()
else()
    message(STATUS "Python 3 not found: integration tests will not be registered")
endif()

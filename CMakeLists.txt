cmake_minimum_required(VERSION 3.21)
project(RingBuffer LANGUAGES CXX)

set(Simulate_Android_ToolChain OFF)
if (Simulate_Android_ToolChain)
    set(CMAKE_CXX_STANDARD 14)
    set(CMAKE_CXX_FLAGS  "-fno-exceptions -Wno-unused-parameter -Werror")
else()
    set(CMAKE_CXX_STANDARD 17)
endif()

add_library(RingBuffer INTERFACE)
add_library(RingBuffer::RingBuffer ALIAS RingBuffer)
target_include_directories(RingBuffer INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

option(RINGBUFFER_BUILD_TESTS "Build RingBuffer tests" ON)
if (RINGBUFFER_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
    )
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(RingBufferTest
        tests/core_lifecycle_tests.cpp
        tests/iterator_tests.cpp
        tests/rule_of_five_tests.cpp
        tests/swap_exception_test.cpp
    )
    target_include_directories(RingBufferTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(RingBufferTest PRIVATE gtest gtest_main RingBuffer::RingBuffer)

    include(GoogleTest)
    gtest_discover_tests(RingBufferTest)

    find_package(Threads REQUIRED)

    add_executable(SpscRingBufferTest
        tests/spsc_tests.cpp
    )
    target_include_directories(SpscRingBufferTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(SpscRingBufferTest PRIVATE gtest gtest_main RingBuffer::RingBuffer Threads::Threads)

    gtest_discover_tests(SpscRingBufferTest)

    # Shared memory cross-process tests
    add_executable(SpscShmTest
        tests/spsc_shm_tests.cpp
    )
    target_include_directories(SpscShmTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(SpscShmTest PRIVATE gtest gtest_main RingBuffer::RingBuffer Threads::Threads rt)

    gtest_discover_tests(SpscShmTest)

    # Race condition tests
    add_executable(SpscRaceTest
        tests/race_condition_test.cpp
    )
    target_include_directories(SpscRaceTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(SpscRaceTest PRIVATE gtest gtest_main RingBuffer::RingBuffer Threads::Threads rt)

    gtest_discover_tests(SpscRaceTest)

    # Simple race condition test
    add_executable(SpscRaceSimpleTest
        tests/race_simple_test.cpp
    )
    target_include_directories(SpscRaceSimpleTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(SpscRaceSimpleTest PRIVATE gtest gtest_main RingBuffer::RingBuffer Threads::Threads rt)

    gtest_discover_tests(SpscRaceSimpleTest)
endif()

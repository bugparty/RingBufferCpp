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
      URL https://github.com/google/googletest/archive/refs/tags/release-1.11.0.zip
    )
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(RingBufferTest test_main.cpp)
    target_link_libraries(RingBufferTest gtest gtest_main RingBuffer::RingBuffer)

    include(GoogleTest)
    gtest_discover_tests(RingBufferTest)
endif()

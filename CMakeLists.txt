cmake_minimum_required(VERSION 3.21)
project(RingBufferTest)

set(Simulate_Android_ToolChain OFF)
if (Simulate_Android_ToolChain)
    set(CMAKE_CXX_STANDARD 14)
    set(CMAKE_CXX_FLAGS  "-fno-exceptions -Wno-unused-parameter -Werror")
else()
    set(CMAKE_CXX_STANDARD 17)
endif()

# Add Google Test
# Include FetchContent to download and manage external dependencies
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/release-1.11.0.zip
)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(RingBufferTest test_main.cpp)
target_link_libraries(RingBufferTest gtest gtest_main)


include(GoogleTest)
gtest_discover_tests(RingBufferTest)

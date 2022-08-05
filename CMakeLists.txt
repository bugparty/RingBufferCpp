cmake_minimum_required(VERSION 3.21)
project(RingBufferTest)

set(Simulate_Android_ToolChain OFF)
if (Simulate_Android_ToolChain)
    set(CMAKE_CXX_STANDARD 14)
    set(CMAKE_CXX_FLAGS  "-fno-exceptions -Wno-unused-parameter -Werror")
else()
    set(CMAKE_CXX_STANDARD 17)
endif()


add_executable(RingBufferTest main.cpp)

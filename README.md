# RingBuffer

[![Build](https://github.com/bugparty/RingBufferCpp/actions/workflows/cmake.yml/badge.svg?branch=main)](https://github.com/bugparty/RingBufferCpp/actions/workflows/cmake.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](https://en.cppreference.com/w/cpp/17)
[![Header-Only](https://img.shields.io/badge/library-header--only-green.svg)](#)

A modern, header-only C++ implementation of a fixed-size circular buffer (ring buffer), designed for performance, safety, and ease of testing with Google Test.

## Features

- 🌀 Templated for any type `T`
- 🔄 Optional overwrite mode when full
- ⚡ Constant time `push_back`, `pop_front`, `front`, `back`, `size`, and `capacity` operations
- 📚 STL-style interface with iterators
- 🛡️ Fully exception-safe and compliant with C++17
- 🔗 Header-only for easy integration
- ✅ Unit tested with Google Test

---

## 🔍 Modern C++ Highlights

This project demonstrates many modern C++ best practices:

| Feature                            | Description |
|-----------------------------------|-------------|
| ✅ **Perfect Forwarding**         | Efficiently constructs elements in-place using `std::forward` |
| ✅ **`constexpr if` Branching**    | Compile-time control flow based on type traits |
| ✅ **Conditional `noexcept`**      | Automatically propagates `noexcept` based on `value_type` properties |
| ✅ **Exception Safety**            | Strong guarantees using RAII and manual object lifetime control |
| ✅ **Overwrite Mode**              | Enable circular overwriting when buffer is full |
| ✅ **STL-Compatible Interface**    | Supports `begin()`, `end()`, `size()`, `empty()`, etc. |
| ✅ **Unit Tested**                 | With Google Test and `FetchContent` integration via CMake |

---

## Getting Started

### Prerequisites

- C++17 compatible compiler
- CMake 3.21 or newer
- Git (for GoogleTest fetch)

### Build Instructions

```bash
git clone https://github.com/bugparty/RingBufferCpp.git
cd RingBufferCpp
mkdir build && cd build
cmake ..
cmake --build .
ctest --output-on-failure
```
This will:
	1.	Fetch and build GoogleTest using FetchContent
	2.	Build the RingBufferTest executable from test_main.cpp
	3.	Run all unit tests

---

## FetchContent Integration

```cmake
include(FetchContent)
FetchContent_Declare(
  RingBuffer
  GIT_REPOSITORY https://github.com/bugparty/RingBufferCpp.git
  GIT_TAG main
)
FetchContent_MakeAvailable(RingBuffer)

target_link_libraries(your_target PRIVATE RingBuffer::RingBuffer)
```

To skip building tests in your parent project, set:

```cmake
set(RINGBUFFER_BUILD_TESTS OFF)
```

---

## Testing Policy

When changing the library, run the tests before pushing changes:

```bash
cmake --build build
ctest --output-on-failure --test-dir build
```

---

Usage

```cpp
#include "RingBuffer.hpp"

ring_buffer<int, 8> buf;

buf.push_back(42);
buf.push_back(1337);

std::cout << buf.front() << ", " << buf.back() << std::endl;

buf.pop_front();
```

With overwrite mode:

```cpp
ring_buffer<int, 3, true> overwrite_buf;
overwrite_buf.push_back(1);
overwrite_buf.push_back(2);
overwrite_buf.push_back(3);
overwrite_buf.push_back(4); // Overwrites 1
```


---

Project Structure
```bash
RingBuffer/
├── CMakeLists.txt       # CMake build config with GoogleTest support
├── RingBuffer.hpp       # Header-only ring buffer implementation
├── test_main.cpp        # Unit tests using GoogleTest
├── LICENSE              # MIT License
└── README.md            # This file
```


---

License

This project is licensed under the MIT License - see the LICENSE file for details.


# Copilot Instructions for RingBufferCpp

## Project Overview

This is a modern, header-only C++17 ring buffer (circular buffer) implementation designed for performance, safety, and ease of use. The project demonstrates C++17 best practices and is tested with Google Test.

## Architecture

- **Header-only library**: All implementation is in `RingBuffer.hpp`
- **Template-based**: Generic implementation using `template<typename T, size_t N, bool Overwrite>`
- **Namespace**: All code is in the `buffers` namespace
- **Iterator support**: Custom forward iterators in `buffers::detail` namespace

## Code Standards

### C++ Version and Style

- Use **C++17** features and idioms
- Follow modern C++ best practices:
  - Perfect forwarding with `std::forward`
  - `constexpr if` for compile-time branching
  - Conditional `noexcept` specifications
  - RAII and strong exception safety guarantees
  - `[[nodiscard]]` attributes where appropriate

### Naming Conventions

- **Classes/Types**: `snake_case` (e.g., `ring_buffer`, `ring_buffer_iterator`)
- **Member variables**: `snake_case_` with trailing underscore (e.g., `source_`, `index_`, `count_`)
- **Functions/Methods**: `snake_case` (e.g., `push_back`, `pop_front`, `cbegin`)
- **Template parameters**: PascalCase (e.g., `T`, `N`, `Overwrite`, `C`)

### Code Organization

- Place implementation details in `buffers::detail` namespace
- Use forward declarations before implementation
- Include necessary headers at the top (`<iostream>`, `<type_traits>`, `<algorithm>`, `<cstring>`, `<vector>`)
- Use `#pragma once` for header guards

## Building and Testing

### Build Commands

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

### Test Commands

From within the `build` directory:

```bash
ctest --output-on-failure
```

### Test Framework

- Use **Google Test** framework
- Test file: `test_main.cpp`
- GoogleTest is fetched automatically via CMake's `FetchContent`
- Tests use the pattern: `TEST(RingBufferTest, TestName)`

## Testing Guidelines

When adding or modifying functionality:

1. **Write tests first** or alongside implementation
2. **Test naming**: Use descriptive names like `Test1`, `Test2`, or `Test6IteratorOrder`
3. **Use Google Test macros**: `EXPECT_EQ`, `EXPECT_TRUE`, `EXPECT_FALSE`, etc.
4. **Test edge cases**:
   - Empty buffer
   - Full buffer
   - Overwrite behavior
   - Iterator operations
   - Copy/move semantics
5. **Verify both const and non-const operations** where applicable

## Key Features to Maintain

- **Overwrite mode**: When buffer is full, new elements can overwrite oldest
- **STL-compatible interface**: `begin()`, `end()`, `cbegin()`, `cend()`, `size()`, `empty()`
- **Constant time operations**: All core operations should be O(1)
- **Exception safety**: Maintain strong exception guarantees
- **Type safety**: Use SFINAE and type traits appropriately

## Common Patterns in This Codebase

- Use `typename std::enable_if<condition, int>::type* = nullptr` for SFINAE
- Iterator comparisons based on `count()` not `index()`
- Modulo arithmetic for circular indexing: `index_ = (index_ + 1) % N`
- Template specialization for const/non-const iterators

## What NOT to Do

- Don't add dependencies beyond standard library (except GoogleTest for tests)
- Don't break header-only design
- Don't remove or weaken exception safety guarantees
- Don't change API to be incompatible with STL conventions
- Don't add features that would compromise O(1) operation complexity
- Avoid dynamic allocation; maintain fixed-size design

## Special Considerations

- The buffer size `N` is a compile-time constant (template parameter)
- Support both trivial and non-trivial types for `T`
- Handle move-only types appropriately
- Maintain compatibility with C++17 (the project has a `Simulate_Android_ToolChain` CMake option for testing C++14 compatibility without exceptions, but default is C++17)

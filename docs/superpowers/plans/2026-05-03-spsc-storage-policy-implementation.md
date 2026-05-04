# SPSC Storage Policy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Storage Policy support and overflow_count to spsc_ring_buffer for pluggable storage backends (heap/shared memory) and cross-process communication.

**Architecture:** Policy-Based Design with StoragePolicy template parameter. Header+Slots structure stored in shared memory for cross-process access. Two write modes: reject (try_push) and overwrite (push_overwrite).

**Tech Stack:** C++17, std::atomic, POSIX shared memory (shm_open/mmap), GoogleTest

---

## File Structure

**Create:**
- No new files (all changes in existing files)

**Modify:**
- `spsc_ring_buffer.hpp` - Add Storage Policy, header/slots structure, overwrite mode
- `tests/spsc_tests.cpp` - Add overwrite mode tests
- `tests/spsc_shm_tests.cpp` - Add shared memory cross-process tests (new file)

---

## Task 1: Define RingBufferHeader and RingBufferRegion structures

**Files:**
- Modify: `spsc_ring_buffer.hpp:1-20`

- [ ] **Step 1: Add header structure definition**

Add after namespace declaration, before spsc_ring_buffer class:

```cpp
namespace buffers {

// Ring buffer header stored in shared memory
template<typename T>
struct RingBufferHeader {
    uint32_t version{1};
    uint32_t capacity{0};
    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint64_t> tail{0};
    alignas(64) std::atomic<uint64_t> overflow_count{0};
    char padding_[64 - sizeof(std::atomic<uint64_t>)];
};

// Ring buffer region: header + slots
template<typename T, size_t N>
struct RingBufferRegion {
    RingBufferHeader<T> header;
    typename std::aligned_storage<sizeof(T), alignof(T)>::type slots[N];
};

} // namespace buffers
```

- [ ] **Step 2: Verify compilation**

Run: `cd /home/bowmanhan/Code/RingBufferCpp && g++ -std=c++17 -c spsc_ring_buffer.hpp -o /dev/null`
Expected: No errors

- [ ] **Step 3: Commit**

```bash
git add spsc_ring_buffer.hpp
git commit -m "feat: add RingBufferHeader and RingBufferRegion structures"
```

---

## Task 2: Implement HeapStorage policy

**Files:**
- Modify: `spsc_ring_buffer.hpp:25-50` (after RingBufferRegion)

- [ ] **Step 1: Write HeapStorage class**

Add after RingBufferRegion definition:

```cpp
// Heap storage policy (default)
template<typename T, size_t N>
class HeapStorage {
public:
    using header_type = RingBufferHeader<T>;
    using element_type = typename std::aligned_storage<sizeof(T), alignof(T)>::type;
    using region_type = RingBufferRegion<T, N>;

private:
    region_type* region_ = nullptr;

public:
    HeapStorage() {
        region_ = new region_type;
        std::memset(region_, 0, sizeof(region_type));
        region_->header.version = 1;
        region_->header.capacity = N;
    }

    ~HeapStorage() {
        delete region_;
    }

    HeapStorage(HeapStorage const&) = delete;
    HeapStorage& operator=(HeapStorage const&) = delete;
    HeapStorage(HeapStorage&&) = delete;
    HeapStorage& operator=(HeapStorage&&) = delete;

    header_type* header() noexcept { return &region_->header; }
    element_type* slots() noexcept { return region_->slots; }
    bool valid() const noexcept { return region_ != nullptr; }
    bool is_creator() const noexcept { return true; }
};
```

- [ ] **Step 2: Verify compilation**

Run: `cd /home/bowmanhan/Code/RingBufferCpp && g++ -std=c++17 -c spsc_ring_buffer.hpp -o /dev/null`
Expected: No errors

- [ ] **Step 3: Commit**

```bash
git add spsc_ring_buffer.hpp
git commit -m "feat: add HeapStorage policy"
```

---

## Task 3: Implement ShmStorage policy

**Files:**
- Modify: `spsc_ring_buffer.hpp:75-150` (after HeapStorage)

- [ ] **Step 1: Add ShmOpenMode enum**

Add before ShmStorage class:

```cpp
// Shared memory open mode
enum class ShmOpenMode {
    create,           // Create new, fail if exists
    open,             // Open existing, fail if not found
    create_or_open    // Create if not exists, open if exists (default)
};
```

- [ ] **Step 2: Write ShmStorage class declaration**

Add after enum:

```cpp
// Shared memory storage policy
template<typename T, size_t N>
class ShmStorage {
public:
    using header_type = RingBufferHeader<T>;
    using element_type = typename std::aligned_storage<sizeof(T), alignof(T)>::type;
    using region_type = RingBufferRegion<T, N>;

private:
    std::string shm_name_;
    ShmOpenMode mode_;
    int fd_ = -1;
    region_type* region_ = nullptr;
    bool is_creator_ = false;

public:
    explicit ShmStorage(const char* name,
                       ShmOpenMode mode = ShmOpenMode::create_or_open);
    ~ShmStorage();

    ShmStorage(ShmStorage const&) = delete;
    ShmStorage& operator=(ShmStorage const&) = delete;
    ShmStorage(ShmStorage&&) = delete;
    ShmStorage& operator=(ShmStorage&&) = delete;

    header_type* header() noexcept { return region_ ? &region_->header : nullptr; }
    element_type* slots() noexcept { return region_ ? region_->slots : nullptr; }
    bool valid() const noexcept { return region_ != nullptr; }
    bool is_creator() const noexcept { return is_creator_; }
};
```

- [ ] **Step 3: Implement ShmStorage constructor**

Add after class declaration (outside namespace or at end of header):

```cpp
template<typename T, size_t N>
ShmStorage<T, N>::ShmStorage(const char* name, ShmOpenMode mode)
    : shm_name_(name), mode_(mode) {
    
    int flags = O_RDWR;
    switch (mode_) {
        case ShmOpenMode::create:
            flags |= O_CREAT | O_EXCL;
            break;
        case ShmOpenMode::open:
            // Only O_RDWR
            break;
        case ShmOpenMode::create_or_open:
            flags |= O_CREAT;
            break;
    }

    // Open shared memory
    fd_ = shm_open(shm_name_.c_str(), flags, 0666);
    if (fd_ < 0) {
        return;
    }

    // Check if newly created
    struct stat st;
    if (fstat(fd_, &st) == 0) {
        is_creator_ = (st.st_size == 0);
    }

    // Creator calls ftruncate
    if (is_creator_) {
        const size_t size = sizeof(region_type);
        if (ftruncate(fd_, static_cast<off_t>(size)) != 0) {
            close(fd_);
            fd_ = -1;
            return;
        }
    }

    // Map to memory
    void* mapped = mmap(nullptr, sizeof(region_type),
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapped == MAP_FAILED) {
        close(fd_);
        fd_ = -1;
        return;
    }

    region_ = static_cast<region_type*>(mapped);

    // Creator initializes header
    if (is_creator_) {
        std::memset(region_, 0, sizeof(region_type));
        region_->header.version = 1;
        region_->header.capacity = N;
    }
}

template<typename T, size_t N>
ShmStorage<T, N>::~ShmStorage() {
    if (region_) {
        munmap(region_, sizeof(region_type));
        region_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}
```

- [ ] **Step 4: Add necessary includes at top of file**

Add after `#ifndef SPSC_RING_BUFFER_HPP`:

```cpp
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
```

- [ ] **Step 5: Verify compilation**

Run: `cd /home/bowmanhan/Code/RingBufferCpp && g++ -std=c++17 -c spsc_ring_buffer.hpp -o /dev/null`
Expected: No errors

- [ ] **Step 6: Commit**

```bash
git add spsc_ring_buffer.hpp
git commit -m "feat: add ShmStorage policy with create/open modes"
```

---

## Task 4: Refactor spsc_ring_buffer to use StoragePolicy

**Files:**
- Modify: `spsc_ring_buffer.hpp:18-124` (entire class)

- [ ] **Step 1: Update class template declaration**

Replace existing class declaration:

```cpp
template<
    typename T,
    size_t N,
    template<typename, size_t> class StoragePolicy = HeapStorage
>
class spsc_ring_buffer {
    static_assert(N > 0, "SPSC ring buffer size must be greater than zero.");

    using self_type = spsc_ring_buffer;
    using storage_policy = StoragePolicy<T, N>;

public:
    using value_type = T;
    using reference = T&;
    using const_reference = T const&;
    using pointer = T*;
    using const_pointer = T const*;
    using size_type = size_t;
```

- [ ] **Step 2: Update constructors**

Replace existing constructors:

```cpp
    // Default constructor (for HeapStorage)
    template<typename S = StoragePolicy<T, N>,
             typename = std::enable_if_t<std::is_default_constructible_v<S>>>
    spsc_ring_buffer() : storage_() {}

    // Constructor with arguments (for ShmStorage)
    template<typename... Args>
    explicit spsc_ring_buffer(Args&&... args)
        : storage_(std::forward<Args>(args)...) {}

    spsc_ring_buffer(spsc_ring_buffer const&) = delete;
    spsc_ring_buffer& operator=(spsc_ring_buffer const&) = delete;
    spsc_ring_buffer(spsc_ring_buffer&&) = delete;
    spsc_ring_buffer& operator=(spsc_ring_buffer&&) = delete;

    ~spsc_ring_buffer() { clear(); }
```

- [ ] **Step 3: Update try_push method**

Replace existing try_push:

```cpp
    // Try to enqueue an element. Returns false if the buffer is full.
    template<typename U>
    bool try_push(U&& value) noexcept(std::is_nothrow_constructible<T, U&&>::value) {
        auto* header = storage_.header();
        auto* slots = storage_.slots();

        auto h = header->head.load(std::memory_order_relaxed);
        auto t = header->tail.load(std::memory_order_acquire);

        if (h - t >= N) {
            header->overflow_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        new(&slots[h % N]) T(std::forward<U>(value));
        header->head.store(h + 1, std::memory_order_release);
        return true;
    }
```

- [ ] **Step 4: Add push_overwrite method**

Add after try_push:

```cpp
    // Overwrite mode: always succeeds, overwrites oldest if full
    template<typename U>
    void push_overwrite(U&& value) noexcept(std::is_nothrow_constructible<T, U&&>::value) {
        auto* header = storage_.header();
        auto* slots = storage_.slots();

        auto h = header->head.load(std::memory_order_relaxed);
        auto t = header->tail.load(std::memory_order_acquire);

        if (h - t >= N) {
            // Buffer full, overwrite oldest
            header->overflow_count.fetch_add(1, std::memory_order_relaxed);

            if constexpr (!std::is_trivially_destructible_v<T>) {
                reinterpret_cast<T*>(&slots[h % N])->~T();
            }

            new(&slots[h % N]) T(std::forward<U>(value));
            header->head.store(h + 1, std::memory_order_release);
            header->tail.store(t + 1, std::memory_order_release);
        } else {
            // Buffer not full, normal push
            new(&slots[h % N]) T(std::forward<U>(value));
            header->head.store(h + 1, std::memory_order_release);
        }
    }
```

- [ ] **Step 5: Update try_pop method**

Replace existing try_pop:

```cpp
    // Try to dequeue an element. Returns false if the buffer is empty.
    bool try_pop(T& result) noexcept(std::is_nothrow_move_assignable<T>::value) {
        auto* header = storage_.header();
        auto* slots = storage_.slots();

        auto t = header->tail.load(std::memory_order_relaxed);
        auto h = header->head.load(std::memory_order_acquire);

        if (t == h)
            return false;

        auto idx = t % N;
        result = std::move(*reinterpret_cast<pointer>(&slots[idx]));
        
        if constexpr (!std::is_trivially_destructible_v<T>) {
            reinterpret_cast<pointer>(&slots[idx])->~T();
        }
        
        header->tail.store(t + 1, std::memory_order_release);
        return true;
    }
```

- [ ] **Step 6: Update front/back methods**

Replace existing front/back:

```cpp
    // Access the oldest element. UB if empty.
    [[nodiscard]] reference front() noexcept {
        auto* header = storage_.header();
        auto* slots = storage_.slots();
        return *reinterpret_cast<pointer>(&slots[header->tail.load(std::memory_order_relaxed) % N]);
    }
    [[nodiscard]] const_reference front() const noexcept {
        return const_cast<self_type*>(this)->front();
    }

    // Access the newest element. UB if empty.
    [[nodiscard]] reference back() noexcept {
        auto* header = storage_.header();
        auto* slots = storage_.slots();
        return *reinterpret_cast<pointer>(&slots[(header->head.load(std::memory_order_relaxed) - 1) % N]);
    }
    [[nodiscard]] const_reference back() const noexcept {
        return const_cast<self_type*>(this)->back();
    }
```

- [ ] **Step 7: Update empty/full/size methods**

Replace existing methods:

```cpp
    [[nodiscard]] bool empty() const noexcept {
        auto* header = storage_.header();
        return header->head.load(std::memory_order_acquire) ==
               header->tail.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool full() const noexcept {
        auto* header = storage_.header();
        return header->head.load(std::memory_order_acquire) -
               header->tail.load(std::memory_order_acquire) >= N;
    }

    // Snapshot of current element count.
    [[nodiscard]] size_type size() const noexcept {
        auto* header = storage_.header();
        return header->head.load(std::memory_order_acquire) -
               header->tail.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr size_type capacity() noexcept { return N; }
```

- [ ] **Step 8: Update clear method**

Replace existing clear:

```cpp
    void clear() noexcept {
        auto* header = storage_.header();
        auto* slots = storage_.slots();

        auto t = header->tail.load(std::memory_order_relaxed);
        auto h = header->head.load(std::memory_order_acquire);

        if constexpr (!std::is_trivially_destructible_v<T>) {
            while (t != h) {
                reinterpret_cast<pointer>(&slots[t % N])->~T();
                ++t;
            }
        }

        header->tail.store(t, std::memory_order_release);
    }
```

- [ ] **Step 9: Add statistics methods**

Add after clear method:

```cpp
    // Statistics
    [[nodiscard]] uint64_t overflow_count() const noexcept {
        auto* header = storage_.header();
        return header->overflow_count.load(std::memory_order_relaxed);
    }

    void reset_stats() noexcept {
        auto* header = storage_.header();
        header->overflow_count.store(0, std::memory_order_relaxed);
    }

    // Storage status
    bool valid() const noexcept { return storage_.valid(); }
    bool is_creator() const noexcept { return storage_.is_creator(); }
```

- [ ] **Step 10: Update private members**

Replace existing private members:

```cpp
private:
    storage_policy storage_;
};
```

- [ ] **Step 11: Run existing tests to verify compatibility**

Run: `cd /home/bowmanhan/Code/RingBufferCpp/build && ./SpscRingBufferTest`
Expected: All tests pass (backward compatibility maintained)

- [ ] **Step 12: Commit**

```bash
git add spsc_ring_buffer.hpp
git commit -m "refactor: convert spsc_ring_buffer to use StoragePolicy"
```

---

## Task 5: Add overwrite mode tests

**Files:**
- Modify: `tests/spsc_tests.cpp:387-450` (append to end)

- [ ] **Step 1: Write test for push_overwrite basic behavior**

Add at end of file:

```cpp
// --- Overwrite mode tests ---

TEST(SpscTest, OverwriteBasic) {
    spsc_ring_buffer<int, 4> buf;
    
    // Fill buffer
    for (int i = 0; i < 4; ++i)
        buf.push_overwrite(i);
    
    EXPECT_TRUE(buf.full());
    EXPECT_EQ(buf.size(), 4u);
    
    // Overwrite should succeed
    buf.push_overwrite(100);
    EXPECT_EQ(buf.size(), 4u);
    EXPECT_EQ(buf.overflow_count(), 1u);
    
    // Oldest element (0) should be gone, newest should be 100
    int val;
    for (int i = 1; i < 4; ++i) {
        ASSERT_TRUE(buf.try_pop(val));
        EXPECT_EQ(val, i);
    }
    ASSERT_TRUE(buf.try_pop(val));
    EXPECT_EQ(val, 100);
}
```

- [ ] **Step 2: Write test for multiple overwrites**

Add after previous test:

```cpp
TEST(SpscTest, OverwriteMultiple) {
    spsc_ring_buffer<int, 4> buf;
    
    // Fill and overwrite multiple times
    for (int i = 0; i < 10; ++i)
        buf.push_overwrite(i);
    
    EXPECT_EQ(buf.overflow_count(), 6u);
    EXPECT_EQ(buf.size(), 4u);
    
    // Should have last 4 elements: 6, 7, 8, 9
    int val;
    for (int i = 6; i < 10; ++i) {
        ASSERT_TRUE(buf.try_pop(val));
        EXPECT_EQ(val, i);
    }
    EXPECT_TRUE(buf.empty());
}
```

- [ ] **Step 3: Write test for overwrite with non-trivial type**

Add after previous test:

```cpp
TEST(SpscTest, OverwriteNonTrivial) {
    spsc_ring_buffer<std::string, 3> buf;
    
    buf.push_overwrite("one");
    buf.push_overwrite("two");
    buf.push_overwrite("three");
    
    // Overwrite should properly destruct old strings
    buf.push_overwrite("four");
    EXPECT_EQ(buf.overflow_count(), 1u);
    
    std::string val;
    ASSERT_TRUE(buf.try_pop(val));
    EXPECT_EQ(val, "two");
    ASSERT_TRUE(buf.try_pop(val));
    EXPECT_EQ(val, "three");
    ASSERT_TRUE(buf.try_pop(val));
    EXPECT_EQ(val, "four");
}
```

- [ ] **Step 4: Write concurrent overwrite test**

Add after previous test:

```cpp
TEST(SpscTest, ConcurrentOverwrite) {
    spsc_ring_buffer<int, 16> buf;
    constexpr int count = 100000;
    
    std::thread producer([&buf]() {
        for (int i = 0; i < count; ++i) {
            buf.push_overwrite(i);
        }
    });
    
    std::thread consumer([&buf, count]() {
        int last = -1;
        for (int i = 0; i < count / 2; ++i) {
            int val;
            while (!buf.try_pop(val))
                ;
            // Values should be monotonically increasing
            EXPECT_GT(val, last);
            last = val;
        }
    });
    
    producer.join();
    consumer.join();
    
    EXPECT_GT(buf.overflow_count(), 0u);
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd /home/bowmanhan/Code/RingBufferCpp/build && ./SpscRingBufferTest --gtest_filter="SpscTest.Overwrite*"`
Expected: All 4 overwrite tests pass

- [ ] **Step 6: Commit**

```bash
git add tests/spsc_tests.cpp
git commit -m "test: add overwrite mode tests for push_overwrite"
```

---

## Task 6: Create shared memory test file

**Files:**
- Create: `tests/spsc_shm_tests.cpp`

- [ ] **Step 1: Write basic shared memory test**

Create new file:

```cpp
#include <gtest/gtest.h>
#include "spsc_ring_buffer.hpp"
#include <sys/wait.h>
#include <unistd.h>

using namespace buffers;

// Basic shared memory creation test
TEST(SpscShmTest, CreateAndOpen) {
    const char* shm_name = "/test_ring_buffer_create";
    
    // Clean up any existing shm
    shm_unlink(shm_name);
    
    // Create new shared memory buffer
    spsc_ring_buffer<int, 16, ShmStorage> writer(
        shm_name, ShmOpenMode::create
    );
    
    EXPECT_TRUE(writer.valid());
    EXPECT_TRUE(writer.is_creator());
    EXPECT_TRUE(writer.empty());
    
    // Clean up
    shm_unlink(shm_name);
}

TEST(SpscShmTest, OpenExisting) {
    const char* shm_name = "/test_ring_buffer_open";
    shm_unlink(shm_name);
    
    // Create
    {
        spsc_ring_buffer<int, 16, ShmStorage> writer(
            shm_name, ShmOpenMode::create
        );
        EXPECT_TRUE(writer.valid());
    }
    
    // Open existing
    {
        spsc_ring_buffer<int, 16, ShmStorage> reader(
            shm_name, ShmOpenMode::open
        );
        EXPECT_TRUE(reader.valid());
        EXPECT_FALSE(reader.is_creator());
    }
    
    shm_unlink(shm_name);
}
```

- [ ] **Step 2: Write cross-process communication test**

Add to file:

```cpp
TEST(SpscShmTest, CrossProcessCommunication) {
    const char* shm_name = "/test_ring_buffer_ipc";
    shm_unlink(shm_name);
    
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    
    if (pid == 0) {
        // Child process: writer
        spsc_ring_buffer<int, 64, ShmStorage> writer(
            shm_name, ShmOpenMode::create
        );
        
        if (!writer.valid()) {
            exit(1);
        }
        
        // Write data
        for (int i = 0; i < 100; ++i) {
            while (!writer.try_push(i))
                ;
        }
        
        exit(0);
    } else {
        // Parent process: reader
        usleep(10000);  // Wait for writer to start
        
        spsc_ring_buffer<int, 64, ShmStorage> reader(
            shm_name, ShmOpenMode::open
        );
        
        ASSERT_TRUE(reader.valid());
        
        int expected = 0;
        int attempts = 0;
        while (expected < 100 && attempts < 10000) {
            int val;
            if (reader.try_pop(val)) {
                EXPECT_EQ(val, expected);
                ++expected;
            }
            ++attempts;
            if (attempts % 100 == 0)
                usleep(100);
        }
        
        EXPECT_EQ(expected, 100);
        
        int status;
        waitpid(pid, &status, 0);
        EXPECT_EQ(WEXITSTATUS(status), 0);
    }
    
    shm_unlink(shm_name);
}
```

- [ ] **Step 3: Write overflow count test**

Add to file:

```cpp
TEST(SpscShmTest, OverflowCountShared) {
    const char* shm_name = "/test_ring_buffer_overflow";
    shm_unlink(shm_name);
    
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    
    if (pid == 0) {
        // Writer: overwrite mode
        spsc_ring_buffer<int, 4, ShmStorage> writer(
            shm_name, ShmOpenMode::create
        );
        
        for (int i = 0; i < 10; ++i) {
            writer.push_overwrite(i);
        }
        
        sleep(1);  // Give reader time to check
        exit(0);
    } else {
        // Reader
        usleep(10000);
        
        spsc_ring_buffer<int, 4, ShmStorage> reader(
            shm_name, ShmOpenMode::open
        );
        
        ASSERT_TRUE(reader.valid());
        
        // Wait for writer to finish
        int status;
        waitpid(pid, &status, 0);
        
        // Overflow count should be visible across processes
        EXPECT_EQ(reader.overflow_count(), 6u);
    }
    
    shm_unlink(shm_name);
}
```

- [ ] **Step 4: Update CMakeLists.txt**

Find and modify `tests/CMakeLists.txt` or main `CMakeLists.txt` to add new test executable:

```cmake
# Add after existing SpscRingBufferTest
add_executable(SpscShmTest tests/spsc_shm_tests.cpp)
target_link_libraries(SpscShmTest GTest::gtest_main pthread rt)
add_test(NAME SpscShmTest COMMAND SpscShmTest)
```

- [ ] **Step 5: Build and run tests**

Run: 
```bash
cd /home/bowmanhan/Code/RingBufferCpp/build
cmake ..
make SpscShmTest
./SpscShmTest
```
Expected: All 4 shared memory tests pass

- [ ] **Step 6: Commit**

```bash
git add tests/spsc_shm_tests.cpp CMakeLists.txt
git commit -m "test: add shared memory cross-process tests"
```

---

## Task 7: Run full test suite and verify

**Files:**
- None (verification task)

- [ ] **Step 1: Run all SPSC tests**

Run: `cd /home/bowmanhan/Code/RingBufferCpp/build && ./SpscRingBufferTest`
Expected: All tests pass (including new overwrite tests)

- [ ] **Step 2: Run shared memory tests**

Run: `cd /home/bowmanhan/Code/RingBufferCpp/build && ./SpscShmTest`
Expected: All shared memory tests pass

- [ ] **Step 3: Run with thread sanitizer**

Run:
```bash
cd /home/bowmanhan/Code/RingBufferCpp/build
g++ -std=c++17 -fsanitize=thread -g ../tests/spsc_tests.cpp -lgtest -lgtest_main -lpthread -o ts_test
./ts_test
```
Expected: No data races detected

- [ ] **Step 4: Verify performance hasn't degraded**

Run existing concurrent tests and compare timing (manual verification).

---

## Spec Coverage Check

✅ Storage Policy interface - Task 1, 2, 3  
✅ HeapStorage implementation - Task 2  
✅ ShmStorage implementation - Task 3  
✅ ShmOpenMode enum - Task 3  
✅ RingBufferHeader/Region structures - Task 1  
✅ try_push (reject mode) - Task 4  
✅ push_overwrite (overwrite mode) - Task 4  
✅ overflow_count tracking - Task 4  
✅ Non-trivial type handling - Task 4, Task 5  
✅ Backward compatibility - Task 4 (existing tests pass)  
✅ Overwrite mode tests - Task 5  
✅ Shared memory tests - Task 6  
✅ Cross-process communication - Task 6  

---

## Execution Notes

**Critical implementation details:**
- ShmStorage requires POSIX shared memory support (Linux/Unix)
- Tests may need `sudo` or proper permissions for shm_open
- Shared memory tests use fork() - ensure proper cleanup with shm_unlink
- Thread sanitizer may report false positives on atomics with certain GCC versions

**Testing requirements:**
- GoogleTest framework (already configured)
- POSIX real-time library (-lrt for shm_open)
- Thread sanitizer for concurrency verification

**Potential gotchas:**
- shm_open name must start with '/'
- ftruncate size must match exactly between processes
- Destructor must not call shm_unlink (user responsibility)
- is_creator_ detection relies on st.st_size == 0 check

---

**Plan complete and saved to `docs/superpowers/plans/2026-05-03-spsc-storage-policy-implementation.md`. Two execution options:**

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**

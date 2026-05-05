# Uncovered Code Paths — Test Coverage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Write tests covering 7 uncovered code paths in `spsc_ring_buffer.hpp` — `reset_stats()` on valid buffers, `clear()` with non-trivial destructors, and 5 shared-memory constructor error paths.

**Architecture:** Two test files: `tests/spsc_tests.cpp` for heap-storage paths (Tasks 1-2), `tests/spsc_shm_tests.cpp` for shared-memory paths (Tasks 3-6). All tests use GoogleTest. Fork-based tests use `pid_t`/`waitpid` pattern already established in `spsc_shm_tests.cpp`.

**Tech Stack:** C++17, GoogleTest, POSIX shared memory (`shm_open`, `mmap`, `ftruncate`, `fork`)

---

### Uncovered Paths

| # | Snippet | Location | Trigger |
|---|---------|----------|---------|
| 1 | `overflow_count.store(0, ...)` | `reset_stats()` :591 | Valid buffer with non-zero overflow |
| 2 | `while (t != h) { ->~T(); ++t; }` | `clear()` :563-565 | Non-trivially-destructible type, buffer non-empty |
| 3 | `munmap/close/return` | shm ctor :271-276 | Non-creator waits for capacity, creator never inits |
| 4 | `munmap/close/return` | shm ctor :282-287 | Schema version mismatch |
| 5 | `yield/usleep/++retries` | shm ctor :263-265 | Non-creator spins waiting for uninitialized capacity |
| 6 | `close(fd_)/return` | shm ctor :228-230 | Non-creator fstat timeout (shm too small) |
| 7 | `close(fd_)/return` | shm ctor :209-211 | ftruncate fails (RLIMIT_FSIZE = 0) |

Paths 3 and 5 share the same test (non-creator timeout), so they are covered together in Task 4.

---

### Task 1: Test `reset_stats()` on valid buffer

**Files:**
- Modify: `tests/spsc_tests.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/spsc_tests.cpp` (after the `ConcurrentOverwrite` test, before the closing of the file):

```cpp
TEST(SpscTest, ResetStatsClearsOverflowCount) {
    spsc_ring_buffer<int, 4> buf;

    // Fill buffer completely
    for (int i = 0; i < 4; ++i)
        buf.push_overwrite(i);

    // Trigger overflows
    buf.push_overwrite(100);
    buf.push_overwrite(200);
    EXPECT_GT(buf.overflow_count(), 0u);

    // Reset stats
    buf.reset_stats();
    EXPECT_EQ(buf.overflow_count(), 0u);

    // Buffer should still be functional
    EXPECT_EQ(buf.size(), 4u);
    EXPECT_TRUE(buf.full());
}
```

- [ ] **Step 2: Build and run the test**

Run: `cmake --build cmake-build-debug --target SpscRingBufferTest && cmake-build-debug/SpscRingBufferTest --gtest_filter="SpscTest.ResetStatsClearsOverflowCount"`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add tests/spsc_tests.cpp
git commit -m "test: add coverage for reset_stats() on valid buffer"
```

---

### Task 2: Test `clear()` with non-trivially-destructible type

**Files:**
- Modify: `tests/spsc_tests.cpp`

- [ ] **Step 1: Write the failing test**

This test covers the `while (t != h) { reinterpret_cast<pointer>(&slots[t % N])->~T(); ++t; }` loop inside `clear()` by using a type with a non-trivial destructor that has an observable side effect.

Append to `tests/spsc_tests.cpp` (after the `ResetStatsClearsOverflowCount` test):

```cpp
// Type with a non-trivial destructor that increments a counter
struct DtorCounter {
    int* counter_;
    explicit DtorCounter(int* c = nullptr) : counter_(c) {}
    ~DtorCounter() {
        if (counter_) ++(*counter_);
    }
    DtorCounter(DtorCounter const& o) : counter_(o.counter_) {}
    DtorCounter(DtorCounter&& o) noexcept : counter_(o.counter_) { o.counter_ = nullptr; }
    DtorCounter& operator=(DtorCounter&& o) noexcept {
        counter_ = o.counter_;
        o.counter_ = nullptr;
        return *this;
    }
    DtorCounter& operator=(DtorCounter const& o) {
        counter_ = o.counter_;
        return *this;
    }
};

TEST(SpscTest, ClearCallsDestructorsOnNonTrivialType) {
    int dtor_count = 0;
    {
        spsc_ring_buffer<DtorCounter, 8> buf;

        // Push 5 elements, each pointing to dtor_count
        for (int i = 0; i < 5; ++i) {
            buf.try_push(DtorCounter(&dtor_count));
        }
        EXPECT_EQ(dtor_count, 0);  // No destructors called yet

        // clear() should call ~DtorCounter() for all 5 elements
        buf.clear();
        EXPECT_EQ(dtor_count, 5);
        EXPECT_TRUE(buf.empty());
    }
    // No additional destructors from buf destruction — clear() already destroyed everything
    EXPECT_EQ(dtor_count, 5);
}

TEST(SpscTest, ClearWithNonTrivialTypeEmptyBuffer) {
    int dtor_count = 0;
    {
        spsc_ring_buffer<DtorCounter, 4> buf;
        buf.clear();  // No-op on empty buffer
        EXPECT_EQ(dtor_count, 0);
    }
    EXPECT_EQ(dtor_count, 0);
}
```

- [ ] **Step 2: Build and run the tests**

Run: `cmake --build cmake-build-debug --target SpscRingBufferTest && cmake-build-debug/SpscRingBufferTest --gtest_filter="SpscTest.Clear*"`
Expected: Both PASS

- [ ] **Step 3: Commit**

```bash
git add tests/spsc_tests.cpp
git commit -m "test: add coverage for clear() with non-trivially-destructible type"
```

---

### Task 3: Test schema version mismatch (munmap/close/return path)

**Files:**
- Modify: `tests/spsc_shm_tests.cpp`

This covers the path at lines 280-287: creator writes `schema_version = 1`, opener expects `schema_version = 2`, mismatch triggers `munmap` + `close` + `return`.

- [ ] **Step 1: Write the failing test**

Append to `tests/spsc_shm_tests.cpp` (after the last test):

```cpp
TEST(SpscShmTest, SchemaVersionMismatch) {
    std::string shm_name = generate_unique_shm_name("test_schema_mismatch");
    shm_unlink(shm_name.c_str());

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        // Child: create with schema version 1
        spsc_ring_buffer<int, 16, ShmStorage> writer(
            shm_name.c_str(), 1, ShmOpenMode::create
        );
        if (!writer.valid()) exit(1);
        writer.try_push(42);
        sleep(1);  // Keep alive so parent can try to open
        exit(0);
    } else {
        // Parent: wait for child to create, then try to open with wrong version
        usleep(50000);  // 50ms

        spsc_ring_buffer<int, 16, ShmStorage> reader(
            shm_name.c_str(), 2, ShmOpenMode::open
        );

        // Construction should fail due to schema mismatch
        EXPECT_FALSE(reader.valid());
        EXPECT_FALSE(reader.is_compatible());

        // All operations should be safe
        EXPECT_TRUE(reader.empty());
        EXPECT_FALSE(reader.try_push(1));
        int val;
        EXPECT_FALSE(reader.try_pop(val));

        int status;
        waitpid(pid, &status, 0);
    }

    shm_unlink(shm_name.c_str());
}
```

- [ ] **Step 2: Build and run the test**

Run: `cmake --build cmake-build-debug --target SpscShmTest && cmake-build-debug/SpscShmTest --gtest_filter="SpscShmTest.SchemaVersionMismatch"`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add tests/spsc_shm_tests.cpp
git commit -m "test: add coverage for schema version mismatch in shm_storage"
```

---

### Task 4: Test non-creator timeout waiting for uninitialized capacity

**Files:**
- Modify: `tests/spsc_shm_tests.cpp`

This covers TWO uncovered paths:
- `std::this_thread::yield(); usleep(1000); ++retries;` (lines 263-265) — the spin-wait loop
- `munmap(region_, sizeof(region_type)); region_ = nullptr; close(fd_); fd_ = -1; return;` (lines 271-276) — the cleanup after timeout

The trick: use raw POSIX calls (`shm_open` + `ftruncate`) to create a shared memory segment with capacity left at 0 (the constructor never ran), then open it as a non-creator. The non-creator will spin through the yield/usleep loop and hit the munmap cleanup path.

- [ ] **Step 1: Write the failing test**

Append to `tests/spsc_shm_tests.cpp`:

```cpp
TEST(SpscShmTest, NonCreatorTimeoutWaitingForCapacity) {
    std::string shm_name = generate_unique_shm_name("test_capacity_timeout");
    shm_unlink(shm_name.c_str());

    // Create raw shared memory segment (bypass constructor)
    // Leave capacity at 0 — the non-creator will timeout
    int fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, sizeof(buffers::detail::ring_buffer_region<int, 16>)), 0);
    close(fd);

    // Open as non-creator with open mode
    // This will mmap the region, see capacity==0, spin through yield/usleep loop, timeout
    spsc_ring_buffer<int, 16, ShmStorage> reader(
        shm_name.c_str(), 0, ShmOpenMode::open
    );

    // Construction should fail — creator never initialized capacity
    EXPECT_FALSE(reader.valid());

    // Operations should be safe
    EXPECT_TRUE(reader.empty());
    EXPECT_FALSE(reader.try_push(1));
    int val;
    EXPECT_FALSE(reader.try_pop(val));

    shm_unlink(shm_name.c_str());
}
```

- [ ] **Step 2: Build and run the test**

Run: `cmake --build cmake-build-debug --target SpscShmTest && cmake-build-debug/SpscShmTest --gtest_filter="SpscShmTest.NonCreatorTimeoutWaitingForCapacity"`
Expected: PASS (takes ~100ms due to retry loop)

- [ ] **Step 3: Commit**

```bash
git add tests/spsc_shm_tests.cpp
git commit -m "test: add coverage for non-creator timeout waiting for capacity init"
```

---

### Task 5: Test non-creator fstat timeout (shm too small)

**Files:**
- Modify: `tests/spsc_shm_tests.cpp`

This covers the `close(fd_); fd_ = -1; return;` path at lines 228-230: non-creator opens an shm segment that is too small (never resized by creator), fstat loop times out.

- [ ] **Step 1: Write the failing test**

Append to `tests/spsc_shm_tests.cpp`:

```cpp
TEST(SpscShmTest, NonCreatorFstatTimeoutTooSmall) {
    std::string shm_name = generate_unique_shm_name("test_fstat_timeout");
    shm_unlink(shm_name.c_str());

    // Create raw shm segment with only 1 byte — too small for region_type
    int fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, 1), 0);  // Way too small
    close(fd);

    // Open as non-creator: fstat sees size < sizeof(region_type), retries, times out
    spsc_ring_buffer<int, 16, ShmStorage> reader(
        shm_name.c_str(), 0, ShmOpenMode::open
    );

    // Construction should fail
    EXPECT_FALSE(reader.valid());

    // Operations should be safe
    EXPECT_TRUE(reader.empty());
    EXPECT_EQ(reader.size(), 0u);

    shm_unlink(shm_name.c_str());
}
```

- [ ] **Step 2: Build and run the test**

Run: `cmake --build cmake-build-debug --target SpscShmTest && cmake-build-debug/SpscShmTest --gtest_filter="SpscShmTest.NonCreatorFstatTimeoutTooSmall"`
Expected: PASS (takes ~100ms due to retry loop)

- [ ] **Step 3: Commit**

```bash
git add tests/spsc_shm_tests.cpp
git commit -m "test: add coverage for non-creator fstat timeout on undersized shm"
```

---

### Task 6: Test ftruncate failure (RLIMIT_FSIZE = 0)

**Files:**
- Modify: `tests/spsc_shm_tests.cpp`

This covers the `close(fd_); fd_ = -1; return;` path at lines 209-211: creator's `ftruncate` call fails. We trigger this by setting `RLIMIT_FSIZE` to 0 in a forked child process, so any file write (including ftruncate) fails with `EFBIG`.

- [ ] **Step 1: Write the failing test**

Append to `tests/spsc_shm_tests.cpp`:

```cpp
#include <sys/resource.h>

TEST(SpscShmTest, CreatorFtruncateFailure) {
    std::string shm_name = generate_unique_shm_name("test_ftruncate_fail");
    shm_unlink(shm_name.c_str());

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        // Child: set file size limit to 0 so ftruncate fails
        struct rlimit rl;
        rl.rlim_cur = 0;
        rl.rlim_max = 0;
        setrlimit(RLIMIT_FSIZE, &rl);

        spsc_ring_buffer<int, 16, ShmStorage> writer(
            shm_name.c_str(), 0, ShmOpenMode::create
        );

        // ftruncate should have failed — buffer invalid
        if (writer.valid()) {
            exit(1);
        }

        // Operations should be safe
        if (!writer.empty()) exit(2);
        if (writer.try_push(1)) exit(3);

        exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        EXPECT_EQ(WEXITSTATUS(status), 0);
    }

    shm_unlink(shm_name.c_str());
}
```

Note: Add `#include <sys/resource.h>` at the top of `tests/spsc_shm_tests.cpp` alongside the existing includes (after `#include <unistd.h>`).

- [ ] **Step 2: Build and run the test**

Run: `cmake --build cmake-build-debug --target SpscShmTest && cmake-build-debug/SpscShmTest --gtest_filter="SpscShmTest.CreatorFtruncateFailure"`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add tests/spsc_shm_tests.cpp
git commit -m "test: add coverage for ftruncate failure via RLIMIT_FSIZE"
```

---

### Task 7: Run full test suite and verify

- [ ] **Step 1: Run all tests**

Run: `cmake --build cmake-build-debug && cmake-build-debug/SpscRingBufferTest && cmake-build-debug/SpscShmTest`
Expected: All tests PASS, no regressions.

- [ ] **Step 2: Commit (if any fixups were needed)**

Only if changes were made during verification.

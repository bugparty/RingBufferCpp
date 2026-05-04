# SPSC Ring Buffer Storage Policy Design

**Date:** 2026-05-03  
**Status:** Approved  
**Author:** Claude

## 1. Overview

Add Storage Policy support and overflow_count statistics to `spsc_ring_buffer`, enabling:
- Pluggable storage backends (heap memory, shared memory)
- Two write modes (reject mode, overwrite mode)
- Overflow counting for both modes
- Cross-process communication via shared memory

## 2. Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Design Pattern | Policy-Based Design | Compile-time polymorphism, zero-overhead abstraction |
| Storage Backends | HeapStorage + ShmStorage | Minimum viable implementation |
| overflow Semantics | Both reject and overwrite modes | Flexibility for different scenarios |
| Statistics Storage | In shared memory header | Cross-process sharing, no extra policy needed |
| Shared Memory Mode | create / open / create_or_open | Clear semantics, prevent initialization races |
| Destructor Handling | Compile-time type traits | Performance + safety balance |
| API Naming | try_push() / push_overwrite() | Clear distinction between modes |

## 3. Architecture

```
spsc_ring_buffer<T, N, StoragePolicy>
├── StoragePolicy<T, N>
│   └── RingBufferRegion<T, N>
│       ├── RingBufferHeader
│       │   ├── version
│       │   ├── capacity
│       │   ├── head (atomic, alignas(64))
│       │   ├── tail (atomic, alignas(64))
│       │   └── overflow_count (atomic, alignas(64))
│       └── slots[N]
```

## 4. Storage Policy Interface

```cpp
template<typename T, size_t N>
class StoragePolicy {
public:
    using header_type = RingBufferHeader<T>;
    using element_type = /* aligned storage */;

    header_type* header() noexcept;
    element_type* slots() noexcept;
    bool valid() const noexcept;
    bool is_creator() const noexcept;
};
```

## 5. HeapStorage

```cpp
template<typename T, size_t N>
class HeapStorage {
public:
    HeapStorage();  // Creates region on heap
    ~HeapStorage();

    header_type* header() noexcept { return &region_->header; }
    element_type* slots() noexcept { return region_->slots; }
    bool valid() const noexcept { return region_ != nullptr; }
    bool is_creator() const noexcept { return true; }

private:
    RingBufferRegion<T, N>* region_ = nullptr;
};
```

## 6. ShmStorage

### ShmOpenMode Enum

```cpp
enum class ShmOpenMode {
    create,         // Create new, fail if exists
    open,           // Open existing, fail if not found
    create_or_open  // Create if not exists, open if exists (default)
};
```

### Implementation

```cpp
template<typename T, size_t N>
class ShmStorage {
public:
    explicit ShmStorage(const char* name,
                       ShmOpenMode mode = ShmOpenMode::create_or_open);
    ~ShmStorage();

    header_type* header() noexcept;
    element_type* slots() noexcept;
    bool valid() const noexcept { return region_ != nullptr; }
    bool is_creator() const noexcept { return is_creator_; }

private:
    std::string shm_name_;
    ShmOpenMode mode_;
    int fd_ = -1;
    RingBufferRegion<T, N>* region_ = nullptr;
    bool is_creator_ = false;
};
```

### Key Behaviors

- Creator calls `ftruncate()` to set region size
- Creator initializes header (version, capacity, zeros)
- Opener maps existing region without reinitializing
- Destructor unmaps but does NOT call `shm_unlink`

## 7. spsc_ring_buffer Class

### Template Parameters

```cpp
template<
    typename T,
    size_t N,
    template<typename, size_t> class StoragePolicy = HeapStorage
>
class spsc_ring_buffer;
```

### Public Interface

```cpp
// Construction
template<typename... Args>
explicit spsc_ring_buffer(Args&&... args);

// Write
template<typename U>
bool try_push(U&& value);       // Reject mode, returns false if full
template<typename U>
void push_overwrite(U&& value); // Overwrite mode, overwrites oldest if full

// Read
bool try_pop(T& result);

// Access
reference front();
reference back();

// State
bool empty() const;
bool full() const;
size_type size() const;
static constexpr size_type capacity();

// Clear
void clear() noexcept;

// Statistics
uint64_t overflow_count() const;
void reset_stats() noexcept;

// Storage status
bool valid() const;
bool is_creator() const;
```

### Memory Order Semantics

| Operation | Head | Tail | Rationale |
|-----------|------|------|-----------|
| try_push read | relaxed | acquire | Only need to see latest tail when checking full |
| try_push write | release | - | Publish new element |
| try_pop read | acquire | relaxed | Only need to see latest head when checking empty |
| try_pop write | - | release | Publish consumption |
| overflow_count | relaxed | - | Approximate count, relaxed is sufficient |

## 8. Overflow Count Semantics

- **Reject mode:** Increments when `try_push()` returns false due to full buffer
- **Overwrite mode:** Increments when an old element is overwritten
- Storage: In shared memory header, accessible across processes

## 9. Non-Trivial Type Handling

```cpp
// In push_overwrite, when buffer is full:
if (h - t >= N) {
    if constexpr (!std::is_trivially_destructible_v<T>) {
        reinterpret_cast<T*>(&slots[idx])->~T();
    }
    new(&slots[idx]) T(std::forward<U>(value));
}
```

## 10. Backward Compatibility

Existing code requires no changes:

```cpp
spsc_ring_buffer<int, 1024> buf;  // Still works, uses HeapStorage

// New features are opt-in
buf.try_push(42);
buf.push_overwrite(42);
buf.overflow_count();
```

## 11. Usage Examples

### Heap Storage (Default)

```cpp
spsc_ring_buffer<int, 1024> buf;
buf.push_overwrite(42);
int val;
buf.try_pop(val);
```

### Shared Memory (Cross-Process)

**Writer Process:**
```cpp
spsc_ring_buffer<Sample, 4096, ShmStorage> writer(
    "/power_metrics",
    ShmOpenMode::create
);

if (!writer.valid()) {
    // Handle error
}

Sample s = {...};
writer.push_overwrite(s);
```

**Reader Process:**
```cpp
spsc_ring_buffer<Sample, 4096, ShmStorage> reader(
    "/power_metrics",
    ShmOpenMode::open
);

if (!reader.valid()) {
    // Handle error
}

Sample s;
if (reader.try_pop(s)) {
    // Process sample
}
```

## 12. Testing Strategy

### Unit Tests
- HeapStorage basic functionality
- ShmStorage create/open/mmap
- try_push / push_overwrite correctness
- overflow_count accuracy
- Non-trivial type destruction

### Concurrency Tests
- SPSC correctness (single process, multiple threads)
- Cross-process communication
- High-pressure scenarios (1M+ messages)

### Boundary Tests
- Capacity = 1 edge case
- Wraparound (head/tail near UINT64_MAX)
- Shared memory permission tests

## 13. File Organization

```
spsc_ring_buffer.hpp    # Main header with all implementations
tests/
  spsc_tests.cpp        # Existing tests
  spsc_shm_tests.cpp    # New: shared memory tests
  spsc_overwrite_tests.cpp  # New: overwrite mode tests
```

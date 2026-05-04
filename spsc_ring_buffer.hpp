//
// Single-Producer Single-Consumer lock-free ring buffer.
//
// Uses std::atomic with acquire/release memory ordering for synchronization.
// Head and tail are on separate cache lines to avoid false sharing.
// Monotonic counters (no modular wraparound of indices) with % N for slot access.
//
#ifndef SPSC_RING_BUFFER_HPP
#define SPSC_RING_BUFFER_HPP

#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace buffers {

// Ring buffer header stored in shared memory
struct RingBufferHeader {
    std::atomic<uint32_t> version{0};  // Changed to atomic for spin-wait sync
    uint32_t capacity{0};
    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint64_t> tail{0};
    alignas(64) std::atomic<uint64_t> overflow_count{0};
};

// Ring buffer region: header + slots
template<typename T, size_t N>
struct RingBufferRegion {
    RingBufferHeader header;
    typename std::aligned_storage<sizeof(T), alignof(T)>::type slots[N];
};

// Heap storage policy (default)
template<typename T, size_t N>
class HeapStorage {
public:
    using header_type = RingBufferHeader;
    using element_type = typename std::aligned_storage<sizeof(T), alignof(T)>::type;
    using region_type = RingBufferRegion<T, N>;

private:
    region_type* region_ = nullptr;

public:
    HeapStorage() : region_(new region_type{}) {
        region_->header.version.store(1, std::memory_order_relaxed);
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
    const header_type* header() const noexcept { return &region_->header; }
    element_type* slots() noexcept { return region_->slots; }
    const element_type* slots() const noexcept { return region_->slots; }
    bool valid() const noexcept { return region_ != nullptr; }
    bool is_creator() const noexcept { return true; }
};

// Shared memory open mode
enum class ShmOpenMode {
    create,           // Create new, fail if exists
    open,             // Open existing, fail if not found
    create_or_open    // Create if not exists, open if exists (default)
};

// Shared memory storage policy
template<typename T, size_t N>
class ShmStorage {
public:
    using header_type = RingBufferHeader;
    using element_type = typename std::aligned_storage<sizeof(T), alignof(T)>::type;
    using region_type = RingBufferRegion<T, N>;

private:
    std::string shm_name_;
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
    const header_type* header() const noexcept { return region_ ? &region_->header : nullptr; }
    element_type* slots() noexcept { return region_ ? region_->slots : nullptr; }
    const element_type* slots() const noexcept { return region_ ? region_->slots : nullptr; }
    bool valid() const noexcept { return region_ != nullptr; }
    bool is_creator() const noexcept { return is_creator_; }
};

template<typename T, size_t N>
ShmStorage<T, N>::ShmStorage(const char* name, ShmOpenMode mode)
    : shm_name_(name) {

    // Validate name
    if (!name || name[0] != '/') {
        return;
    }

    int flags = O_RDWR;
    bool try_exclusive_create = false;

    switch (mode) {
        case ShmOpenMode::create:
            flags |= O_CREAT | O_EXCL;
            break;
        case ShmOpenMode::open:
            // Only O_RDWR
            break;
        case ShmOpenMode::create_or_open:
            // Try exclusive create first
            try_exclusive_create = true;
            break;
    }

    if (try_exclusive_create) {
        // Try to create exclusively
        fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
        if (fd_ >= 0) {
            is_creator_ = true;
        } else if (errno == EEXIST) {
            // Already exists, just open it
            fd_ = shm_open(shm_name_.c_str(), O_RDWR, 0666);
            is_creator_ = false;
        } else {
            // Other error
            return;
        }
    } else {
        // Open with specified flags
        fd_ = shm_open(shm_name_.c_str(), flags, 0666);
        if (fd_ < 0) {
            return;
        }

        // For create mode, we are the creator
        if (mode == ShmOpenMode::create) {
            is_creator_ = true;
        }
    }

    if (fd_ < 0) {
        return;
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

    // Creator initializes header using memset + memory fence
    if (is_creator_) {
        std::memset(region_, 0, sizeof(region_type));
        std::atomic_thread_fence(std::memory_order_release);
        region_->header.version.store(1, std::memory_order_release);
        region_->header.capacity = N;
    } else {
        // Wait for creator to finish initialization
        // Spin until version is set (indicates creator finished)
        while (region_->header.version.load(std::memory_order_acquire) == 0) {
            // Busy wait - creator should finish quickly
        }
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

    // Try to enqueue an element. Returns false if the buffer is full or invalid.
    template<typename U>
    bool try_push(U&& value) noexcept(std::is_nothrow_constructible<T, U&&>::value) {
        if (!storage_.valid()) {
            return false;
        }

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

    // Overwrite mode: always succeeds, overwrites oldest if full. No-op if invalid.
    template<typename U>
    void push_overwrite(U&& value) noexcept(std::is_nothrow_constructible<T, U&&>::value) {
        if (!storage_.valid()) {
            return;
        }

        auto* header = storage_.header();
        auto* slots = storage_.slots();

        auto h = header->head.load(std::memory_order_relaxed);
        auto t = header->tail.load(std::memory_order_acquire);

        if (h - t >= N) {
            // Buffer full, overwrite oldest element at t % N
            header->overflow_count.fetch_add(1, std::memory_order_relaxed);

            // Advance tail first to prevent consumer from reading this slot
            header->tail.store(t + 1, std::memory_order_release);

            // Now safe to destroy/overwrite at the old position
            if constexpr (!std::is_trivially_destructible_v<T>) {
                reinterpret_cast<T*>(&slots[t % N])->~T();
            }
            new(&slots[t % N]) T(std::forward<U>(value));

            // Finally advance head
            header->head.store(h + 1, std::memory_order_release);
        } else {
            // Buffer not full, normal push
            new(&slots[h % N]) T(std::forward<U>(value));
            header->head.store(h + 1, std::memory_order_release);
        }
    }

    // Try to dequeue an element. Returns false if the buffer is empty or invalid.
    bool try_pop(T& result) noexcept(std::is_nothrow_move_assignable<T>::value) {
        if (!storage_.valid()) {
            return false;
        }

        auto* header = storage_.header();
        auto* slots = storage_.slots();

        auto t = header->tail.load(std::memory_order_acquire);  // Fixed: acquire to sync with push_overwrite's release
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

    // Access the oldest element. UB if empty OR invalid.
    // WARNING: Not safe to call concurrently with push_overwrite()
    [[nodiscard]] reference front() noexcept {
        // UB if empty OR invalid
        assert(storage_.valid() && "Cannot access invalid buffer");

        auto* header = storage_.header();
        auto* slots = storage_.slots();
        return *reinterpret_cast<pointer>(&slots[header->tail.load(std::memory_order_acquire) % N]);
    }
    [[nodiscard]] const_reference front() const noexcept {
        return const_cast<self_type*>(this)->front();
    }

    // Access the newest element. UB if empty OR invalid.
    // WARNING: Not safe to call concurrently with push_overwrite()
    [[nodiscard]] reference back() noexcept {
        // UB if empty OR invalid
        assert(storage_.valid() && "Cannot access invalid buffer");

        auto* header = storage_.header();
        auto* slots = storage_.slots();
        return *reinterpret_cast<pointer>(&slots[(header->head.load(std::memory_order_acquire) - 1) % N]);
    }
    [[nodiscard]] const_reference back() const noexcept {
        return const_cast<self_type*>(this)->back();
    }

    // Returns true if the buffer is empty or invalid.
    [[nodiscard]] bool empty() const noexcept {
        if (!storage_.valid()) {
            return true;
        }

        auto* header = storage_.header();
        return header->head.load(std::memory_order_acquire) ==
               header->tail.load(std::memory_order_acquire);
    }

    // Returns true if the buffer is full, false if not full or invalid.
    [[nodiscard]] bool full() const noexcept {
        if (!storage_.valid()) {
            return false;
        }

        auto* header = storage_.header();
        return header->head.load(std::memory_order_acquire) -
               header->tail.load(std::memory_order_acquire) >= N;
    }

    // Snapshot of current element count. Returns 0 if invalid.
    [[nodiscard]] size_type size() const noexcept {
        if (!storage_.valid()) {
            return 0;
        }

        auto* header = storage_.header();
        return header->head.load(std::memory_order_acquire) -
               header->tail.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr size_type capacity() noexcept { return N; }

    void clear() noexcept {
        // Guard against invalid storage (e.g., failed ShmStorage construction)
        if (!storage_.valid()) {
            return;
        }

        auto* header = storage_.header();
        auto* slots = storage_.slots();

        // First, advance tail to head to prevent consumer from accessing elements
        auto h = header->head.load(std::memory_order_relaxed);
        auto t = header->tail.load(std::memory_order_acquire);

        if (t != h) {
            header->tail.store(h, std::memory_order_release);

            // Now safe to destroy elements (consumer won't access)
            if constexpr (!std::is_trivially_destructible_v<T>) {
                while (t != h) {
                    reinterpret_cast<pointer>(&slots[t % N])->~T();
                    ++t;
                }
            }
        }

        // Finally reset both head and tail to 0
        header->head.store(0, std::memory_order_release);
        header->tail.store(0, std::memory_order_release);
    }

    // Statistics. Returns 0 if invalid.
    [[nodiscard]] uint64_t overflow_count() const noexcept {
        if (!storage_.valid()) {
            return 0;
        }

        auto* header = storage_.header();
        return header->overflow_count.load(std::memory_order_relaxed);
    }

    void reset_stats() noexcept {
        if (!storage_.valid()) {
            return;
        }

        auto* header = storage_.header();
        header->overflow_count.store(0, std::memory_order_relaxed);
    }

    // Storage status
    bool valid() const noexcept { return storage_.valid(); }
    bool is_creator() const noexcept { return storage_.is_creator(); }

private:
    storage_policy storage_;
};

} // namespace buffers

#endif

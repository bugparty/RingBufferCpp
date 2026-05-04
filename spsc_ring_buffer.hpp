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
    element_type* slots() noexcept { return region_->slots; }
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
    element_type* slots() noexcept { return region_ ? region_->slots : nullptr; }
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

template<typename T, size_t N>
class spsc_ring_buffer {
    static_assert(N > 0, "SPSC ring buffer size must be greater than zero.");

    using self_type = spsc_ring_buffer;
    using storage_type = typename std::aligned_storage<sizeof(T), alignof(T)>::type;

public:
    using value_type = T;
    using reference = T&;
    using const_reference = T const&;
    using pointer = T*;
    using const_pointer = T const*;
    using size_type = size_t;

    spsc_ring_buffer() noexcept = default;

    spsc_ring_buffer(spsc_ring_buffer const&) = delete;
    spsc_ring_buffer& operator=(spsc_ring_buffer const&) = delete;
    spsc_ring_buffer(spsc_ring_buffer&&) = delete;
    spsc_ring_buffer& operator=(spsc_ring_buffer&&) = delete;

    ~spsc_ring_buffer() { clear(); }

    // Try to enqueue an element. Returns false if the buffer is full.
    template<typename U>
    bool try_push(U&& value) noexcept(std::is_nothrow_constructible<T, U&&>::value) {
        auto h = head_.load(std::memory_order_relaxed);
        auto t = tail_.load(std::memory_order_acquire);

        if (h - t >= N)
            return false;

        new(&elements_[h % N]) T(std::forward<U>(value));
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Try to dequeue an element. Returns false if the buffer is empty.
    bool try_pop(T& result) noexcept(std::is_nothrow_move_assignable<T>::value) {
        auto t = tail_.load(std::memory_order_relaxed);
        auto h = head_.load(std::memory_order_acquire);

        if (t == h)
            return false;

        auto idx = t % N;
        result = std::move(*reinterpret_cast<pointer>(&elements_[idx]));
        destroy(idx);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Access the oldest element. UB if empty.
    [[nodiscard]] reference front() noexcept {
        return *reinterpret_cast<pointer>(&elements_[tail_.load(std::memory_order_relaxed) % N]);
    }
    [[nodiscard]] const_reference front() const noexcept {
        return const_cast<self_type*>(this)->front();
    }

    // Access the newest element. UB if empty.
    [[nodiscard]] reference back() noexcept {
        return *reinterpret_cast<pointer>(&elements_[(head_.load(std::memory_order_relaxed) - 1) % N]);
    }
    [[nodiscard]] const_reference back() const noexcept {
        return const_cast<self_type*>(this)->back();
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool full() const noexcept {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire) >= N;
    }

    // Snapshot of current element count.
    [[nodiscard]] size_type size() const noexcept {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr size_type capacity() noexcept { return N; }

    void clear() noexcept {
        auto t = tail_.load(std::memory_order_relaxed);
        auto h = head_.load(std::memory_order_acquire);
        while (t != h) {
            destroy(t % N);
            ++t;
        }
        tail_.store(t, std::memory_order_release);
    }

private:
    void destroy(size_type index) noexcept {
        destroy(index, std::is_trivially_destructible<T>{});
    }
    void destroy(size_type, std::true_type) noexcept {}
    void destroy(size_type index, std::false_type) noexcept {
        reinterpret_cast<pointer>(&elements_[index])->~T();
    }

    storage_type elements_[N]{};
    alignas(64) std::atomic<size_type> head_{};
    alignas(64) std::atomic<size_type> tail_{};
};

} // namespace buffers

#endif

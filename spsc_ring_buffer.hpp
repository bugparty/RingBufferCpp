//
// Single-Producer Single-Consumer lock-free ring buffer.
//
// Uses std::atomic with acquire/release memory ordering for synchronization.
// Head and tail are on separate cache lines to avoid false sharing.
// Monotonic counters (no modular wraparound of indices) with % N for slot access.
//

#pragma once

#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace buffers {

namespace detail {

// Ring buffer header stored in shared memory
struct ring_buffer_header {
    uint32_t schema_version{0};           // User-defined schema version for compatibility
    std::atomic<uint32_t> capacity{0};    // Atomic for proper synchronization with sequence initialization
    std::atomic<uint64_t> writer_pid{0};  // PID of the creator process (for debugging)
    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint64_t> tail{0};
    alignas(64) std::atomic<uint64_t> overflow_count{0};
};

// Ring buffer region: header + sequence array + slots
// The sequence array provides per-slot versioning to detect overwrites in push_overwrite mode.
template<typename T, size_t N>
struct ring_buffer_region {
    ring_buffer_header header;
    std::atomic<uint64_t> sequence[N];  // Per-slot sequence for overwrite detection
    typename std::aligned_storage<sizeof(T), alignof(T)>::type slots[N];
};

} // namespace detail

// Heap storage policy (default)
template<typename T, size_t N>
class heap_storage {
public:
    using header_type = detail::ring_buffer_header;
    using element_type = typename std::aligned_storage<sizeof(T), alignof(T)>::type;
    using region_type = detail::ring_buffer_region<T, N>;
    using sequence_type = std::atomic<uint64_t>;

private:
    region_type* region_ = nullptr;

public:
    explicit heap_storage(uint32_t schema_version = 0) : region_(new region_type{}) {
        // Initialize sequence array to 0
        for (size_t i = 0; i < N; ++i) {
            new(&region_->sequence[i]) std::atomic<uint64_t>(0);
        }
        region_->header.schema_version = schema_version;
        region_->header.capacity.store(N, std::memory_order_release);
        region_->header.writer_pid.store(static_cast<uint64_t>(::getpid()), std::memory_order_relaxed);
    }

    ~heap_storage() {
        delete region_;
    }

    heap_storage(heap_storage const&) = delete;
    heap_storage& operator=(heap_storage const&) = delete;
    heap_storage(heap_storage&&) = delete;
    heap_storage& operator=(heap_storage&&) = delete;

    header_type* header() noexcept { return &region_->header; }
    const header_type* header() const noexcept { return &region_->header; }
    element_type* slots() noexcept { return region_->slots; }
    const element_type* slots() const noexcept { return region_->slots; }
    sequence_type* sequence() noexcept { return region_->sequence; }
    const sequence_type* sequence() const noexcept { return region_->sequence; }
    bool valid() const noexcept { return region_ != nullptr; }
    bool is_creator() const noexcept { return true; }
    static constexpr bool is_shared() noexcept { return false; }
    uint32_t schema_version() const noexcept { return region_->header.schema_version; }
    bool is_compatible() const noexcept { return true; }  // Heap is always compatible
};

// Shared memory open mode
enum class shm_open_mode {
    create,           // Create new, fail if exists
    open,             // Open existing, fail if not found
    create_or_open    // Create if not exists, open if exists (default)
};

// Shared memory storage policy
template<typename T, size_t N>
class shm_storage {
public:
    using header_type = detail::ring_buffer_header;
    using element_type = typename std::aligned_storage<sizeof(T), alignof(T)>::type;
    using region_type = detail::ring_buffer_region<T, N>;
    using sequence_type = std::atomic<uint64_t>;

private:
    std::string shm_name_;
    int fd_ = -1;
    region_type* region_ = nullptr;
    bool is_creator_ = false;
    uint32_t schema_version_ = 0;

public:
    explicit shm_storage(const char* name,
                       uint32_t schema_version,
                       shm_open_mode mode = shm_open_mode::create_or_open);

    // Overload for backward compatibility (schema_version defaults to 0)
    explicit shm_storage(const char* name,
                       shm_open_mode mode)
        : shm_storage(name, 0, mode) {}

    ~shm_storage();

    shm_storage(shm_storage const&) = delete;
    shm_storage& operator=(shm_storage const&) = delete;
    shm_storage(shm_storage&&) = delete;
    shm_storage& operator=(shm_storage&&) = delete;

    header_type* header() noexcept { return region_ ? &region_->header : nullptr; }
    const header_type* header() const noexcept { return region_ ? &region_->header : nullptr; }
    element_type* slots() noexcept { return region_ ? region_->slots : nullptr; }
    const element_type* slots() const noexcept { return region_ ? region_->slots : nullptr; }
    sequence_type* sequence() noexcept { return region_ ? region_->sequence : nullptr; }
    const sequence_type* sequence() const noexcept { return region_ ? region_->sequence : nullptr; }
    bool valid() const noexcept { return region_ != nullptr; }
    bool is_creator() const noexcept { return is_creator_; }
    static constexpr bool is_shared() noexcept { return true; }
    uint32_t schema_version() const noexcept { return schema_version_; }
    bool is_compatible() const noexcept {
        return region_ && region_->header.schema_version == schema_version_;
    }
};

template<typename T, size_t N>
shm_storage<T, N>::shm_storage(const char* name, uint32_t schema_version, shm_open_mode mode)
    : shm_name_(name), schema_version_(schema_version) {

    // Validate name
    if (!name || name[0] != '/') {
        return;
    }

    int flags = O_RDWR;
    bool try_exclusive_create = false;

    switch (mode) {
        case shm_open_mode::create:
            flags |= O_CREAT | O_EXCL;
            break;
        case shm_open_mode::open:
            // Only O_RDWR
            break;
        case shm_open_mode::create_or_open:
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
        if (mode == shm_open_mode::create) {
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
    } else {
        // Non-creator: wait for creator to resize the shared memory
        // The creator must ftruncate before we can safely mmap
        struct stat st;
        constexpr int max_retries = 100;  // ~100ms max wait
        for (int i = 0; i < max_retries; ++i) {
            if (fstat(fd_, &st) == 0 && st.st_size >= static_cast<off_t>(sizeof(region_type))) {
                break;  // Creator has resized, proceed
            }
            // Brief sleep before retry (1ms)
            usleep(1000);
        }
        // Final check before proceeding
        if (fstat(fd_, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(region_type))) {
            // Creator failed to resize in time
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

    // Creator initializes header by constructing atomic objects
    if (is_creator_) {
        // Static_assert: shared memory atomics must be trivially destructible
        // since we placement-new over them without explicit destruction.
        static_assert(std::is_trivially_destructible_v<detail::ring_buffer_header>,
                      "ring_buffer_header must be trivially destructible for shared memory");
        static_assert(std::is_trivially_destructible_v<std::atomic<uint64_t>>,
                      "std::atomic<uint64_t> must be trivially destructible for shared memory");

        // Construct the header with placement new to start atomic object lifetimes
        new(&region_->header) detail::ring_buffer_header();
        region_->header.schema_version = schema_version_;
        region_->header.writer_pid.store(static_cast<uint64_t>(::getpid()), std::memory_order_relaxed);
        // Initialize sequence array to 0
        for (size_t i = 0; i < N; ++i) {
            new(&region_->sequence[i]) std::atomic<uint64_t>(0);
        }
        // Set capacity LAST with release semantics to publish initialization completion
        // The release store synchronizes with the acquire load in the opener
        region_->header.capacity.store(N, std::memory_order_release);
    } else {
        // Wait for creator to finish initialization
        // Spin until capacity is set (indicates creator finished)
        // Bounded wait to prevent hanging if creator is unresponsive
        constexpr int max_retries = 100;  // ~100ms max wait
        int retries = 0;
        uint32_t cap = 0;
        while ((cap = region_->header.capacity.load(std::memory_order_acquire)) == 0 && retries < max_retries) {
            std::this_thread::yield();
            usleep(1000);  // 1ms sleep
            ++retries;
        }

        // Check if initialization completed
        if (cap == 0) {
            // Creator failed to initialize in time
            munmap(region_, sizeof(region_type));
            region_ = nullptr;
            close(fd_);
            fd_ = -1;
            return;
        }

        // Verify capacity matches expected size
        if (cap != N) {
            // Capacity mismatch - unmap and fail
            munmap(region_, sizeof(region_type));
            region_ = nullptr;
            close(fd_);
            fd_ = -1;
            return;
        }

        // Verify schema version compatibility
        if (region_->header.schema_version != schema_version_) {
            // Schema mismatch - close and fail
            munmap(region_, sizeof(region_type));
            region_ = nullptr;
            close(fd_);
            fd_ = -1;
            return;
        }
    }
}

template<typename T, size_t N>
shm_storage<T, N>::~shm_storage() {
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
    template<typename, size_t> class StoragePolicy = heap_storage
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

    // Default constructor (for heap_storage)
    template<typename S = StoragePolicy<T, N>,
             typename = std::enable_if_t<std::is_default_constructible_v<S>>>
    spsc_ring_buffer() : storage_() {}

    // Constructor with schema version (for heap_storage with version)
    template<typename S = StoragePolicy<T, N>,
             typename = std::enable_if_t<std::is_default_constructible_v<S>>>
    explicit spsc_ring_buffer(uint32_t schema_version) : storage_(schema_version) {}

    // Constructor with arguments (for shm_storage)
    template<typename... Args>
    explicit spsc_ring_buffer(Args&&... args)
        : storage_(std::forward<Args>(args)...) {}

    spsc_ring_buffer(spsc_ring_buffer const&) = delete;
    spsc_ring_buffer& operator=(spsc_ring_buffer const&) = delete;
    spsc_ring_buffer(spsc_ring_buffer&&) = delete;
    spsc_ring_buffer& operator=(spsc_ring_buffer&&) = delete;

    ~spsc_ring_buffer() {
        // Only call clear() for non-shared storage (heap/local memory)
        // For shared storage, we must not reset head/tail as other processes may be using it
        if (!storage_policy::is_shared()) {
            clear();
        }
    }

    // Try to enqueue an element. Returns false if the buffer is full or invalid.
    template<typename U>
    bool try_push(U&& value) noexcept(std::is_nothrow_constructible<T, U&&>::value) {
        if (!storage_.valid()) {
            return false;
        }

        auto* header = storage_.header();
        auto* slots = storage_.slots();
        auto* sequence = storage_.sequence();

        auto h = header->head.load(std::memory_order_relaxed);
        auto t = header->tail.load(std::memory_order_acquire);

        if (h - t >= N) {
            header->overflow_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        auto idx = h % N;
        new(&slots[idx]) T(std::forward<U>(value));
        // Set sequence to the new head value (which will be the value consumer's t+1 should match)
        // After this store, consumer with t=h can verify sequence[idx] == t+1 == h+1
        sequence[idx].store(h + 1, std::memory_order_release);
        header->head.store(h + 1, std::memory_order_release);
        return true;
    }

    // Overwrite mode: always succeeds, overwrites oldest if full. No-op if invalid.
    //
    // ⚠️  THREAD SAFETY WARNING ⚠️
    //
    // For non-trivially copyable types (e.g., std::string, std::vector), concurrent
    // calls to push_overwrite() and try_pop() create DATA RACES and UNDEFINED BEHAVIOR.
    //
    // SAFE USAGE:
    // - Single-threaded usage with any type
    // - Concurrent usage with trivially copyable types only (int, float, POD structs)
    // - External synchronization for non-trivial types
    //
    // For concurrent usage with non-trivial types, use try_push() instead or ensure
    // external synchronization (e.g., mutex).
    template<typename U>
    void push_overwrite(U&& value) noexcept(std::is_nothrow_constructible<T, U&&>::value) {
        if (!storage_.valid()) {
            return;
        }

        auto* header = storage_.header();
        auto* slots = storage_.slots();
        auto* sequence = storage_.sequence();

        auto h = header->head.load(std::memory_order_relaxed);
        auto t = header->tail.load(std::memory_order_acquire);

        if (h - t >= N) {
            // Buffer full, overwrite oldest element at t % N
            header->overflow_count.fetch_add(1, std::memory_order_relaxed);

            auto idx = t % N;

            // For trivial types, we can safely overwrite in-place:
            // 1. Overwrite the slot (safe for trivial types - just memcpy)
            // 2. Update sequence to publish the new value
            // Consumer will see updated sequence and skip this slot
            new(&slots[idx]) T(std::forward<U>(value));
            sequence[idx].store(h + 1, std::memory_order_release);

            // Advance head
            header->head.store(h + 1, std::memory_order_release);

            // Advance tail with CAS to maintain size invariant.
            auto expected_tail = t;
            header->tail.compare_exchange_strong(expected_tail, t + 1,
                std::memory_order_release, std::memory_order_relaxed);
        } else {
            // Buffer not full, normal push
            auto idx = h % N;
            new(&slots[idx]) T(std::forward<U>(value));
            sequence[idx].store(h + 1, std::memory_order_release);
            header->head.store(h + 1, std::memory_order_release);
        }
    }

    // Try to dequeue an element. Returns false if the buffer is empty or invalid.
    // When concurrent with push_overwrite, skips overwritten slots internally.
    bool try_pop(T& result) noexcept(std::is_nothrow_move_assignable<T>::value) {
        if (!storage_.valid()) {
            return false;
        }

        auto* header = storage_.header();
        auto* slots = storage_.slots();
        auto* sequence = storage_.sequence();

        while (true) {
            auto t = header->tail.load(std::memory_order_acquire);
            auto h = header->head.load(std::memory_order_acquire);

            if (t == h)
                return false;  // Buffer is empty

            auto idx = t % N;
            uint64_t expected_seq = t + 1;
            uint64_t actual_seq = sequence[idx].load(std::memory_order_acquire);

            if (actual_seq != expected_seq) {
                // Slot was overwritten (actual_seq > expected_seq). The producer
                // overwrote this slot. Just advance tail to skip - we missed this element.
                header->tail.store(t + 1, std::memory_order_release);
                continue;
            }

            // Sequence matches — slot should contain valid data for position t.
            result = std::move(*reinterpret_cast<pointer>(&slots[idx]));

            // Re-check sequence to detect concurrent overwrite during the read.
            if (sequence[idx].load(std::memory_order_acquire) != expected_seq) {
                // Producer overwrote this slot while we were reading.
                // For trivial types, this is safe (just a copy). For non-trivial types,
                // this is a potential issue, but the static_assert in push_overwrite
                // prevents concurrent usage with non-trivial types.
                header->tail.store(t + 1, std::memory_order_release);
                continue;
            }

            // Read was valid. Destruct and advance tail.
            if constexpr (!std::is_trivially_destructible_v<T>) {
                reinterpret_cast<pointer>(&slots[idx])->~T();
            }

            header->tail.store(t + 1, std::memory_order_release);
            return true;
        }
    }

    // Access the oldest element. UB if empty. Returns nullptr if invalid.
    // WARNING: Not safe to call concurrently with push_overwrite()
    [[nodiscard]] pointer front_ptr() noexcept {
        if (!storage_.valid()) {
            return nullptr;
        }

        auto* header = storage_.header();
        auto* slots = storage_.slots();
        auto t = header->tail.load(std::memory_order_acquire);
        auto h = header->head.load(std::memory_order_acquire);
        if (t == h) {
            return nullptr;  // Empty
        }
        return reinterpret_cast<pointer>(&slots[t % N]);
    }
    [[nodiscard]] const_pointer front_ptr() const noexcept {
        return const_cast<self_type*>(this)->front_ptr();
    }

    // Access the newest element. UB if empty. Returns nullptr if invalid.
    // WARNING: Not safe to call concurrently with push_overwrite()
    [[nodiscard]] pointer back_ptr() noexcept {
        if (!storage_.valid()) {
            return nullptr;
        }

        auto* header = storage_.header();
        auto* slots = storage_.slots();
        auto h = header->head.load(std::memory_order_acquire);
        auto t = header->tail.load(std::memory_order_acquire);
        if (t == h) {
            return nullptr;  // Empty
        }
        return reinterpret_cast<pointer>(&slots[(h - 1) % N]);
    }
    [[nodiscard]] const_pointer back_ptr() const noexcept {
        return const_cast<self_type*>(this)->back_ptr();
    }

    // Access the oldest element. UB if empty OR invalid.
    // WARNING: Not safe to call concurrently with push_overwrite()
    // Prefer front_ptr() for safe access with validity check.
    [[nodiscard]] reference front() noexcept {
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
    // Prefer back_ptr() for safe access with validity check.
    [[nodiscard]] reference back() noexcept {
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
        // Guard against invalid storage (e.g., failed shm_storage construction)
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
    uint32_t schema_version() const noexcept { return storage_.schema_version(); }
    bool is_compatible() const noexcept { return storage_.is_compatible(); }

private:
    storage_policy storage_;
};

// Backward-compatible type aliases (CamelCase -> snake_case)
using RingBufferHeader = detail::ring_buffer_header;
template<typename T, size_t N>
using RingBufferRegion = detail::ring_buffer_region<T, N>;
template<typename T, size_t N>
using HeapStorage = heap_storage<T, N>;
template<typename T, size_t N>
using ShmStorage = shm_storage<T, N>;
using ShmOpenMode = shm_open_mode;

} // namespace buffers

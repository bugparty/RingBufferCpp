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
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

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

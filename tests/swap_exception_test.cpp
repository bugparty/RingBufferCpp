#include "test_common.hpp"
#include <exception>

struct ThrowingMove {
    int value;
    static int move_count;
    static int throw_at;

    explicit ThrowingMove(int v = 0) : value(v) {}
    
    ThrowingMove(const ThrowingMove& other) = default;
    
    ThrowingMove(ThrowingMove&& other) noexcept(false) : value(other.value) {
        if (++move_count == throw_at) {
            throw std::runtime_error("Simulated move exception");
        }
    }

    ThrowingMove& operator=(const ThrowingMove&) = default;
    ThrowingMove& operator=(ThrowingMove&&) = default;
};

int ThrowingMove::move_count = 0;
int ThrowingMove::throw_at = -1;

TEST(RingBufferSwapTest, SwapInconsistentOnException) {
    ring_buffer<ThrowingMove, 3> b1;
    ring_buffer<ThrowingMove, 3> b2;

    b1.push_back(ThrowingMove(1));
    b1.push_back(ThrowingMove(2));
    
    b2.push_back(ThrowingMove(3));
    b2.push_back(ThrowingMove(4));

    // Now swap_impl has changed.
    // It uses std::swap to swap elements, and ThrowingMove's move constructor throws exceptions.
    // std::swap(a, b) is typically implemented as: T temp(std::move(a)); a = std::move(b); b = std::move(temp);
    // In our ThrowingMove, the move constructor throws an exception when move_count == throw_at.
    
    ThrowingMove::move_count = 0;
    ThrowingMove::throw_at = 1; 

    try {
        b1.swap(b2);
        // If std::swap uses the move constructor, it should throw an exception here.
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "Simulated move exception");
    }

    std::cout << "b1 size: " << b1.size() << std::endl;
    std::cout << "b2 size: " << b2.size() << std::endl;
    for (auto const& x : b1) std::cout << "b1 val: " << x.value << " "; std::cout << std::endl;
    for (auto const& x : b2) std::cout << "b2 val: " << x.value << " "; std::cout << std::endl;

    // If swap provides strong exception safety, b1 and b2 should remain unchanged.
    // But std::swap for individual elements does not provide strong exception safety
    // for the entire container, unless we catch exceptions and roll back.
}

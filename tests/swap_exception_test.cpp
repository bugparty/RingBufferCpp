#include "test_common.hpp"
#include <exception>

struct ThrowingMove {
    int value;
    static int move_count;
    static int throw_at;

    explicit ThrowingMove(int v = 0) : value(v) {}
    
    ThrowingMove(const ThrowingMove& other) : value(other.value) {}
    
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

    // move_impl (std::false_type) 会循环移动所有元素。
    // swap_impl 的逻辑是：
    // 1. temp.move_impl(*this, std::false_type{});  -> 移动 b1 到 temp
    // 2. this->move_impl(rhs, std::false_type{});   -> 移动 b2 到 b1
    // 3. rhs.move_impl(temp, std::false_type{});    -> 移动 temp 到 b2

    // 我们让第 3 次移动操作抛出异常（即 b2 的第一个元素移动时，或者更早）
    // b1 有 2 个元素，b2 有 2 个元素。
    // 第一步 temp.move_impl(*this) 会发生 2 次移动。
    // 第二步 this->move_impl(rhs) 会发生 2 次移动。
    // 我们设置在第 3 次移动时抛出异常，这应该发生在第二步的第一个元素移动时。
    
    ThrowingMove::move_count = 0;
    ThrowingMove::throw_at = 3; 

    try {
        b1.swap(b2);
        FAIL() << "Should have thrown an exception";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "Simulated move exception");
    }

    // 此时 b1 的状态应该是损坏的。
    // 第一步之后，b1 被 clear 了，数据在 temp 中。
    // 第二步开始移动 b2 到 b1，但在第一个元素时就抛出了异常。
    // move_impl 内部有 try-catch，如果抛出异常会调用 clear/destroy 逻辑。
    // 查看 RingBuffer.hpp 的 move_impl (std::false_type):
    /*
    try {
        for (auto i = 0; i < size_; ++i)
            new( elements_ + ((tail_ + i) % N)) T(std::move(rhs[(tail_ + i) % N]));
    }catch(...) {
        while(!empty()) {
            destroy(tail_, bool_constant<std::is_trivially_destructible_v<value_type>>{});
            tail_ = (tail_ + 1) % N;
            --size_;
        }
        throw;
    }
    */
    // 注意：this->move_impl(rhs) 中，this 的 size_ 已经被设置为 rhs.size_ 了！
    // 如果在循环中抛出异常，catch 块会尝试 destroy 所有的 size_ 个元素，即使有的还没构造！
    
    // 复现不一致性：
    // b1 应该是空的（或者部分填充但被 catch 块清空了），
    // 但原先 b1 的数据在 temp 中，而 temp 是 swap_impl 的局部变量，已经丢失了！
    // b2 的数据也可能处于不确定状态，因为有些元素已经被 move 走了。

    std::cout << "b1 size: " << b1.size() << std::endl;
    std::cout << "b2 size: " << b2.size() << std::endl;

    // 验证不一致性：
    // 在异常发生后，swap 应该保证强异常安全性（即要么交换成功，要么保持原样）。
    // 但目前的实现导致 b1 变为空，而 b2 保持原样（部分元素可能已被 move 构造但未失效）。
    // 甚至如果 move_impl 抛出异常，b1 的 size_ 已经被设置但元素可能未被构造。
    
    // 原始状态：b1={1,2}, b2={3,4}
    // 期望：如果抛出异常，b1={1,2}, b2={3,4}
    // 实际：b1.size() == 0, b2.size() == 2
    EXPECT_EQ(b1.size(), 2) << "b1 should still have 2 elements after failed swap";
    EXPECT_EQ(b2.size(), 2) << "b2 should still have 2 elements after failed swap";
}

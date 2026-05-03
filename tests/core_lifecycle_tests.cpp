#include "test_common.hpp"

#include <algorithm>
#include <vector>

TEST(RingBufferTest, Test2) {
    ring_buffer<int, 3> b1;
    for (int i = 0; i < 10; i++) {
        b1.push_back(i);
        EXPECT_EQ(b1.front(), std::max(0, i - 2));
    }
}

TEST(RingBufferTest, Test3) {
    ring_buffer<int, 3> b1;
    b1.push_back(1);
    b1.push_back(2);
    b1.push_back(3);
    EXPECT_EQ(b1.front(), 1);
    EXPECT_EQ(b1.back(), 3);
    b1.push_back(4); // This should overwrite the first element (1)

    EXPECT_EQ(b1.front(), 2); // Oldest element is now 2
    EXPECT_EQ(b1.back(), 4);  // Newest element is now 4
    b1.pop_front(); // Remove the first element (2)
    EXPECT_EQ(b1.front(), 3); // Oldest element is now 3
    EXPECT_EQ(b1.back(), 4);  // Newest element remains 4
}

TEST(RingBufferTest, Test4) {
    ring_buffer<int, 3> b1;
    b1.push_back(1);
    b1.push_back(2);
    b1.push_back(3);
    b1.pop_front(); // Remove the first element (1)
    b1.push_back(4); // Add 4, buffer now contains {2, 3, 4}

    EXPECT_EQ(b1.front(), 2); // Oldest element is 2
    EXPECT_EQ(b1.back(), 4);
}

TEST(RingBufferTest, Test5) {
    ring_buffer<int, 3> b1;
    b1.push_back(1);
    b1.push_back(2);
    b1.push_back(3);
    b1.clear(); // Clear the buffer

    EXPECT_TRUE(b1.empty()); // Buffer should be empty
    EXPECT_EQ(b1.size(), 0); // Size should be 0
}

TEST(RingBufferTest, NoOverwriteWhenFull) {
    ring_buffer<int, 3, false> b1;
    b1.push_back(1);
    b1.push_back(2);
    b1.push_back(3);
    b1.push_back(4);

    EXPECT_EQ(b1.size(), 3);
    EXPECT_EQ(b1.front(), 1);
    EXPECT_EQ(b1.back(), 3);
}

TEST(RingBufferTest, PopFrontOnEmptyIsNoOp) {
    ring_buffer<int, 2> b1;
    b1.pop_front();

    EXPECT_TRUE(b1.empty());
    EXPECT_EQ(b1.size(), 0);

    b1.push_back(7);
    EXPECT_FALSE(b1.empty());
    EXPECT_EQ(b1.front(), 7);
    EXPECT_EQ(b1.back(), 7);
}

TEST(RingBufferTest, ClearResetsAfterWrap) {
    ring_buffer<int, 3> b1;
    b1.push_back(1);
    b1.push_back(2);
    b1.push_back(3);
    b1.push_back(4);

    b1.clear();

    EXPECT_TRUE(b1.empty());
    EXPECT_EQ(b1.size(), 0);

    b1.push_back(9);
    b1.push_back(10);
    EXPECT_EQ(b1.front(), 9);
    EXPECT_EQ(b1.back(), 10);
}

TEST(RingBufferTest, OverwriteKeepsCapacitySize) {
    ring_buffer<int, 4> b1;
    for (int i = 1; i <= 10; ++i) {
        b1.push_back(i);
    }

    EXPECT_TRUE(b1.full());
    EXPECT_EQ(b1.size(), 4);
    EXPECT_EQ(b1.front(), 7);
    EXPECT_EQ(b1.back(), 10);
}

TEST(RingBufferTest, LargeBufferOverwriteKeepsLastN) {
    ring_buffer<int, 64> b1;
    for (int i = 0; i < 1000; ++i) {
        b1.push_back(i);
    }

    EXPECT_TRUE(b1.full());
    EXPECT_EQ(b1.size(), 64);
    EXPECT_EQ(b1.front(), 936);
    EXPECT_EQ(b1.back(), 999);
}

TEST(RingBufferTest, SingleElementCapacityBehavesCorrectly) {
    ring_buffer<int, 1> b1;
    EXPECT_TRUE(b1.empty());
    EXPECT_EQ(b1.size(), 0);
    EXPECT_EQ(b1.capacity(), 1);

    b1.push_back(10);
    EXPECT_TRUE(b1.full());
    EXPECT_EQ(b1.front(), 10);
    EXPECT_EQ(b1.back(), 10);

    b1.push_back(20);
    EXPECT_TRUE(b1.full());
    EXPECT_EQ(b1.size(), 1);
    EXPECT_EQ(b1.front(), 20);
    EXPECT_EQ(b1.back(), 20);

    b1.pop_front();
    EXPECT_TRUE(b1.empty());
}

#include <gtest/gtest.h>
#include "RingBuffer.hpp"
using namespace buffers;

TEST(RingBufferTest, Test1) {
    ring_buffer<std::vector<int>, 3> b1;

    for(auto i = 0; i < 9; ++i){
        b1.push_back(std::vector<int>{i, i + 1, i + 2});
    }

    auto b2 = b1;

    ring_buffer<std::vector<int>, 3> b3;
    b3 = b2;
    b3.pop_front();

    std::vector<std::vector<int>> expected = {{7, 8, 9}, {8, 9, 10}};
    auto it = b3.cbegin();
    for (const auto& vec : expected) {
        EXPECT_EQ(*it, vec);
        ++it;
    }
}

TEST(RingBufferTest, Test2) {
    ring_buffer<int, 3> b1;
    for(int i = 0; i < 10; i++){
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

TEST(RingBufferTest, Test6IteratorOrder) {
    ring_buffer<int, 3> b1;
    b1.push_back(10);
    b1.push_back(20);
    b1.push_back(30);
    b1.push_back(40); // overwrite 10

    std::vector<int> values;
    for (auto it = b1.cbegin(); it != b1.cend(); ++it) {
        values.push_back(*it);
    }

    std::vector<int> expected = {20, 30, 40};
    EXPECT_EQ(values, expected);
}

TEST(RingBufferTest, Test7ArrowOperator) {
    ring_buffer<std::vector<int>, 2> b1;
    b1.push_back(std::vector<int>{1, 2});
    b1.push_back(std::vector<int>{3, 4});

    auto it = b1.cbegin();
    EXPECT_EQ(it->at(0), 1);
    EXPECT_EQ((++it)->at(1), 4);
}

TEST(RingBufferTest, CopyPreservesWrappedDataForTrivialTypes) {
    ring_buffer<int, 5> b1;
    b1.push_back(1);
    b1.push_back(2);
    b1.push_back(3);
    b1.pop_front();
    b1.push_back(4);
    b1.push_back(5);

    ring_buffer<int, 5> b2 = b1;
    std::vector<int> values;
    for (auto it = b2.cbegin(); it != b2.cend(); ++it) {
        values.push_back(*it);
    }

    std::vector<int> expected = {2, 3, 4, 5};
    EXPECT_EQ(values, expected);
}

TEST(RingBufferTest, IteratorsFromDifferentBuffersAreNotEqual) {
    ring_buffer<int, 3> b1;
    ring_buffer<int, 3> b2;

    // Iterators from different buffers should not be equal
    EXPECT_NE(b1.cbegin(), b2.cbegin());
    
    // Add elements to both buffers
    b1.push_back(1);
    b1.push_back(2);
    b2.push_back(1);
    b2.push_back(2);
    
    // Iterators at the same logical position from different buffers should still be unequal
    EXPECT_NE(b1.cbegin(), b2.cbegin());
    EXPECT_NE(b1.cend(), b2.cend());
    
    // Iterators from the same buffer at the same position should be equal
    EXPECT_EQ(b1.cbegin(), b1.cbegin());
    EXPECT_EQ(b1.cend(), b1.cend());
    
    // begin() and end() from the same buffer should be different when not empty
    EXPECT_NE(b1.cbegin(), b1.cend());
    EXPECT_NE(b2.cbegin(), b2.cend());
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

TEST(RingBufferTest, IteratorOrderAfterWrapAndPop) {
    ring_buffer<int, 4> b1;
    b1.push_back(1);
    b1.push_back(2);
    b1.push_back(3);
    b1.push_back(4);
    b1.pop_front();
    b1.pop_front();
    b1.push_back(5);
    b1.push_back(6);

    std::vector<int> values;
    for (auto it = b1.cbegin(); it != b1.cend(); ++it) {
        values.push_back(*it);
    }

    std::vector<int> expected = {3, 4, 5, 6};
    EXPECT_EQ(values, expected);
}

TEST(RingBufferTest, SelfAssignmentDoesNotCorrupt) {
    ring_buffer<int, 3> b1;
    b1.push_back(1);
    b1.push_back(2);
    b1.push_back(3);

    b1 = b1;

    std::vector<int> values;
    for (auto it = b1.cbegin(); it != b1.cend(); ++it) {
        values.push_back(*it);
    }

    std::vector<int> expected = {1, 2, 3};
    EXPECT_EQ(values, expected);
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

TEST(RingBufferTest, LargeBufferCopyPreservesOrder) {
    ring_buffer<int, 128> b1;
    for (int i = 0; i < 256; ++i) {
        b1.push_back(i);
    }

    ring_buffer<int, 128> b2 = b1;
    std::vector<int> values;
    for (auto it = b2.cbegin(); it != b2.cend(); ++it) {
        values.push_back(*it);
    }

    std::vector<int> expected;
    for (int i = 128; i < 256; ++i) {
        expected.push_back(i);
    }

    EXPECT_EQ(values, expected);
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

TEST(RingBufferTest, MoveConstructionTransfersState) {
    ring_buffer<int, 4> b1;
    b1.push_back(1);
    b1.push_back(2);
    b1.push_back(3);
    b1.pop_front();
    b1.push_back(4);

    ring_buffer<int, 4> b2(std::move(b1));

    EXPECT_TRUE(b1.empty());
    EXPECT_EQ(b2.size(), 3);
    EXPECT_EQ(b2.front(), 2);
    EXPECT_EQ(b2.back(), 4);
}

TEST(RingBufferTest, MoveAssignmentTransfersState) {
    ring_buffer<int, 3> b1;
    b1.push_back(5);
    b1.push_back(6);

    ring_buffer<int, 3> b2;
    b2.push_back(10);
    b2 = std::move(b1);

    EXPECT_TRUE(b1.empty());
    EXPECT_EQ(b2.size(), 2);
    EXPECT_EQ(b2.front(), 5);
    EXPECT_EQ(b2.back(), 6);
}

TEST(RingBufferTest, SwapExchangesBuffers) {
    ring_buffer<int, 3> b1;
    b1.push_back(1);
    b1.push_back(2);

    ring_buffer<int, 3> b2;
    b2.push_back(9);
    b2.push_back(10);
    b2.push_back(11);

    b1.swap(b2);

    EXPECT_EQ(b1.size(), 3);
    EXPECT_EQ(b1.front(), 9);
    EXPECT_EQ(b1.back(), 11);
    EXPECT_EQ(b2.size(), 2);
    EXPECT_EQ(b2.front(), 1);
    EXPECT_EQ(b2.back(), 2);
}

TEST(RingBufferTest, NonMemberSwapWorks) {
    ring_buffer<int, 2> b1;
    b1.push_back(3);
    b1.push_back(4);

    ring_buffer<int, 2> b2;
    b2.push_back(7);

    using std::swap;
    swap(b1, b2);

    EXPECT_EQ(b1.size(), 1);
    EXPECT_EQ(b1.front(), 7);
    EXPECT_EQ(b2.size(), 2);
    EXPECT_EQ(b2.front(), 3);
    EXPECT_EQ(b2.back(), 4);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

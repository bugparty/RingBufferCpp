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
    b1.push_back(4); // This should overwrite the first element (1)

    EXPECT_EQ(b1.front(), 2);
    EXPECT_EQ(b1.back(), 4);
}

TEST(RingBufferTest, Test4) {
    ring_buffer<int, 3> b1;
    b1.push_back(1);
    b1.push_back(2);
    b1.push_back(3);
    b1.pop_front(); // Remove the first element (1)
    b1.push_back(4);

    EXPECT_EQ(b1.front(), 2);
    EXPECT_EQ(b1.back(), 4);
}

TEST(RingBufferTest, Test5) {
    ring_buffer<int, 3> b1;
    b1.push_back(1);
    b1.push_back(2);
    b1.push_back(3);
    b1.clear();

    EXPECT_TRUE(b1.empty());
    EXPECT_EQ(b1.size(), 0);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
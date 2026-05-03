#include "test_common.hpp"

#include <string>
#include <utility>
#include <vector>

TEST(RingBufferTest, Test1) {
    ring_buffer<std::vector<int>, 3> b1;

    for (auto i = 0; i < 9; ++i) {
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

TEST(RingBufferTest, MoveConstructionWithNonTriviallyCopyableType) {
    ring_buffer<std::string, 4> b1;
    b1.push_back("hello");
    b1.push_back("world");
    b1.push_back("test");
    b1.pop_front();
    b1.push_back("data");

    ring_buffer<std::string, 4> b2(std::move(b1));

    EXPECT_TRUE(b1.empty());
    EXPECT_EQ(b2.size(), 3);
    EXPECT_EQ(b2.front(), "world");
    EXPECT_EQ(b2.back(), "data");
}

TEST(RingBufferTest, MoveAssignmentWithNonTriviallyCopyableType) {
    ring_buffer<std::string, 3> b1;
    b1.push_back("foo");
    b1.push_back("bar");

    ring_buffer<std::string, 3> b2;
    b2.push_back("existing");
    b2 = std::move(b1);

    EXPECT_TRUE(b1.empty());
    EXPECT_EQ(b2.size(), 2);
    EXPECT_EQ(b2.front(), "foo");
    EXPECT_EQ(b2.back(), "bar");
}

TEST(RingBufferTest, SwapWithNonTriviallyCopyableType) {
    ring_buffer<std::string, 3> b1;
    b1.push_back("one");
    b1.push_back("two");

    ring_buffer<std::string, 3> b2;
    b2.push_back("alpha");
    b2.push_back("beta");
    b2.push_back("gamma");

    b1.swap(b2);

    EXPECT_EQ(b1.size(), 3);
    EXPECT_EQ(b1.front(), "alpha");
    EXPECT_EQ(b1.back(), "gamma");
    EXPECT_EQ(b2.size(), 2);
    EXPECT_EQ(b2.front(), "one");
    EXPECT_EQ(b2.back(), "two");
}

TEST(RingBufferTest, NonMemberSwapWithNonTriviallyCopyableType) {
    ring_buffer<std::vector<int>, 2> b1;
    b1.push_back(std::vector<int>{1, 2, 3});
    b1.push_back(std::vector<int>{4, 5, 6});

    ring_buffer<std::vector<int>, 2> b2;
    b2.push_back(std::vector<int>{7, 8, 9});

    using std::swap;
    swap(b1, b2);

    EXPECT_EQ(b1.size(), 1);
    EXPECT_EQ(b1.front(), (std::vector<int>{7, 8, 9}));
    EXPECT_EQ(b2.size(), 2);
    EXPECT_EQ(b2.front(), (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(b2.back(), (std::vector<int>{4, 5, 6}));
}

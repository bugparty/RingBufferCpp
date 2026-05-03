#include "test_common.hpp"

#include <iterator>
#include <utility>
#include <vector>

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

TEST(RingBufferTest, IteratorsFromDifferentBuffersAreNotEqual) {
    ring_buffer<int, 3> b1;
    ring_buffer<int, 3> b2;

    EXPECT_NE(b1.cbegin(), b2.cbegin());

    b1.push_back(1);
    b1.push_back(2);
    b2.push_back(1);
    b2.push_back(2);

    EXPECT_NE(b1.cbegin(), b2.cbegin());
    EXPECT_NE(b1.cend(), b2.cend());

    EXPECT_EQ(b1.cbegin(), b1.cbegin());
    EXPECT_EQ(b1.cend(), b1.cend());

    EXPECT_NE(b1.cbegin(), b1.cend());
    EXPECT_NE(b2.cbegin(), b2.cend());
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

TEST(RingBufferIteratorTest, EmptyBufferBeginMatchesEnd) {
    ring_buffer<int, 5> buf;
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.begin(), buf.end());
    EXPECT_EQ(buf.cbegin(), buf.cend());
}

TEST(RingBufferIteratorTest, PrefixIncrementWalksOldestToNewest) {
    ring_buffer<int, 4> buf;
    buf.push_back(1);
    buf.push_back(2);
    buf.push_back(3);

    auto it = buf.begin();
    EXPECT_EQ(it.count(), 0u);
    EXPECT_EQ(*it, 1);
    ++it;
    EXPECT_EQ(it.count(), 1u);
    EXPECT_EQ(*it, 2);
    ++it;
    EXPECT_EQ(*it, 3);
    ++it;
    EXPECT_EQ(it, buf.end());
    EXPECT_EQ(it.index(), buf.capacity());
}

TEST(RingBufferIteratorTest, PrefixIncrementMatchesStdDistance) {
    ring_buffer<int, 10> buf;
    for (int i = 0; i < 7; ++i) {
        buf.push_back(i * 11);
    }
    EXPECT_EQ(std::distance(buf.cbegin(), buf.cend()), 7);
}

TEST(RingBufferIteratorTest, PostfixIncrementReturnsPriorPosition) {
    ring_buffer<int, 5> buf;
    buf.push_back(10);
    buf.push_back(20);

    auto it = buf.begin();
    auto prev = it++;
    EXPECT_EQ(*prev, 10);
    EXPECT_EQ(*it, 20);

    ++it;
    EXPECT_EQ(it, buf.end());
}

TEST(RingBufferIteratorTest, MutableIteratorWritesThroughRefs) {
    ring_buffer<std::pair<int, int>, 3> buf;
    buf.push_back(std::make_pair(0, 0));
    buf.push_back(std::make_pair(0, 0));

    auto it = buf.begin();
    it->first = 42;
    ++it;
    it->second = 99;

    EXPECT_EQ(buf.front().first, 42);
    EXPECT_EQ(buf.back().second, 99);
}

TEST(RingBufferIteratorTest, SamePositionIteratorsCompareEqualDifferentPositionNot) {
    ring_buffer<int, 3> buf;
    buf.push_back(1);
    buf.push_back(2);

    auto it1 = buf.cbegin();
    auto it2 = buf.cbegin();
    EXPECT_EQ(it1, it2);

    ++it1;
    EXPECT_NE(it1, it2);

    ++it2;
    EXPECT_EQ(it1, it2);
}

TEST(RingBufferIteratorTest, ReachEnd_CountEqualsCapacityIndexIsSentinel) {
    ring_buffer<int, 5> buf;
    for (int v : {100, 200, 300}) {
        buf.push_back(v);
    }
    auto e = buf.end();
    EXPECT_EQ(e.count(), 3u);
    EXPECT_EQ(e.index(), buf.capacity());
}

TEST(RingBufferIteratorTest, ClearAfterIterateBeginMatchesEndAgain) {
    ring_buffer<int, 2> buf;
    buf.push_back(1);

    ASSERT_NE(buf.cbegin(), buf.cend());

    buf.clear();
    EXPECT_EQ(buf.cbegin(), buf.cend());
}

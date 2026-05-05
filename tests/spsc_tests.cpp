#include <gtest/gtest.h>
#include "spsc_ring_buffer.hpp"
#include <thread>
#include <atomic>
#include <string>
#include <chrono>

using namespace buffers;

// --- Single-threaded tests ---

TEST(SpscTest, PushPopOrdering) {
    spsc_ring_buffer<int, 8> buf;
    for (int i = 0; i < 5; ++i)
        ASSERT_TRUE(buf.try_push(i));

    for (int i = 0; i < 5; ++i) {
        int val{};
        ASSERT_TRUE(buf.try_pop(val));
        EXPECT_EQ(val, i);
    }
    EXPECT_TRUE(buf.empty());
}

TEST(SpscTest, FullRejects) {
    spsc_ring_buffer<int, 4> buf;
    for (int i = 0; i < 4; ++i)
        ASSERT_TRUE(buf.try_push(i));

    EXPECT_TRUE(buf.full());
    EXPECT_FALSE(buf.try_push(999));
    EXPECT_EQ(buf.size(), 4u);
}

TEST(SpscTest, EmptyRejects) {
    spsc_ring_buffer<int, 4> buf;
    int val{};
    EXPECT_TRUE(buf.empty());
    EXPECT_FALSE(buf.try_pop(val));
}

TEST(SpscTest, FrontBack) {
    spsc_ring_buffer<int, 8> buf;
    buf.try_push(10);
    buf.try_push(20);
    buf.try_push(30);
    EXPECT_EQ(buf.front(), 10);
    EXPECT_EQ(buf.back(), 30);
}

TEST(SpscTest, Wraparound) {
    spsc_ring_buffer<int, 4> buf;
    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 4; ++i)
            ASSERT_TRUE(buf.try_push(round * 4 + i));

        for (int i = 0; i < 4; ++i) {
            int val{};
            ASSERT_TRUE(buf.try_pop(val));
            EXPECT_EQ(val, round * 4 + i);
        }
    }
    EXPECT_TRUE(buf.empty());
}

TEST(SpscTest, Clear) {
    spsc_ring_buffer<int, 8> buf;
    for (int i = 0; i < 5; ++i) buf.try_push(i);
    EXPECT_EQ(buf.size(), 5u);
    buf.clear();
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.size(), 0u);
}

TEST(SpscTest, Capacity) {
    spsc_ring_buffer<int, 16> buf;
    EXPECT_EQ(buf.capacity(), 16u);
}

TEST(SpscTest, SizeAfterPushPop) {
    spsc_ring_buffer<int, 8> buf;
    EXPECT_EQ(buf.size(), 0u);
    buf.try_push(1);
    EXPECT_EQ(buf.size(), 1u);
    buf.try_push(2);
    EXPECT_EQ(buf.size(), 2u);
    int val{};
    buf.try_pop(val);
    EXPECT_EQ(buf.size(), 1u);
}

// --- Non-trivial type ---

struct NonTrivial {
    int val;
    NonTrivial() : val(0) {}
    explicit NonTrivial(int v) : val(v) {}
    NonTrivial(NonTrivial const& o) : val(o.val) {}
    NonTrivial(NonTrivial&& o) noexcept : val(o.val) { o.val = 0; }
    NonTrivial& operator=(NonTrivial&& o) noexcept { val = o.val; o.val = 0; return *this; }
    NonTrivial& operator=(NonTrivial const& o) { val = o.val; return *this; }
    bool operator==(int v) const { return val == v; }
};

TEST(SpscTest, NonTrivialType) {
    spsc_ring_buffer<NonTrivial, 4> buf;
    for (int i = 0; i < 4; ++i)
        ASSERT_TRUE(buf.try_push(NonTrivial(i)));

    for (int i = 0; i < 4; ++i) {
        NonTrivial val;
        ASSERT_TRUE(buf.try_pop(val));
        EXPECT_EQ(val, i);
    }
    EXPECT_TRUE(buf.empty());
}

TEST(SpscTest, StringType) {
    spsc_ring_buffer<std::string, 4> buf;
    buf.try_push("hello");
    buf.try_push("world");
    std::string s;
    ASSERT_TRUE(buf.try_pop(s));
    EXPECT_EQ(s, "hello");
    ASSERT_TRUE(buf.try_pop(s));
    EXPECT_EQ(s, "world");
}

// --- Multi-threaded tests ---

TEST(SpscTest, ConcurrentSPSC) {
    spsc_ring_buffer<int, 1024> buf;
    constexpr int count = 100000;
    std::atomic<int> errors{0};

    std::thread producer([&buf]() {
        for (int i = 0; i < count; ++i) {
            int attempts = 0;
            while (!buf.try_push(i)) {
                if (++attempts > 10000000) { ADD_FAILURE() << "Timeout"; break; }
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&buf, &errors]() {
        for (int i = 0; i < count; ++i) {
            int val{};
            int attempts = 0;
            while (!buf.try_pop(val)) {
                if (++attempts > 10000000) { ADD_FAILURE() << "Timeout"; break; }
                std::this_thread::yield();
            }
            if (val != i) ++errors;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(errors.load(), 0);
    EXPECT_TRUE(buf.empty());
}

TEST(SpscTest, ConcurrentSmallBuffer) {
    spsc_ring_buffer<int, 4> buf;
    constexpr int count = 1000000;
    std::atomic<int> errors{0};

    std::thread producer([&buf]() {
        for (int i = 0; i < count; ++i) {
            int attempts = 0;
            while (!buf.try_push(i)) {
                if (++attempts > 10000000) { ADD_FAILURE() << "Timeout"; break; }
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&buf, &errors]() {
        for (int i = 0; i < count; ++i) {
            int val{};
            int attempts = 0;
            while (!buf.try_pop(val)) {
                if (++attempts > 10000000) { ADD_FAILURE() << "Timeout"; break; }
                std::this_thread::yield();
            }
            if (val != i) ++errors;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(errors.load(), 0);
    EXPECT_TRUE(buf.empty());
}

TEST(SpscTest, ConcurrentNonTrivial) {
    spsc_ring_buffer<std::string, 64> buf;
    constexpr int count = 10000;
    std::atomic<int> errors{0};

    std::thread producer([&buf]() {
        for (int i = 0; i < count; ++i) {
            int attempts = 0;
            while (!buf.try_push(std::to_string(i))) {
                if (++attempts > 10000000) { ADD_FAILURE() << "Timeout"; break; }
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&buf, &errors]() {
        for (int i = 0; i < count; ++i) {
            std::string val;
            int attempts = 0;
            while (!buf.try_pop(val)) {
                if (++attempts > 10000000) { ADD_FAILURE() << "Timeout"; break; }
                std::this_thread::yield();
            }
            if (val != std::to_string(i)) ++errors;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(errors.load(), 0);
    EXPECT_TRUE(buf.empty());
}

// --- Timing-dependent concurrent tests ---
// For thorough data-race detection, build with -fsanitize=thread.

TEST(SpscTest, DelayedConsumer) {
    spsc_ring_buffer<int, 16> buf;
    constexpr int count = 10000;
    std::atomic<int> errors{0};

    std::thread producer([&buf]() {
        for (int i = 0; i < count; ++i) {
            int attempts = 0;
            while (!buf.try_push(i)) {
                if (++attempts > 10000000) { ADD_FAILURE() << "Timeout"; break; }
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&buf, &errors]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (int i = 0; i < count; ++i) {
            int val{};
            int attempts = 0;
            while (!buf.try_pop(val)) {
                if (++attempts > 10000000) { ADD_FAILURE() << "Timeout"; break; }
                std::this_thread::yield();
            }
            if (val != i) ++errors;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(errors.load(), 0);
    EXPECT_TRUE(buf.empty());
}

TEST(SpscTest, DelayedProducer) {
    spsc_ring_buffer<int, 16> buf;
    constexpr int count = 10000;
    std::atomic<int> errors{0};

    std::thread producer([&buf]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (int i = 0; i < count; ++i) {
            int attempts = 0;
            while (!buf.try_push(i)) {
                if (++attempts > 10000000) { ADD_FAILURE() << "Timeout"; break; }
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&buf, &errors]() {
        for (int i = 0; i < count; ++i) {
            int val{};
            int attempts = 0;
            while (!buf.try_pop(val)) {
                if (++attempts > 10000000) { ADD_FAILURE() << "Timeout"; break; }
                std::this_thread::yield();
            }
            if (val != i) ++errors;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(errors.load(), 0);
    EXPECT_TRUE(buf.empty());
}

TEST(SpscTest, BurstProduction) {
    spsc_ring_buffer<int, 32> buf;
    constexpr int bursts = 50;
    constexpr int burst_size = 100;
    constexpr int count = bursts * burst_size;
    std::atomic<int> errors{0};

    std::thread producer([&buf]() {
        for (int b = 0; b < bursts; ++b) {
            int base = b * burst_size;
            for (int i = 0; i < burst_size; ++i) {
                int attempts = 0;
                while (!buf.try_push(base + i)) {
                    if (++attempts > 10000000) { ADD_FAILURE() << "Timeout"; break; }
                    std::this_thread::yield();
                }
            }
            if (b < bursts - 1)
                std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    std::thread consumer([&buf, &errors, count]() {
        for (int i = 0; i < count; ++i) {
            int val{};
            int attempts = 0;
            while (!buf.try_pop(val)) {
                if (++attempts > 10000000) { ADD_FAILURE() << "Timeout"; break; }
                std::this_thread::yield();
            }
            if (val != i) ++errors;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(errors.load(), 0);
    EXPECT_TRUE(buf.empty());
}

TEST(SpscTest, MultiFieldStructIntegrity) {
    struct Packet {
        int seq;
        int checksum;
        long timestamp;
        double payload;
    };

    spsc_ring_buffer<Packet, 64> buf;
    constexpr int count = 50000;
    std::atomic<int> errors{0};

    std::thread producer([&buf]() {
        for (int i = 0; i < count; ++i) {
            Packet p{};
            p.seq = i;
            p.checksum = i ^ 0xDEADBEEF;
            p.timestamp = static_cast<long>(i) * 1000;
            p.payload = static_cast<double>(i) * 3.14;
            int attempts = 0;
            while (!buf.try_push(std::move(p))) {
                if (++attempts > 10000000) { ADD_FAILURE() << "Timeout"; break; }
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&buf, &errors]() {
        for (int i = 0; i < count; ++i) {
            Packet val{};
            int attempts = 0;
            while (!buf.try_pop(val)) {
                if (++attempts > 10000000) { ADD_FAILURE() << "Timeout"; break; }
                std::this_thread::yield();
            }
            if (val.seq != i) { ++errors; continue; }
            if (val.checksum != (i ^ 0xDEADBEEF)) { ++errors; continue; }
            if (val.timestamp != static_cast<long>(i) * 1000) { ++errors; continue; }
            if (val.payload != static_cast<double>(i) * 3.14) { ++errors; continue; }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(errors.load(), 0);
    EXPECT_TRUE(buf.empty());
}

TEST(SpscTest, FullCycleBoundary) {
    spsc_ring_buffer<int, 8> buf;
    constexpr int cycles = 5000;
    std::atomic<int> errors{0};

    std::thread producer([&buf]() {
        for (int c = 0; c < cycles; ++c) {
            for (int i = 0; i < 8; ++i) {
                int attempts = 0;
                while (!buf.try_push(c * 8 + i)) {
                    if (++attempts > 10000000) { ADD_FAILURE() << "Timeout"; break; }
                    std::this_thread::yield();
                }
            }
            while (!buf.empty())
                std::this_thread::yield();
        }
    });

    std::thread consumer([&buf, &errors]() {
        for (int c = 0; c < cycles; ++c) {
            for (int i = 0; i < 8; ++i) {
                int val{};
                int attempts = 0;
                while (!buf.try_pop(val)) {
                    if (++attempts > 10000000) { ADD_FAILURE() << "Timeout"; break; }
                    std::this_thread::yield();
                }
                if (val != c * 8 + i) ++errors;
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(errors.load(), 0);
    EXPECT_TRUE(buf.empty());
}

// --- Overwrite mode tests ---

TEST(SpscTest, OverwriteBasic) {
    spsc_ring_buffer<int, 4> buf;

    // Fill buffer
    for (int i = 0; i < 4; ++i)
        buf.push_overwrite(i);

    EXPECT_TRUE(buf.full());
    EXPECT_EQ(buf.size(), 4u);

    // Overwrite should succeed
    buf.push_overwrite(100);
    EXPECT_EQ(buf.size(), 4u);
    EXPECT_EQ(buf.overflow_count(), 1u);

    // Oldest element (0) should be gone, newest should be 100
    int val;
    for (int i = 1; i < 4; ++i) {
        ASSERT_TRUE(buf.try_pop(val));
        EXPECT_EQ(val, i);
    }
    ASSERT_TRUE(buf.try_pop(val));
    EXPECT_EQ(val, 100);
}

TEST(SpscTest, OverwriteMultiple) {
    spsc_ring_buffer<int, 4> buf;

    // Fill and overwrite multiple times
    for (int i = 0; i < 10; ++i)
        buf.push_overwrite(i);

    EXPECT_EQ(buf.overflow_count(), 6u);
    EXPECT_EQ(buf.size(), 4u);

    // Should have last 4 elements: 6, 7, 8, 9
    int val;
    for (int i = 6; i < 10; ++i) {
        ASSERT_TRUE(buf.try_pop(val));
        EXPECT_EQ(val, i);
    }
    EXPECT_TRUE(buf.empty());
}

TEST(SpscTest, OverwriteNonTrivial) {
    spsc_ring_buffer<std::string, 3> buf;

    buf.push_overwrite("one");
    buf.push_overwrite("two");
    buf.push_overwrite("three");

    // Overwrite should properly destruct old strings
    buf.push_overwrite("four");
    EXPECT_EQ(buf.overflow_count(), 1u);

    std::string val;
    ASSERT_TRUE(buf.try_pop(val));
    EXPECT_EQ(val, "two");
    ASSERT_TRUE(buf.try_pop(val));
    EXPECT_EQ(val, "three");
    ASSERT_TRUE(buf.try_pop(val));
    EXPECT_EQ(val, "four");
}

TEST(SpscTest, ConcurrentOverwrite) {
    spsc_ring_buffer<int, 16> buf;
    constexpr int count = 100000;

    std::atomic<int> consumed{0};
    std::atomic<bool> producer_done{false};

    std::thread producer([&buf, &producer_done]() {
        for (int i = 0; i < count; ++i) {
            buf.push_overwrite(i);
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&buf, &consumed, &producer_done, count]() {
        int val;
        // Keep consuming until producer is done and buffer is empty
        while (!producer_done.load(std::memory_order_acquire) || !buf.empty()) {
            if (buf.try_pop(val)) {
                // Values received are within expected range [0, count)
                EXPECT_GE(val, 0);
                EXPECT_LT(val, count);
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                // Yield to avoid live-lock when producer is overwriting faster than we can read
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    // With overwrite mode, some values were lost
    EXPECT_GT(buf.overflow_count(), 0u);
}

TEST(SpscTest, ResetStatsClearsOverflowCount) {
    spsc_ring_buffer<int, 4> buf;

    // Fill buffer completely
    for (int i = 0; i < 4; ++i)
        buf.push_overwrite(i);

    // Trigger overflows
    buf.push_overwrite(100);
    buf.push_overwrite(200);
    EXPECT_GT(buf.overflow_count(), 0u);

    // Reset stats
    buf.reset_stats();
    EXPECT_EQ(buf.overflow_count(), 0u);

    // Buffer should still be functional
    EXPECT_EQ(buf.size(), 4u);
    EXPECT_TRUE(buf.full());
}

// Type with a non-trivial destructor that increments a counter
struct DtorCounter {
    int* counter_;
    explicit DtorCounter(int* c = nullptr) : counter_(c) {}
    ~DtorCounter() {
        if (counter_) ++(*counter_);
    }
    DtorCounter(DtorCounter const& o) : counter_(o.counter_) {}
    DtorCounter(DtorCounter&& o) noexcept : counter_(o.counter_) { o.counter_ = nullptr; }
    DtorCounter& operator=(DtorCounter&& o) noexcept {
        counter_ = o.counter_;
        o.counter_ = nullptr;
        return *this;
    }
    DtorCounter& operator=(DtorCounter const& o) {
        counter_ = o.counter_;
        return *this;
    }
};

TEST(SpscTest, ClearCallsDestructorsOnNonTrivialType) {
    int dtor_count = 0;
    {
        spsc_ring_buffer<DtorCounter, 8> buf;

        // Push 5 elements, each pointing to dtor_count
        for (int i = 0; i < 5; ++i) {
            buf.try_push(DtorCounter(&dtor_count));
        }
        EXPECT_EQ(dtor_count, 0);  // No destructors called yet

        // clear() should call ~DtorCounter() for all 5 elements
        buf.clear();
        EXPECT_EQ(dtor_count, 5);
        EXPECT_TRUE(buf.empty());
    }
    // No additional destructors from buf destruction — clear() already destroyed everything
    EXPECT_EQ(dtor_count, 5);
}

TEST(SpscTest, ClearWithNonTrivialTypeEmptyBuffer) {
    int dtor_count = 0;
    {
        spsc_ring_buffer<DtorCounter, 4> buf;
        buf.clear();  // No-op on empty buffer
        EXPECT_EQ(dtor_count, 0);
    }
    EXPECT_EQ(dtor_count, 0);
}

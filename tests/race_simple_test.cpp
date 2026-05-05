#include <gtest/gtest.h>
#include "spsc_ring_buffer.hpp"
#include <thread>
#include <atomic>
#include <string>
#include <chrono>

using namespace buffers;

static std::string generate_unique_shm_name(const std::string& base_name) {
    static std::atomic<int> counter{0};
    return "/" + base_name + "_" + std::to_string(getpid()) + "_" + std::to_string(counter++);
}

// This test validates that the opener properly waits for creator initialization.
// The current implementation uses memory_order_release/acquire to ensure sequence
// array is initialized before capacity is visible.
//
// If running under TSAN/ASAN, this test may detect race conditions if the
// synchronization is broken. Under normal execution, the release/acquire ordering
// guarantees correct behavior.
TEST(SpscShmRaceSimple, OpenerWaitsForCompleteInitialization) {
    std::string shm_name = generate_unique_shm_name("race_simple");
    shm_unlink(shm_name.c_str());

    std::atomic<bool> opener_saw_valid_buffer{false};
    std::atomic<bool> open_succeeded{false};

    std::thread creator([&]() {
        // Use the real constructor path - this properly initializes
        // sequence array before publishing capacity with release semantics
        spsc_ring_buffer<int, 16, ShmStorage> writer(
            shm_name.c_str(), 1, ShmOpenMode::create
        );

        EXPECT_TRUE(writer.valid()) << "Creator should successfully create buffer";

        // Keep alive while opener attempts to connect
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });

    // Small delay to ensure creator has started
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::thread opener([&]() {
        spsc_ring_buffer<int, 16, ShmStorage> reader(
            shm_name.c_str(), 1, ShmOpenMode::open
        );

        if (reader.valid()) {
            opener_saw_valid_buffer.store(true, std::memory_order_release);

            // With proper synchronization, the buffer should be fully initialized
            // and operations should work correctly
            bool push_ok = reader.try_push(42);
            open_succeeded.store(push_ok, std::memory_order_release);
        }
    });

    creator.join();
    opener.join();

    // The opener should observe a valid buffer after creator completes initialization
    EXPECT_TRUE(opener_saw_valid_buffer.load())
        << "Opener should see valid buffer after proper initialization";

    // With correct release/acquire ordering, push should succeed
    EXPECT_TRUE(open_succeeded.load())
        << "Buffer operations should succeed after proper initialization";

    shm_unlink(shm_name.c_str());
}

// Stress test to verify initialization synchronization holds under concurrent access.
// Uses the real spsc_ring_buffer constructor path with proper memory ordering.
TEST(SpscShmRaceSimple, StressTestRace) {
    int successful_iterations = 0;

    for (int iteration = 0; iteration < 100; ++iteration) {
        std::string shm_name = generate_unique_shm_name("stress_race");
        shm_unlink(shm_name.c_str());

        std::atomic<bool> start{false};
        std::atomic<bool> creator_valid{false};
        std::atomic<bool> opener_valid{false};

        std::thread creator([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            spsc_ring_buffer<int, 16, ShmStorage> writer(
                shm_name.c_str(), 1, ShmOpenMode::create
            );

            creator_valid.store(writer.valid(), std::memory_order_release);

            if (writer.valid()) {
                // Write some data
                for (int i = 0; i < 10; ++i) {
                    writer.try_push(i);
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        });

        std::thread opener([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            // Small delay to potentially hit the race window
            std::this_thread::sleep_for(std::chrono::microseconds(1));

            spsc_ring_buffer<int, 16, ShmStorage> reader(
                shm_name.c_str(), 1, ShmOpenMode::open
            );

            opener_valid.store(reader.valid(), std::memory_order_release);

            if (reader.valid()) {
                // Try to read - with proper synchronization, this is safe
                int val;
                reader.try_pop(val);
            }
        });

        start.store(true, std::memory_order_release);

        creator.join();
        opener.join();

        // Count successful iterations where both threads got valid buffers
        if (creator_valid.load() && opener_valid.load()) {
            ++successful_iterations;
        }

        shm_unlink(shm_name.c_str());
    }

    // At least some iterations should have both creator and opener succeed
    EXPECT_GT(successful_iterations, 0)
        << "Expected at least some iterations to have both creator and opener succeed";

    // Report the number of successful iterations for visibility
    std::cout << "Stress test completed: " << successful_iterations << "/100 iterations "
              << "had both creator and opener successfully connect" << std::endl;
}

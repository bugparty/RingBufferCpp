#include <gtest/gtest.h>
#include "spsc_ring_buffer.hpp"
#include <thread>
#include <atomic>
#include <string>
#include <chrono>

using namespace buffers;

// Helper function to generate unique shared memory names
static std::string generate_unique_shm_name(const std::string& base_name) {
    static std::atomic<int> counter{0};
    return "/" + base_name + "_" + std::to_string(getpid()) + "_" + std::to_string(counter++);
}

// Test to reproduce the race condition where opener sees capacity set
// but sequence array is not yet initialized
TEST(SpscShmRaceTest, OpenerReadsUninitializedSequence) {
    std::string shm_name = generate_unique_shm_name("race_init_test");
    shm_unlink(shm_name.c_str());

    std::atomic<bool> opener_started{false};
    std::atomic<bool> opener_passed_wait{false};
    std::atomic<bool> creator_initialized{false};
    std::atomic<uint64_t> sequence_value_read{0};

    // Creator thread - will set capacity then initialize sequence
    std::thread creator_thread([&]() {
        // Delay start to ensure opener is waiting
        while (!opener_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // Create the buffer - this sets capacity then initializes sequence
        spsc_ring_buffer<int, 16, ShmStorage> writer(
            shm_name.c_str(), 1, ShmOpenMode::create
        );

        EXPECT_TRUE(writer.valid());
        creator_initialized.store(true, std::memory_order_release);
    });

    // Opener thread - will race to read sequence before it's initialized
    std::thread opener_thread([&]() {
        opener_started.store(true, std::memory_order_release);

        // Try to open immediately - racing with creator's initialization
        // This will busy-wait on capacity, then read sequence
        spsc_ring_buffer<int, 16, ShmStorage> reader(
            shm_name.c_str(), 1, ShmOpenMode::open
        );

        // If we passed the capacity check while sequence was uninitialized,
        // we might have read garbage data
        opener_passed_wait.store(true, std::memory_order_release);

        if (reader.valid()) {
            // Try to read sequence - this could be garbage if race occurred
            // The sequence values should be 0 (initialized), but if we raced,
            // we might read uninitialized memory
            bool success = reader.try_push(42);
            // In a proper implementation, this should always succeed
            // But if we read uninitialized sequence, behavior is undefined
        }
    });

    creator_thread.join();
    opener_thread.join();

    shm_unlink(shm_name.c_str());
}

// More aggressive test using manual delay injection
// This directly tests the race by manipulating timing
TEST(SpscShmRaceTest, CapacitySetBeforeSequenceInit) {
    std::string shm_name = generate_unique_shm_name("capacity_sequence_race");
    shm_unlink(shm_name.c_str());

    std::atomic<bool> capacity_observed{false};
    std::atomic<bool> opener_finished{false};
    std::atomic<bool> race_detected{false};

    // Creator thread
    std::thread creator([&]() {
        // Create shared memory segment manually to control initialization timing
        int fd = shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
        ASSERT_GE(fd, 0);

        using region_type = buffers::detail::ring_buffer_region<int, 16>;
        ASSERT_EQ(ftruncate(fd, sizeof(region_type)), 0);

        void* ptr = mmap(nullptr, sizeof(region_type),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ASSERT_NE(ptr, MAP_FAILED);

        auto* region = static_cast<region_type*>(ptr);

        // Step 1: Set capacity FIRST (this is the bug)
        region->header.capacity = 16;
        region->header.schema_version = 1;

        // Step 2: Signal that capacity is set
        capacity_observed.store(true, std::memory_order_release);

        // Step 3: Add delay to amplify race window
        // During this time, opener might see capacity but uninitialized sequence
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Step 4: Initialize sequence array NOW
        for (size_t i = 0; i < 16; ++i) {
            new(&region->sequence[i]) std::atomic<uint64_t>(0);
        }

        // Keep region alive
        while (!opener_finished.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        munmap(ptr, sizeof(region_type));
        close(fd);
    });

    // Opener thread - tries to use the buffer immediately when capacity is set
    std::thread opener([&]() {
        // Wait until capacity is set
        while (!capacity_observed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // Now open the buffer - this will read sequence array
        // If race occurs, sequence might not be initialized yet
        spsc_ring_buffer<int, 16, ShmStorage> reader(
            shm_name.c_str(), 1, ShmOpenMode::open
        );

        if (reader.valid()) {
            // Try to use the buffer
            // If sequence was uninitialized, this has undefined behavior
            // In practice, might read garbage values for sequence
            int val = 0;
            bool could_pop = reader.try_pop(val);

            // The sequence should be 0, but if we raced,
            // we might have observed non-zero values
            // This indicates we accessed uninitialized memory
        } else {
            // Buffer invalid - could be due to race condition
            race_detected.store(true, std::memory_order_release);
        }

        opener_finished.store(true, std::memory_order_release);
    });

    creator.join();
    opener.join();

    // Note: This test demonstrates the race exists
    // In TSAN builds, this should trigger a data race warning

    shm_unlink(shm_name.c_str());
}

// Test with TSAN to detect the actual data race
TEST(SpscShmRaceTest, DetectRaceWithSanitizer) {
    std::string shm_name = generate_unique_shm_name("tsan_race_test");
    shm_unlink(shm_name.c_str());

    constexpr int iterations = 100;

    for (int i = 0; i < iterations; ++i) {
        std::atomic<bool> start{false};

        std::thread creator([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            spsc_ring_buffer<int, 16, ShmStorage> writer(
                shm_name.c_str(), 1, ShmOpenMode::create
            );
        });

        std::thread opener([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            // Small delay to hit the race window
            std::this_thread::sleep_for(std::chrono::microseconds(1));

            spsc_ring_buffer<int, 16, ShmStorage> reader(
                shm_name.c_str(), 1, ShmOpenMode::open
            );
        });

        start.store(true, std::memory_order_release);

        creator.join();
        opener.join();

        shm_unlink(shm_name.c_str());
    }
}

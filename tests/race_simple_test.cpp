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

// Simple test: demonstrate that opener can observe capacity set but sequence not initialized
TEST(SpscShmRaceSimple, OpenerSeesCapacityBeforeSequenceInit) {
    std::string shm_name = generate_unique_shm_name("race_simple");
    shm_unlink(shm_name.c_str());

    std::atomic<bool> opener_saw_valid_buffer{false};
    std::atomic<bool> race_occurred{false};

    // This test demonstrates the timing issue:
    // Creator: sets capacity -> initializes sequence
    // Opener: reads capacity != 0 -> reads sequence (might be uninitialized)

    std::thread creator([&]() {
        // Manually create shared memory to control initialization order
        int fd = shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
        ASSERT_GE(fd, 0);

        using region_type = buffers::detail::ring_buffer_region<int, 16>;
        EXPECT_EQ(ftruncate(fd, sizeof(region_type)), 0);

        void* ptr = mmap(nullptr, sizeof(region_type),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ASSERT_NE(ptr, MAP_FAILED);

        auto* region = static_cast<region_type*>(ptr);

        // Simulate the buggy initialization order:
        // 1. Set capacity FIRST (line 249 in spsc_ring_buffer.hpp)
        region->header.capacity = 16;
        region->header.schema_version = 1;

        // Add artificial delay to amplify race window
        // In real code, the delay between setting capacity and initializing
        // sequence is small but non-zero
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // 2. Initialize sequence AFTER (lines 252-254 in spsc_ring_buffer.hpp)
        for (size_t i = 0; i < 16; ++i) {
            new(&region->sequence[i]) std::atomic<uint64_t>(0);
        }

        // Keep alive
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        munmap(ptr, sizeof(region_type));
        close(fd);
    });

    // Give creator a moment to start
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Opener tries to use the buffer during the race window
    std::thread opener([&]() {
        spsc_ring_buffer<int, 16, ShmStorage> reader(
            shm_name.c_str(), 1, ShmOpenMode::open
        );

        if (reader.valid()) {
            opener_saw_valid_buffer.store(true, std::memory_order_release);

            // Try to use the buffer - if sequence was uninitialized,
            // this could read garbage values
            // The sequence values should be 0, but if we raced,
            // they might be random memory contents

            // Try to push - this reads sequence[0]
            bool push_ok = reader.try_push(42);

            // If the race occurred, sequence[0] might not be properly initialized
            // In practice, this leads to undefined behavior
            if (!push_ok) {
                race_occurred.store(true, std::memory_order_release);
            }
        }
    });

    creator.join();
    opener.join();

    // The bug: opener can get a valid buffer even though sequence wasn't initialized
    // In a correct implementation with proper synchronization, this wouldn't happen
    std::cout << "Opener saw valid buffer: " << opener_saw_valid_buffer.load() << std::endl;
    std::cout << "Race detected (push failed): " << race_occurred.load() << std::endl;

    // This demonstrates the bug exists - opener could access buffer
    // before sequence was initialized
    if (opener_saw_valid_buffer.load()) {
        std::cout << "WARNING: Opener accessed buffer with potentially uninitialized sequence!" << std::endl;
    }

    shm_unlink(shm_name.c_str());
}

// Test with stress testing to increase likelihood of hitting the race
TEST(SpscShmRaceSimple, StressTestRace) {
    for (int iteration = 0; iteration < 100; ++iteration) {
        std::string shm_name = generate_unique_shm_name("stress_race");
        shm_unlink(shm_name.c_str());

        std::atomic<bool> start{false};

        std::thread creator([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            spsc_ring_buffer<int, 16, ShmStorage> writer(
                shm_name.c_str(), 1, ShmOpenMode::create
            );

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

            // Tiny delay to hit the race window
            std::this_thread::sleep_for(std::chrono::microseconds(1));

            spsc_ring_buffer<int, 16, ShmStorage> reader(
                shm_name.c_str(), 1, ShmOpenMode::open
            );

            if (reader.valid()) {
                // Try to read - if sequence was uninitialized, undefined behavior
                int val;
                reader.try_pop(val);
            }
        });

        start.store(true, std::memory_order_release);

        creator.join();
        opener.join();

        shm_unlink(shm_name.c_str());
    }

    std::cout << "Stress test completed 100 iterations" << std::endl;
}

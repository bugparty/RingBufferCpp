#include <gtest/gtest.h>
#include "spsc_ring_buffer.hpp"
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <string>

using namespace buffers;

// Helper function to generate unique shared memory names
// Uses PID + counter to avoid collisions when running tests in parallel
static std::string generate_unique_shm_name(const std::string& base_name) {
    static std::atomic<int> counter{0};
    return "/" + base_name + "_" + std::to_string(getpid()) + "_" + std::to_string(counter++);
}

// Basic shared memory creation test
TEST(SpscShmTest, CreateAndOpen) {
    std::string shm_name = generate_unique_shm_name("test_ring_buffer_create");

    // Clean up any existing shm
    shm_unlink(shm_name.c_str());

    // Create new shared memory buffer
    spsc_ring_buffer<int, 16, ShmStorage> writer(
        shm_name.c_str(), ShmOpenMode::create
    );

    EXPECT_TRUE(writer.valid());
    EXPECT_TRUE(writer.is_creator());
    EXPECT_TRUE(writer.empty());

    // Clean up
    shm_unlink(shm_name.c_str());
}

TEST(SpscShmTest, OpenExisting) {
    std::string shm_name = generate_unique_shm_name("test_ring_buffer_open");
    shm_unlink(shm_name.c_str());

    // Create
    {
        spsc_ring_buffer<int, 16, ShmStorage> writer(
            shm_name.c_str(), ShmOpenMode::create
        );
        EXPECT_TRUE(writer.valid());
    }

    // Open existing
    {
        spsc_ring_buffer<int, 16, ShmStorage> reader(
            shm_name.c_str(), ShmOpenMode::open
        );
        EXPECT_TRUE(reader.valid());
        EXPECT_FALSE(reader.is_creator());
    }

    shm_unlink(shm_name.c_str());
}

TEST(SpscShmTest, CrossProcessCommunication) {
    std::string shm_name = generate_unique_shm_name("test_ring_buffer_ipc");
    shm_unlink(shm_name.c_str());

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        // Child process: writer
        spsc_ring_buffer<int, 64, ShmStorage> writer(
            shm_name.c_str(), ShmOpenMode::create
        );

        if (!writer.valid()) {
            exit(1);
        }

        // Write data
        for (int i = 0; i < 100; ++i) {
            while (!writer.try_push(i))
                ;
        }

        exit(0);
    } else {
        // Parent process: reader
        usleep(10000);  // Wait for writer to start

        spsc_ring_buffer<int, 64, ShmStorage> reader(
            shm_name.c_str(), ShmOpenMode::open
        );

        ASSERT_TRUE(reader.valid());

        int expected = 0;
        int attempts = 0;
        while (expected < 100 && attempts < 10000) {
            int val;
            if (reader.try_pop(val)) {
                EXPECT_EQ(val, expected);
                ++expected;
            }
            ++attempts;
            if (attempts % 100 == 0)
                usleep(100);
        }

        EXPECT_EQ(expected, 100);

        int status;
        waitpid(pid, &status, 0);
        EXPECT_EQ(WEXITSTATUS(status), 0);
    }

    shm_unlink(shm_name.c_str());
}

TEST(SpscShmTest, OverflowCountShared) {
    std::string shm_name = generate_unique_shm_name("test_ring_buffer_overflow");
    shm_unlink(shm_name.c_str());

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        // Writer: overwrite mode
        spsc_ring_buffer<int, 4, ShmStorage> writer(
            shm_name.c_str(), ShmOpenMode::create
        );

        for (int i = 0; i < 10; ++i) {
            writer.push_overwrite(i);
        }

        sleep(1);  // Give reader time to check
        exit(0);
    } else {
        // Reader
        usleep(10000);

        spsc_ring_buffer<int, 4, ShmStorage> reader(
            shm_name.c_str(), ShmOpenMode::open
        );

        ASSERT_TRUE(reader.valid());

        // Wait for writer to finish
        int status;
        waitpid(pid, &status, 0);

        // Overflow count should be visible across processes
        EXPECT_EQ(reader.overflow_count(), 6u);
    }

    shm_unlink(shm_name.c_str());
}

// --- Error handling and safety tests ---

TEST(SpscShmTest, InvalidNameHandling) {
    // Test invalid name (doesn't start with '/')
    spsc_ring_buffer<int, 16, ShmStorage> invalid_name(
        "invalid_name", ShmOpenMode::create
    );

    EXPECT_FALSE(invalid_name.valid());
    EXPECT_FALSE(invalid_name.is_creator());

    // All operations should be safe on invalid buffer
    EXPECT_TRUE(invalid_name.empty());  // Invalid buffer is "empty"
    EXPECT_FALSE(invalid_name.full());  // Invalid buffer is not "full"
    EXPECT_EQ(invalid_name.size(), 0u);
    EXPECT_EQ(invalid_name.overflow_count(), 0u);

    // Operations should return false/no-op
    EXPECT_FALSE(invalid_name.try_push(42));

    int val;
    EXPECT_FALSE(invalid_name.try_pop(val));

    // No-op operations should not crash
    invalid_name.push_overwrite(42);
    invalid_name.reset_stats();
    invalid_name.clear();
}

TEST(SpscShmTest, OpenNonExistentSegment) {
    // Try to open a segment that doesn't exist
    std::string shm_name = generate_unique_shm_name("test_nonexistent_segment");
    shm_unlink(shm_name.c_str());  // Ensure it doesn't exist

    spsc_ring_buffer<int, 16, ShmStorage> reader(
        shm_name.c_str(), ShmOpenMode::open
    );

    EXPECT_FALSE(reader.valid());
    EXPECT_FALSE(reader.is_creator());

    // All operations should be safe
    EXPECT_TRUE(reader.empty());
    EXPECT_FALSE(reader.full());
    EXPECT_EQ(reader.size(), 0u);

    EXPECT_FALSE(reader.try_push(1));
    int val;
    EXPECT_FALSE(reader.try_pop(val));
}

TEST(SpscShmTest, ValidityAfterFailedConstruction) {
    // Test that buffer is usable after failed construction
    // First create an invalid buffer
    spsc_ring_buffer<int, 16, ShmStorage> invalid(
        "no_slash", ShmOpenMode::create
    );

    ASSERT_FALSE(invalid.valid());

    // Multiple operations should all be safe
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(invalid.try_push(i));
        invalid.push_overwrite(i);
        EXPECT_TRUE(invalid.empty());
        EXPECT_FALSE(invalid.full());
        EXPECT_EQ(invalid.size(), 0u);
        EXPECT_EQ(invalid.overflow_count(), 0u);
    }

    // Clear should be no-op
    invalid.clear();
    invalid.reset_stats();
}

TEST(SpscShmTest, DestructorSafeWithInvalidBuffer) {
    // Test that destructor doesn't crash with invalid buffer
    // This test passes if it doesn't crash/segfault
    {
        spsc_ring_buffer<int, 16, ShmStorage> invalid(
            "bad_name", ShmOpenMode::create
        );
        // Buffer is invalid, destructor should handle it gracefully
    }
    // If we reach here, destructor was safe
    SUCCEED();
}

TEST(SpscShmTest, CreateExclusively) {
    std::string shm_name = generate_unique_shm_name("test_exclusive_create");
    shm_unlink(shm_name.c_str());

    // First create should succeed
    {
        spsc_ring_buffer<int, 16, ShmStorage> first(
            shm_name.c_str(), ShmOpenMode::create
        );
        EXPECT_TRUE(first.valid());
        EXPECT_TRUE(first.is_creator());
    }

    // Second create should fail (segment already exists)
    {
        spsc_ring_buffer<int, 16, ShmStorage> second(
            shm_name.c_str(), ShmOpenMode::create
        );
        EXPECT_FALSE(second.valid());  // O_EXCL should cause failure
        EXPECT_FALSE(second.is_creator());

        // Operations should still be safe
        EXPECT_TRUE(second.empty());
        EXPECT_FALSE(second.try_push(1));
    }

    shm_unlink(shm_name.c_str());
}

TEST(SpscShmTest, CreateOrOpenIdempotent) {
    std::string shm_name = generate_unique_shm_name("test_create_or_open");
    shm_unlink(shm_name.c_str());

    // First call: creates the segment
    {
        spsc_ring_buffer<int, 16, ShmStorage> first(
            shm_name.c_str(), ShmOpenMode::create_or_open
        );
        EXPECT_TRUE(first.valid());
        EXPECT_TRUE(first.is_creator());
    }

    // Second call: opens existing segment (even though first instance was destroyed)
    {
        spsc_ring_buffer<int, 16, ShmStorage> second(
            shm_name.c_str(), ShmOpenMode::create_or_open
        );
        EXPECT_TRUE(second.valid());
        // Second instance is NOT the creator (segment already exists)
        // Note: Destructor skips clear() for shared storage, so buffer persists.
        // Second instance is empty because first instance never pushed any data.
        EXPECT_FALSE(second.is_creator());
        EXPECT_TRUE(second.empty());

        // Should be able to use the buffer normally
        EXPECT_TRUE(second.try_push(42));
        int val;
        EXPECT_TRUE(second.try_pop(val));
        EXPECT_EQ(val, 42);
    }

    shm_unlink(shm_name.c_str());
}

TEST(SpscShmTest, SchemaVersionMismatch) {
    std::string shm_name = generate_unique_shm_name("test_schema_mismatch");
    shm_unlink(shm_name.c_str());

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        // Child: create with schema version 1
        spsc_ring_buffer<int, 16, ShmStorage> writer(
            shm_name.c_str(), 1, ShmOpenMode::create
        );
        if (!writer.valid()) exit(1);
        writer.try_push(42);
        sleep(1);  // Keep alive so parent can try to open
        exit(0);
    } else {
        // Parent: wait for child to create, then try to open with wrong version
        usleep(50000);  // 50ms

        spsc_ring_buffer<int, 16, ShmStorage> reader(
            shm_name.c_str(), 2, ShmOpenMode::open
        );

        // Construction should fail due to schema mismatch
        EXPECT_FALSE(reader.valid());
        EXPECT_FALSE(reader.is_compatible());

        // All operations should be safe
        EXPECT_TRUE(reader.empty());
        EXPECT_FALSE(reader.try_push(1));
        int val;
        EXPECT_FALSE(reader.try_pop(val));

        int status;
        waitpid(pid, &status, 0);
    }

    shm_unlink(shm_name.c_str());
}

TEST(SpscShmTest, NonCreatorTimeoutWaitingForCapacity) {
    std::string shm_name = generate_unique_shm_name("test_capacity_timeout");
    shm_unlink(shm_name.c_str());

    // Create raw shared memory segment (bypass constructor)
    // Leave capacity at 0 -- the non-creator will timeout
    int fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, sizeof(buffers::detail::ring_buffer_region<int, 16>)), 0);
    close(fd);

    // Open as non-creator with open mode
    // This will mmap the region, see capacity==0, spin through yield/usleep loop, timeout
    spsc_ring_buffer<int, 16, ShmStorage> reader(
        shm_name.c_str(), 0, ShmOpenMode::open
    );

    // Construction should fail -- creator never initialized capacity
    EXPECT_FALSE(reader.valid());

    // Operations should be safe
    EXPECT_TRUE(reader.empty());
    EXPECT_FALSE(reader.try_push(1));
    int val;
    EXPECT_FALSE(reader.try_pop(val));

    shm_unlink(shm_name.c_str());
}

TEST(SpscShmTest, NonCreatorFstatTimeoutTooSmall) {
    std::string shm_name = generate_unique_shm_name("test_fstat_timeout");
    shm_unlink(shm_name.c_str());

    // Create raw shm segment with only 1 byte — too small for region_type
    int fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, 1), 0);  // Way too small
    close(fd);

    // Open as non-creator: fstat sees size < sizeof(region_type), retries, times out
    spsc_ring_buffer<int, 16, ShmStorage> reader(
        shm_name.c_str(), 0, ShmOpenMode::open
    );

    // Construction should fail
    EXPECT_FALSE(reader.valid());

    // Operations should be safe
    EXPECT_TRUE(reader.empty());
    EXPECT_EQ(reader.size(), 0u);

    shm_unlink(shm_name.c_str());
}

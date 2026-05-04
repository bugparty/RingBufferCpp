#include <gtest/gtest.h>
#include "spsc_ring_buffer.hpp"
#include <sys/wait.h>
#include <unistd.h>

using namespace buffers;

// Basic shared memory creation test
TEST(SpscShmTest, CreateAndOpen) {
    const char* shm_name = "/test_ring_buffer_create";

    // Clean up any existing shm
    shm_unlink(shm_name);

    // Create new shared memory buffer
    spsc_ring_buffer<int, 16, ShmStorage> writer(
        shm_name, ShmOpenMode::create
    );

    EXPECT_TRUE(writer.valid());
    EXPECT_TRUE(writer.is_creator());
    EXPECT_TRUE(writer.empty());

    // Clean up
    shm_unlink(shm_name);
}

TEST(SpscShmTest, OpenExisting) {
    const char* shm_name = "/test_ring_buffer_open";
    shm_unlink(shm_name);

    // Create
    {
        spsc_ring_buffer<int, 16, ShmStorage> writer(
            shm_name, ShmOpenMode::create
        );
        EXPECT_TRUE(writer.valid());
    }

    // Open existing
    {
        spsc_ring_buffer<int, 16, ShmStorage> reader(
            shm_name, ShmOpenMode::open
        );
        EXPECT_TRUE(reader.valid());
        EXPECT_FALSE(reader.is_creator());
    }

    shm_unlink(shm_name);
}

TEST(SpscShmTest, CrossProcessCommunication) {
    const char* shm_name = "/test_ring_buffer_ipc";
    shm_unlink(shm_name);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        // Child process: writer
        spsc_ring_buffer<int, 64, ShmStorage> writer(
            shm_name, ShmOpenMode::create
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
            shm_name, ShmOpenMode::open
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

    shm_unlink(shm_name);
}

TEST(SpscShmTest, OverflowCountShared) {
    const char* shm_name = "/test_ring_buffer_overflow";
    shm_unlink(shm_name);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        // Writer: overwrite mode
        spsc_ring_buffer<int, 4, ShmStorage> writer(
            shm_name, ShmOpenMode::create
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
            shm_name, ShmOpenMode::open
        );

        ASSERT_TRUE(reader.valid());

        // Wait for writer to finish
        int status;
        waitpid(pid, &status, 0);

        // Overflow count should be visible across processes
        EXPECT_EQ(reader.overflow_count(), 6u);
    }

    shm_unlink(shm_name);
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
    const char* shm_name = "/test_nonexistent_segment";
    shm_unlink(shm_name);  // Ensure it doesn't exist

    spsc_ring_buffer<int, 16, ShmStorage> reader(
        shm_name, ShmOpenMode::open
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
    const char* shm_name = "/test_exclusive_create";
    shm_unlink(shm_name);

    // First create should succeed
    {
        spsc_ring_buffer<int, 16, ShmStorage> first(
            shm_name, ShmOpenMode::create
        );
        EXPECT_TRUE(first.valid());
        EXPECT_TRUE(first.is_creator());
    }

    // Second create should fail (segment already exists)
    {
        spsc_ring_buffer<int, 16, ShmStorage> second(
            shm_name, ShmOpenMode::create
        );
        EXPECT_FALSE(second.valid());  // O_EXCL should cause failure
        EXPECT_FALSE(second.is_creator());

        // Operations should still be safe
        EXPECT_TRUE(second.empty());
        EXPECT_FALSE(second.try_push(1));
    }

    shm_unlink(shm_name);
}

TEST(SpscShmTest, CreateOrOpenIdempotent) {
    const char* shm_name = "/test_create_or_open";
    shm_unlink(shm_name);

    // First call: creates the segment
    {
        spsc_ring_buffer<int, 16, ShmStorage> first(
            shm_name, ShmOpenMode::create_or_open
        );
        EXPECT_TRUE(first.valid());
        EXPECT_TRUE(first.is_creator());
    }

    // Second call: opens existing segment (even though first instance was destroyed)
    {
        spsc_ring_buffer<int, 16, ShmStorage> second(
            shm_name, ShmOpenMode::create_or_open
        );
        EXPECT_TRUE(second.valid());
        // Second instance is NOT the creator (segment already exists)
        // Note: Due to destructor clearing the buffer, second instance sees empty buffer
        EXPECT_FALSE(second.is_creator());
        EXPECT_TRUE(second.empty());

        // Should be able to use the buffer normally
        EXPECT_TRUE(second.try_push(42));
        int val;
        EXPECT_TRUE(second.try_pop(val));
        EXPECT_EQ(val, 42);
    }

    shm_unlink(shm_name);
}

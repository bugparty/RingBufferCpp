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

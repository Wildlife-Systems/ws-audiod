#include <gtest/gtest.h>
#include "audio_daemon/ring_buffer.hpp"
#include <vector>
#include <numeric>

using namespace audio_daemon;

TEST(RingBufferTest, Construction) {
    RingBuffer rb(4800, 1, 2);
    EXPECT_EQ(rb.capacity_frames(), 4800u);
    EXPECT_EQ(rb.channels(), 1);
    EXPECT_EQ(rb.available_frames(), 0u);
}

TEST(RingBufferTest, WriteAndRead) {
    RingBuffer rb(100, 1, 2);
    std::vector<int16_t> data(50);
    std::iota(data.begin(), data.end(), 1); // 1..50

    rb.write(reinterpret_cast<const uint8_t*>(data.data()), 50);
    EXPECT_EQ(rb.available_frames(), 50u);

    std::vector<int16_t> out(50);
    size_t read = rb.read(reinterpret_cast<uint8_t*>(out.data()), 0, 50);
    EXPECT_EQ(read, 50u);

    // Most recent frame should be data[49]=50
    EXPECT_EQ(out[49], 50);
}

TEST(RingBufferTest, WrapAround) {
    RingBuffer rb(10, 1, 2);

    // Write 15 frames into a 10-frame buffer
    std::vector<int16_t> data(15);
    std::iota(data.begin(), data.end(), 1); // 1..15
    rb.write(reinterpret_cast<const uint8_t*>(data.data()), 15);

    EXPECT_EQ(rb.available_frames(), 10u);

    // Should contain the last 10 values: 6..15
    std::vector<int16_t> out(10);
    size_t read = rb.read(reinterpret_cast<uint8_t*>(out.data()), 0, 10);
    EXPECT_EQ(read, 10u);
    EXPECT_EQ(out[0], 6);
    EXPECT_EQ(out[9], 15);
}

TEST(RingBufferTest, StereoChannels) {
    RingBuffer rb(100, 2, 2);
    // 10 stereo frames = 20 samples
    std::vector<int16_t> data(20);
    for (int i = 0; i < 10; ++i) {
        data[i * 2]     = static_cast<int16_t>(i);     // left
        data[i * 2 + 1] = static_cast<int16_t>(-i);    // right
    }

    rb.write(reinterpret_cast<const uint8_t*>(data.data()), 10);
    EXPECT_EQ(rb.available_frames(), 10u);

    std::vector<int16_t> out(20);
    size_t read = rb.read(reinterpret_cast<uint8_t*>(out.data()), 0, 10);
    EXPECT_EQ(read, 10u);
    EXPECT_EQ(out[0], 0);
    EXPECT_EQ(out[1], 0);
    EXPECT_EQ(out[18], 9);
    EXPECT_EQ(out[19], -9);
}

TEST(RingBufferTest, PartialRead) {
    RingBuffer rb(100, 1, 2);
    std::vector<int16_t> data(5, 42);
    rb.write(reinterpret_cast<const uint8_t*>(data.data()), 5);

    // Request more than available
    std::vector<int16_t> out(20, 0);
    size_t read = rb.read(reinterpret_cast<uint8_t*>(out.data()), 0, 20);
    EXPECT_EQ(read, 5u);
}

TEST(RingBufferTest, OffsetRead) {
    RingBuffer rb(100, 1, 2);
    std::vector<int16_t> data(10);
    std::iota(data.begin(), data.end(), 1); // 1..10
    rb.write(reinterpret_cast<const uint8_t*>(data.data()), 10);

    // Read 5 frames, offset 3 from the head
    std::vector<int16_t> out(5);
    size_t read = rb.read(reinterpret_cast<uint8_t*>(out.data()), 3, 5);
    EXPECT_EQ(read, 5u);
    // Head is at 10; offset 3 means start from frame 7 going backwards 5
    // frames 3..7 → values 3,4,5,6,7
    EXPECT_EQ(out[0], 3);
    EXPECT_EQ(out[4], 7);
}

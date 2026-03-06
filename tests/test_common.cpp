#include <gtest/gtest.h>
#include "audio_daemon/common.hpp"
#include <cstring>

using namespace audio_daemon;

TEST(CommonTest, DefaultAudioConfig) {
    AudioConfig cfg;
    EXPECT_EQ(cfg.device, "default");
    EXPECT_EQ(cfg.sample_rate, 48000u);
    EXPECT_EQ(cfg.channels, 1);
    EXPECT_EQ(cfg.bits_per_sample, 16);
    EXPECT_EQ(cfg.period_size, 1024u);
    EXPECT_EQ(cfg.buffer_periods, 4u);
    EXPECT_DOUBLE_EQ(cfg.gain_db, 0.0);
    EXPECT_FALSE(cfg.dc_remove);
}

TEST(CommonTest, DefaultDaemonConfig) {
    DaemonConfig cfg;
    EXPECT_EQ(cfg.socket_path, DEFAULT_SOCKET_PATH);
    EXPECT_EQ(cfg.clips_dir, DEFAULT_CLIPS_DIR);
    EXPECT_EQ(cfg.ring_buffer_seconds, 60u);
    EXPECT_FALSE(cfg.enable_streaming);
    EXPECT_EQ(cfg.stream_port, 8001);
    EXPECT_FALSE(cfg.enable_sample_sharing);
}

TEST(CommonTest, DefaultBlockRecorderConfig) {
    BlockRecorderConfig cfg;
    EXPECT_FALSE(cfg.enabled);
    EXPECT_EQ(cfg.output_dir, DEFAULT_BLOCKS_DIR);
    EXPECT_EQ(cfg.block_duration_seconds, 300u);
    EXPECT_EQ(cfg.format, "wav");
    EXPECT_EQ(cfg.max_blocks, 0u);
}

TEST(CommonTest, FormatConstants) {
    EXPECT_EQ(FMT_S16_LE, 1u);
    EXPECT_EQ(FMT_S24_LE, 2u);
    EXPECT_EQ(FMT_S32_LE, 3u);
}

TEST(CommonTest, VersionDefined) {
    EXPECT_NE(VERSION, nullptr);
    EXPECT_GT(std::strlen(VERSION), 0u);
}

TEST(CommonTest, TimestampFilename) {
    std::string name = timestamp_to_filename();
    // Format: YYYYMMDD_HHMMSS_mmm  (19 chars)
    EXPECT_EQ(name.size(), 19u);
    EXPECT_EQ(name[8], '_');
    EXPECT_EQ(name[15], '_');
}

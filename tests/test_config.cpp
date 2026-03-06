#include <gtest/gtest.h>
#include "audio_daemon/daemon.hpp"
#include <fstream>
#include <filesystem>

using namespace audio_daemon;

class ConfigTest : public ::testing::Test {
protected:
    std::string temp_dir;

    void SetUp() override {
        temp_dir = "/tmp/audiod_test_" + std::to_string(getpid());
        std::filesystem::create_directories(temp_dir);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir);
    }

    std::string write_config(const std::string& content) {
        std::string path = temp_dir + "/test.conf";
        std::ofstream f(path);
        f << content;
        f.close();
        return path;
    }
};

TEST_F(ConfigTest, DefaultsWhenFileNotFound) {
    DaemonConfig config = load_config("/nonexistent/path/config.conf");
    EXPECT_EQ(config.socket_path, DEFAULT_SOCKET_PATH);
    EXPECT_EQ(config.clips_dir, DEFAULT_CLIPS_DIR);
    EXPECT_EQ(config.audio.sample_rate, 48000u);
    EXPECT_EQ(config.audio.channels, 1);
}

TEST_F(ConfigTest, ParsesDaemonSection) {
    auto path = write_config(R"(
[daemon]
socket_path = /run/test/socket.sock
clips_dir = /data/clips
ring_buffer_seconds = 120
)");

    DaemonConfig config = load_config(path);
    EXPECT_EQ(config.socket_path, "/run/test/socket.sock");
    EXPECT_EQ(config.clips_dir, "/data/clips");
    EXPECT_EQ(config.ring_buffer_seconds, 120u);
}

TEST_F(ConfigTest, ParsesAudioSection) {
    auto path = write_config(R"(
[audio]
device = hw:1,0
rate = 96000
channels = 2
bits_per_sample = 24
period_size = 2048
gain_db = 6.5
dc_remove = true
)");

    DaemonConfig config = load_config(path);
    EXPECT_EQ(config.audio.device, "hw:1,0");
    EXPECT_EQ(config.audio.sample_rate, 96000u);
    EXPECT_EQ(config.audio.channels, 2);
    EXPECT_EQ(config.audio.bits_per_sample, 24);
    EXPECT_EQ(config.audio.period_size, 2048u);
    EXPECT_DOUBLE_EQ(config.audio.gain_db, 6.5);
    EXPECT_TRUE(config.audio.dc_remove);
}

TEST_F(ConfigTest, ParsesBlockRecorderSection) {
    auto path = write_config(R"(
[block_recorder]
enabled = true
output_dir = /mnt/storage/blocks
block_duration_seconds = 600
format = flac
max_blocks = 288
)");

    DaemonConfig config = load_config(path);
    EXPECT_TRUE(config.block_recorder.enabled);
    EXPECT_EQ(config.block_recorder.output_dir, "/mnt/storage/blocks");
    EXPECT_EQ(config.block_recorder.block_duration_seconds, 600u);
    EXPECT_EQ(config.block_recorder.format, "flac");
    EXPECT_EQ(config.block_recorder.max_blocks, 288u);
}

TEST_F(ConfigTest, IgnoresCommentsAndBlankLines) {
    auto path = write_config(R"(
# This is a comment
; This is also a comment

[audio]
# rate = 11025
rate = 44100
)");

    DaemonConfig config = load_config(path);
    EXPECT_EQ(config.audio.sample_rate, 44100u);
}

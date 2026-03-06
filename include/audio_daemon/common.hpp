#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>
#include <vector>
#include <chrono>
#include <audio_daemon/version.hpp>

namespace audio_daemon {

// Audio format identifiers
constexpr uint32_t FMT_S16_LE = 1;
constexpr uint32_t FMT_S24_LE = 2;
constexpr uint32_t FMT_S32_LE = 3;

// Default paths
constexpr const char* DEFAULT_SOCKET_PATH = "/run/ws-audiod/control.sock";
constexpr const char* DEFAULT_CLIPS_DIR = "/var/ws/audiod/clips";
constexpr const char* DEFAULT_BLOCKS_DIR = "/var/ws/audiod/blocks";
constexpr const char* DEFAULT_SHM_NAME = "/ws_audiod_samples";
constexpr const char* DEFAULT_FRAME_NOTIFY_PATH = "/run/ws-audiod/frames.sock";

// Metadata for a chunk of captured audio
struct AudioChunkMeta {
    uint64_t timestamp_us;       // Capture timestamp (microseconds since epoch)
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint64_t frame_count;        // Number of audio frames in this chunk
    uint64_t total_frames;       // Running total since capture start
};

// Audio device configuration
struct AudioConfig {
    std::string device = "default";    // ALSA device identifier
    uint32_t sample_rate = 48000;
    uint16_t channels = 1;
    uint16_t bits_per_sample = 16;     // 16, 24, or 32
    uint32_t period_size = 1024;       // Frames per ALSA period
    uint32_t buffer_periods = 4;       // Number of periods in ALSA buffer
    double gain_db = 0.0;              // Input gain in dB
    bool dc_remove = false;            // DC offset removal filter
};

// Block recorder configuration
struct BlockRecorderConfig {
    bool enabled = false;
    std::string output_dir = DEFAULT_BLOCKS_DIR;
    uint32_t block_duration_seconds = 300;  // 5 minutes default
    std::string format = "wav";             // "wav" or "flac"
    uint32_t max_blocks = 0;                // 0 = unlimited
};

// Daemon configuration
struct DaemonConfig {
    std::string socket_path = DEFAULT_SOCKET_PATH;
    std::string clips_dir = DEFAULT_CLIPS_DIR;
    std::string shm_name = DEFAULT_SHM_NAME;

    uint32_t ring_buffer_seconds = 60;  // Rolling buffer length

    bool enable_streaming = false;
    uint16_t stream_port = 8001;

    bool enable_sample_sharing = false; // Share raw samples via shm

    AudioConfig audio;
    BlockRecorderConfig block_recorder;
};

// Callback types
using AudioChunkCallback = std::function<void(const AudioChunkMeta&, const uint8_t*, size_t)>;

// Utility functions
inline uint64_t get_timestamp_us() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
}

inline std::string timestamp_to_filename() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    struct tm tm_buf;
    localtime_r(&time, &tm_buf);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d_%03d",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
        static_cast<int>(ms));
    return buf;
}

} // namespace audio_daemon

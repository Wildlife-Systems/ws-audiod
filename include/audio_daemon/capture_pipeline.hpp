#pragma once

#include "audio_daemon/common.hpp"
#include "audio_daemon/alsa_capture.hpp"
#include "audio_daemon/ring_buffer.hpp"
#include "audio_daemon/clip_extractor.hpp"
#include "audio_daemon/block_recorder.hpp"
#include "audio_daemon/sample_publisher.hpp"
#include <memory>
#include <atomic>
#include <string>

namespace audio_daemon {

/**
 * Main capture pipeline — the central coordinator.
 *
 * Owns the ALSA capture, ring buffer, clip extractor, block recorder
 * and shared-memory publisher.  Wires the capture callback to all
 * downstream consumers.
 */
class CapturePipeline {
public:
    explicit CapturePipeline(const DaemonConfig& config);
    ~CapturePipeline();

    CapturePipeline(const CapturePipeline&) = delete;
    CapturePipeline& operator=(const CapturePipeline&) = delete;

    bool initialize();
    bool start();
    void stop();

    bool is_running() const { return running_.load(); }

    /** Extract a clip from the ring buffer. */
    std::string capture_clip(int start_offset, int end_offset,
                             const std::string& format = "");

    /** Apply a runtime parameter change. */
    bool set_parameter(const std::string& key, const std::string& value);

    /** Get current status as JSON string. */
    std::string get_status_json() const;

    /** Get current audio levels. */
    std::string get_levels_json() const;

    struct Stats {
        uint64_t frames_captured;
        double capture_fps;
        size_t ring_buffer_frames;
        int16_t peak_level;
        double rms_level;
    };
    Stats get_stats() const;

private:
    void on_audio_chunk(const AudioChunkMeta& meta,
                        const uint8_t* data, size_t sample_count);

    DaemonConfig config_;
    std::atomic<bool> running_{false};

    std::unique_ptr<AlsaCapture> capture_;
    std::unique_ptr<RingBuffer> ring_buffer_;
    std::unique_ptr<ClipExtractor> clip_extractor_;
    std::unique_ptr<BlockRecorder> block_recorder_;
    std::unique_ptr<SamplePublisher> sample_publisher_;

    // Level metering (updated from capture thread)
    std::atomic<int16_t> peak_level_{0};
    std::atomic<double> rms_level_{0.0};
    std::atomic<uint64_t> total_frames_{0};
};

} // namespace audio_daemon

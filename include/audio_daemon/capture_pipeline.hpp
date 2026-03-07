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
#include <vector>

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

    // Mic boost / gain
    std::atomic<double> gain_linear_{1.0};
    std::atomic<double> gain_db_{0.0};
    std::vector<uint8_t> gain_buffer_;  // scratch buffer for gain-adjusted data

    void apply_gain(const uint8_t* src, uint8_t* dst,
                    size_t sample_count, uint16_t bits_per_sample,
                    double gain) const;

    // Stereo-to-mono downmix
    bool downmix_mono_ = false;
    std::vector<uint8_t> mono_buffer_;  // scratch buffer for downmixed data

    void apply_downmix(const uint8_t* src, uint8_t* dst,
                       size_t frame_count, uint16_t bits_per_sample) const;

    // DC offset removal (single-pole high-pass IIR per channel)
    std::atomic<bool> dc_remove_{false};
    std::vector<float> dc_prev_x_;  // previous input per channel
    std::vector<float> dc_prev_y_;  // previous output per channel
    static constexpr float DC_ALPHA = 0.999f;

    void apply_dc_remove(uint8_t* data, size_t sample_count,
                         uint16_t channels, uint16_t bits_per_sample);

    // Level metering (updated from capture thread)
    std::atomic<int16_t> peak_level_{0};
    std::atomic<double> rms_level_{0.0};
    std::atomic<uint64_t> total_frames_{0};
};

} // namespace audio_daemon

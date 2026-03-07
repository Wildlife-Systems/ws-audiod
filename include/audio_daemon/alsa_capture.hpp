#pragma once

#include "audio_daemon/common.hpp"
#include <alsa/asoundlib.h>
#include <thread>
#include <atomic>
#include <vector>

namespace audio_daemon {

/**
 * ALSA PCM capture.
 *
 * Opens an ALSA capture device, configures hardware parameters and reads
 * interleaved PCM samples in a dedicated thread.  Each period is delivered
 * to registered callbacks as a contiguous block of int16_t samples.
 */
class AlsaCapture {
public:
    explicit AlsaCapture(const AudioConfig& config);
    ~AlsaCapture();

    AlsaCapture(const AlsaCapture&) = delete;
    AlsaCapture& operator=(const AlsaCapture&) = delete;

    /** Open the ALSA device and configure hardware parameters. */
    bool initialize();

    /** Start the capture thread. */
    bool start();

    /** Stop the capture thread and close the device. */
    void stop();

    /** Register a callback invoked for every captured period. */
    void set_callback(AudioChunkCallback cb) { callback_ = std::move(cb); }

    /** Actual negotiated sample rate (may differ from requested). */
    uint32_t actual_rate() const { return actual_rate_; }

    /** Actual negotiated period size. */
    uint32_t actual_period_size() const { return actual_period_; }

private:
    void capture_thread_func();

    AudioConfig config_;
    snd_pcm_t* pcm_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    AudioChunkCallback callback_;

    uint32_t actual_rate_ = 0;
    uint32_t actual_period_ = 0;
    uint64_t total_frames_ = 0;

    std::vector<uint8_t> period_buffer_;
};

} // namespace audio_daemon

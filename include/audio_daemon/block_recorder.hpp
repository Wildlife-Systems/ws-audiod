#pragma once

#include "audio_daemon/common.hpp"
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <sndfile.h>

namespace audio_daemon {

/**
 * Gapless block recorder.
 *
 * Writes consecutive audio files of a fixed duration (e.g. 5 minutes)
 * with zero inter-file sample gap.  The recorder is fed samples from
 * the capture thread via push(); it accumulates exactly
 * (block_duration_seconds * sample_rate) frames per file, then closes
 * the current file and immediately opens the next one.  No samples are
 * buffered in memory between files — the transition is a simple file
 * handle swap.
 *
 * Output filenames encode the UTC start time of each block:
 *   block_20260305_143000.wav   (started at 14:30:00)
 *   block_20260305_143500.wav   (started at 14:35:00)
 *
 * Thread safety: push() is called from the capture thread.  File I/O
 * runs inline (the ALSA period callback writes directly to disk).
 * This keeps latency bounded to a single disk write per period and
 * avoids the need for an intermediate queue.
 */
class BlockRecorder {
public:
    explicit BlockRecorder(const BlockRecorderConfig& config,
                           uint32_t sample_rate, uint16_t channels,
                           uint16_t bits_per_sample);
    ~BlockRecorder();

    BlockRecorder(const BlockRecorder&) = delete;
    BlockRecorder& operator=(const BlockRecorder&) = delete;

    /** Prepare output directory; must be called before push(). */
    bool initialize();

    /** Start accepting samples (enables push). */
    void start();

    /** Flush and close the current file. */
    void stop();

    /**
     * Feed captured samples to the recorder.
     * Called from the ALSA capture thread for every period.
     * Writes are synchronous — the function returns after the disk
     * write completes.
     */
    void push(const uint8_t* data, size_t frame_count);

    /** Whether the recorder is actively writing. */
    bool is_recording() const { return recording_.load(); }

    struct Stats {
        uint64_t blocks_written;
        uint64_t total_frames_written;
        std::string current_file;
    };
    Stats get_stats() const;

private:
    /** Open (or rotate to) a new output file. */
    bool open_next_file();

    /** Close the current file. */
    void close_current_file();

    /** Build a filename for a block starting at the given time. */
    std::string make_filename(uint64_t timestamp_us) const;

    BlockRecorderConfig config_;
    uint32_t sample_rate_;
    uint16_t channels_;
    uint16_t bits_per_sample_;
    uint64_t block_frames_;        // frames per block

    std::atomic<bool> recording_{false};

    // Current output file state (accessed only from capture thread)
    SNDFILE* sndfile_ = nullptr;
    std::string current_path_;
    uint64_t frames_in_block_ = 0;

    // Stats (atomic for lock-free reads from control thread)
    std::atomic<uint64_t> blocks_written_{0};
    std::atomic<uint64_t> total_frames_written_{0};
    mutable std::mutex stats_mutex_;
    std::string stats_current_file_;
};

} // namespace audio_daemon

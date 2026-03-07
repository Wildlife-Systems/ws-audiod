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
 * the capture thread via push(), which copies into an SPSC ring buffer.
 * A dedicated writer thread drains the ring and performs all disk I/O,
 * keeping the capture thread free of blocking syscalls.
 *
 * Output filenames encode the UTC start time of each block:
 *   block_20260305_143000.wav   (started at 14:30:00)
 *   block_20260305_143500.wav   (started at 14:35:00)
 *
 * Thread safety: push() is lock-free (capture thread).  All file I/O
 * runs on the writer thread.
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

    /** Start accepting samples and the writer thread. */
    void start();

    /** Signal the writer thread to drain remaining data and exit. */
    void stop();

    /**
     * Feed captured samples to the recorder.
     * Called from the ALSA capture thread for every period.
     * Lock-free: copies into an SPSC ring buffer and signals the writer.
     */
    void push(const uint8_t* data, size_t frame_count);

    struct Stats {
        uint64_t blocks_written;
        uint64_t total_frames_written;
        std::string current_file;
    };
    Stats get_stats() const;

private:
    // ── writer thread ──────────────────────────────────────────────
    void writer_thread_func();

    /** Open (or rotate to) a new output file. */
    bool open_next_file();

    /** Close the current file (flushes buffer first). */
    void close_current_file();

    /** Flush the write buffer to the current sndfile. */
    void flush_buffer();

    /** Build a filename for a block starting at the given time. */
    std::string make_filename(uint64_t timestamp_us) const;

    BlockRecorderConfig config_;
    uint32_t sample_rate_;
    uint16_t channels_;
    uint16_t bits_per_sample_;
    uint64_t block_frames_;        // frames per block
    size_t bytes_per_frame_;

    std::atomic<bool> recording_{false};

    // ── SPSC ring buffer (capture thread → writer thread) ──────────
    std::vector<uint8_t> ring_;
    size_t ring_capacity_ = 0;            // bytes, always power of 2
    size_t ring_mask_ = 0;
    std::atomic<size_t> ring_write_{0};   // write cursor (bytes)
    std::atomic<size_t> ring_read_{0};    // read cursor (bytes)

    // Writer thread signalling
    std::thread writer_thread_;
    std::mutex writer_mutex_;
    std::condition_variable writer_cv_;
    std::atomic<bool> writer_running_{false};

    // File state — accessed only from writer thread
    SNDFILE* sndfile_ = nullptr;
    std::string current_path_;
    uint64_t frames_in_block_ = 0;

    // Disk write buffer — batches small ring drains into larger writes
    std::vector<uint8_t> write_buffer_;
    size_t buffer_used_ = 0;
    size_t flush_threshold_;

    // Stats (atomic for lock-free reads from control thread)
    std::atomic<uint64_t> blocks_written_{0};
    std::atomic<uint64_t> total_frames_written_{0};
    mutable std::mutex stats_mutex_;
    std::string stats_current_file_;
};

} // namespace audio_daemon

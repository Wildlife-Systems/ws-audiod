#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <mutex>
#include <atomic>

namespace audio_daemon {

/**
 * Lock-free-ish ring buffer for PCM audio samples.
 *
 * Stores raw interleaved int16_t samples in a fixed-size circular buffer.
 * The writer (capture thread) appends samples; readers can extract arbitrary
 * time ranges from the buffered history.
 */
class RingBuffer {
public:
    /**
     * @param capacity_frames  Total frames the buffer can hold.
     * @param channels         Number of interleaved channels per frame.
     * @param bytes_per_sample Bytes per sample (2 for 16-bit, 3 for 24-bit, 4 for 32-bit).
     */
    RingBuffer(size_t capacity_frames, uint16_t channels, uint16_t bytes_per_sample = 2);

    /** Append interleaved samples.  Overwrites oldest data when full. */
    void write(const uint8_t* data, size_t frame_count);

    /**
     * Read a range of frames into @p out.
     * @param offset_frames  How many frames back from the write head
     *                       (0 = most recent frame).
     * @param count_frames   Number of frames to read.
     * @return Actual number of frames copied (may be less if the
     *         requested range exceeds buffered data).
     */
    size_t read(uint8_t* out, size_t offset_frames, size_t count_frames) const;

    /** Number of frames currently stored (up to capacity). */
    size_t available_frames() const;

    /** Total capacity in frames. */
    size_t capacity_frames() const { return capacity_frames_; }

    uint16_t channels() const { return channels_; }

    uint16_t bytes_per_sample() const { return bytes_per_sample_; }

    /** Bytes per frame (channels * bytes_per_sample). */
    size_t bytes_per_frame() const { return static_cast<size_t>(channels_) * bytes_per_sample_; }

private:
    std::vector<uint8_t> buffer_;
    size_t capacity_frames_;     // always a power of 2
    size_t capacity_mask_;       // capacity_frames_ - 1, for fast modulo
    uint16_t channels_;
    uint16_t bytes_per_sample_;
    std::atomic<size_t> write_pos_{0};   // next write position (in frames)
    std::atomic<size_t> total_written_{0};
    mutable std::mutex read_mutex_;
};

} // namespace audio_daemon

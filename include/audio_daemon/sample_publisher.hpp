#pragma once

#include <string>
#include <atomic>
#include <cstdint>

namespace audio_daemon {

/**
 * Publishes raw audio samples to POSIX shared memory.
 *
 * Layout (all little-endian):
 *   [0..3]   uint32  magic  (0x41554449 = "AUDI")
 *   [4..7]   uint32  sample_rate
 *   [8..9]   uint16  channels
 *   [10..11] uint16  bits_per_sample
 *   [12..15] uint32  period_frames  (frames per update)
 *   [16..23] uint64  write_counter  (monotonic, increments per period)
 *   [24..31] uint64  timestamp_us
 *   [32..63] reserved
 *   [64..]   sample data (interleaved int16_t, one period)
 */
class SamplePublisher {
public:
    static constexpr size_t HEADER_SIZE = 64;
    static constexpr uint32_t MAGIC = 0x41554449; // "AUDI"

    SamplePublisher(const std::string& shm_name,
                    uint32_t sample_rate, uint16_t channels,
                    uint16_t bits_per_sample, uint32_t period_frames);
    ~SamplePublisher();

    SamplePublisher(const SamplePublisher&) = delete;
    SamplePublisher& operator=(const SamplePublisher&) = delete;

    bool initialize();
    void publish(const uint8_t* data, size_t frame_count, uint64_t timestamp_us);
    void cleanup();

private:
    std::string shm_name_;
    uint32_t sample_rate_;
    uint16_t channels_;
    uint16_t bits_per_sample_;
    uint32_t period_frames_;

    int shm_fd_ = -1;
    uint8_t* shm_ptr_ = nullptr;
    size_t shm_size_ = 0;
    std::atomic<uint64_t> write_counter_{0};
};

} // namespace audio_daemon

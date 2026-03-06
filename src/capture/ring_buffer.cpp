#include "audio_daemon/ring_buffer.hpp"
#include <algorithm>
#include <cstring>

namespace audio_daemon {

RingBuffer::RingBuffer(size_t capacity_frames, uint16_t channels, uint16_t bytes_per_sample)
    : capacity_frames_(capacity_frames), channels_(channels), bytes_per_sample_(bytes_per_sample) {
    buffer_.resize(capacity_frames * channels * bytes_per_sample, 0);
}

void RingBuffer::write(const uint8_t* data, size_t frame_count) {
    size_t bpf = bytes_per_frame();
    size_t pos = write_pos_.load(std::memory_order_relaxed);

    size_t remaining = frame_count;
    const uint8_t* src = data;

    while (remaining > 0) {
        size_t idx = pos % capacity_frames_;
        size_t batch = std::min(remaining, capacity_frames_ - idx);
        std::memcpy(&buffer_[idx * bpf], src, batch * bpf);
        src += batch * bpf;
        pos += batch;
        remaining -= batch;
    }

    write_pos_.store(pos, std::memory_order_release);
    total_written_.fetch_add(frame_count, std::memory_order_release);
}

size_t RingBuffer::read(uint8_t* out, size_t offset_frames,
                        size_t count_frames) const {
    std::lock_guard<std::mutex> lock(read_mutex_);

    size_t total = total_written_.load(std::memory_order_acquire);
    size_t avail = std::min(total, capacity_frames_);

    if (offset_frames >= avail) return 0;

    size_t readable = std::min(count_frames, avail - offset_frames);
    size_t wp = write_pos_.load(std::memory_order_acquire);
    size_t bpf = bytes_per_frame();

    // Start reading from (wp - offset_frames - readable) mod capacity
    size_t start;
    if (wp >= offset_frames + readable) {
        start = wp - offset_frames - readable;
    } else {
        // wrap-around arithmetic
        start = capacity_frames_ - ((offset_frames + readable - wp) % capacity_frames_);
    }

    size_t copied = 0;
    while (copied < readable) {
        size_t idx = (start + copied) % capacity_frames_;
        size_t batch = std::min(readable - copied, capacity_frames_ - idx);
        std::memcpy(out + copied * bpf,
                     &buffer_[idx * bpf],
                     batch * bpf);
        copied += batch;
    }

    return readable;
}

size_t RingBuffer::available_frames() const {
    size_t total = total_written_.load(std::memory_order_acquire);
    return std::min(total, capacity_frames_);
}

} // namespace audio_daemon

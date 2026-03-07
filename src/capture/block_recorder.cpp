#include "audio_daemon/block_recorder.hpp"
#include "audio_daemon/logger.hpp"
#include <filesystem>
#include <ctime>
#include <cstring>

namespace audio_daemon {

// Round up to next power of 2
static size_t next_pow2(size_t v) {
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16; v |= v >> 32;
    return v + 1;
}

BlockRecorder::BlockRecorder(const BlockRecorderConfig& config,
                             uint32_t sample_rate, uint16_t channels,
                             uint16_t bits_per_sample)
    : config_(config),
      sample_rate_(sample_rate),
      channels_(channels),
      bits_per_sample_(bits_per_sample),
      block_frames_(static_cast<uint64_t>(config.block_duration_seconds) * sample_rate),
      bytes_per_frame_(static_cast<size_t>(channels) * (bits_per_sample / 8)) {
    // Flush threshold: ~1 second of audio, or 64KB, whichever is larger.
    size_t one_second = sample_rate * bytes_per_frame_;
    flush_threshold_ = std::max(one_second, static_cast<size_t>(65536));
    write_buffer_.resize(flush_threshold_ * 2);

    // SPSC ring: hold at least 4 seconds of audio so the writer thread
    // can survive an fsync stall without the capture thread dropping data.
    size_t ring_bytes = next_pow2(one_second * 4);
    ring_.resize(ring_bytes);
    ring_capacity_ = ring_bytes;
    ring_mask_ = ring_bytes - 1;
}

BlockRecorder::~BlockRecorder() {
    stop();
}

bool BlockRecorder::initialize() {
    if (!config_.enabled) {
        LOG_INFO("Block recorder disabled");
        return true;
    }

    std::filesystem::create_directories(config_.output_dir);
    LOG_INFO("Block recorder initialised: ",
             config_.block_duration_seconds, "s blocks, format=",
             config_.format, ", output=", config_.output_dir,
             ", ring=", ring_capacity_ / 1024, "KB");
    return true;
}

void BlockRecorder::start() {
    if (!config_.enabled) return;
    recording_ = true;
    writer_running_ = true;
    writer_thread_ = std::thread(&BlockRecorder::writer_thread_func, this);
    LOG_INFO("Block recorder started");
}

void BlockRecorder::stop() {
    recording_ = false;
    if (writer_running_) {
        writer_running_ = false;
        writer_cv_.notify_one();
        if (writer_thread_.joinable()) writer_thread_.join();
    }
    LOG_INFO("Block recorder stopped");
}

void BlockRecorder::push(const uint8_t* data, size_t frame_count) {
    if (!recording_.load(std::memory_order_relaxed)) return;

    size_t bytes = frame_count * bytes_per_frame_;

    // Check available space in SPSC ring
    size_t w = ring_write_.load(std::memory_order_relaxed);
    size_t r = ring_read_.load(std::memory_order_acquire);
    size_t used = w - r;  // works due to unsigned wraparound
    size_t avail = ring_capacity_ - used;

    if (bytes > avail) {
        // Ring full — writer thread can't keep up, drop this period
        return;
    }

    // Copy into ring (may wrap around)
    size_t pos = w & ring_mask_;
    size_t first = std::min(bytes, ring_capacity_ - pos);
    std::memcpy(ring_.data() + pos, data, first);
    if (first < bytes) {
        std::memcpy(ring_.data(), data + first, bytes - first);
    }

    ring_write_.store(w + bytes, std::memory_order_release);
    writer_cv_.notify_one();
}

// ── writer thread ────────────────────────────────────────────────

void BlockRecorder::writer_thread_func() {
    LOG_DEBUG("Block recorder writer thread started");

    while (true) {
        // Wait until there's data or we're told to stop
        {
            std::unique_lock<std::mutex> lock(writer_mutex_);
            writer_cv_.wait(lock, [this] {
                size_t w = ring_write_.load(std::memory_order_acquire);
                size_t r = ring_read_.load(std::memory_order_relaxed);
                return (w - r) >= flush_threshold_ || !writer_running_;
            });
        }

        // Drain everything available from the ring
        while (true) {
            size_t w = ring_write_.load(std::memory_order_acquire);
            size_t r = ring_read_.load(std::memory_order_relaxed);
            size_t avail = w - r;
            if (avail == 0) break;

            // Read a chunk — up to what fits in the write buffer
            size_t chunk = std::min(avail, write_buffer_.size() - buffer_used_);
            size_t pos = r & ring_mask_;
            size_t first = std::min(chunk, ring_capacity_ - pos);
            std::memcpy(write_buffer_.data() + buffer_used_,
                        ring_.data() + pos, first);
            if (first < chunk) {
                std::memcpy(write_buffer_.data() + buffer_used_ + first,
                            ring_.data(), chunk - first);
            }
            buffer_used_ += chunk;
            ring_read_.store(r + chunk, std::memory_order_release);

            // Process frames from the write buffer
            size_t frame_bytes = buffer_used_ - (buffer_used_ % bytes_per_frame_);
            size_t frames = frame_bytes / bytes_per_frame_;
            if (frames == 0) continue;

            const uint8_t* src = write_buffer_.data();
            size_t src_remaining = frames;

            while (src_remaining > 0) {
                if (!sndfile_) {
                    if (!open_next_file()) {
                        LOG_ERROR("Block recorder: cannot open output file, "
                                  "dropping samples");
                        buffer_used_ = 0;
                        goto drain_done;
                    }
                }

                size_t space = block_frames_ - frames_in_block_;
                size_t batch = std::min(src_remaining, static_cast<size_t>(space));
                size_t batch_bytes_len = batch * bytes_per_frame_;

                // Write to file
                sf_count_t written;
                if (bits_per_sample_ >= 24) {
                    written = sf_writef_int(
                        sndfile_,
                        reinterpret_cast<const int*>(src),
                        static_cast<sf_count_t>(batch));
                } else {
                    written = sf_writef_short(
                        sndfile_,
                        reinterpret_cast<const short*>(src),
                        static_cast<sf_count_t>(batch));
                }

                if (written < 0) {
                    LOG_ERROR("Block recorder: write error: ",
                              sf_strerror(sndfile_));
                } else {
                    total_frames_written_.fetch_add(
                        static_cast<uint64_t>(written),
                        std::memory_order_relaxed);
                }

                frames_in_block_ += batch;
                src += batch_bytes_len;
                src_remaining -= batch;

                if (frames_in_block_ >= block_frames_) {
                    close_current_file();
                    blocks_written_.fetch_add(1, std::memory_order_relaxed);
                }
            }

            // Keep any leftover partial frame
            size_t leftover = buffer_used_ - frame_bytes;
            if (leftover > 0) {
                std::memmove(write_buffer_.data(),
                             write_buffer_.data() + frame_bytes, leftover);
            }
            buffer_used_ = leftover;
        }
        drain_done:

        if (!writer_running_ && ring_write_.load(std::memory_order_acquire)
                                == ring_read_.load(std::memory_order_relaxed)) {
            break;
        }
    }

    close_current_file();
    LOG_DEBUG("Block recorder writer thread stopped");
}

BlockRecorder::Stats BlockRecorder::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return {
        blocks_written_.load(),
        total_frames_written_.load(),
        stats_current_file_
    };
}

bool BlockRecorder::open_next_file() {
    std::string path = make_filename(get_timestamp_us());

    int sf_subtype;
    switch (bits_per_sample_) {
        case 32: sf_subtype = SF_FORMAT_PCM_32; break;
        case 24: sf_subtype = SF_FORMAT_PCM_24; break;
        default: sf_subtype = SF_FORMAT_PCM_16; break;
    }

    int sf_format;
    if (config_.format == "flac") {
        sf_format = SF_FORMAT_FLAC | sf_subtype;
    } else {
        sf_format = SF_FORMAT_WAV | sf_subtype;
    }

    SF_INFO info = {};
    info.samplerate = static_cast<int>(sample_rate_);
    info.channels = channels_;
    info.format = sf_format;

    sndfile_ = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!sndfile_) {
        LOG_ERROR("Cannot open block file: ", path,
                  " (", sf_strerror(nullptr), ")");
        return false;
    }

    current_path_ = path;
    frames_in_block_ = 0;

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_current_file_ = path;
    }

    LOG_INFO("Block recorder: new file ", path);
    return true;
}

void BlockRecorder::close_current_file() {
    if (sndfile_) {
        sf_write_sync(sndfile_);
        sf_close(sndfile_);
        sndfile_ = nullptr;
        LOG_INFO("Block recorder: closed ", current_path_,
                 " (", frames_in_block_, " frames)");
        current_path_.clear();
    }
}

void BlockRecorder::flush_buffer() {
    // No longer used — writes happen inline in writer_thread_func.
    // Kept for interface compatibility.
}

std::string BlockRecorder::make_filename(uint64_t timestamp_us) const {
    auto secs = static_cast<time_t>(timestamp_us / 1'000'000);
    struct tm tm_buf;
    gmtime_r(&secs, &tm_buf);

    char name[128];
    std::snprintf(name, sizeof(name), "block_%04d%02d%02d_%02d%02d%02d.%s",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
        config_.format.c_str());

    return config_.output_dir + "/" + name;
}

} // namespace audio_daemon

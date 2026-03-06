#include "audio_daemon/block_recorder.hpp"
#include "audio_daemon/logger.hpp"
#include <filesystem>
#include <ctime>
#include <cstring>

namespace audio_daemon {

BlockRecorder::BlockRecorder(const BlockRecorderConfig& config,
                             uint32_t sample_rate, uint16_t channels,
                             uint16_t bits_per_sample)
    : config_(config),
      sample_rate_(sample_rate),
      channels_(channels),
      bits_per_sample_(bits_per_sample),
      block_frames_(static_cast<uint64_t>(config.block_duration_seconds) * sample_rate) {}

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
             config_.format, ", output=", config_.output_dir);
    return true;
}

void BlockRecorder::start() {
    if (!config_.enabled) return;
    recording_ = true;
    LOG_INFO("Block recorder started");
}

void BlockRecorder::stop() {
    recording_ = false;
    close_current_file();
    LOG_INFO("Block recorder stopped");
}

void BlockRecorder::push(const uint8_t* data, size_t frame_count) {
    if (!recording_) return;

    const uint8_t* src = data;
    size_t remaining = frame_count;
    size_t bytes_per_frame = static_cast<size_t>(channels_) * (bits_per_sample_ / 8);

    while (remaining > 0) {
        // Open a new file if we don't have one
        if (!sndfile_) {
            if (!open_next_file()) {
                LOG_ERROR("Block recorder: cannot open output file, dropping samples");
                return;
            }
        }

        // How many frames fit in the current block?
        size_t space = block_frames_ - frames_in_block_;
        size_t batch = std::min(remaining, space);

        sf_count_t written;
        if (bits_per_sample_ == 32) {
            written = sf_writef_int(sndfile_, reinterpret_cast<const int*>(src),
                                    static_cast<sf_count_t>(batch));
        } else if (bits_per_sample_ == 24) {
            // libsndfile handles 24-bit via int with SF_FORMAT_PCM_24
            written = sf_writef_int(sndfile_, reinterpret_cast<const int*>(src),
                                    static_cast<sf_count_t>(batch));
        } else {
            written = sf_writef_short(sndfile_, reinterpret_cast<const short*>(src),
                                      static_cast<sf_count_t>(batch));
        }
        if (written < 0) {
            LOG_ERROR("Block recorder: write error: ", sf_strerror(sndfile_));
            close_current_file();
            return;
        }

        frames_in_block_ += static_cast<uint64_t>(written);
        total_frames_written_.fetch_add(static_cast<uint64_t>(written),
                                        std::memory_order_relaxed);
        src += static_cast<size_t>(written) * bytes_per_frame;
        remaining -= static_cast<size_t>(written);

        // Rotate to next file if block is full
        if (frames_in_block_ >= block_frames_) {
            close_current_file();
            blocks_written_.fetch_add(1, std::memory_order_relaxed);
        }
    }
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

std::string BlockRecorder::make_filename(uint64_t timestamp_us) const {
    // Convert to time struct
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

#include "audio_daemon/clip_extractor.hpp"
#include "audio_daemon/logger.hpp"
#include <sndfile.h>
#include <filesystem>

namespace audio_daemon {

ClipExtractor::ClipExtractor(const Config& config, RingBuffer& ring_buffer,
                             uint32_t sample_rate, uint16_t channels,
                             uint16_t bits_per_sample)
    : config_(config), ring_buffer_(ring_buffer),
      sample_rate_(sample_rate), channels_(channels),
      bits_per_sample_(bits_per_sample) {
    std::filesystem::create_directories(config_.output_dir);
}

ClipExtractor::~ClipExtractor() {
    stop();
}

bool ClipExtractor::start() {
    if (running_) return true;
    running_ = true;
    thread_ = std::thread(&ClipExtractor::worker_func, this);
    LOG_INFO("Clip extractor started, output: ", config_.output_dir);
    return true;
}

void ClipExtractor::stop() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        running_ = false;
    }
    queue_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

uint64_t ClipExtractor::request_clip(int start_offset, int end_offset,
                                     const std::string& format) {
    ClipRequest req;
    req.request_id = next_id_.fetch_add(1);
    req.start_offset = start_offset;
    req.end_offset = end_offset;
    req.format = format.empty() ? config_.format : format;
    req.request_timestamp = get_timestamp_us();

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push(req);
    }
    queue_cv_.notify_one();

    return req.request_id;
}

std::string ClipExtractor::request_clip_sync(int start_offset, int end_offset,
                                              const std::string& format) {
    uint64_t id = request_clip(start_offset, end_offset, format);

    std::unique_lock<std::mutex> lock(results_mutex_);
    results_cv_.wait(lock, [&] {
        return results_.count(id) > 0 || !running_;
    });

    auto it = results_.find(id);
    if (it == results_.end()) return "";
    std::string path = std::move(it->second);
    results_.erase(it);
    return path;
}

void ClipExtractor::worker_func() {
    LOG_DEBUG("Clip extractor thread started");

    while (true) {
        ClipRequest req;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !queue_.empty() || !running_;
            });
            if (!running_ && queue_.empty()) break;
            req = queue_.front();
            queue_.pop();
        }

        std::string path = extract_clip(req);
        if (path.empty()) {
            LOG_ERROR("Clip extraction failed for request ", req.request_id);
        } else {
            LOG_INFO("Clip saved: ", path);
        }

        {
            std::lock_guard<std::mutex> lock(results_mutex_);
            results_[req.request_id] = path;
        }
        results_cv_.notify_all();
    }

    LOG_DEBUG("Clip extractor thread stopped");
}

std::string ClipExtractor::extract_clip(const ClipRequest& req) {
    // If end_offset is positive (future), wait for samples to arrive
    if (req.end_offset > 0) {
        auto wait_ms = static_cast<int>(req.end_offset * 1000);
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    }

    // Calculate frame range
    int total_seconds = req.end_offset - req.start_offset;
    if (total_seconds <= 0) {
        LOG_ERROR("Invalid clip range: ", req.start_offset, " to ", req.end_offset);
        return "";
    }

    size_t total_frames = static_cast<size_t>(total_seconds) * sample_rate_;

    // Offset from write head: -start_offset seconds of history
    // For CLIP -5 5: after waiting 5s, the -5s data is 10s back
    size_t offset_from_head = 0;  // we already waited for end_offset

    size_t bytes_per_sample = bits_per_sample_ / 8;
    size_t bytes_per_frame = static_cast<size_t>(channels_) * bytes_per_sample;

    std::vector<uint8_t> samples(total_frames * bytes_per_frame);
    size_t read = ring_buffer_.read(samples.data(), offset_from_head, total_frames);
    if (read == 0) {
        LOG_ERROR("No samples available in ring buffer");
        return "";
    }
    samples.resize(read * bytes_per_frame);

    std::string base = config_.output_dir + "/clip_" + timestamp_to_filename();
    return write_sndfile(samples, base, req.format);
}

std::string ClipExtractor::write_sndfile(const std::vector<uint8_t>& samples,
                                         const std::string& path,
                                         const std::string& format) {
    int sf_pcm_format;
    switch (bits_per_sample_) {
        case 32: sf_pcm_format = SF_FORMAT_PCM_32; break;
        case 24: sf_pcm_format = SF_FORMAT_PCM_24; break;
        default: sf_pcm_format = SF_FORMAT_PCM_16; break;
    }

    int sf_format;
    std::string ext;
    if (format == "flac") {
        sf_format = SF_FORMAT_FLAC | sf_pcm_format;
        ext = ".flac";
    } else {
        sf_format = SF_FORMAT_WAV | sf_pcm_format;
        ext = ".wav";
    }

    std::string full_path = path + ext;

    SF_INFO info = {};
    info.samplerate = static_cast<int>(sample_rate_);
    info.channels = channels_;
    info.format = sf_format;

    SNDFILE* sf = sf_open(full_path.c_str(), SFM_WRITE, &info);
    if (!sf) {
        LOG_ERROR("Cannot open output file: ", full_path,
                  " (", sf_strerror(nullptr), ")");
        return "";
    }

    size_t bytes_per_sample = bits_per_sample_ / 8;
    sf_count_t total_samples = static_cast<sf_count_t>(samples.size() / bytes_per_sample);
    sf_count_t written;
    if (bits_per_sample_ == 32) {
        written = sf_write_int(sf, reinterpret_cast<const int*>(samples.data()), total_samples);
    } else if (bits_per_sample_ == 24) {
        written = sf_write_int(sf, reinterpret_cast<const int*>(samples.data()), total_samples);
    } else {
        written = sf_write_short(sf, reinterpret_cast<const short*>(samples.data()), total_samples);
    }
    sf_close(sf);

    if (written != total_samples) {
        LOG_WARN("Short write: ", written, " of ", total_samples, " samples");
    }

    return full_path;
}

} // namespace audio_daemon

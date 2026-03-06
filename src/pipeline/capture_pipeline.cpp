#include "audio_daemon/capture_pipeline.hpp"
#include "audio_daemon/logger.hpp"
#include <cmath>
#include <sstream>

namespace audio_daemon {

CapturePipeline::CapturePipeline(const DaemonConfig& config)
    : config_(config) {}

CapturePipeline::~CapturePipeline() {
    stop();
}

bool CapturePipeline::initialize() {
    LOG_INFO("Initialising capture pipeline");

    // ALSA capture
    capture_ = std::make_unique<AlsaCapture>(config_.audio);
    if (!capture_->initialize()) {
        LOG_ERROR("Failed to initialise ALSA capture");
        return false;
    }

    uint32_t rate = capture_->actual_rate();
    uint16_t ch = config_.audio.channels;
    uint16_t bps = config_.audio.bits_per_sample;

    // Ring buffer
    size_t rb_frames = static_cast<size_t>(config_.ring_buffer_seconds) * rate;
    ring_buffer_ = std::make_unique<RingBuffer>(rb_frames, ch, bps / 8);

    // Clip extractor
    ClipExtractor::Config clip_cfg;
    clip_cfg.output_dir = config_.clips_dir;
    clip_extractor_ = std::make_unique<ClipExtractor>(
        clip_cfg, *ring_buffer_, rate, ch, bps);

    // Block recorder
    if (config_.block_recorder.enabled) {
        block_recorder_ = std::make_unique<BlockRecorder>(
            config_.block_recorder, rate, ch, bps);
        if (!block_recorder_->initialize()) {
            LOG_ERROR("Failed to initialise block recorder");
            return false;
        }
    }

    // Shared memory publisher
    if (config_.enable_sample_sharing) {
        sample_publisher_ = std::make_unique<SamplePublisher>(
            config_.shm_name, rate, ch, bps, capture_->actual_period_size());
        if (!sample_publisher_->initialize()) {
            LOG_ERROR("Failed to initialise sample publisher");
            return false;
        }
    }

    // Wire capture callback
    capture_->set_callback(
        [this](const AudioChunkMeta& meta, const uint8_t* data, size_t count) {
            on_audio_chunk(meta, data, count);
        });

    LOG_INFO("Capture pipeline initialised");
    return true;
}

bool CapturePipeline::start() {
    if (running_) return true;

    if (!clip_extractor_->start()) {
        LOG_ERROR("Failed to start clip extractor");
        return false;
    }

    if (block_recorder_) {
        block_recorder_->start();
    }

    if (!capture_->start()) {
        LOG_ERROR("Failed to start ALSA capture");
        return false;
    }

    running_ = true;
    LOG_INFO("Capture pipeline started");
    return true;
}

void CapturePipeline::stop() {
    if (!running_) return;
    running_ = false;

    capture_->stop();
    clip_extractor_->stop();
    if (block_recorder_) block_recorder_->stop();
    if (sample_publisher_) sample_publisher_->cleanup();

    LOG_INFO("Capture pipeline stopped");
}

std::string CapturePipeline::capture_clip(int start_offset, int end_offset,
                                          const std::string& format) {
    return clip_extractor_->request_clip_sync(start_offset, end_offset, format);
}

bool CapturePipeline::set_parameter(const std::string& key,
                                    const std::string& value) {
    if (key == "gain_db" || key == "gain") {
        // Runtime gain change would go here
        LOG_INFO("Set gain_db = ", value);
        return true;
    }
    LOG_WARN("Unknown parameter: ", key);
    return false;
}

void CapturePipeline::on_audio_chunk(const AudioChunkMeta& meta,
                                     const uint8_t* data,
                                     size_t sample_count) {
    size_t frame_count = sample_count / meta.channels;

    // Feed ring buffer
    ring_buffer_->write(data, frame_count);

    // Feed block recorder
    if (block_recorder_) {
        block_recorder_->push(data, frame_count);
    }

    // Feed shared memory publisher
    if (sample_publisher_) {
        sample_publisher_->publish(data, frame_count, meta.timestamp_us);
    }

    // Update level metering (interpret samples based on bit depth)
    int32_t peak = 0;
    double sum_sq = 0.0;
    size_t bytes_per_sample = meta.bits_per_sample / 8;
    for (size_t i = 0; i < sample_count; ++i) {
        int32_t s = 0;
        const uint8_t* p = data + i * bytes_per_sample;
        switch (meta.bits_per_sample) {
            case 16:
                s = static_cast<int16_t>(p[0] | (p[1] << 8));
                break;
            case 24:
                s = p[0] | (p[1] << 8) | (p[2] << 16);
                if (s & 0x800000) s |= 0xFF000000; // sign extend
                break;
            case 32:
                s = static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
                s >>= 16; // scale to 16-bit range for metering
                break;
        }
        int32_t abs_s = std::abs(s);
        if (abs_s > peak) peak = abs_s;
        sum_sq += static_cast<double>(s) * s;
    }
    peak_level_.store(static_cast<int16_t>(std::min(peak, (int32_t)32767)), std::memory_order_relaxed);
    rms_level_.store(std::sqrt(sum_sq / static_cast<double>(sample_count)),
                     std::memory_order_relaxed);
    total_frames_.fetch_add(frame_count, std::memory_order_relaxed);
}

std::string CapturePipeline::get_status_json() const {
    auto stats = get_stats();
    std::ostringstream oss;
    oss << R"({"running":)" << (running_.load() ? "true" : "false")
        << R"(,"capture":{"frames":)" << stats.frames_captured
        << R"(,"rate":)" << capture_->actual_rate()
        << R"(,"channels":)" << config_.audio.channels
        << R"(,"bits":)" << config_.audio.bits_per_sample
        << R"(,"device":")" << config_.audio.device << R"(")"
        << R"(,"ring_buffer_frames":)" << stats.ring_buffer_frames
        << "}";

    if (block_recorder_) {
        auto br = block_recorder_->get_stats();
        oss << R"(,"block_recorder":{"blocks_written":)" << br.blocks_written
            << R"(,"total_frames":)" << br.total_frames_written
            << R"(,"current_file":")" << br.current_file << R"("})";
    }

    oss << "}";
    return oss.str();
}

std::string CapturePipeline::get_levels_json() const {
    std::ostringstream oss;
    int16_t peak = peak_level_.load(std::memory_order_relaxed);
    double rms = rms_level_.load(std::memory_order_relaxed);
    double peak_db = (peak > 0)
        ? 20.0 * std::log10(static_cast<double>(peak) / 32768.0) : -96.0;
    double rms_db = (rms > 0)
        ? 20.0 * std::log10(rms / 32768.0) : -96.0;
    oss << R"({"peak":)" << peak
        << R"(,"peak_db":)" << peak_db
        << R"(,"rms":)" << rms
        << R"(,"rms_db":)" << rms_db << "}";
    return oss.str();
}

CapturePipeline::Stats CapturePipeline::get_stats() const {
    return {
        total_frames_.load(),
        0.0,  // TODO compute actual FPS
        ring_buffer_ ? ring_buffer_->available_frames() : 0,
        peak_level_.load(),
        rms_level_.load()
    };
}

} // namespace audio_daemon

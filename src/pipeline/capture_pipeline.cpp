#include "audio_daemon/capture_pipeline.hpp"
#include "audio_daemon/logger.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <sstream>

namespace audio_daemon {

CapturePipeline::CapturePipeline(const DaemonConfig& config)
    : config_(config) {
    gain_db_.store(config.audio.gain_db, std::memory_order_relaxed);
    gain_linear_.store(std::pow(10.0, config.audio.gain_db / 20.0),
                       std::memory_order_relaxed);
    if (config.audio.gain_db != 0.0) {
        LOG_INFO("Mic boost: ", config.audio.gain_db, " dB (linear ",
                 gain_linear_.load(), ")");
    }

    dc_remove_.store(config.audio.dc_remove, std::memory_order_relaxed);
    dc_prev_x_.resize(config.audio.channels, 0.0);
    dc_prev_y_.resize(config.audio.channels, 0.0);
    if (config.audio.dc_remove) {
        LOG_INFO("DC offset removal enabled");
    }
}

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
        double db = std::stod(value);
        gain_db_.store(db, std::memory_order_relaxed);
        gain_linear_.store(std::pow(10.0, db / 20.0),
                           std::memory_order_relaxed);
        LOG_INFO("Mic boost set to ", db, " dB");
        return true;
    }
    if (key == "dc_remove") {
        bool enabled = (value == "true" || value == "1");
        dc_remove_.store(enabled, std::memory_order_relaxed);
        if (!enabled) {
            std::fill(dc_prev_x_.begin(), dc_prev_x_.end(), 0.0);
            std::fill(dc_prev_y_.begin(), dc_prev_y_.end(), 0.0);
        }
        LOG_INFO("DC offset removal ", enabled ? "enabled" : "disabled");
        return true;
    }
    LOG_WARN("Unknown parameter: ", key);
    return false;
}

void CapturePipeline::on_audio_chunk(const AudioChunkMeta& meta,
                                     const uint8_t* data,
                                     size_t sample_count) {
    size_t frame_count = sample_count / meta.channels;
    size_t bytes_per_sample = meta.bits_per_sample / 8;
    size_t data_size = sample_count * bytes_per_sample;

    // Apply mic boost if gain != 0 dB
    double gain = gain_linear_.load(std::memory_order_relaxed);
    const uint8_t* out_data = data;
    if (gain != 1.0) {
        if (gain_buffer_.size() < data_size) {
            gain_buffer_.resize(data_size);
        }
        apply_gain(data, gain_buffer_.data(), sample_count,
                   meta.bits_per_sample, gain);
        out_data = gain_buffer_.data();
    }

    // Apply DC offset removal (needs mutable buffer)
    if (dc_remove_.load(std::memory_order_relaxed)) {
        if (out_data == data) {
            // Haven't copied yet — need a mutable buffer
            if (gain_buffer_.size() < data_size) {
                gain_buffer_.resize(data_size);
            }
            std::memcpy(gain_buffer_.data(), data, data_size);
            out_data = gain_buffer_.data();
        }
        apply_dc_remove(gain_buffer_.data(), sample_count,
                        meta.channels, meta.bits_per_sample);
    }

    // Feed ring buffer
    ring_buffer_->write(out_data, frame_count);

    // Feed block recorder
    if (block_recorder_) {
        block_recorder_->push(out_data, frame_count);
    }

    // Feed shared memory publisher
    if (sample_publisher_) {
        sample_publisher_->publish(out_data, frame_count, meta.timestamp_us);
    }

    // Update level metering (use boosted data)
    // Switch hoisted outside the loop so the inner loop is tight and vectorisable.
    int32_t peak = 0;
    double sum_sq = 0.0;
    switch (meta.bits_per_sample) {
        case 16: {
            const int16_t* samples16 = reinterpret_cast<const int16_t*>(out_data);
            for (size_t i = 0; i < sample_count; ++i) {
                int32_t s = samples16[i];
                int32_t abs_s = std::abs(s);
                if (abs_s > peak) peak = abs_s;
                sum_sq += static_cast<double>(s) * s;
            }
            break;
        }
        case 24:
            for (size_t i = 0; i < sample_count; ++i) {
                const uint8_t* p = out_data + i * 3;
                int32_t s = p[0] | (p[1] << 8) | (p[2] << 16);
                if (s & 0x800000) s |= 0xFF000000;
                int32_t abs_s = std::abs(s);
                if (abs_s > peak) peak = abs_s;
                sum_sq += static_cast<double>(s) * s;
            }
            break;
        case 32: {
            const int32_t* samples32 = reinterpret_cast<const int32_t*>(out_data);
            for (size_t i = 0; i < sample_count; ++i) {
                int32_t s = samples32[i] >> 16;
                int32_t abs_s = std::abs(s);
                if (abs_s > peak) peak = abs_s;
                sum_sq += static_cast<double>(s) * s;
            }
            break;
        }
    }
    peak_level_.store(static_cast<int16_t>(std::min(peak, (int32_t)32767)), std::memory_order_relaxed);
    rms_level_.store(std::sqrt(sum_sq / static_cast<double>(sample_count)),
                     std::memory_order_relaxed);
    total_frames_.fetch_add(frame_count, std::memory_order_relaxed);
}

void CapturePipeline::apply_gain(const uint8_t* src, uint8_t* dst,
                                 size_t sample_count, uint16_t bits_per_sample,
                                 double gain) const {
    switch (bits_per_sample) {
        case 16: {
            const int16_t* sp = reinterpret_cast<const int16_t*>(src);
            int16_t* dp = reinterpret_cast<int16_t*>(dst);
            for (size_t i = 0; i < sample_count; ++i) {
                int32_t s = static_cast<int32_t>(sp[i] * gain);
                dp[i] = static_cast<int16_t>(std::clamp(s, (int32_t)-32768, (int32_t)32767));
            }
            break;
        }
        case 24:
            for (size_t i = 0; i < sample_count; ++i) {
                const uint8_t* s = src + i * 3;
                uint8_t* d = dst + i * 3;
                int32_t v = s[0] | (s[1] << 8) | (s[2] << 16);
                if (v & 0x800000) v |= 0xFF000000;
                v = static_cast<int32_t>(v * gain);
                v = std::clamp(v, (int32_t)-8388608, (int32_t)8388607);
                d[0] = static_cast<uint8_t>(v & 0xFF);
                d[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
                d[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
            }
            break;
        case 32: {
            const int32_t* sp = reinterpret_cast<const int32_t*>(src);
            int32_t* dp = reinterpret_cast<int32_t*>(dst);
            for (size_t i = 0; i < sample_count; ++i) {
                int64_t s = static_cast<int64_t>(sp[i] * gain);
                dp[i] = static_cast<int32_t>(std::clamp(
                    s, (int64_t)INT32_MIN, (int64_t)INT32_MAX));
            }
            break;
        }
    }
}

void CapturePipeline::apply_dc_remove(uint8_t* data, size_t sample_count,
                                      uint16_t channels,
                                      uint16_t bits_per_sample) {
    // Single-pole high-pass IIR: y[n] = x[n] - x[n-1] + alpha * y[n-1]
    size_t frame_count = sample_count / channels;

    switch (bits_per_sample) {
        case 16: {
            int16_t* samples = reinterpret_cast<int16_t*>(data);
            for (size_t f = 0; f < frame_count; ++f) {
                for (uint16_t ch = 0; ch < channels; ++ch) {
                    double x = samples[f * channels + ch];
                    double y = x - dc_prev_x_[ch] + DC_ALPHA * dc_prev_y_[ch];
                    dc_prev_x_[ch] = x;
                    dc_prev_y_[ch] = y;
                    samples[f * channels + ch] = static_cast<int16_t>(
                        std::clamp(static_cast<int32_t>(y), (int32_t)-32768, (int32_t)32767));
                }
            }
            break;
        }
        case 24:
            for (size_t f = 0; f < frame_count; ++f) {
                for (uint16_t ch = 0; ch < channels; ++ch) {
                    size_t idx = (f * channels + ch) * 3;
                    uint8_t* p = data + idx;
                    int32_t s = p[0] | (p[1] << 8) | (p[2] << 16);
                    if (s & 0x800000) s |= 0xFF000000;
                    double x = s;
                    double y = x - dc_prev_x_[ch] + DC_ALPHA * dc_prev_y_[ch];
                    dc_prev_x_[ch] = x;
                    dc_prev_y_[ch] = y;
                    int32_t out = std::clamp(static_cast<int32_t>(y),
                                            (int32_t)-8388608, (int32_t)8388607);
                    p[0] = static_cast<uint8_t>(out & 0xFF);
                    p[1] = static_cast<uint8_t>((out >> 8) & 0xFF);
                    p[2] = static_cast<uint8_t>((out >> 16) & 0xFF);
                }
            }
            break;
        case 32: {
            int32_t* samples = reinterpret_cast<int32_t*>(data);
            for (size_t f = 0; f < frame_count; ++f) {
                for (uint16_t ch = 0; ch < channels; ++ch) {
                    double x = samples[f * channels + ch];
                    double y = x - dc_prev_x_[ch] + DC_ALPHA * dc_prev_y_[ch];
                    dc_prev_x_[ch] = x;
                    dc_prev_y_[ch] = y;
                    int64_t out64 = static_cast<int64_t>(y);
                    samples[f * channels + ch] = static_cast<int32_t>(std::clamp(
                        out64, (int64_t)INT32_MIN, (int64_t)INT32_MAX));
                }
            }
            break;
        }
    }
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
        << R"(,"gain_db":)" << gain_db_.load(std::memory_order_relaxed)
        << R"(,"dc_remove":)" << (dc_remove_.load(std::memory_order_relaxed) ? "true" : "false")
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

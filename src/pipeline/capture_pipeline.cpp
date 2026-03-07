#include "audio_daemon/capture_pipeline.hpp"
#include "audio_daemon/logger.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <sstream>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace audio_daemon {

CapturePipeline::CapturePipeline(const DaemonConfig& config)
    : config_(config), downmix_mono_(config.audio.downmix_mono) {
    gain_db_.store(static_cast<float>(config.audio.gain_db), std::memory_order_relaxed);
    gain_linear_.store(static_cast<float>(std::pow(10.0, config.audio.gain_db / 20.0)),
                       std::memory_order_relaxed);
    if (config.audio.gain_db != 0.0) {
        LOG_INFO("Mic boost: ", config.audio.gain_db, " dB (linear ",
                 gain_linear_.load(), ")");
    }

    // DC removal state uses output channel count (1 if downmixing)
    uint16_t out_ch = downmix_mono_ ? 1 : config.audio.channels;
    dc_remove_.store(config.audio.dc_remove, std::memory_order_relaxed);
    dc_prev_x_.resize(out_ch, 0.0f);
    dc_prev_y_.resize(out_ch, 0.0f);
    if (config.audio.dc_remove) {
        LOG_INFO("DC offset removal enabled");
    }
    if (downmix_mono_) {
        LOG_INFO("Stereo-to-mono downmix enabled (keep L, discard R)");
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

    // Output channel count: 1 if downmixing stereo to mono
    uint16_t out_ch = (downmix_mono_ && ch >= 2) ? 1 : ch;

    // Ring buffer (sized for output channels)
    size_t rb_frames = static_cast<size_t>(config_.ring_buffer_seconds) * rate;
    ring_buffer_ = std::make_unique<RingBuffer>(rb_frames, out_ch, bps / 8);

    // Clip extractor
    ClipExtractor::Config clip_cfg;
    clip_cfg.output_dir = config_.clips_dir;
    clip_extractor_ = std::make_unique<ClipExtractor>(
        clip_cfg, *ring_buffer_, rate, out_ch, bps);

    // Block recorder
    if (config_.block_recorder.enabled) {
        block_recorder_ = std::make_unique<BlockRecorder>(
            config_.block_recorder, rate, out_ch, bps);
        if (!block_recorder_->initialize()) {
            LOG_ERROR("Failed to initialise block recorder");
            return false;
        }
    }

    // Shared memory publisher
    if (config_.enable_sample_sharing) {
        sample_publisher_ = std::make_unique<SamplePublisher>(
            config_.shm_name, rate, out_ch, bps, capture_->actual_period_size());
        if (!sample_publisher_->initialize()) {
            LOG_ERROR("Failed to initialise sample publisher");
            return false;
        }
    }

    // Pre-allocate buffers to avoid resizing during capture
    size_t max_chunk_bytes = static_cast<size_t>(capture_->actual_period_size())
                             * ch * (bps / 8);
    gain_buffer_.resize(max_chunk_bytes);
    if (downmix_mono_ && ch >= 2) {
        // Mono buffer is half the stereo size
        mono_buffer_.resize(static_cast<size_t>(capture_->actual_period_size())
                            * 1 * (bps / 8));
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
        gain_db_.store(static_cast<float>(db), std::memory_order_relaxed);
        gain_linear_.store(static_cast<float>(std::pow(10.0, db / 20.0)),
                           std::memory_order_relaxed);
        LOG_INFO("Mic boost set to ", db, " dB");
        return true;
    }
    if (key == "dc_remove") {
        bool enabled = (value == "true" || value == "1");
        dc_remove_.store(enabled, std::memory_order_relaxed);
        if (!enabled) {
            std::fill(dc_prev_x_.begin(), dc_prev_x_.end(), 0.0f);
            std::fill(dc_prev_y_.begin(), dc_prev_y_.end(), 0.0f);
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

    // Stereo-to-mono downmix: keep L channel, discard R
    const uint8_t* cur_data = data;
    uint16_t cur_channels = meta.channels;
    size_t cur_samples = sample_count;
    if (downmix_mono_ && meta.channels >= 2) {
        apply_downmix(data, mono_buffer_.data(), frame_count,
                      meta.bits_per_sample);
        cur_data = mono_buffer_.data();
        cur_channels = 1;
        cur_samples = frame_count;  // 1 sample per frame now
    }

    size_t data_size = cur_samples * bytes_per_sample;

    // Apply mic boost if gain != 0 dB
    float gain = gain_linear_.load(std::memory_order_relaxed);
    const uint8_t* out_data = cur_data;
    if (gain != 1.0f) {
        if (gain_buffer_.size() < data_size) {
            gain_buffer_.resize(data_size);
        }
        apply_gain(cur_data, gain_buffer_.data(), cur_samples,
                   meta.bits_per_sample, gain);
        out_data = gain_buffer_.data();
    }

    // Apply DC offset removal (needs mutable buffer)
    if (dc_remove_.load(std::memory_order_relaxed)) {
        if (out_data == cur_data) {
            // Haven't copied yet — need a mutable buffer
            if (gain_buffer_.size() < data_size) {
                gain_buffer_.resize(data_size);
            }
            std::memcpy(gain_buffer_.data(), cur_data, data_size);
            out_data = gain_buffer_.data();
        }
        apply_dc_remove(gain_buffer_.data(), cur_samples,
                        cur_channels, meta.bits_per_sample);
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
#ifdef __ARM_NEON
            // NEON: process 8 samples at a time
            int16x8_t vpeak = vdupq_n_s16(0);
            int64x2_t vsum = vdupq_n_s64(0);
            size_t i = 0;
            size_t neon_end = cur_samples & ~7u;
            for (; i < neon_end; i += 8) {
                int16x8_t v = vld1q_s16(samples16 + i);
                int16x8_t va = vabsq_s16(v);
                vpeak = vmaxq_s16(vpeak, va);
                // Widen to 32-bit and accumulate squares
                int16x4_t lo = vget_low_s16(v);
                int16x4_t hi = vget_high_s16(v);
                int32x4_t sq_lo = vmull_s16(lo, lo);
                int32x4_t sq_hi = vmull_s16(hi, hi);
                // Pairwise add 32->64, accumulate
                vsum = vpadalq_s32(vsum, sq_lo);
                vsum = vpadalq_s32(vsum, sq_hi);
            }
            // Reduce NEON peak
            int16x4_t pk4 = vmax_s16(vget_low_s16(vpeak), vget_high_s16(vpeak));
            int16_t pk_arr[4];
            vst1_s16(pk_arr, pk4);
            for (int j = 0; j < 4; ++j)
                if (pk_arr[j] > peak) peak = pk_arr[j];
            // Reduce NEON sum
            int64_t sum_arr[2];
            vst1q_s64(sum_arr, vsum);
            sum_sq += static_cast<double>(sum_arr[0]) + static_cast<double>(sum_arr[1]);
            // Scalar tail
            for (; i < cur_samples; ++i) {
                int32_t s = samples16[i];
                int32_t abs_s = std::abs(s);
                if (abs_s > peak) peak = abs_s;
                sum_sq += static_cast<double>(s) * s;
            }
#else
            for (size_t i = 0; i < cur_samples; ++i) {
                int32_t s = samples16[i];
                int32_t abs_s = std::abs(s);
                if (abs_s > peak) peak = abs_s;
                sum_sq += static_cast<double>(s) * s;
            }
#endif
            break;
        }
        case 24:
            for (size_t i = 0; i < cur_samples; ++i) {
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
#ifdef __ARM_NEON
            // NEON: process 4 x int32 at a time, shift >> 16 for metering range
            int32x4_t vpeak = vdupq_n_s32(0);
            int64x2_t vsum = vdupq_n_s64(0);
            size_t i = 0;
            size_t neon_end = cur_samples & ~3u;
            for (; i < neon_end; i += 4) {
                int32x4_t v = vld1q_s32(samples32 + i);
                int32x4_t shifted = vshrq_n_s32(v, 16);
                int32x4_t va = vabsq_s32(shifted);
                vpeak = vmaxq_s32(vpeak, va);
                // Accumulate squares: widen to 64-bit
                int32x2_t lo = vget_low_s32(shifted);
                int32x2_t hi = vget_high_s32(shifted);
                vsum = vmlal_s32(vsum, lo, lo);
                vsum = vmlal_s32(vsum, hi, hi);
            }
            // Reduce NEON peak
            int32x2_t pk2 = vmax_s32(vget_low_s32(vpeak), vget_high_s32(vpeak));
            int32_t pk_arr[2];
            vst1_s32(pk_arr, pk2);
            peak = std::max(pk_arr[0], pk_arr[1]);
            // Reduce NEON sum
            int64_t sum_arr[2];
            vst1q_s64(sum_arr, vsum);
            sum_sq += static_cast<double>(sum_arr[0]) + static_cast<double>(sum_arr[1]);
            // Scalar tail
            for (; i < cur_samples; ++i) {
                int32_t s = samples32[i] >> 16;
                int32_t abs_s = std::abs(s);
                if (abs_s > peak) peak = abs_s;
                sum_sq += static_cast<double>(s) * s;
            }
#else
            for (size_t i = 0; i < cur_samples; ++i) {
                int32_t s = samples32[i] >> 16;
                int32_t abs_s = std::abs(s);
                if (abs_s > peak) peak = abs_s;
                sum_sq += static_cast<double>(s) * s;
            }
#endif
            break;
        }
    }
    peak_level_.store(static_cast<int16_t>(std::min(peak, (int32_t)32767)), std::memory_order_relaxed);
    rms_level_.store(static_cast<float>(std::sqrt(sum_sq / static_cast<double>(cur_samples))),
                     std::memory_order_relaxed);
    total_frames_.fetch_add(frame_count, std::memory_order_relaxed);
}

void CapturePipeline::apply_gain(const uint8_t* src, uint8_t* dst,
                                 size_t sample_count, uint16_t bits_per_sample,
                                 float gain) const {
    switch (bits_per_sample) {
        case 16: {
            const int16_t* sp = reinterpret_cast<const int16_t*>(src);
            int16_t* dp = reinterpret_cast<int16_t*>(dst);
#ifdef __ARM_NEON
            // NEON: fixed-point gain — scale by 2^14, multiply, shift back
            // Supports gains roughly in [-4.0, +4.0] without overflow
            int16_t gain_fp = static_cast<int16_t>(std::clamp(
                gain * 16384.0, -32768.0, 32767.0));
            int16x8_t vgain = vdupq_n_s16(gain_fp);
            size_t i = 0;
            size_t neon_end = sample_count & ~7u;
            for (; i < neon_end; i += 8) {
                int16x8_t v = vld1q_s16(sp + i);
                // Multiply and take high half (>> 14 via qdmulh which does >> 15,
                // but we use manual widening for exact >> 14)
                int32x4_t lo = vmull_s16(vget_low_s16(v), vget_low_s16(vgain));
                int32x4_t hi = vmull_s16(vget_high_s16(v), vget_high_s16(vgain));
                // Shift right by 14 and saturating narrow back to int16
                int16x4_t r_lo = vqshrn_n_s32(lo, 14);
                int16x4_t r_hi = vqshrn_n_s32(hi, 14);
                vst1q_s16(dp + i, vcombine_s16(r_lo, r_hi));
            }
            // Scalar tail
            for (; i < sample_count; ++i) {
                int32_t s = static_cast<int32_t>(sp[i] * gain);
                dp[i] = static_cast<int16_t>(std::clamp(s, (int32_t)-32768, (int32_t)32767));
            }
#else
            for (size_t i = 0; i < sample_count; ++i) {
                int32_t s = static_cast<int32_t>(sp[i] * gain);
                dp[i] = static_cast<int16_t>(std::clamp(s, (int32_t)-32768, (int32_t)32767));
            }
#endif
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
#ifdef __ARM_NEON
            // NEON: fixed-point gain — scale by 2^14, multiply, shift back
            // Supports gains roughly in [-131072, +131072] (well beyond audio range)
            int32_t gain_fp = static_cast<int32_t>(std::clamp(
                gain * 16384.0, (double)INT32_MIN, (double)INT32_MAX));
            int32x4_t vgain = vdupq_n_s32(gain_fp);
            size_t i = 0;
            size_t neon_end = sample_count & ~3u;
            for (; i < neon_end; i += 4) {
                int32x4_t v = vld1q_s32(sp + i);
                // Widen to 64-bit, multiply, shift right by 14, saturating narrow
                int64x2_t prod_lo = vmull_s32(vget_low_s32(v), vget_low_s32(vgain));
                int64x2_t prod_hi = vmull_s32(vget_high_s32(v), vget_high_s32(vgain));
                // Shift right by 14
                int32x2_t r_lo = vmovn_s64(vshrq_n_s64(prod_lo, 14));
                int32x2_t r_hi = vmovn_s64(vshrq_n_s64(prod_hi, 14));
                vst1q_s32(dp + i, vcombine_s32(r_lo, r_hi));
            }
            // Scalar tail
            for (; i < sample_count; ++i) {
                int64_t s = static_cast<int64_t>(sp[i] * gain);
                dp[i] = static_cast<int32_t>(std::clamp(
                    s, (int64_t)INT32_MIN, (int64_t)INT32_MAX));
            }
#else
            for (size_t i = 0; i < sample_count; ++i) {
                int64_t s = static_cast<int64_t>(sp[i] * gain);
                dp[i] = static_cast<int32_t>(std::clamp(
                    s, (int64_t)INT32_MIN, (int64_t)INT32_MAX));
            }
#endif
            break;
        }
    }
}

void CapturePipeline::apply_downmix(const uint8_t* src, uint8_t* dst,
                                     size_t frame_count,
                                     uint16_t bits_per_sample) const {
    // Keep left channel, discard right — stride-2 to stride-1 copy
    size_t bps = bits_per_sample / 8;
    size_t src_stride = 2 * bps;  // stereo: 2 samples per frame

    switch (bits_per_sample) {
        case 16: {
            const int16_t* sp = reinterpret_cast<const int16_t*>(src);
            int16_t* dp = reinterpret_cast<int16_t*>(dst);
#ifdef __ARM_NEON
            size_t f = 0;
            size_t neon_end = frame_count & ~7u;
            for (; f < neon_end; f += 8) {
                int16x8x2_t stereo = vld2q_s16(sp + f * 2);
                vst1q_s16(dp + f, stereo.val[0]);  // val[0] = L channels
            }
            for (; f < frame_count; ++f) {
                dp[f] = sp[f * 2];
            }
#else
            for (size_t f = 0; f < frame_count; ++f) {
                dp[f] = sp[f * 2];
            }
#endif
            break;
        }
        case 24:
            for (size_t f = 0; f < frame_count; ++f) {
                const uint8_t* s = src + f * src_stride;
                uint8_t* d = dst + f * 3;
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            }
            break;
        case 32: {
            const int32_t* sp = reinterpret_cast<const int32_t*>(src);
            int32_t* dp = reinterpret_cast<int32_t*>(dst);
#ifdef __ARM_NEON
            // NEON: deinterleave pairs, keep even elements (L channel)
            size_t f = 0;
            size_t neon_end = frame_count & ~3u;
            for (; f < neon_end; f += 4) {
                int32x4x2_t stereo = vld2q_s32(sp + f * 2);
                vst1q_s32(dp + f, stereo.val[0]);  // val[0] = L channels
            }
            for (; f < frame_count; ++f) {
                dp[f] = sp[f * 2];
            }
#else
            for (size_t f = 0; f < frame_count; ++f) {
                dp[f] = sp[f * 2];
            }
#endif
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
                    float x = samples[f * channels + ch];
                    float y = x - dc_prev_x_[ch] + DC_ALPHA * dc_prev_y_[ch];
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
                    float x = static_cast<float>(s);
                    float y = x - dc_prev_x_[ch] + DC_ALPHA * dc_prev_y_[ch];
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
                    float x = static_cast<float>(samples[f * channels + ch]);
                    float y = x - dc_prev_x_[ch] + DC_ALPHA * dc_prev_y_[ch];
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
        << R"(,"channels":)" << (downmix_mono_ ? 1 : config_.audio.channels)
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
    float rms = rms_level_.load(std::memory_order_relaxed);
    double peak_db = (peak > 0)
        ? 20.0 * std::log10(static_cast<double>(peak) / 32768.0) : -96.0;
    double rms_db = (rms > 0)
        ? 20.0 * std::log10(static_cast<double>(rms) / 32768.0) : -96.0;
    oss << R"({"peak":)" << peak
        << R"(,"peak_db":)" << peak_db
        << R"(,"rms":)" << rms
        << R"(,"rms_db":)" << rms_db << "}";
    return oss.str();
}

CapturePipeline::Stats CapturePipeline::get_stats() const {
    return {
        total_frames_.load(),
        ring_buffer_ ? ring_buffer_->available_frames() : 0,
        peak_level_.load(),
        rms_level_.load()
    };
}

} // namespace audio_daemon

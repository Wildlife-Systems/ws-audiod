#include "audio_daemon/alsa_capture.hpp"
#include "audio_daemon/logger.hpp"
#include <cstring>
#include <cerrno>
#include <sched.h>
#include <pthread.h>

namespace audio_daemon {

AlsaCapture::AlsaCapture(const AudioConfig& config) : config_(config) {}

AlsaCapture::~AlsaCapture() {
    stop();
}

bool AlsaCapture::initialize() {
    LOG_INFO("Opening ALSA device: ", config_.device);

    int err = snd_pcm_open(&pcm_, config_.device.c_str(),
                           SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        LOG_ERROR("Cannot open ALSA device '", config_.device, "': ",
                  snd_strerror(err));
        return false;
    }

    snd_pcm_hw_params_t* hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm_, hw_params);

    // Interleaved access
    err = snd_pcm_hw_params_set_access(pcm_, hw_params,
                                        SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        LOG_ERROR("Cannot set access type: ", snd_strerror(err));
        return false;
    }

    // Sample format
    snd_pcm_format_t fmt;
    switch (config_.bits_per_sample) {
        case 32: fmt = SND_PCM_FORMAT_S32_LE; break;
        case 24: fmt = SND_PCM_FORMAT_S24_LE; break;
        default: fmt = SND_PCM_FORMAT_S16_LE; break;
    }
    err = snd_pcm_hw_params_set_format(pcm_, hw_params, fmt);
    if (err < 0) {
        LOG_ERROR("Cannot set format: ", snd_strerror(err));
        return false;
    }

    // Channels
    err = snd_pcm_hw_params_set_channels(pcm_, hw_params, config_.channels);
    if (err < 0) {
        LOG_ERROR("Cannot set channels to ", config_.channels, ": ",
                  snd_strerror(err));
        return false;
    }

    // Sample rate (ALSA may negotiate a different rate)
    unsigned int rate = config_.sample_rate;
    err = snd_pcm_hw_params_set_rate_near(pcm_, hw_params, &rate, nullptr);
    if (err < 0) {
        LOG_ERROR("Cannot set sample rate: ", snd_strerror(err));
        return false;
    }
    actual_rate_ = rate;
    if (rate != config_.sample_rate) {
        LOG_WARN("Requested rate ", config_.sample_rate,
                 " Hz, got ", rate, " Hz");
    }

    // Period size
    snd_pcm_uframes_t period = config_.period_size;
    err = snd_pcm_hw_params_set_period_size_near(pcm_, hw_params,
                                                  &period, nullptr);
    if (err < 0) {
        LOG_ERROR("Cannot set period size: ", snd_strerror(err));
        return false;
    }
    actual_period_ = static_cast<uint32_t>(period);

    // Buffer size = period_size * buffer_periods
    snd_pcm_uframes_t buffer_size = period * config_.buffer_periods;
    err = snd_pcm_hw_params_set_buffer_size_near(pcm_, hw_params,
                                                  &buffer_size);
    if (err < 0) {
        LOG_ERROR("Cannot set buffer size: ", snd_strerror(err));
        return false;
    }

    // Apply parameters
    err = snd_pcm_hw_params(pcm_, hw_params);
    if (err < 0) {
        LOG_ERROR("Cannot apply HW params: ", snd_strerror(err));
        return false;
    }

    err = snd_pcm_prepare(pcm_);
    if (err < 0) {
        LOG_ERROR("Cannot prepare device: ", snd_strerror(err));
        return false;
    }

    // Allocate period buffer (sized in bytes for any sample format)
    size_t bytes_per_sample = config_.bits_per_sample / 8;
    period_buffer_.resize(actual_period_ * config_.channels * bytes_per_sample);

    LOG_INFO("ALSA device configured: ", actual_rate_, " Hz, ",
             config_.channels, " ch, ", config_.bits_per_sample, " bit, ",
             "period=", actual_period_, " frames, ",
             "buffer=", buffer_size, " frames");
    return true;
}

bool AlsaCapture::start() {
    if (running_) return true;

    running_ = true;
    total_frames_ = 0;
    thread_ = std::thread(&AlsaCapture::capture_thread_func, this);

    LOG_INFO("ALSA capture started");
    return true;
}

void AlsaCapture::stop() {
    bool was_running = running_.exchange(false);
    if (pcm_) {
        snd_pcm_drop(pcm_);  // unblock snd_pcm_readi
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (pcm_) {
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
    if (was_running) {
        LOG_INFO("ALSA capture stopped");
    }
}

void AlsaCapture::capture_thread_func() {
    LOG_DEBUG("Capture thread started");

    // Elevate to SCHED_FIFO realtime priority to prevent xruns
    struct sched_param param;
    param.sched_priority = 70;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        LOG_WARN("Could not set SCHED_FIFO (need CAP_SYS_NICE): ",
                 strerror(errno));
    } else {
        LOG_INFO("Capture thread: SCHED_FIFO priority ", param.sched_priority);
    }

    // Pin to CPU 0 to avoid cache migration
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
        LOG_WARN("Could not set CPU affinity: ", strerror(errno));
    }

    while (running_) {
        snd_pcm_sframes_t frames = snd_pcm_readi(
            pcm_, period_buffer_.data(), actual_period_);

        if (frames < 0) {
            // Handle overrun / error
            if (frames == -EPIPE) {
                LOG_WARN("ALSA overrun (xrun)");
                snd_pcm_prepare(pcm_);
                continue;
            }
            frames = snd_pcm_recover(pcm_, static_cast<int>(frames), 0);
            if (frames < 0) {
                LOG_ERROR("ALSA read error: ", snd_strerror(static_cast<int>(frames)));
                break;
            }
        }

        if (frames > 0 && callback_) {
            AudioChunkMeta meta;
            meta.timestamp_us = get_timestamp_us();
            meta.sample_rate = actual_rate_;
            meta.channels = config_.channels;
            meta.bits_per_sample = config_.bits_per_sample;
            meta.frame_count = static_cast<uint64_t>(frames);
            total_frames_ += static_cast<uint64_t>(frames);
            meta.total_frames = total_frames_;

            callback_(meta, period_buffer_.data(),
                      static_cast<size_t>(frames) * config_.channels);
        }
    }

    LOG_DEBUG("Capture thread stopped");
}

} // namespace audio_daemon

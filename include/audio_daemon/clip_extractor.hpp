#pragma once

#include "audio_daemon/common.hpp"
#include "audio_daemon/ring_buffer.hpp"
#include <string>
#include <queue>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace audio_daemon {

/**
 * Extracts audio clips from the ring buffer on demand.
 *
 * Supports pre-event and post-event recording: a clip request like
 * CLIP -5 5 extracts 10 seconds of audio centred on the request time.
 * When end_offset is positive the extractor waits for the required
 * future samples to arrive before writing the file.
 */
class ClipExtractor {
public:
    struct Config {
        std::string output_dir = DEFAULT_CLIPS_DIR;
        std::string format = "wav";  // "wav" or "flac"
    };

    struct ClipRequest {
        uint64_t request_id;
        int start_offset;          // seconds relative to request time
        int end_offset;            // seconds relative to request time
        std::string format;        // per-request format override
        uint64_t request_timestamp;
    };

    ClipExtractor(const Config& config, RingBuffer& ring_buffer,
                  uint32_t sample_rate, uint16_t channels,
                  uint16_t bits_per_sample);
    ~ClipExtractor();

    ClipExtractor(const ClipExtractor&) = delete;
    ClipExtractor& operator=(const ClipExtractor&) = delete;

    bool start();
    void stop();

    /**
     * Submit a clip extraction request.
     * @return A request id that can be used to track the result.
     */
    uint64_t request_clip(int start_offset, int end_offset,
                          const std::string& format = "");

    /** Submit and wait for result. Returns path or empty on failure. */
    std::string request_clip_sync(int start_offset, int end_offset,
                                  const std::string& format = "");

    struct Stats {
        uint64_t clips_extracted;
        uint64_t clips_failed;
    };
    Stats get_stats() const;

private:
    void worker_func();
    std::string extract_clip(const ClipRequest& req);
    std::string write_wav(const std::vector<uint8_t>& samples,
                          const std::string& path);
    std::string write_sndfile(const std::vector<uint8_t>& samples,
                              const std::string& path,
                              const std::string& format);

    Config config_;
    RingBuffer& ring_buffer_;
    uint32_t sample_rate_;
    uint16_t channels_;
    uint16_t bits_per_sample_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    std::queue<ClipRequest> queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    std::atomic<uint64_t> next_id_{1};
    std::atomic<uint64_t> clips_extracted_{0};
    std::atomic<uint64_t> clips_failed_{0};

    std::mutex results_mutex_;
    std::condition_variable results_cv_;
    std::map<uint64_t, std::string> results_;  // request_id -> path (empty=failed)
};

} // namespace audio_daemon

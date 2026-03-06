#include "audio_daemon/sample_publisher.hpp"
#include "audio_daemon/logger.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

namespace audio_daemon {

SamplePublisher::SamplePublisher(const std::string& shm_name,
                                 uint32_t sample_rate, uint16_t channels,
                                 uint16_t bits_per_sample,
                                 uint32_t period_frames)
    : shm_name_(shm_name), sample_rate_(sample_rate),
      channels_(channels), bits_per_sample_(bits_per_sample),
      period_frames_(period_frames) {}

SamplePublisher::~SamplePublisher() {
    cleanup();
}

bool SamplePublisher::initialize() {
    size_t data_size = static_cast<size_t>(period_frames_) * channels_ *
                       (bits_per_sample_ / 8);
    shm_size_ = HEADER_SIZE + data_size;

    shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR, 0644);
    if (shm_fd_ < 0) {
        LOG_ERROR("Cannot create shared memory '", shm_name_, "': ",
                  strerror(errno));
        return false;
    }

    if (ftruncate(shm_fd_, static_cast<off_t>(shm_size_)) < 0) {
        LOG_ERROR("Cannot size shared memory: ", strerror(errno));
        close(shm_fd_);
        shm_fd_ = -1;
        return false;
    }

    shm_ptr_ = static_cast<uint8_t*>(mmap(nullptr, shm_size_,
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED, shm_fd_, 0));
    if (shm_ptr_ == MAP_FAILED) {
        LOG_ERROR("Cannot mmap shared memory: ", strerror(errno));
        close(shm_fd_);
        shm_fd_ = -1;
        shm_ptr_ = nullptr;
        return false;
    }

    // Write header
    std::memset(shm_ptr_, 0, HEADER_SIZE);
    std::memcpy(shm_ptr_ + 0,  &MAGIC,           4);
    std::memcpy(shm_ptr_ + 4,  &sample_rate_,     4);
    std::memcpy(shm_ptr_ + 8,  &channels_,        2);
    std::memcpy(shm_ptr_ + 10, &bits_per_sample_, 2);
    std::memcpy(shm_ptr_ + 12, &period_frames_,   4);

    LOG_INFO("Shared memory publisher ready: ", shm_name_,
             " (", shm_size_, " bytes)");
    return true;
}

void SamplePublisher::publish(const uint8_t* data, size_t frame_count,
                              uint64_t timestamp_us) {
    if (!shm_ptr_) return;

    size_t bytes = frame_count * channels_ * (bits_per_sample_ / 8);
    size_t max_bytes = shm_size_ - HEADER_SIZE;
    if (bytes > max_bytes) bytes = max_bytes;

    // Update header fields
    uint64_t counter = write_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::memcpy(shm_ptr_ + 16, &counter,      8);
    std::memcpy(shm_ptr_ + 24, &timestamp_us, 8);

    // Copy sample data
    std::memcpy(shm_ptr_ + HEADER_SIZE, data, bytes);

    // Memory fence so readers see consistent data
    __sync_synchronize();
}

void SamplePublisher::cleanup() {
    if (shm_ptr_ && shm_ptr_ != MAP_FAILED) {
        munmap(shm_ptr_, shm_size_);
        shm_ptr_ = nullptr;
    }
    if (shm_fd_ >= 0) {
        close(shm_fd_);
        shm_unlink(shm_name_.c_str());
        shm_fd_ = -1;
    }
}

} // namespace audio_daemon

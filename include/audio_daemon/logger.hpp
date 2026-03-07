#pragma once

#include <iostream>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <chrono>
#include <ctime>

namespace audio_daemon {

enum class LogLevel { DEBUG, INFO, WARN, ERROR, FATAL };

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void set_level(LogLevel level) { level_ = level; }

    template<typename... Args>
    void log(LogLevel level, Args&&... args) {
        if (level < level_) return;

        std::ostringstream oss;
        format_timestamp(oss);
        oss << " [" << level_str(level) << "] ";
        (oss << ... << std::forward<Args>(args));

        // Use try_lock to avoid blocking the caller (critical for the
        // ALSA capture thread where a mutex stall can cause an xrun).
        // If the lock is contended, drop the message rather than block.
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            std::cerr << oss.str() << std::endl;
        }
    }

    /** Blocking log — always emits, acceptable for startup/shutdown paths. */
    template<typename... Args>
    void log_blocking(LogLevel level, Args&&... args) {
        if (level < level_) return;

        std::ostringstream oss;
        format_timestamp(oss);
        oss << " [" << level_str(level) << "] ";
        (oss << ... << std::forward<Args>(args));

        std::lock_guard<std::mutex> lock(mutex_);
        std::cerr << oss.str() << std::endl;
    }

private:
    Logger() = default;

    void format_timestamp(std::ostringstream& oss) {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000;
        struct tm tm_buf;
        localtime_r(&time, &tm_buf);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
        oss << buf << "." << std::setfill('0') << std::setw(3) << ms;
    }

    static const char* level_str(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO";
            case LogLevel::WARN:  return "WARN";
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::FATAL: return "FATAL";
        }
        return "?";
    }

    LogLevel level_ = LogLevel::INFO;
    std::mutex mutex_;
};

#define LOG_DEBUG(...) audio_daemon::Logger::instance().log(audio_daemon::LogLevel::DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  audio_daemon::Logger::instance().log(audio_daemon::LogLevel::INFO,  __VA_ARGS__)
#define LOG_WARN(...)  audio_daemon::Logger::instance().log(audio_daemon::LogLevel::WARN,  __VA_ARGS__)
#define LOG_ERROR(...) audio_daemon::Logger::instance().log(audio_daemon::LogLevel::ERROR, __VA_ARGS__)
#define LOG_FATAL(...) audio_daemon::Logger::instance().log(audio_daemon::LogLevel::FATAL, __VA_ARGS__)

} // namespace audio_daemon

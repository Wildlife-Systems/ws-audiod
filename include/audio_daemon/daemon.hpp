#pragma once

#include "audio_daemon/common.hpp"
#include "audio_daemon/capture_pipeline.hpp"
#include "audio_daemon/control_socket.hpp"
#include <memory>
#include <atomic>

namespace audio_daemon {

/**
 * Main daemon class.
 */
class Daemon {
public:
    explicit Daemon(const DaemonConfig& config);
    ~Daemon();

    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    bool initialize();
    int run();
    void stop();

    static void signal_handler(int signum);
    static Daemon* instance() { return instance_; }

private:
    Response handle_clip_command(const Command& cmd);
    Response handle_set_command(const Command& cmd);
    Response handle_get_command(const Command& cmd);

    DaemonConfig config_;
    std::unique_ptr<CapturePipeline> pipeline_;
    std::unique_ptr<ControlSocket> control_socket_;
    std::atomic<bool> running_{false};

    static Daemon* instance_;
};

DaemonConfig parse_args(int argc, char* argv[]);
DaemonConfig load_config(const std::string& path);

} // namespace audio_daemon

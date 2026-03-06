#include "audio_daemon/daemon.hpp"
#include "audio_daemon/logger.hpp"
#include <csignal>
#include <iostream>

namespace audio_daemon {

Daemon* Daemon::instance_ = nullptr;

Daemon::Daemon(const DaemonConfig& config) : config_(config) {
    instance_ = this;
}

Daemon::~Daemon() {
    stop();
    instance_ = nullptr;
}

bool Daemon::initialize() {
    LOG_INFO("Audio daemon version ", VERSION);
    LOG_INFO("Initialising daemon...");

    // Create capture pipeline
    pipeline_ = std::make_unique<CapturePipeline>(config_);
    if (!pipeline_->initialize()) {
        LOG_ERROR("Failed to initialise capture pipeline");
        return false;
    }

    // Create control socket
    control_socket_ = std::make_unique<ControlSocket>(config_.socket_path);

    control_socket_->set_clip_handler([this](const Command& cmd) {
        return handle_clip_command(cmd);
    });
    control_socket_->set_set_handler([this](const Command& cmd) {
        return handle_set_command(cmd);
    });
    control_socket_->set_get_handler([this](const Command& cmd) {
        return handle_get_command(cmd);
    });

    LOG_INFO("Daemon initialised successfully");
    return true;
}

int Daemon::run() {
    LOG_INFO("Starting daemon...");

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);

    if (!control_socket_->start()) {
        LOG_ERROR("Failed to start control socket");
        return 1;
    }

    if (!pipeline_->start()) {
        LOG_ERROR("Failed to start capture pipeline");
        return 1;
    }

    running_ = true;

    LOG_INFO("Daemon running. Press Ctrl+C to stop.");
    LOG_INFO("Control socket: ", config_.socket_path);
    LOG_INFO("Audio: ", config_.audio.device, " @ ",
             config_.audio.sample_rate, " Hz, ",
             config_.audio.channels, " ch, ",
             config_.audio.bits_per_sample, " bit");

    if (config_.block_recorder.enabled) {
        LOG_INFO("Block recording: ",
                 config_.block_recorder.block_duration_seconds,
                 "s blocks -> ", config_.block_recorder.output_dir);
    }

    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("Daemon shutting down...");
    pipeline_->stop();
    control_socket_->stop();
    LOG_INFO("Daemon stopped");
    return 0;
}

void Daemon::stop() {
    running_ = false;
}

void Daemon::signal_handler(int signum) {
    LOG_INFO("Received signal ", signum);
    if (instance_) instance_->stop();
}

Response Daemon::handle_clip_command(const Command& cmd) {
    LOG_DEBUG("Handling CLIP command, start=", cmd.int_value,
              " end=", cmd.int_value2);

    std::string path = pipeline_->capture_clip(
        cmd.int_value, cmd.int_value2, cmd.key);
    if (path.empty()) {
        return Response::error("Clip extraction failed");
    }
    return Response::ok_path(path);
}

Response Daemon::handle_set_command(const Command& cmd) {
    LOG_DEBUG("Handling SET command: ", cmd.key, "=", cmd.value);

    if (pipeline_->set_parameter(cmd.key, cmd.value)) {
        return Response::ok();
    }
    return Response::error("Failed to set parameter");
}

Response Daemon::handle_get_command(const Command& cmd) {
    LOG_DEBUG("Handling GET command: ", cmd.key);

    if (cmd.key == "STATUS") {
        return Response::ok_data(pipeline_->get_status_json());
    }
    if (cmd.key == "LEVEL" || cmd.key == "LEVELS") {
        return Response::ok_data(pipeline_->get_levels_json());
    }
    return Response::error("Unknown key: " + cmd.key);
}

} // namespace audio_daemon

int main(int argc, char* argv[]) {
    using namespace audio_daemon;

    DaemonConfig config = parse_args(argc, argv);

    Daemon daemon(config);
    if (!daemon.initialize()) {
        LOG_FATAL("Failed to initialise daemon");
        return 1;
    }

    return daemon.run();
}

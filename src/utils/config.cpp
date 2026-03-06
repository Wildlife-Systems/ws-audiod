#include "audio_daemon/common.hpp"
#include "audio_daemon/daemon.hpp"
#include "audio_daemon/logger.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <getopt.h>

namespace audio_daemon {

namespace {

std::string trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

std::pair<std::string, std::string> parse_line(const std::string& line) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) return {"", ""};
    return {trim(line.substr(0, eq)), trim(line.substr(eq + 1))};
}

} // anonymous namespace

DaemonConfig load_config(const std::string& path) {
    DaemonConfig config;
    std::ifstream file(path);

    if (!file.is_open()) {
        LOG_WARN("Could not open config file: ", path, ", using defaults");
        return config;
    }

    std::string line;
    std::string section;

    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }

        auto [key, value] = parse_line(line);
        if (key.empty()) continue;

        if (section == "daemon" || section.empty()) {
            if (key == "socket_path")          config.socket_path = value;
            else if (key == "clips_dir")       config.clips_dir = value;
            else if (key == "shm_name")        config.shm_name = value;
            else if (key == "ring_buffer_seconds")
                config.ring_buffer_seconds = std::stoul(value);
            else if (key == "enable_sample_sharing")
                config.enable_sample_sharing = (value == "true" || value == "1");
        } else if (section == "audio") {
            if (key == "device")              config.audio.device = value;
            else if (key == "rate")
                config.audio.sample_rate = std::stoul(value);
            else if (key == "channels")
                config.audio.channels = static_cast<uint16_t>(std::stoul(value));
            else if (key == "bits" || key == "bits_per_sample")
                config.audio.bits_per_sample = static_cast<uint16_t>(std::stoul(value));
            else if (key == "period_size")
                config.audio.period_size = std::stoul(value);
            else if (key == "buffer_periods")
                config.audio.buffer_periods = std::stoul(value);
            else if (key == "gain_db")
                config.audio.gain_db = std::stod(value);
            else if (key == "dc_remove")
                config.audio.dc_remove = (value == "true" || value == "1");
        } else if (section == "block_recorder") {
            if (key == "enabled")
                config.block_recorder.enabled = (value == "true" || value == "1");
            else if (key == "output_dir")
                config.block_recorder.output_dir = value;
            else if (key == "block_duration_seconds" || key == "duration")
                config.block_recorder.block_duration_seconds = std::stoul(value);
            else if (key == "format")
                config.block_recorder.format = value;
            else if (key == "max_blocks")
                config.block_recorder.max_blocks = std::stoul(value);
        }
    }

    return config;
}

DaemonConfig parse_args(int argc, char* argv[]) {
    DaemonConfig config;

    static struct option long_options[] = {
        {"config",      required_argument, 0, 'c'},
        {"socket",      required_argument, 0, 's'},
        {"device",      required_argument, 0, 'D'},
        {"rate",        required_argument, 0, 'r'},
        {"channels",    required_argument, 0, 'C'},
        {"bits",        required_argument, 0, 'b'},
        {"gain",        required_argument, 0, 'g'},
        {"period-size", required_argument, 0, 'p'},
        {"block-record",required_argument, 0, 'B'},
        {"shm",         no_argument,       0, 'S'},
        {"debug",       no_argument,       0, 'd'},
        {"help",        no_argument,       0, 'h'},
        {"version",     no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    std::string config_path;

    while ((opt = getopt_long(argc, argv, "c:s:D:r:C:b:g:p:B:Sdhv",
                              long_options, &option_index)) != -1) {
        switch (opt) {
            case 'c': config_path = optarg; break;
            case 's': config.socket_path = optarg; break;
            case 'D': config.audio.device = optarg; break;
            case 'r': config.audio.sample_rate = std::stoul(optarg); break;
            case 'C': config.audio.channels = static_cast<uint16_t>(std::stoul(optarg)); break;
            case 'b': config.audio.bits_per_sample = static_cast<uint16_t>(std::stoul(optarg)); break;
            case 'g': config.audio.gain_db = std::stod(optarg); break;
            case 'p': config.audio.period_size = std::stoul(optarg); break;
            case 'B':
                config.block_recorder.enabled = true;
                config.block_recorder.block_duration_seconds = std::stoul(optarg);
                break;
            case 'S':
                config.enable_sample_sharing = true;
                break;
            case 'd':
                Logger::instance().set_level(LogLevel::DEBUG);
                break;
            case 'v':
                std::cout << "ws-audiod version " << VERSION << std::endl;
                exit(0);
            case 'h':
            default:
                std::cout << "Usage: " << argv[0] << " [OPTIONS]\n"
                          << "Options:\n"
                          << "  -c, --config FILE        Configuration file path\n"
                          << "  -s, --socket PATH        Control socket path\n"
                          << "  -D, --device DEVICE      ALSA device (default: default)\n"
                          << "  -r, --rate RATE          Sample rate in Hz (default: 48000)\n"
                          << "  -C, --channels N         Channel count (default: 1)\n"
                          << "  -b, --bits N             Bit depth: 16, 24 or 32 (default: 16)\n"
                          << "  -g, --gain DB            Input gain in dB (default: 0.0)\n"
                          << "  -p, --period-size N      ALSA period size (default: 1024)\n"
                          << "  -B, --block-record SECS  Enable block recording with SECS duration\n"
                          << "  -S, --shm                Enable shared memory sample publishing\n"
                          << "  -d, --debug              Enable debug logging\n"
                          << "  -h, --help               Show this help\n"
                          << "  -v, --version            Show version\n";
                exit(opt == 'h' ? 0 : 1);
        }
    }

    if (!config_path.empty()) {
        config = load_config(config_path);

        // Re-parse to let CLI override config file
        optind = 1;
        while ((opt = getopt_long(argc, argv, "c:s:D:r:C:b:g:p:B:Sdhv",
                                  long_options, &option_index)) != -1) {
            switch (opt) {
                case 's': config.socket_path = optarg; break;
                case 'D': config.audio.device = optarg; break;
                case 'r': config.audio.sample_rate = std::stoul(optarg); break;
                case 'C': config.audio.channels = static_cast<uint16_t>(std::stoul(optarg)); break;
                case 'b': config.audio.bits_per_sample = static_cast<uint16_t>(std::stoul(optarg)); break;
                case 'g': config.audio.gain_db = std::stod(optarg); break;
                case 'p': config.audio.period_size = std::stoul(optarg); break;
                case 'B':
                    config.block_recorder.enabled = true;
                    config.block_recorder.block_duration_seconds = std::stoul(optarg);
                    break;
                case 'S':
                    config.enable_sample_sharing = true;
                    break;
                default: break;
            }
        }
    }

    return config;
}

} // namespace audio_daemon

// audiod-mon: Real-time terminal audio monitor for ws-audiod
//
// Reads audio from the daemon's shared memory and displays a live
// level meter.  Keyboard controls adjust mic boost via the control
// socket.
//
// Usage: audiod-mon [options]
//   -s, --shm NAME      Shared memory name  (default: /ws_audiod_samples)
//   -S, --socket PATH   Control socket path  (default: /run/ws-audiod/control.sock)
//   -h, --help          Show help

#include <iostream>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <csignal>
#include <string>
#include <sstream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <vector>
#include <getopt.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>

// ── shared-memory layout ────────────────────────────────────────────
static constexpr size_t   HEADER_SIZE = 64;
static constexpr uint32_t MAGIC       = 0x41554449; // "AUDI"

// ── defaults ────────────────────────────────────────────────────────
static constexpr const char* DEFAULT_SHM    = "/ws_audiod_samples";
static constexpr const char* DEFAULT_SOCKET = "/run/ws-audiod/control.sock";
static constexpr double GAIN_STEP           = 1.0; // dB per key press

// ── global state ────────────────────────────────────────────────────
static volatile bool g_running = true;
static struct termios g_orig_termios;
static bool g_termios_saved = false;

static void signal_handler(int) { g_running = false; }

// ── terminal raw mode ───────────────────────────────────────────────
static void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == 0) {
        g_termios_saved = true;
        struct termios raw = g_orig_termios;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
}

static void disable_raw_mode() {
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    }
}

// ── control socket helper ───────────────────────────────────────────
static std::string send_command(const std::string& socket_path,
                                const std::string& cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return "";

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(),
                 sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                sizeof(addr)) < 0) {
        close(fd);
        return "";
    }

    std::string msg = cmd + "\n";
    send(fd, msg.c_str(), msg.size(), 0);

    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);

    if (n > 0) {
        buf[n] = '\0';
        return std::string(buf);
    }
    return "";
}

// ── dB helpers ──────────────────────────────────────────────────────
static double amplitude_to_db(double amp, double ref) {
    if (amp <= 0) return -96.0;
    return 20.0 * std::log10(amp / ref);
}

// ── meter rendering ─────────────────────────────────────────────────
static constexpr int BAR_WIDTH = 50;

static void draw_bar(double db, double peak_db, const char* label) {
    // Map -60 dB .. 0 dB to 0 .. BAR_WIDTH
    auto map = [](double v) -> int {
        return std::clamp(static_cast<int>((v + 60.0) * BAR_WIDTH / 60.0),
                          0, BAR_WIDTH);
    };

    int rms_pos  = map(db);
    int peak_pos = map(peak_db);

    std::cout << label << " [";
    for (int i = 0; i < BAR_WIDTH; ++i) {
        if (i == peak_pos)       std::cout << '|';
        else if (i < rms_pos)    std::cout << '#';
        else                     std::cout << ' ';
    }
    std::cout << "] " << std::fixed << std::setprecision(1)
              << std::setw(6) << db << " dB  pk "
              << std::setw(6) << peak_db << " dB";
}

// ── usage ───────────────────────────────────────────────────────────
static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  -s, --shm NAME      Shared memory name  (default: "
              << DEFAULT_SHM << ")\n"
              << "  -S, --socket PATH   Control socket path  (default: "
              << DEFAULT_SOCKET << ")\n"
              << "  -h, --help          Show this help\n";
}

// ── main ────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    std::string shm_name    = DEFAULT_SHM;
    std::string socket_path = DEFAULT_SOCKET;

    static struct option long_opts[] = {
        {"shm",    required_argument, nullptr, 's'},
        {"socket", required_argument, nullptr, 'S'},
        {"help",   no_argument,       nullptr, 'h'},
        {nullptr,  0,                 nullptr,  0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:S:h", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 's': shm_name    = optarg; break;
            case 'S': socket_path = optarg; break;
            case 'h':
            default:
                print_usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    // ── open shared memory ──────────────────────────────────────────
    int fd = shm_open(shm_name.c_str(), O_RDONLY, 0);
    if (fd < 0) {
        std::cerr << "Cannot open shared memory " << shm_name << ": "
                  << strerror(errno) << "\n"
                  << "Is ws-audiod running with enable_sample_sharing=true?\n";
        return 1;
    }

    struct stat st;
    fstat(fd, &st);
    size_t shm_size = static_cast<size_t>(st.st_size);

    auto* ptr = static_cast<const uint8_t*>(
        mmap(nullptr, shm_size, PROT_READ, MAP_SHARED, fd, 0));
    if (ptr == MAP_FAILED) {
        std::cerr << "mmap failed: " << strerror(errno) << "\n";
        close(fd);
        return 1;
    }

    uint32_t magic;
    std::memcpy(&magic, ptr, 4);
    if (magic != MAGIC) {
        std::cerr << "Invalid shared memory magic: 0x" << std::hex
                  << magic << "\n";
        munmap(const_cast<uint8_t*>(ptr), shm_size);
        close(fd);
        return 1;
    }

    uint32_t sample_rate, period_frames;
    uint16_t channels, bits;
    std::memcpy(&sample_rate,   ptr + 4,  4);
    std::memcpy(&channels,      ptr + 8,  2);
    std::memcpy(&bits,          ptr + 10, 2);
    std::memcpy(&period_frames, ptr + 12, 4);

    size_t bytes_per_sample = bits / 8;
    size_t samples_per_period = static_cast<size_t>(period_frames) * channels;
    double ref = (bits == 32) ? 2147483648.0
               : (bits == 24) ? 8388608.0
               :                32768.0;

    // ── set up terminal ─────────────────────────────────────────────
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
    enable_raw_mode();

    double gain_db = 0.0;
    // Try to get current gain from daemon status
    {
        std::string resp = send_command(socket_path, "GET STATUS");
        auto pos = resp.find("\"gain_db\":");
        if (pos != std::string::npos) {
            gain_db = std::stod(resp.substr(pos + 10));
        }
    }

    // ── header ──────────────────────────────────────────────────────
    std::cout << "\033[2J\033[H"; // clear screen
    std::cout << "audiod-mon - ws-audiod live monitor\n"
              << "  " << sample_rate << " Hz / " << bits << "-bit / "
              << channels << " ch / " << period_frames << " frames\n"
              << "  UP/DOWN or +/-: adjust gain   q: quit\n";
    for (int i = 0; i < 70; ++i) std::cout << '-';
    std::cout << '\n';

    uint64_t last_counter = 0;
    const int header_lines = 4;

    // Per-channel peak-hold (decaying)
    std::vector<double> peak_hold_db(channels, -96.0);
    constexpr double PEAK_DECAY = 0.2; // dB per update

    // ── main loop ───────────────────────────────────────────────────
    while (g_running) {
        // ── check keyboard ──────────────────────────────────────────
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        if (poll(&pfd, 1, 0) > 0) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                bool gain_changed = false;

                if (c == 'q' || c == 'Q') {
                    g_running = false;
                    break;
                } else if (c == '+' || c == '=') {
                    gain_db += GAIN_STEP;
                    gain_changed = true;
                } else if (c == '-' || c == '_') {
                    gain_db -= GAIN_STEP;
                    gain_changed = true;
                } else if (c == '\033') {
                    // Arrow key escape sequence
                    char seq[2];
                    if (read(STDIN_FILENO, &seq[0], 1) == 1 &&
                        read(STDIN_FILENO, &seq[1], 1) == 1) {
                        if (seq[0] == '[') {
                            if (seq[1] == 'A') { // Up
                                gain_db += GAIN_STEP;
                                gain_changed = true;
                            } else if (seq[1] == 'B') { // Down
                                gain_db -= GAIN_STEP;
                                gain_changed = true;
                            }
                        }
                    }
                }

                if (gain_changed) {
                    std::ostringstream cmd;
                    cmd << "SET gain_db " << gain_db;
                    send_command(socket_path, cmd.str());
                }
            }
        }

        // ── read shared memory ──────────────────────────────────────
        uint64_t counter;
        std::memcpy(&counter, ptr + 16, 8);

        if (counter == last_counter) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }
        last_counter = counter;

        __sync_synchronize(); // match publisher's fence

        const uint8_t* data = ptr + HEADER_SIZE;

        // ── compute per-channel RMS and peak ────────────────────────
        size_t frame_count = period_frames;

        for (uint16_t ch = 0; ch < channels; ++ch) {
            double sum_sq = 0.0;
            double peak   = 0.0;

            for (size_t f = 0; f < frame_count; ++f) {
                size_t idx = f * channels + ch;
                const uint8_t* p = data + idx * bytes_per_sample;
                double s = 0.0;

                switch (bits) {
                    case 16:
                        s = static_cast<int16_t>(p[0] | (p[1] << 8));
                        break;
                    case 24: {
                        int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
                        if (v & 0x800000) v |= 0xFF000000;
                        s = v;
                        break;
                    }
                    case 32:
                        s = static_cast<int32_t>(
                            p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
                        break;
                }

                double abs_s = std::abs(s);
                if (abs_s > peak) peak = abs_s;
                sum_sq += s * s;
            }

            double rms = std::sqrt(sum_sq / frame_count);
            double rms_db  = amplitude_to_db(rms,  ref);
            double peak_db_val = amplitude_to_db(peak, ref);

            // Peak hold with decay
            if (peak_db_val > peak_hold_db[ch]) {
                peak_hold_db[ch] = peak_db_val;
            } else {
                peak_hold_db[ch] -= PEAK_DECAY;
                if (peak_hold_db[ch] < -96.0) peak_hold_db[ch] = -96.0;
            }

            // Position cursor and draw
            std::cout << "\033[" << (header_lines + 1 + ch) << ";1H"
                      << "\033[K"; // clear line

            char label[16];
            std::snprintf(label, sizeof(label), "CH%d ", ch);
            draw_bar(rms_db, peak_hold_db[ch], label);
        }

        // ── gain display ────────────────────────────────────────────
        std::cout << "\033[" << (header_lines + 1 + channels + 1) << ";1H"
                  << "\033[K"
                  << "  Gain: " << std::fixed << std::setprecision(1)
                  << gain_db << " dB";

        std::cout << std::flush;
    }

    // ── cleanup ─────────────────────────────────────────────────────
    disable_raw_mode();
    std::cout << "\n";

    munmap(const_cast<uint8_t*>(ptr), shm_size);
    close(fd);
    return 0;
}

// Shared memory audio consumer for ws-audiod
// Reads raw audio periods from the daemon's shared memory region.

#include <iostream>
#include <cstring>
#include <cstdint>
#include <csignal>
#include <thread>
#include <chrono>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

constexpr const char* DEFAULT_SHM = "/ws_audiod_samples";
constexpr size_t HEADER_SIZE = 64;
constexpr uint32_t MAGIC = 0x41554449;

static volatile bool running = true;

void signal_handler(int) { running = false; }

int main(int argc, char* argv[]) {
    const char* shm_name = (argc > 1) ? argv[1] : DEFAULT_SHM;

    int fd = shm_open(shm_name, O_RDONLY, 0);
    if (fd < 0) {
        std::cerr << "Cannot open shared memory " << shm_name << ": "
                  << strerror(errno) << "\n"
                  << "Is ws-audiod running with enable_sample_sharing=true?\n";
        return 1;
    }

    struct stat st;
    fstat(fd, &st);
    size_t size = static_cast<size_t>(st.st_size);

    auto* ptr = static_cast<uint8_t*>(
        mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0));
    if (ptr == MAP_FAILED) {
        std::cerr << "mmap failed: " << strerror(errno) << "\n";
        close(fd);
        return 1;
    }

    // Verify magic
    uint32_t magic;
    std::memcpy(&magic, ptr, 4);
    if (magic != MAGIC) {
        std::cerr << "Invalid magic: 0x" << std::hex << magic << "\n";
        munmap(ptr, size);
        close(fd);
        return 1;
    }

    uint32_t sample_rate, period_frames;
    uint16_t channels, bits;
    std::memcpy(&sample_rate,   ptr + 4,  4);
    std::memcpy(&channels,      ptr + 8,  2);
    std::memcpy(&bits,          ptr + 10, 2);
    std::memcpy(&period_frames, ptr + 12, 4);

    std::cout << "Connected to " << shm_name << "\n"
              << "  Rate:   " << sample_rate << " Hz\n"
              << "  Ch:     " << channels << "\n"
              << "  Bits:   " << bits << "\n"
              << "  Period: " << period_frames << " frames\n"
              << "Consuming... (Ctrl+C to stop)\n";

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    uint64_t last_counter = 0;
    uint64_t periods = 0;
    auto start = std::chrono::steady_clock::now();

    while (running) {
        uint64_t counter;
        std::memcpy(&counter, ptr + 16, 8);

        if (counter != last_counter) {
            last_counter = counter;
            periods++;

            // Access sample data at ptr + HEADER_SIZE
            // (actual processing would go here)

            if (periods % 500 == 0) {
                auto now = std::chrono::steady_clock::now();
                double elapsed = std::chrono::duration<double>(
                    now - start).count();
                std::cout << "  periods: " << periods
                          << "  elapsed: " << elapsed << "s"
                          << "  rate: " << (periods / elapsed)
                          << " periods/s\n";
            }
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

    std::cout << "\nConsumed " << periods << " periods\n";

    munmap(ptr, size);
    close(fd);
    return 0;
}

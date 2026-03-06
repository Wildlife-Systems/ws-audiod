// Clip extraction client for ws-audiod
// Usage: clip_client <start_offset> <end_offset> [format]
//   e.g. clip_client -5 5        (10s WAV clip)
//        clip_client -30 0 flac  (30s FLAC clip from buffer)

#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

constexpr const char* DEFAULT_SOCKET = "/run/ws-audiod/control.sock";

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <start_offset> <end_offset> [format]\n"
                  << "Example: " << argv[0] << " -5 5\n";
        return 1;
    }

    int start = std::stoi(argv[1]);
    int end   = std::stoi(argv[2]);
    std::string format = (argc > 3) ? argv[3] : "";

    std::string cmd = "CLIP " + std::to_string(start) + " "
                      + std::to_string(end);
    if (!format.empty()) cmd += " " + format;
    cmd += "\n";

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, DEFAULT_SOCKET, sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    send(fd, cmd.c_str(), cmd.size(), 0);

    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);

    if (n > 0) {
        buf[n] = '\0';
        std::cout << buf;
    }

    return 0;
}

#include "audio_daemon/control_socket.hpp"
#include "audio_daemon/logger.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <filesystem>
#include <sstream>
#include <algorithm>
#include <cstring>

namespace audio_daemon {

ControlSocket::ControlSocket(const std::string& path) : socket_path_(path) {}

ControlSocket::~ControlSocket() {
    stop();
}

bool ControlSocket::start() {
    // Ensure parent directory exists
    std::filesystem::create_directories(
        std::filesystem::path(socket_path_).parent_path());

    // Remove stale socket
    unlink(socket_path_.c_str());

    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        LOG_ERROR("Cannot create socket: ", strerror(errno));
        return false;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(),
                 sizeof(addr.sun_path) - 1);

    if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        LOG_ERROR("Cannot bind socket: ", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 8) < 0) {
        LOG_ERROR("Cannot listen on socket: ", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    running_ = true;
    thread_ = std::thread(&ControlSocket::accept_thread_func, this);

    LOG_INFO("Control socket listening on ", socket_path_);
    return true;
}

void ControlSocket::stop() {
    running_ = false;
    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
    unlink(socket_path_.c_str());
}

void ControlSocket::accept_thread_func() {
    while (running_) {
        int client_fd = accept(server_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (running_) LOG_DEBUG("Accept failed: ", strerror(errno));
            continue;
        }

        // Set a receive timeout
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        handle_client(client_fd);
        close(client_fd);
    }
}

void ControlSocket::handle_client(int client_fd) {
    char buf[1024];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';

    // Remove trailing newline
    std::string line(buf);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }

    Command cmd = parse_command(line);
    Response resp;

    switch (cmd.type) {
        case Command::CLIP:
            resp = clip_handler_ ? clip_handler_(cmd)
                                 : Response::error("No clip handler");
            break;
        case Command::SET:
            resp = set_handler_ ? set_handler_(cmd)
                                : Response::error("No set handler");
            break;
        case Command::GET:
            resp = get_handler_ ? get_handler_(cmd)
                                : Response::error("No get handler");
            break;
        default:
            resp = Response::error("Invalid command");
    }

    std::string out = resp.json + "\n";
    send(client_fd, out.c_str(), out.size(), MSG_NOSIGNAL);
}

Command ControlSocket::parse_command(const std::string& line) {
    Command cmd;
    std::istringstream iss(line);
    std::string word;
    iss >> word;

    // Upper-case the command word
    std::transform(word.begin(), word.end(), word.begin(), ::toupper);

    if (word == "CLIP") {
        cmd.type = Command::CLIP;
        iss >> cmd.int_value >> cmd.int_value2;
        // Optional format
        std::string fmt;
        if (iss >> fmt) {
            std::transform(fmt.begin(), fmt.end(), fmt.begin(), ::tolower);
            cmd.key = fmt;
        }
    } else if (word == "SET") {
        cmd.type = Command::SET;
        iss >> cmd.key >> cmd.value;
    } else if (word == "GET") {
        cmd.type = Command::GET;
        iss >> cmd.key;
        std::transform(cmd.key.begin(), cmd.key.end(),
                       cmd.key.begin(), ::toupper);
    }

    return cmd;
}

} // namespace audio_daemon

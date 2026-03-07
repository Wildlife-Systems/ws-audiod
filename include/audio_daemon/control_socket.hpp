#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>

namespace audio_daemon {

/** Parsed command from a client. */
struct Command {
    enum Type { CLIP, SET, GET, UNKNOWN };
    Type type = UNKNOWN;
    int int_value = 0;       // start offset for CLIP
    int int_value2 = 0;      // end offset for CLIP
    std::string key;         // key for SET/GET, format override for CLIP
    std::string value;       // value for SET
};

/** JSON response sent back to client. */
struct Response {
    static Response ok() { return {R"({"ok":true})"}; }
    static Response ok_path(const std::string& p) {
        return {R"({"ok":true,"path":")" + p + R"("})"};
    }
    static Response ok_data(const std::string& json) {
        return {R"({"ok":true,"data":)" + json + "}"};
    }
    static Response error(const std::string& msg) {
        return {R"({"ok":false,"error":")" + msg + R"("})"};
    }
    std::string json;
};

using CommandHandler = std::function<Response(const Command&)>;

/**
 * Unix domain socket server for the control interface.
 *
 * Accepts newline-delimited text commands and returns single-line
 * JSON responses.
 */
class ControlSocket {
public:
    explicit ControlSocket(const std::string& path);
    ~ControlSocket();

    ControlSocket(const ControlSocket&) = delete;
    ControlSocket& operator=(const ControlSocket&) = delete;

    bool start();
    void stop();

    void set_clip_handler(CommandHandler h)  { clip_handler_ = std::move(h); }
    void set_set_handler(CommandHandler h)   { set_handler_ = std::move(h); }
    void set_get_handler(CommandHandler h)   { get_handler_ = std::move(h); }

private:
    void accept_thread_func();
    void handle_client(int client_fd);
    Command parse_command(const std::string& line);

    std::string socket_path_;
    int server_fd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};

    CommandHandler clip_handler_;
    CommandHandler set_handler_;
    CommandHandler get_handler_;
};

} // namespace audio_daemon

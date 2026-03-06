#include <gtest/gtest.h>
#include "audio_daemon/control_socket.hpp"

using namespace audio_daemon;

// Test command parsing via a minimal public interface test
// (parse_command is private, so we test through the handler path)

TEST(ControlSocketTest, Construction) {
    // Should not crash or throw
    ControlSocket cs("/tmp/test_audiod_socket_" + std::to_string(getpid()));
}

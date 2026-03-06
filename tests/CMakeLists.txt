# Unit tests for audio_daemon
find_package(GTest REQUIRED)

set(UNIT_TEST_SOURCES
    test_ring_buffer.cpp
    test_common.cpp
    test_config.cpp
    test_control_socket.cpp
    test_mic_boost.cpp
)

add_executable(audio_daemon_tests ${UNIT_TEST_SOURCES})

target_link_libraries(audio_daemon_tests
    audio_core
    GTest::GTest
    GTest::Main
)

target_include_directories(audio_daemon_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

include(GoogleTest)
gtest_discover_tests(audio_daemon_tests)

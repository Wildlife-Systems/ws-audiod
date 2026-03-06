cmake_minimum_required(VERSION 3.16)
project(audio_daemon VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(SNDFILE REQUIRED sndfile)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/include
    ${ALSA_INCLUDE_DIRS}
    ${SNDFILE_INCLUDE_DIRS}
)

# Generate version header from debian/changelog version (passed via -DPROJECT_VERSION)
configure_file(
    ${CMAKE_SOURCE_DIR}/include/audio_daemon/version.hpp.in
    ${CMAKE_BINARY_DIR}/include/audio_daemon/version.hpp
    @ONLY
)

# Link directories
link_directories(
    ${ALSA_LIBRARY_DIRS}
    ${SNDFILE_LIBRARY_DIRS}
)

# Core library (all daemon components except main)
add_library(audio_core STATIC
    src/capture/alsa_capture.cpp
    src/capture/ring_buffer.cpp
    src/capture/clip_extractor.cpp
    src/capture/block_recorder.cpp
    src/sharing/sample_publisher.cpp
    src/pipeline/capture_pipeline.cpp
    src/utils/control_socket.cpp
    src/utils/config.cpp
)

target_link_libraries(audio_core
    ${ALSA_LIBRARIES}
    ${SNDFILE_LIBRARIES}
    pthread
    rt
)

# Daemon executable
add_executable(ws-audiod
    src/main.cpp
)

target_link_libraries(ws-audiod
    audio_core
)

# Example clients
add_executable(clip_client
    examples/clip_client.cpp
)

add_executable(audio_consumer
    examples/audio_consumer.cpp
)
target_link_libraries(audio_consumer rt pthread)

add_executable(audio_mon
    examples/audio_mon.cpp
)
target_link_libraries(audio_mon rt pthread)

# GNUInstallDirs for proper FHS-compliant paths
include(GNUInstallDirs)

# Install daemon executable
install(TARGETS ws-audiod
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install example executables
install(TARGETS clip_client audio_consumer audio_mon
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install headers
install(DIRECTORY include/audio_daemon
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install default config
install(FILES config/ws-audiod.conf
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/ws-audiod
)

# Install Python examples
install(DIRECTORY examples/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/ws-audiod/examples
    FILES_MATCHING
    PATTERN "*.py"
    PATTERN "*.cpp"
    PATTERN "__pycache__" EXCLUDE
)

# Create data directories
install(DIRECTORY DESTINATION ${CMAKE_INSTALL_LOCALSTATEDIR}/lib/ws-audiod/clips)
install(DIRECTORY DESTINATION ${CMAKE_INSTALL_LOCALSTATEDIR}/lib/ws-audiod/blocks)

# Unit tests
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

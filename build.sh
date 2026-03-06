#!/bin/bash
#
# Build script for audio daemon
#

set -e

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
INSTALL_PREFIX="${INSTALL_PREFIX:-/usr/local}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

check_dependencies() {
    info "Checking dependencies..."

    local missing=()

    if ! command -v cmake &> /dev/null; then
        missing+=("cmake")
    fi

    if ! pkg-config --exists alsa 2>/dev/null; then
        missing+=("libasound2-dev")
    fi

    if ! pkg-config --exists sndfile 2>/dev/null; then
        missing+=("libsndfile1-dev")
    fi

    if [ ${#missing[@]} -ne 0 ]; then
        error "Missing dependencies: ${missing[*]}\nInstall with: sudo apt install ${missing[*]}"
    fi

    info "All dependencies found"
}

build() {
    info "Building audio daemon..."

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    cmake .. \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"

    make -j$(nproc)

    info "Build complete"
}

install_daemon() {
    info "Installing..."

    cd "$BUILD_DIR"
    sudo make install

    sudo mkdir -p /var/lib/ws-audiod/clips
    sudo mkdir -p /var/lib/ws-audiod/blocks
    sudo mkdir -p /etc/ws/audiod

    if [ ! -f /etc/ws/audiod/ws-audiod.conf ]; then
        sudo cp ../config/ws-audiod.conf /etc/ws/audiod/
    fi

    sudo cp ../config/ws-audiod.service /etc/systemd/system/
    sudo systemctl daemon-reload

    info "Installation complete"
    info "Start with: sudo systemctl start ws-audiod"
}

clean() {
    info "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    info "Clean complete"
}

usage() {
    echo "Usage: $0 [command]"
    echo ""
    echo "Commands:"
    echo "  deps      Check dependencies"
    echo "  build     Build the project (default)"
    echo "  install   Install to system"
    echo "  clean     Clean build directory"
    echo "  all       Build and install"
    echo ""
    echo "Environment variables:"
    echo "  BUILD_DIR      Build directory (default: build)"
    echo "  BUILD_TYPE     CMake build type (default: Release)"
    echo "  INSTALL_PREFIX Install prefix (default: /usr/local)"
}

case "${1:-build}" in
    deps)    check_dependencies ;;
    build)   check_dependencies && build ;;
    install) install_daemon ;;
    clean)   clean ;;
    all)     check_dependencies && build && install_daemon ;;
    *)       usage ;;
esac

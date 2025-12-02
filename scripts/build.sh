#!/bin/bash
#
# Build script for Brewer BLE HID Bridge firmware
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(dirname "$SCRIPT_DIR")"
PROJECT_ROOT="$(dirname "$FIRMWARE_DIR")"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Default values
BOARD="${BOARD:-nrf52840dk_nrf52840}"
BUILD_TYPE="${BUILD_TYPE:-debug}"
CLEAN_BUILD=false
USE_DOCKER=true
REBUILD_DOCKER=false

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -b, --board BOARD     Target board (default: nrf52840dk_nrf52840)"
    echo "                        Options: nrf52840dk_nrf52840, nrf52840dongle_nrf52840"
    echo "  -c, --clean           Clean build (remove build directory first)"
    echo "  -d, --docker-rebuild  Rebuild Docker image from scratch (no cache)"
    echo "  -l, --local           Build locally instead of using Docker"
    echo "  -r, --release         Build in release mode"
    echo "  -h, --help            Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                    # Build for DK using Docker"
    echo "  $0 -c                 # Clean build"
    echo "  $0 -b nrf52840dongle_nrf52840  # Build for dongle"
    echo "  $0 -l                 # Build locally (requires nRF Connect SDK)"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -b|--board)
            BOARD="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN_BUILD=true
            shift
            ;;
        -d|--docker-rebuild)
            REBUILD_DOCKER=true
            shift
            ;;
        -l|--local)
            USE_DOCKER=false
            shift
            ;;
        -r|--release)
            BUILD_TYPE="release"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            usage
            exit 1
            ;;
    esac
done

echo -e "${GREEN}======================================${NC}"
echo -e "${GREEN}Brewer BLE HID Bridge - Build${NC}"
echo -e "${GREEN}======================================${NC}"
echo ""
echo -e "Board:      ${YELLOW}${BOARD}${NC}"
echo -e "Build type: ${YELLOW}${BUILD_TYPE}${NC}"
echo -e "Docker:     ${YELLOW}${USE_DOCKER}${NC}"
echo ""

cd "$FIRMWARE_DIR"

# Clean if requested
if [ "$CLEAN_BUILD" = true ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf build
fi

if [ "$USE_DOCKER" = true ]; then
    # Check if Docker is available
    if ! command -v docker &> /dev/null; then
        echo -e "${RED}Docker not found. Please install Docker or use -l for local build.${NC}"
        exit 1
    fi

    # Build Docker image if needed
    IMAGE_NAME="brewer-hid-bridge-builder"

    DOCKER_BUILD_ARGS=""
    if [ "$REBUILD_DOCKER" = true ]; then
        echo -e "${YELLOW}Rebuilding Docker image from scratch (no cache)...${NC}"
        DOCKER_BUILD_ARGS="--no-cache"
    else
        echo -e "${YELLOW}Building Docker image (this may take a while on first run)...${NC}"
    fi

    docker build $DOCKER_BUILD_ARGS -t "$IMAGE_NAME" -f Dockerfile .

    # Run build in Docker
    echo -e "${YELLOW}Building firmware in Docker...${NC}"

    BUILD_ARGS="-b $BOARD"
    if [ "$CLEAN_BUILD" = true ]; then
        BUILD_ARGS="$BUILD_ARGS --pristine"
    fi

    docker run --rm \
        -v "$FIRMWARE_DIR:/app" \
        -w /app \
        "$IMAGE_NAME" \
        west build $BUILD_ARGS

else
    # Local build
    if [ -z "$ZEPHYR_BASE" ]; then
        echo -e "${RED}ZEPHYR_BASE not set. Please source zephyr-env.sh or use Docker build.${NC}"
        exit 1
    fi

    echo -e "${YELLOW}Building firmware locally...${NC}"

    BUILD_ARGS="-b $BOARD"
    if [ "$CLEAN_BUILD" = true ]; then
        BUILD_ARGS="$BUILD_ARGS --pristine"
    fi

    west build $BUILD_ARGS
fi

# Check if build succeeded
if [ -f "build/zephyr/zephyr.hex" ]; then
    echo ""
    echo -e "${GREEN}======================================${NC}"
    echo -e "${GREEN}Build successful!${NC}"
    echo -e "${GREEN}======================================${NC}"
    echo ""
    echo "Output files:"
    echo "  build/zephyr/zephyr.hex"
    echo "  build/zephyr/zephyr.bin"
    echo ""
    echo "To flash, run:"
    echo "  ./scripts/flash.sh"
else
    echo -e "${RED}Build failed!${NC}"
    exit 1
fi

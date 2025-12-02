#!/bin/bash
#
# Flash script for Brewer BLE HID Bridge firmware
#
# Supports:
#   - nRF52840-DK with J-Link (full board)
#   - nRF52840-DK breakaway dongle (USB DFU)
#   - nRF52840 Dongle (USB DFU)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(dirname "$SCRIPT_DIR")"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Default values
HEX_FILE="${FIRMWARE_DIR}/build/zephyr/zephyr.hex"
FLASH_METHOD="auto"
DO_RECOVER=false

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -m, --method METHOD   Flash method: auto, jlink, dfu (default: auto)"
    echo "  -f, --file FILE       Hex file to flash (default: build/zephyr/zephyr.hex)"
    echo "  -p, --port PORT       Serial port for DFU (auto-detected if not specified)"
    echo "  -r, --recover         Recover protected device (full erase) before flashing"
    echo "  -h, --help            Show this help message"
    echo ""
    echo "Methods:"
    echo "  jlink   Use J-Link (full DK board with Debug USB)"
    echo "  dfu     Use USB DFU bootloader (breakaway dongle or standalone dongle)"
    echo "  auto    Try J-Link first, fall back to DFU"
    echo ""
    echo "For DFU mode, put device in bootloader first:"
    echo "  - Hold RESET button while plugging in USB"
    echo "  - Or double-tap RESET quickly"
}

# Parse arguments
DFU_PORT=""
while [[ $# -gt 0 ]]; do
    case $1 in
        -m|--method)
            FLASH_METHOD="$2"
            shift 2
            ;;
        -f|--file)
            HEX_FILE="$2"
            shift 2
            ;;
        -p|--port)
            DFU_PORT="$2"
            shift 2
            ;;
        -r|--recover)
            DO_RECOVER=true
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
echo -e "${GREEN}Brewer BLE HID Bridge - Flash${NC}"
echo -e "${GREEN}======================================${NC}"
echo ""

# Check if hex file exists
if [ ! -f "$HEX_FILE" ]; then
    echo -e "${RED}Hex file not found: $HEX_FILE${NC}"
    echo "Run ./scripts/build.sh first"
    exit 1
fi

echo -e "Hex file: ${CYAN}${HEX_FILE}${NC}"
echo ""

# Function to recover protected device
recover_device() {
    echo -e "${YELLOW}Recovering protected device...${NC}"
    echo -e "${RED}WARNING: This will erase ALL data on the chip!${NC}"
    echo ""

    nrfjprog --recover

    if [ $? -eq 0 ]; then
        echo -e "${GREEN}Recovery successful!${NC}"
        return 0
    else
        echo -e "${RED}Recovery failed${NC}"
        return 1
    fi
}

# Function to flash via J-Link
flash_jlink() {
    echo -e "${YELLOW}Flashing via J-Link...${NC}"

    if ! command -v nrfjprog &> /dev/null; then
        echo -e "${RED}nrfjprog not found!${NC}"
        echo "Install: brew install --cask nordic-nrf-command-line-tools"
        return 1
    fi

    # Check for connected J-Link
    DEVICES=$(nrfjprog --ids 2>/dev/null || true)
    if [ -z "$DEVICES" ]; then
        echo -e "${YELLOW}No J-Link device found${NC}"
        return 1
    fi

    echo -e "Found J-Link: ${GREEN}${DEVICES}${NC}"

    # Recover if requested
    if [ "$DO_RECOVER" = true ]; then
        recover_device || return 1
    fi

    nrfjprog --program "$HEX_FILE" --sectorerase --verify
    nrfjprog --reset
    return 0
}

# Function to flash via DFU
flash_dfu() {
    echo -e "${YELLOW}Flashing via USB DFU...${NC}"

    # Find DFU port if not specified
    if [ -z "$DFU_PORT" ]; then
        echo -e "${YELLOW}Looking for DFU device...${NC}"

        if [[ "$OSTYPE" == "darwin"* ]]; then
            # macOS - look for Nordic DFU or CDC ACM
            DFU_PORT=$(ls /dev/tty.usbmodem* 2>/dev/null | head -1)
        else
            # Linux
            DFU_PORT=$(ls /dev/ttyACM* 2>/dev/null | head -1)
        fi
    fi

    if [ -z "$DFU_PORT" ]; then
        echo -e "${RED}No DFU device found!${NC}"
        echo ""
        echo -e "${CYAN}To enter DFU bootloader mode:${NC}"
        echo "  1. Unplug the USB cable"
        echo "  2. Hold down the RESET button (or SW1 on dongle)"
        echo "  3. While holding, plug in the USB cable"
        echo "  4. Release the button after 1 second"
        echo ""
        echo "The device should appear as a serial port."
        echo "On some devices, the LED will pulse to indicate bootloader mode."
        return 1
    fi

    echo -e "DFU port: ${GREEN}${DFU_PORT}${NC}"

    # Generate DFU package
    DFU_PKG="${FIRMWARE_DIR}/build/dfu_package.zip"
    echo -e "${YELLOW}Generating DFU package...${NC}"

    # Find nrfutil - prefer homebrew standalone version
    NRFUTIL=""
    if [ -x "/opt/homebrew/bin/nrfutil" ]; then
        NRFUTIL="/opt/homebrew/bin/nrfutil"
    elif [ -x "/usr/local/bin/nrfutil" ]; then
        NRFUTIL="/usr/local/bin/nrfutil"
    elif command -v nrfutil &> /dev/null; then
        NRFUTIL="nrfutil"
    fi

    if [ -z "$NRFUTIL" ]; then
        echo -e "${RED}nrfutil not found!${NC}"
        echo "Install with: brew install nrfutil"
        return 1
    fi

    # Check if this is the new standalone nrfutil (has 'install' subcommand)
    if "$NRFUTIL" install --help &>/dev/null 2>&1; then
        echo -e "${CYAN}Using standalone nrfutil${NC}"

        # Install nrf5sdk-tools if not present
        if ! "$NRFUTIL" nrf5sdk-tools --help &>/dev/null 2>&1; then
            echo -e "${YELLOW}Installing nrfutil nrf5sdk-tools...${NC}"
            "$NRFUTIL" install nrf5sdk-tools
        fi

        "$NRFUTIL" nrf5sdk-tools pkg generate \
            --hw-version 52 \
            --sd-req 0x00 \
            --application "$HEX_FILE" \
            --application-version 1 \
            "$DFU_PKG"

        echo -e "${YELLOW}Uploading via DFU...${NC}"
        if ! "$NRFUTIL" nrf5sdk-tools dfu usb-serial -pkg "$DFU_PKG" -p "$DFU_PORT"; then
            echo -e "${RED}DFU upload failed!${NC}"
            echo ""
            echo -e "${CYAN}Make sure device is in bootloader mode:${NC}"
            echo "  - Unplug USB, hold RESET/SW1, plug in, release after 1s"
            echo "  - LED should pulse (not solid) to indicate bootloader"
            return 1
        fi
    else
        # Legacy Python nrfutil
        echo -e "${CYAN}Using legacy Python nrfutil${NC}"

        "$NRFUTIL" pkg generate \
            --hw-version 52 \
            --sd-req 0x00 \
            --application "$HEX_FILE" \
            --application-version 1 \
            "$DFU_PKG"

        echo -e "${YELLOW}Uploading via DFU...${NC}"
        if ! "$NRFUTIL" dfu usb-serial -pkg "$DFU_PKG" -p "$DFU_PORT"; then
            echo -e "${RED}DFU upload failed!${NC}"
            echo ""
            echo -e "${CYAN}Make sure device is in bootloader mode:${NC}"
            echo "  - Unplug USB, hold RESET/SW1, plug in, release after 1s"
            echo "  - LED should pulse (not solid) to indicate bootloader"
            return 1
        fi
    fi

    return 0
}

# Execute based on method
case $FLASH_METHOD in
    jlink)
        if ! flash_jlink; then
            echo -e "${RED}J-Link flash failed${NC}"
            exit 1
        fi
        ;;
    dfu)
        if ! flash_dfu; then
            echo -e "${RED}DFU flash failed${NC}"
            exit 1
        fi
        ;;
    auto)
        echo -e "${YELLOW}Auto-detecting flash method...${NC}"
        if flash_jlink; then
            echo ""
        else
            echo ""
            echo -e "${YELLOW}J-Link not available, trying DFU...${NC}"
            echo ""
            if ! flash_dfu; then
                echo -e "${RED}Flash failed${NC}"
                exit 1
            fi
        fi
        ;;
    *)
        echo -e "${RED}Unknown method: $FLASH_METHOD${NC}"
        usage
        exit 1
        ;;
esac

echo ""
echo -e "${GREEN}======================================${NC}"
echo -e "${GREEN}Flash complete!${NC}"
echo -e "${GREEN}======================================${NC}"
echo ""
echo "The device should now be running. Look for:"
echo "  - LED1 blinking slowly (status indicator)"
echo "  - 'HID Bridge' appearing in Bluetooth scan"
echo ""
echo "Connect USB to your target PC to use HID functions."

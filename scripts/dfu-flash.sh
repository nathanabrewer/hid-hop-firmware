#!/bin/bash
#
# Simple DFU flash script for HID-HOP firmware
# Builds, packages, and flashes via USB DFU with port confirmation
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(dirname "$SCRIPT_DIR")"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
RED='\033[0;31m'
NC='\033[0m'

# DFU is for dongles, so default to dongle board
BOARD="${BOARD:-nrf52840dongle_nrf52840}"

HEX_FILE="${FIRMWARE_DIR}/build/zephyr/zephyr.hex"
DFU_PKG="${FIRMWARE_DIR}/build/firmware.zip"

echo -e "${GREEN}=== HID-HOP DFU Flash ===${NC}"
echo -e "Board: ${CYAN}${BOARD}${NC}"
echo ""

# Step 1: Build
echo -e "${YELLOW}[1/4] Building firmware...${NC}"
"${SCRIPT_DIR}/build.sh" -b "$BOARD"
echo ""

# Check hex file exists
if [ ! -f "$HEX_FILE" ]; then
    echo -e "${RED}Build failed - hex file not found${NC}"
    exit 1
fi

# Step 2: Generate DFU package
echo -e "${YELLOW}[2/4] Generating DFU package...${NC}"
nrfutil pkg generate --hw-version 52 --sd-req 0x00 \
    --application "$HEX_FILE" \
    --application-version 1 \
    "$DFU_PKG"
echo -e "${GREEN}Created: ${DFU_PKG}${NC}"
echo ""

# Step 3: Find and confirm port
echo -e "${YELLOW}[3/4] Detecting USB devices...${NC}"
echo ""

if [[ "$OSTYPE" == "darwin"* ]]; then
    PORTS=$(ls /dev/tty.usbmodem* 2>/dev/null || true)
else
    PORTS=$(ls /dev/ttyACM* 2>/dev/null || true)
fi

if [ -z "$PORTS" ]; then
    echo -e "${RED}No DFU devices found!${NC}"
    echo ""
    echo -e "${CYAN}To enter DFU bootloader mode:${NC}"
    echo "  1. Hold the RESET button"
    echo "  2. Plug in USB (while holding)"
    echo "  3. Release after 1 second"
    echo ""
    exit 1
fi

echo -e "${CYAN}Available ports:${NC}"
i=1
declare -a PORT_ARRAY
for port in $PORTS; do
    echo "  [$i] $port"
    PORT_ARRAY+=("$port")
    ((i++))
done
echo ""

# If only one port, suggest it as default
if [ ${#PORT_ARRAY[@]} -eq 1 ]; then
    read -p "Use ${PORT_ARRAY[0]}? [Y/n]: " confirm
    if [[ "$confirm" =~ ^[Nn] ]]; then
        echo "Aborted."
        exit 0
    fi
    SELECTED_PORT="${PORT_ARRAY[0]}"
else
    read -p "Select port number [1-${#PORT_ARRAY[@]}]: " selection
    if [ -z "$selection" ] || [ "$selection" -lt 1 ] || [ "$selection" -gt ${#PORT_ARRAY[@]} ]; then
        echo -e "${RED}Invalid selection${NC}"
        exit 1
    fi
    SELECTED_PORT="${PORT_ARRAY[$((selection-1))]}"
fi

echo ""
echo -e "Selected: ${GREEN}${SELECTED_PORT}${NC}"
echo ""

# Step 4: Flash
echo -e "${YELLOW}[4/4] Flashing via DFU...${NC}"
nrfutil dfu usb-serial -pkg "$DFU_PKG" -p "$SELECTED_PORT"

echo ""
echo -e "${GREEN}=== Flash complete! ===${NC}"
echo ""
echo "Device will reboot automatically."
echo "Default PIN: 123456"

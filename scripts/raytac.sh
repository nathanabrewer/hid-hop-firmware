#!/bin/bash
#
# Build and flash for Raytac nRF52840 USB dongle
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(dirname "$SCRIPT_DIR")"
BOARD="nrf52840dongle_nrf52840"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${GREEN}=== Raytac Dongle Build & Flash ===${NC}"
echo ""

# Build
echo -e "${YELLOW}[1/3] Building...${NC}"
"${SCRIPT_DIR}/build.sh" -b "$BOARD"

# Generate DFU package
echo -e "${YELLOW}[2/3] Generating DFU package...${NC}"
nrfutil pkg generate --hw-version 52 --sd-req 0x00 \
    --application "${FIRMWARE_DIR}/build/zephyr/zephyr.hex" \
    --application-version 1 \
    "${FIRMWARE_DIR}/build/firmware.zip"

# Find port
echo -e "${YELLOW}[3/3] Flashing...${NC}"
if [[ "$OSTYPE" == "darwin"* ]]; then
    PORTS=$(ls /dev/tty.usbmodem* 2>/dev/null || true)
else
    PORTS=$(ls /dev/ttyACM* 2>/dev/null || true)
fi

if [ -z "$PORTS" ]; then
    echo -e "${RED}No DFU device found!${NC}"
    echo "Put dongle in DFU mode: hold RESET while plugging in USB"
    exit 1
fi

# Show ports and confirm
echo -e "${CYAN}Available ports:${NC}"
i=1
declare -a PORT_ARRAY
for port in $PORTS; do
    echo "  [$i] $port"
    PORT_ARRAY+=("$port")
    ((i++))
done

if [ ${#PORT_ARRAY[@]} -eq 1 ]; then
    read -p "Flash to ${PORT_ARRAY[0]}? [Y/n]: " confirm
    [[ "$confirm" =~ ^[Nn] ]] && exit 0
    PORT="${PORT_ARRAY[0]}"
else
    read -p "Select port [1-${#PORT_ARRAY[@]}]: " sel
    PORT="${PORT_ARRAY[$((sel-1))]}"
fi

nrfutil dfu usb-serial -pkg "${FIRMWARE_DIR}/build/firmware.zip" -p "$PORT"

echo ""
echo -e "${GREEN}Done! Default PIN: 123456${NC}"

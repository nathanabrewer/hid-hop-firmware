#!/bin/bash
#
# UF2 flash script for XIAO nRF52840
# Builds, converts to UF2, and flashes via USB mass storage
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

BOARD="xiao_nrf52840"
HEX_FILE="${FIRMWARE_DIR}/build/zephyr/zephyr.hex"
UF2_FILE="${FIRMWARE_DIR}/build/zephyr/hid-hop-xiao.uf2"
UF2CONV="/tmp/uf2conv.py"
UF2FAMILIES="/tmp/uf2families.json"

# XIAO nRF52840 uses Adafruit bootloader family ID
UF2_FAMILY="0xADA52840"

# Volume name for XIAO in bootloader mode
VOLUME_NAME="XIAO-SENSE"

echo -e "${GREEN}=== HID-HOP UF2 Flash (XIAO) ===${NC}"
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

# Step 2: Get uf2conv if needed
echo -e "${YELLOW}[2/4] Preparing UF2 tools...${NC}"
if [ ! -f "$UF2CONV" ]; then
    echo "Downloading uf2conv.py..."
    curl -sL https://raw.githubusercontent.com/microsoft/uf2/master/utils/uf2conv.py -o "$UF2CONV"
    chmod +x "$UF2CONV"
fi
if [ ! -f "$UF2FAMILIES" ]; then
    curl -sL https://raw.githubusercontent.com/microsoft/uf2/master/utils/uf2families.json -o "$UF2FAMILIES"
fi
echo -e "${GREEN}UF2 tools ready${NC}"
echo ""

# Step 3: Convert to UF2
echo -e "${YELLOW}[3/4] Converting to UF2...${NC}"
python3 "$UF2CONV" "$HEX_FILE" --family "$UF2_FAMILY" --convert --output "$UF2_FILE"
echo -e "${GREEN}Created: ${UF2_FILE}${NC}"
echo ""

# Step 4: Wait for volume and flash
echo -e "${YELLOW}[4/4] Waiting for ${VOLUME_NAME} volume...${NC}"
echo ""
echo -e "${CYAN}Put XIAO in bootloader mode:${NC}"
echo "  Double-tap the RESET button quickly"
echo ""

# Wait for volume to appear (timeout after 30 seconds)
TIMEOUT=30
ELAPSED=0
while [ ! -d "/Volumes/${VOLUME_NAME}" ]; do
    sleep 1
    ((ELAPSED++))
    if [ $ELAPSED -ge $TIMEOUT ]; then
        echo -e "${RED}Timeout waiting for ${VOLUME_NAME} volume${NC}"
        echo "Make sure to double-tap reset to enter bootloader mode."
        exit 1
    fi
    printf "."
done
echo ""
echo -e "${GREEN}Found ${VOLUME_NAME}!${NC}"
echo ""

# Copy UF2 file
echo "Copying firmware..."
cp "$UF2_FILE" "/Volumes/${VOLUME_NAME}/" 2>/dev/null || true

# Wait a moment for device to reboot
sleep 2

echo ""
echo -e "${GREEN}=== Flash complete! ===${NC}"
echo ""
echo "Device will reboot automatically."
echo "Look for /dev/tty.usbmodem* to connect."

#!/bin/bash
#
# Setup script for macOS development environment
# Installs nRF Command Line Tools for flashing
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}======================================${NC}"
echo -e "${GREEN}Brewer BLE HID Bridge - macOS Setup${NC}"
echo -e "${GREEN}======================================${NC}"
echo ""

# Check for Homebrew
if ! command -v brew &> /dev/null; then
    echo -e "${RED}Homebrew not found!${NC}"
    echo "Install it from: https://brew.sh"
    exit 1
fi

echo -e "${YELLOW}Installing nRF Command Line Tools...${NC}"
brew install --cask nordic-nrf-command-line-tools

echo ""
echo -e "${YELLOW}Installing serial tools...${NC}"
brew install screen

echo ""
echo -e "${GREEN}Setup complete!${NC}"
echo ""
echo "You can now:"
echo "  1. Build:   ./scripts/build.sh"
echo "  2. Flash:   ./scripts/flash.sh"
echo "  3. Monitor: ./scripts/monitor.sh"
echo ""
echo -e "${YELLOW}nRF52840-DK USB Connections:${NC}"
echo ""
echo "  ┌─────────────────────────────────────┐"
echo "  │         nRF52840-DK                 │"
echo "  │                                     │"
echo "  │  [Debug USB]         [nRF USB]      │"
echo "  │      │                   │          │"
echo "  │      │                   │          │"
echo "  │      ▼                   ▼          │"
echo "  │   To Mac              To PC         │"
echo "  │   (flash +            (HID output)  │"
echo "  │    debug)                           │"
echo "  └─────────────────────────────────────┘"
echo ""
echo "Connect BOTH USB ports:"
echo "  - Debug USB → Your Mac (for programming)"
echo "  - nRF USB   → Target PC (receives keyboard/mouse input)"

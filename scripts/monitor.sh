#!/bin/bash
#
# Serial monitor for Brewer BLE HID Bridge firmware
# Shows debug output from the device
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Default baud rate
BAUD_RATE=115200

usage() {
    echo "Usage: $0 [OPTIONS] [PORT]"
    echo ""
    echo "Options:"
    echo "  -b, --baud RATE       Baud rate (default: 115200)"
    echo "  -l, --list            List available serial ports"
    echo "  -h, --help            Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                    # Auto-detect port"
    echo "  $0 /dev/tty.usbmodem* # Specify port"
    echo "  $0 -l                 # List ports"
}

list_ports() {
    echo "Available serial ports:"
    echo ""

    if [[ "$OSTYPE" == "darwin"* ]]; then
        # macOS
        ls /dev/tty.usbmodem* /dev/tty.usbserial* 2>/dev/null || echo "  No USB serial ports found"
    else
        # Linux
        ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "  No USB serial ports found"
    fi
}

# Parse arguments
PORT=""
while [[ $# -gt 0 ]]; do
    case $1 in
        -b|--baud)
            BAUD_RATE="$2"
            shift 2
            ;;
        -l|--list)
            list_ports
            exit 0
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo -e "${RED}Unknown option: $1${NC}"
            usage
            exit 1
            ;;
        *)
            PORT="$1"
            shift
            ;;
    esac
done

# Auto-detect port if not specified
if [ -z "$PORT" ]; then
    echo -e "${YELLOW}Auto-detecting serial port...${NC}"

    if [[ "$OSTYPE" == "darwin"* ]]; then
        # macOS - look for J-Link CDC port
        PORT=$(ls /dev/tty.usbmodem* 2>/dev/null | head -1)
    else
        # Linux
        PORT=$(ls /dev/ttyACM* 2>/dev/null | head -1)
    fi

    if [ -z "$PORT" ]; then
        echo -e "${RED}No serial port found!${NC}"
        echo ""
        echo "Make sure the nRF52840-DK is connected via Debug USB."
        echo ""
        list_ports
        exit 1
    fi
fi

echo -e "${GREEN}======================================${NC}"
echo -e "${GREEN}Brewer BLE HID Bridge - Serial Monitor${NC}"
echo -e "${GREEN}======================================${NC}"
echo ""
echo -e "Port: ${YELLOW}${PORT}${NC}"
echo -e "Baud: ${YELLOW}${BAUD_RATE}${NC}"
echo ""
echo "Press Ctrl+C to exit"
echo ""
echo -e "${GREEN}--- Output ---${NC}"

# Check what tools are available
if command -v screen &> /dev/null; then
    screen "$PORT" "$BAUD_RATE"
elif command -v minicom &> /dev/null; then
    minicom -D "$PORT" -b "$BAUD_RATE"
elif command -v picocom &> /dev/null; then
    picocom -b "$BAUD_RATE" "$PORT"
elif command -v python3 &> /dev/null; then
    # Use pyserial as fallback
    python3 -c "
import serial
import sys

try:
    ser = serial.Serial('$PORT', $BAUD_RATE, timeout=1)
    print('Connected. Press Ctrl+C to exit.')
    while True:
        line = ser.readline()
        if line:
            print(line.decode('utf-8', errors='replace'), end='')
except KeyboardInterrupt:
    print('\nDisconnected.')
except Exception as e:
    print(f'Error: {e}')
    sys.exit(1)
"
else
    echo -e "${RED}No serial terminal found!${NC}"
    echo ""
    echo "Please install one of: screen, minicom, picocom"
    echo "  macOS:  brew install screen"
    echo "  Linux:  apt install screen"
    exit 1
fi

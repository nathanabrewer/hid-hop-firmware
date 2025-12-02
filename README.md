<p align="center">
  <img src="docs/images/hopguy.png" width="200" alt="HID-HOP Mascot">
</p>

<h1 align="center">HID-HOP Firmware</h1>

<p align="center">
  <strong>nRF52840 BLE-to-USB HID Bridge Firmware</strong>
</p>

<p align="center">
  Turn your phone into a wireless keyboard and mouse
</p>

---

## What is HID-HOP?

HID-HOP is a BLE (Bluetooth Low Energy) to USB HID bridge. A small nRF52840-based dongle plugs into any computer's USB port and appears as a standard keyboard and mouse. Your phone connects to the dongle over BLE and sends HID commands - no drivers, no pairing hassles, just plug and play.

## Features

- **USB HID Composite Device** - Keyboard + Mouse + Consumer Control
- **BLE GATT Service** - Custom protocol for low-latency command transmission
- **PIN Protection** - Optional 4-8 digit PIN for device security
- **Configurable Device Name** - Customize the BLE advertised name
- **Zephyr RTOS** - Built on the nRF Connect SDK

## Hardware

- **nRF52840 Dongle** (PCA10059) or compatible board
- Any computer with a USB port

## Building

### Prerequisites

- [nRF Connect SDK](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/getting_started.html) v2.5.0+
- West build system

### Build Commands

```bash
# Using the build script (recommended)
./scripts/build.sh

# Or manually with west
west build -b nrf52840dongle_nrf52840 firmware
```

### Flashing

```bash
# Using nrfutil (USB DFU)
./scripts/flash.sh

# Or with a J-Link debugger
west flash
```

## Protocol

The HID-HOP protocol uses a simple binary format over BLE GATT:

| Byte | Description |
|------|-------------|
| 0 | Command Type |
| 1 | Payload Length |
| 2+ | Payload Data |

See [protocol documentation](docs/PROTOCOL.md) for full command reference.

## BLE Service

- **Service UUID**: `F8B34000-6E8B-4B5A-9F3E-2C1D4A8E7F00`
- **Command Characteristic**: `F8B34001-...` (Write without response)
- **Response Characteristic**: `F8B34002-...` (Notify)

## Project Structure

```
firmware/
├── src/
│   ├── main.c              # Entry point
│   ├── ble_hid_service.c   # BLE GATT service
│   ├── usb_hid.c           # USB HID device
│   ├── protocol.c          # Command parsing
│   ├── security.c          # PIN authentication
│   └── config.c            # NVS configuration storage
├── inc/                    # Header files
├── prj.conf               # Zephyr configuration
└── CMakeLists.txt
```

## License

Copyright (c) 2025 Nathan Brewer. All Rights Reserved.

See [LICENSE](LICENSE) for details.

## Related Projects

- [hid-hop-ios](https://github.com/nathanabrewer/hid-hop-ios) - iOS companion app

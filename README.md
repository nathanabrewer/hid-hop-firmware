<p align="center">
  <img src="docs/images/hopguy.png" width="200" alt="HID-HOP Mascot">
</p>

<h1 align="center">HID-HOP Firmware</h1>

<p align="center">
  <strong>nRF52840 BLE Mesh HID Bridge</strong>
</p>

<p align="center">
  Encrypted keyboard/mouse control over BLE Mesh
</p>

---

## What is HID-HOP?

HID-HOP is a BLE Mesh-based USB HID bridge. Multiple nRF52840 dongles form a self-healing mesh network, allowing you to send encrypted keyboard and mouse commands to any node. Each node appears as a standard USB HID device to its host computer.

**Use cases:**
- Remote KVM over BLE mesh (no line of sight needed)
- Multi-room keyboard/mouse control
- Secure HID injection with E2E encryption
- IoT control with GPIO integration

## Features

| Feature | Description |
|---------|-------------|
| **BLE Mesh** | Self-provisioning mesh network with relay support |
| **USB HID** | Keyboard + Mouse + Consumer Control composite device |
| **E2E Encryption** | AES-128-CCM encrypted HID payloads |
| **PIN Authentication** | 4-8 digit PIN required for HID access |
| **JSONL Serial** | USB CDC ACM interface for control/monitoring |
| **GPIO Control** | LED and button access via serial commands |
| **RC PWM Control** | Servo/ESC outputs with skid steer mixing (XIAO) |
| **Peer Discovery** | Auto-discovery of mesh nodes with RSSI tracking |
| **Node Naming** | Friendly names persisted to NVS |

## Security Model

```
┌─────────────────────────────────────────────────────────────┐
│                     SECURITY LAYERS                         │
├─────────────────────────────────────────────────────────────┤
│  1. BLE Mesh Network Encryption (shared network key)        │
│  2. BLE Mesh Application Encryption (shared app key)        │
│  3. PIN Authentication (per-node, 30-min session)           │
│  4. E2E Session Key (AES-128-CCM, per-peer, from PIN+nonce) │
└─────────────────────────────────────────────────────────────┘
```

**HID commands require:**
1. PIN authentication (proves you know the device PIN)
2. Key exchange (establishes session key)
3. Encrypted payload (AES-128-CCM with replay protection)

Plaintext HID commands are **rejected by default**.

## Hardware

### Supported Boards

| Board | Status | Notes |
|-------|--------|-------|
| Raytac MDBT50Q-CX-40 | Primary | USB dongle, no crystal, DFU flash |
| Seeed XIAO nRF52840 | Supported | RC PWM outputs, UF2 flash, external crystal |
| nRF52840 Dongle (PCA10059) | Tested | Nordic official dongle |
| Custom nRF52840 boards | Compatible | See board config |

### Board Capabilities

| Feature | Raytac CX-40 | XIAO nRF52840 |
|---------|--------------|---------------|
| LEDs | 2 (blue) | 3 (RGB) |
| Buttons | 1 | 0 |
| RC PWM | No | 2 channels (D0, D1) |
| Crystal | RC oscillator | 32kHz XTAL |
| Flash method | DFU | UF2 |

### Hardware Constraints (Raytac CX-40)

- No external 32kHz crystal - uses RC oscillator
- No DCDC inductor - LDO only
- No UART pins exposed - USB CDC only
- DFU bootloader at 0x0000-0x1000

## Building

### Docker Build (Recommended)

```bash
# Build firmware
./scripts/build.sh -b raytac_mdbt50q_cx_40

# Build and flash via DFU
./scripts/dfu-flash.sh

# Clean build
./scripts/build.sh -b raytac_mdbt50q_cx_40 -c
```

### Flashing

#### Raytac CX-40 (DFU)

```bash
# Put dongle in DFU mode: hold button while plugging in USB

# Flash via nrfutil
nrfutil pkg generate --hw-version 52 --sd-req 0x00 \
  --application build/zephyr/zephyr.hex \
  --application-version 1 firmware.zip

nrfutil dfu usb-serial -pkg firmware.zip -p /dev/tty.usbmodem*
```

#### XIAO nRF52840 (UF2)

```bash
# Build and flash in one command
./scripts/uf2-flash.sh

# Or manually:
# 1. Double-tap RESET to enter bootloader (XIAO-SENSE volume appears)
# 2. Copy UF2 file to the volume
cp build/zephyr/hid-hop-xiao.uf2 /Volumes/XIAO-SENSE/
```

## Serial Interface (JSONL)

Connect via USB CDC ACM at 115200 baud:

```bash
screen /dev/tty.usbmodem1201 115200
```

### Command Reference

#### Mesh Commands

| Command | Description |
|---------|-------------|
| `{"cmd":"mesh_status"}` | Get provisioning status, address, peer count |
| `{"cmd":"mesh_discover"}` | Broadcast discovery to find peers |
| `{"cmd":"mesh_ping","addr":"0x1c04"}` | Ping specific node |
| `{"cmd":"mesh_text","addr":"0x1c04","text":"hello"}` | Send text message |
| `{"cmd":"mesh_reset"}` | Reset mesh provisioning (reboot required) |
| `{"cmd":"peers"}` | List all known peers (with stale status) |
| `{"cmd":"peer_info","addr":"0x1c04"}` | Get peer details |
| `{"cmd":"set_name","name":"MyNode"}` | Set node name |
| `{"cmd":"discovery_settings"}` | Get periodic discovery settings |
| `{"cmd":"set_discovery_interval","interval":60000}` | Set discovery interval (ms, 0=disable) |

#### Authentication & Encryption

| Command | Description |
|---------|-------------|
| `{"cmd":"mesh_auth","addr":"0x1c04","pin":"123456"}` | Authenticate with PIN |
| `{"cmd":"key_exchange","addr":"0x1c04"}` | Establish session key |
| `{"cmd":"encryption_status","addr":"0x1c04"}` | Check encryption state |
| `{"cmd":"set_encryption","required":true}` | Require encryption for HID |
| `{"cmd":"set_pin_required","enabled":true}` | Require PIN for HID |

#### HID Commands (Encrypted)

| Command | Description |
|---------|-------------|
| `{"cmd":"hid_type_enc","addr":"0x1c04","text":"hello"}` | Type text (encrypted) |
| `{"cmd":"hid_key_enc","addr":"0x1c04","key":40,"mod":0}` | Key tap (encrypted) |
| `{"cmd":"hid_click_enc","addr":"0x1c04","btn":1}` | Mouse click (encrypted) |
| `{"cmd":"text_enc","addr":"0x1c04","text":"secret"}` | Encrypted text message |

#### HID Commands (Plaintext - rejected if encryption required)

| Command | Description |
|---------|-------------|
| `{"cmd":"hid_type","addr":"0x1c04","text":"hello"}` | Type text |
| `{"cmd":"hid_key","addr":"0x1c04","key":40,"mod":0}` | Key tap |
| `{"cmd":"hid_click","addr":"0x1c04","btn":1}` | Mouse click (1=L,2=R,4=M) |
| `{"cmd":"hid_move","addr":"0x1c04","dx":10,"dy":-5}` | Mouse move |
| `{"cmd":"hid_media","addr":"0x1c04","usage":205}` | Consumer key |

#### GPIO Commands (Local)

| Command | Description |
|---------|-------------|
| `{"cmd":"gpio_status"}` | Get LED/button counts and states |
| `{"cmd":"gpio_led","id":0,"on":true}` | Set LED state |
| `{"cmd":"gpio_toggle","id":0}` | Toggle LED |
| `{"cmd":"gpio_btn","id":0}` | Read button state |
| `{"cmd":"gpio_blink","id":0,"count":3}` | Blink LED |

#### GPIO Commands (Remote/Mesh - Require PIN Auth)

| Command | Description |
|---------|-------------|
| `{"cmd":"mesh_gpio_led","addr":"0x1c04","id":0,"on":true}` | Set remote LED (plaintext) |
| `{"cmd":"mesh_gpio_toggle","addr":"0x1c04","id":0}` | Toggle remote LED (plaintext) |
| `{"cmd":"mesh_gpio_blink","addr":"0x1c04","id":0,"count":3}` | Blink remote LED (plaintext) |
| `{"cmd":"mesh_gpio_led_enc","addr":"0x1c04","id":0,"on":true}` | Set remote LED (encrypted) |
| `{"cmd":"mesh_gpio_toggle_enc","addr":"0x1c04","id":0}` | Toggle remote LED (encrypted) |
| `{"cmd":"mesh_gpio_blink_enc","addr":"0x1c04","id":0,"count":3}` | Blink remote LED (encrypted) |

**Note:** Mesh GPIO commands require PIN authentication, just like HID commands. When encryption is required (default), only `_enc` variants will work.

#### RC PWM Commands (XIAO only)

| Command | Description |
|---------|-------------|
| `{"cmd":"rc_status"}` | Get RC channel status, mode, and inversion |
| `{"cmd":"rc_set","ch":0,"us":1500}` | Set channel pulse width (1000-2000µs) |
| `{"cmd":"rc_center"}` | Center all channels (1500µs) |
| `{"cmd":"rc_disable","ch":0}` | Disable PWM output on channel |
| `{"cmd":"rc_failsafe","enabled":true}` | Enable/disable auto-center on signal loss |
| `{"cmd":"rc_mode","mode":"skid_steer"}` | Set drive mode (normal/skid_steer) |
| `{"cmd":"rc_invert","ch0":true,"ch1":false,"swap":false}` | Set channel inversion |

**Drive Modes:**
- **normal**: CH0 = X (steering), CH1 = Y (throttle)
- **skid_steer**: CH0 = left motor (Y+X), CH1 = right motor (Y-X)

**Failsafe:** When enabled, channels fade to center (1500µs) if no command received for 500ms.

### Events (from device)

```json
{"event":"connected","device":"HID-HOP"}
{"event":"mesh_status","provisioned":true,"addr":"0x00f1","peer_count":2}
{"event":"discovery_resp_rcvd","from":"0x1c04","rssi":-45}
{"event":"auto_discovery","sent":true}
{"event":"peer_check","stale_peers":1}
{"event":"mesh_text_rcvd","from":"0x1c04","encrypted":true,"text":"hello"}
{"event":"hid_cmd_rcvd","from":"0x1c04","type":"keyboard","encrypted":true,"ok":true}
{"event":"gpio_cmd_rcvd","from":"0x1c04","action":"led_toggle","id":0,"on":true,"encrypted":true}
{"event":"key_exchange_complete","from":"0x1c04","key_established":true}
```

**Note:** Periodic discovery runs every 60 seconds by default. Peers not seen for 3 minutes are marked stale.

## Testing with Node.js

```bash
cd tools
npm install serialport

# Run encryption test
node mesh-encrypt-test.js /dev/tty.usbmodem1201 /dev/tty.usbmodem1401
```

Interactive commands:
```
peers                      # List peers
auth 0x1c04                # Authenticate (default PIN: 123456)
keyex 0x1c04               # Key exchange
send 0x1c04 "hello"        # Encrypted text
type 0x1c04 "secret"       # Encrypted HID type
```

## Mesh Network

### Self-Provisioning

Nodes automatically provision into a shared mesh network on first boot:
- Generates unique address from device ID
- Uses hardcoded network/app keys (fine for dev, customize for production)
- Binds vendor model to app key
- Runs auto-discovery 3 seconds after boot

### Addressing

| Address | Type |
|---------|------|
| `0x0001-0x7FFF` | Unicast (individual nodes) |
| `0xC000` | Group (all HID-HOP nodes) |
| `0xFFFF` | Broadcast (all mesh nodes) |

### Range

- Direct: 10-30m indoor, 30-100m outdoor
- Multi-hop: Up to 5 hops (TTL=5), extends range significantly
- Each node acts as a relay

## BLE Mesh Protocol Reference

### Vendor Model

- **Company ID**: `0x1915` (Nordic Semiconductor)
- **Model ID**: `0x0001` (HID-HOP Vendor Model)

### Opcode Reference

All opcodes are 3-byte vendor opcodes: `[opcode] [company_id_lo] [company_id_hi]`

| Opcode | Name | Payload | Description |
|--------|------|---------|-------------|
| `0x01` | Discovery | - | Find mesh nodes (broadcast) |
| `0x02` | Discovery Response | UUID, name, caps | Node announces itself |
| `0x03` | Beacon | UUID, name, caps | One-way presence broadcast |
| `0x04` | HID Command (Enc) | type, action, data | Encrypted HID command |
| `0x05` | Text (Enc) | ciphertext | Encrypted text message |
| `0x06` | Key Exchange | challenge[16] | Initiate E2E key exchange |
| `0x07` | Key Confirm | challenge[16] | Confirm session key |
| `0x08` | GPIO Command | action, led_id, data | LED control (plaintext) |
| `0x09` | GPIO Command (Enc) | ciphertext | LED control (encrypted) |
| `0x0A` | RC Vector | x[2], y[2], flags | Joystick XY (plaintext) |
| `0x0B` | RC Vector (Enc) | ciphertext | Joystick XY (encrypted) |
| `0x0C` | RC Config | mode, invert | Set drive mode/inversion |
| `0x10` | HID Command | type, action, data | HID command (plaintext) |
| `0x11` | HID Response | status | HID command acknowledgment |
| `0x20` | Status | addr, provisioned, etc | Node status report |
| `0x24` | PIN Auth | PIN string | Authenticate with PIN |
| `0x25` | PIN Response | status | PIN auth result |
| `0x30` | Text | text string | Plain text message |

### RC Vector Payload (5 bytes)

```
Offset  Type      Description
0-1     int16 LE  X axis (-1000 to +1000, left/right)
2-3     int16 LE  Y axis (-1000 to +1000, back/forward)
4       uint8     Flags (reserved, set to 0)
```

### RC Config Payload (2 bytes)

```
Offset  Type    Description
0       uint8   Mode: 0x00=normal, 0x01=skid_steer
1       uint8   Invert flags: 0x01=CH0, 0x02=CH1, 0x04=swap
```

### GPIO Command Payload

```
Offset  Type    Description
0       uint8   Action: 0x00=set, 0x01=toggle, 0x02=blink
1       uint8   LED index (0-based)
2+      varies  Action data (on/off byte, blink count, etc)
```

## Project Structure

```
hid-hop-firmware/
├── boards/
│   ├── arm/
│   │   ├── raytac_mdbt50q_cx_40/  # Raytac USB dongle board
│   │   └── xiao_nrf52840/         # Seeed XIAO board
│   ├── raytac_mdbt50q_cx_40.conf  # Raytac board config overlay
│   └── xiao_nrf52840.conf         # XIAO board config overlay
├── src/
│   ├── main.c              # Entry point, init sequence
│   ├── mesh_hid.c          # BLE Mesh + HID forwarding + E2E encryption + RC
│   ├── jsonl_serial.c      # USB CDC command interface
│   ├── ble_hid_service.c   # BLE GATT service
│   ├── hid_keyboard.c      # USB HID keyboard
│   ├── hid_mouse.c         # USB HID mouse
│   ├── hid_consumer.c      # USB HID consumer control
│   ├── config.c            # Raw flash config (CRC32 validated)
│   ├── gpio_control.c      # LED/button/RC PWM control
│   └── security.c          # PIN authentication
├── inc/                    # Header files
├── tools/
│   ├── mesh-encrypt-test.js   # Node.js test tool
│   ├── serial-cli.js          # Interactive serial CLI
│   └── quick-test.js          # Simple connectivity test
├── scripts/
│   ├── build.sh            # Docker build script
│   ├── dfu-flash.sh        # Build + flash (Raytac DFU)
│   ├── uf2-flash.sh        # Build + flash (XIAO UF2)
│   └── flash.sh            # J-Link flash
├── prj.conf                # Zephyr Kconfig
├── CMakeLists.txt
└── CLAUDE.md               # Build notes for AI assistance
```

## Troubleshooting

### "No trigger interface found" during DFU
Device not in bootloader mode. Unplug, hold button, plug in while holding.

### No serial output
DTR not set. Type something or use a terminal that sets DTR on connect.

### Nodes can't find each other
Run `{"cmd":"mesh_discover"}` manually. Auto-discovery only runs once at boot.

### HID commands rejected
Check: `{"cmd":"encryption_status","addr":"0x1c04"}`
- Need `authenticated: true` (run `mesh_auth`)
- Need `has_session_key: true` (run `key_exchange`)

### Boot loop
Check clock config. Raytac needs `CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y`.

## Storage Architecture

### Flash Partition Layout

| Partition | Address | Size | Purpose |
|-----------|---------|------|---------|
| bootloader | 0x00000 | 4KB | Nordic Open DFU |
| image-0 | 0x01000 | 892KB | Application |
| config | 0xE0000 | 4KB | Raw flash config |
| storage | 0xE1000 | 124KB | NVS (bonding, logs) |

### Why Raw Flash for Config?

Boot-critical configuration uses raw flash with CRC32 validation instead of Zephyr's NVS/settings subsystem:

**The problem with NVS for boot config:**
- If NVS data corrupts, `settings_load()` crashes during boot
- Device becomes bricked with no recovery path
- Partial corruption is hard to detect and handle

**Raw flash + CRC32 solution:**
- Read entire config blob from flash
- Validate: magic → version → struct size → CRC32
- If ANY check fails: erase, use defaults, device boots
- Config is written rarely (name/PIN set once), so no wear leveling needed

**NVS is still used for:**
- BLE bonding data (Zephyr manages this)
- Future: logging, session state, counters
- Anything that's not boot-critical

This hybrid approach gives robust boot-time config validation while still using NVS for its intended purpose.

## Roadmap

### Planned: Serial Port Tunnel over Mesh

Encrypted serial tunnel between mesh nodes - effectively a **virtual RS-485 bus over BLE**.

**Features:**
- Second USB CDC ACM interface dedicated to tunnel data (raw, not JSONL)
- Point-to-point or multi-drop (group addressing) modes
- E2E encryption for secure tunneling
- ~9600 baud effective throughput (mesh MTU limited)
- 500ms failsafe timeout

**Use Cases:**
- Serial console forwarding over mesh
- Modbus/RS-485 protocol bridging
- Remote sensor data collection
- Mesh-connected serial devices

**Architecture:**
```
Computer A ──USB CDC #2──> Node A ──encrypted mesh──> Node B ──USB CDC #2──> Computer B
```

**Multi-drop Mode (RS-485 style):**
```
          ┌──> Node B (0x1C04)
Computer ─┼──> Node C (0x1C05)  (group address 0xC000)
          └──> Node D (0x1C06)
```

Reserved opcodes: `0x0D` (Tunnel Data), `0x0E` (Tunnel Data Encrypted)

## License

Copyright (c) 2025 Nathan Brewer. All Rights Reserved.

## Related Projects

- [hid-hop-ios](https://github.com/nathanabrewer/hid-hop-ios) - iOS companion app

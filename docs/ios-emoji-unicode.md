# iOS integration — `CMD_KEYBOARD_UNICODE` (emoji / Unicode injection)

Firmware: `feature/device-modes` @ `db374b6` (flashed for testing).

Move per-OS Unicode entry out of the app and into firmware. The app now sends
**one command** with the code points + target OS; the dongle emits the correct
host-native keystroke sequence, holding modifiers across taps and splitting
surrogate pairs where needed.

---

## Wire format

Write to the **command characteristic** (`f8b34001-…`), same as every other HID command:

```
[0x25][length][os_mode][count][codepoint ×4 LE] × count
  │      │       │        │      └─ Unicode scalar value, uint32 LITTLE-ENDIAN
  │      │       │        └─ number of code points, 1..15
  │      │       └─ host_os_t (see below)
  │      └─ payload length = 2 + 4·count
  └─ CMD_KEYBOARD_UNICODE
```

`os_mode` (`host_os_t`):

| Value | Host | Method |
|------:|------|--------|
| `0x00` | Linux / IBus | `Ctrl+Shift+U` → hex → Space |
| `0x01` | macOS | "Unicode Hex Input" layout, Option held, 4 hex/UTF-16 unit |
| `0x02` | Windows | Alt held, Keypad-`+`, hex, release Alt |
| `0xFF` | — | use the dongle's persisted **default** host OS |

Multiple code points in one command are entered as a **single grapheme cluster**,
so ZWJ / skin-tone / flag sequences compose correctly.

### Example — 👍🏽 (`U+1F44D` + `U+1F3FD`) on macOS

```
25 0A 01 02  4D F4 01 00  FD F3 01 00
│  │  │  │   └ U+1F44D LE  └ U+1F3FD LE
│  │  │  └ count = 2
│  │  └ os_mode = macOS
│  └ length = 10  (2 + 4·2)
└ CMD_KEYBOARD_UNICODE
```

---

## Swift

```swift
enum HostOS: UInt8 { case linux = 0, macOS = 1, windows = 2, deviceDefault = 0xFF }

/// Build a CMD_KEYBOARD_UNICODE frame for an emoji/string (max 15 code points).
func makeUnicodeCommand(_ text: String, os: HostOS) -> Data {
    let cps = Array(text.unicodeScalars.prefix(15)).map { $0.value }   // UInt32 scalars
    var payload = Data([os.rawValue, UInt8(cps.count)])
    for cp in cps {
        withUnsafeBytes(of: cp.littleEndian) { payload.append(contentsOf: $0) }
    }
    return Data([0x25, UInt8(payload.count)]) + payload                // [type, length] + payload
}

// In BLEManager.sendEmoji — replace the keystroke-list builder with one write:
func sendEmoji(_ emoji: String, os: HostOS) {
    let frame = makeUnicodeCommand(emoji, os: os)
    peripheral.writeValue(frame, for: commandCharacteristic, type: .withoutResponse)
}
```

`String.unicodeScalars` already gives the code points (not UTF-16 units), so a
skin-tone emoji like `"👍🏽"` yields `[0x1F44D, 0x1F3FD]` — exactly what to send.
The firmware does the macOS surrogate-pair split internally; **do not** pre-split.

---

## Acks / status codes

Returned via the existing `status_code_t` path (no new channel):

| Code | Meaning |
|-----:|---------|
| `0x00` | OK — keystrokes sent |
| `0x02` | INVALID_LEN — malformed payload / count |
| `0x04` | USB_BUSY |
| `0x05` | USB_FAILED |
| `0x06` | INVALID_DATA — unknown `os_mode`, code point > `U+10FFFF`, or lone surrogate |

`0x00` means "keystrokes were sent," **not** "emoji rendered on screen" — the
firmware can't see the host. See preconditions.

---

## Preconditions (honored, not hidden)

Each method needs an active Unicode input method on the host:

- **Linux:** IBus/GTK Unicode entry available.
- **macOS:** **"Unicode Hex Input"** keyboard layout must be the *active* layout.
- **Windows:** WinCompose installed, or `EnableHexNumpad` registry value set.

**None of these work in a bare terminal/TTY** — there is no keystroke sequence
that injects an emoji into a raw console. This is a host limitation, by design.

---

## Auth

Gated by the existing PIN/session auth, like all HID commands. Verify PIN / start
a session first, or the dongle returns `STATUS_ERR_AUTH_REQUIRED`.

## Default host OS (`os_mode = 0xFF`)

The dongle persists a default host OS (`config_get/set_default_host_os`,
defaults to Linux). Sending `0xFF` uses it. **Note:** there is not yet a
BLE/serial setter to *change* that default from the app — ask if you want one
(e.g. a `{"cmd":"set_host_os","os":1}` serial command), otherwise pass an
explicit `os_mode` per call.

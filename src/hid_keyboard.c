/*
 * Brewer BLE HID Bridge - USB HID Keyboard Implementation
 *
 * Implements USB HID keyboard functionality using Zephyr's USB subsystem.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

#include "hid_keyboard.h"
#include "protocol.h"   /* MOD_* flags, host_os_t */

LOG_MODULE_REGISTER(hid_keyboard, LOG_LEVEL_INF);

/* USB HID Keyboard Report Descriptor */
static const uint8_t keyboard_report_desc[] = {
    /* Usage Page (Generic Desktop) */
    0x05, 0x01,
    /* Usage (Keyboard) */
    0x09, 0x06,
    /* Collection (Application) */
    0xA1, 0x01,
        /* Report ID 1 */
        0x85, 0x01,
        /* Modifier keys (Ctrl, Shift, Alt, GUI) */
        0x05, 0x07,       /* Usage Page (Key Codes) */
        0x19, 0xE0,       /* Usage Minimum (224) */
        0x29, 0xE7,       /* Usage Maximum (231) */
        0x15, 0x00,       /* Logical Minimum (0) */
        0x25, 0x01,       /* Logical Maximum (1) */
        0x75, 0x01,       /* Report Size (1) */
        0x95, 0x08,       /* Report Count (8) */
        0x81, 0x02,       /* Input (Data, Variable, Absolute) */
        /* Reserved byte */
        0x95, 0x01,       /* Report Count (1) */
        0x75, 0x08,       /* Report Size (8) */
        0x81, 0x01,       /* Input (Constant) */
        /* LEDs (output) */
        0x95, 0x05,       /* Report Count (5) */
        0x75, 0x01,       /* Report Size (1) */
        0x05, 0x08,       /* Usage Page (LEDs) */
        0x19, 0x01,       /* Usage Minimum (1) */
        0x29, 0x05,       /* Usage Maximum (5) */
        0x91, 0x02,       /* Output (Data, Variable, Absolute) */
        /* LED padding */
        0x95, 0x01,       /* Report Count (1) */
        0x75, 0x03,       /* Report Size (3) */
        0x91, 0x01,       /* Output (Constant) */
        /* Key codes (6 simultaneous keys) */
        0x95, 0x06,       /* Report Count (6) */
        0x75, 0x08,       /* Report Size (8) */
        0x15, 0x00,       /* Logical Minimum (0) */
        0x25, 0x65,       /* Logical Maximum (101) */
        0x05, 0x07,       /* Usage Page (Key Codes) */
        0x19, 0x00,       /* Usage Minimum (0) */
        0x29, 0x65,       /* Usage Maximum (101) */
        0x81, 0x00,       /* Input (Data, Array) */
    /* End Collection */
    0xC0
};

/* Keyboard HID report structure */
struct keyboard_report {
    uint8_t report_id;
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[6];
} __packed;

/* Device and state */
static const struct device *hid_dev;
static struct keyboard_report report;
static K_SEM_DEFINE(hid_sem, 1, 1);
static bool initialized = false;

/* LED state from host (NumLock, CapsLock, ScrollLock, Compose, Kana) */
static uint8_t keyboard_led_state = 0;
static bool led_state_changed = false;

/* Timing constants */
#define KEY_PRESS_DELAY_MS   10
#define KEY_RELEASE_DELAY_MS 10

/* USB HID callbacks */
static void hid_int_in_ready_cb(const struct device *dev)
{
    ARG_UNUSED(dev);
    k_sem_give(&hid_sem);
}

/**
 * Callback for SET_REPORT from host (LED state updates)
 * LED bits: 0=NumLock, 1=CapsLock, 2=ScrollLock, 3=Compose, 4=Kana
 *
 * Note: When using Report IDs, the first byte is the report ID.
 * Our keyboard uses Report ID 1, so LED data is at offset 1.
 */
static int hid_set_report_cb(const struct device *dev,
                              struct usb_setup_packet *setup,
                              int32_t *len, uint8_t **data)
{
    ARG_UNUSED(dev);

    LOG_INF("SET_REPORT cb: len=%d, wValue=0x%04x, wIndex=0x%04x",
            *len, setup->wValue, setup->wIndex);

    if (*len > 0 && *data != NULL) {
        /* Log raw data for debugging */
        LOG_HEXDUMP_INF(*data, *len, "SET_REPORT data");

        /*
         * Report format with Report ID:
         * Byte 0: Report ID (0x01)
         * Byte 1: LED state bitmask
         *
         * Without Report ID (len=1), data[0] is LED state directly.
         */
        uint8_t new_state;
        if (*len >= 2) {
            /* Has report ID prefix */
            new_state = (*data)[1];
        } else {
            /* No report ID, LED state is first byte */
            new_state = (*data)[0];
        }

        if (new_state != keyboard_led_state) {
            keyboard_led_state = new_state;
            led_state_changed = true;
            LOG_INF("LED state changed: Num=%d Caps=%d Scroll=%d",
                    (keyboard_led_state >> 0) & 0x01,
                    (keyboard_led_state >> 1) & 0x01,
                    (keyboard_led_state >> 2) & 0x01);
        }
    }

    return 0;
}

/**
 * Callback for interrupt OUT endpoint (LED state via interrupt pipe)
 * Some hosts send LED state via interrupt OUT instead of SET_REPORT.
 */
static void hid_int_out_ready_cb(const struct device *dev)
{
    uint8_t report_buf[8];
    uint32_t read_len;
    int ret;

    ret = hid_int_ep_read(dev, report_buf, sizeof(report_buf), &read_len);
    if (ret < 0) {
        LOG_ERR("INT OUT read failed: %d", ret);
        return;
    }

    LOG_INF("INT OUT report: len=%u", read_len);
    LOG_HEXDUMP_INF(report_buf, read_len, "INT OUT data");

    if (read_len > 0) {
        /*
         * Report format depends on whether Report ID is used:
         * - With Report ID: byte 0 = Report ID, byte 1 = LED state
         * - Without Report ID: byte 0 = LED state
         */
        uint8_t new_state;
        if (read_len >= 2 && report_buf[0] == 0x01) {
            /* Has report ID prefix */
            new_state = report_buf[1];
        } else {
            /* No report ID */
            new_state = report_buf[0];
        }

        if (new_state != keyboard_led_state) {
            keyboard_led_state = new_state;
            led_state_changed = true;
            LOG_INF("LED state changed (INT OUT): Num=%d Caps=%d Scroll=%d",
                    (keyboard_led_state >> 0) & 0x01,
                    (keyboard_led_state >> 1) & 0x01,
                    (keyboard_led_state >> 2) & 0x01);
        }
    }
}

/**
 * Callback for protocol change (boot protocol <-> report protocol)
 * Boot protocol (protocol=0) is simpler and guarantees LED state delivery
 * Report protocol (protocol=1) is the default with full HID capabilities
 */
static void hid_protocol_change_cb(const struct device *dev, uint8_t protocol)
{
    ARG_UNUSED(dev);
    LOG_INF("HID protocol changed to: %s (%u)",
            protocol == 0 ? "BOOT" : "REPORT", protocol);
}

static const struct hid_ops keyboard_ops = {
    .int_in_ready = hid_int_in_ready_cb,
    .int_out_ready = hid_int_out_ready_cb,
    .set_report = hid_set_report_cb,
    .protocol_change = hid_protocol_change_cb,
};

/**
 * Send the current keyboard report
 */
static bool send_report(void)
{
    int ret;

    if (!initialized) {
        return false;
    }

    k_sem_take(&hid_sem, K_FOREVER);
    ret = hid_int_ep_write(hid_dev, (uint8_t *)&report, sizeof(report), NULL);
    if (ret < 0) {
        LOG_ERR("Failed to send keyboard report: %d", ret);
        k_sem_give(&hid_sem);
        return false;
    }

    return true;
}

/**
 * Initialize USB HID keyboard
 */
bool hid_keyboard_init(void)
{
    int ret;

    hid_dev = device_get_binding("HID_0");
    if (hid_dev == NULL) {
        LOG_ERR("Cannot find HID_0 device");
        return false;
    }

    /* Register report descriptor (void in Zephyr 3.x) */
    usb_hid_register_device(hid_dev,
                            keyboard_report_desc,
                            sizeof(keyboard_report_desc),
                            &keyboard_ops);

    ret = usb_hid_init(hid_dev);
    if (ret) {
        LOG_ERR("Failed to init HID keyboard: %d", ret);
        return false;
    }

    /* Initialize report */
    memset(&report, 0, sizeof(report));
    report.report_id = 1;

    initialized = true;
    LOG_INF("HID keyboard initialized");

    return true;
}

/**
 * ASCII to HID keycode lookup table
 * Format: [keycode, needs_shift]
 */
static const uint8_t ascii_to_hid_table[128][2] = {
    /* 0x00 - 0x1F: Control characters (not mapped) */
    [0x00 ... 0x1F] = {0, 0},
    /* Space */
    [' '] = {0x2C, 0},
    /* ! */
    ['!'] = {0x1E, 1},
    /* " */
    ['"'] = {0x34, 1},
    /* # */
    ['#'] = {0x20, 1},
    /* $ */
    ['$'] = {0x21, 1},
    /* % */
    ['%'] = {0x22, 1},
    /* & */
    ['&'] = {0x24, 1},
    /* ' */
    ['\''] = {0x34, 0},
    /* ( */
    ['('] = {0x26, 1},
    /* ) */
    [')'] = {0x27, 1},
    /* * */
    ['*'] = {0x25, 1},
    /* + */
    ['+'] = {0x2E, 1},
    /* , */
    [','] = {0x36, 0},
    /* - */
    ['-'] = {0x2D, 0},
    /* . */
    ['.'] = {0x37, 0},
    /* / */
    ['/'] = {0x38, 0},
    /* 0-9 */
    ['0'] = {0x27, 0},
    ['1'] = {0x1E, 0},
    ['2'] = {0x1F, 0},
    ['3'] = {0x20, 0},
    ['4'] = {0x21, 0},
    ['5'] = {0x22, 0},
    ['6'] = {0x23, 0},
    ['7'] = {0x24, 0},
    ['8'] = {0x25, 0},
    ['9'] = {0x26, 0},
    /* : */
    [':'] = {0x33, 1},
    /* ; */
    [';'] = {0x33, 0},
    /* < */
    ['<'] = {0x36, 1},
    /* = */
    ['='] = {0x2E, 0},
    /* > */
    ['>'] = {0x37, 1},
    /* ? */
    ['?'] = {0x38, 1},
    /* @ */
    ['@'] = {0x1F, 1},
    /* A-Z (uppercase) */
    ['A'] = {0x04, 1},
    ['B'] = {0x05, 1},
    ['C'] = {0x06, 1},
    ['D'] = {0x07, 1},
    ['E'] = {0x08, 1},
    ['F'] = {0x09, 1},
    ['G'] = {0x0A, 1},
    ['H'] = {0x0B, 1},
    ['I'] = {0x0C, 1},
    ['J'] = {0x0D, 1},
    ['K'] = {0x0E, 1},
    ['L'] = {0x0F, 1},
    ['M'] = {0x10, 1},
    ['N'] = {0x11, 1},
    ['O'] = {0x12, 1},
    ['P'] = {0x13, 1},
    ['Q'] = {0x14, 1},
    ['R'] = {0x15, 1},
    ['S'] = {0x16, 1},
    ['T'] = {0x17, 1},
    ['U'] = {0x18, 1},
    ['V'] = {0x19, 1},
    ['W'] = {0x1A, 1},
    ['X'] = {0x1B, 1},
    ['Y'] = {0x1C, 1},
    ['Z'] = {0x1D, 1},
    /* [ */
    ['['] = {0x2F, 0},
    /* \ */
    ['\\'] = {0x31, 0},
    /* ] */
    [']'] = {0x30, 0},
    /* ^ */
    ['^'] = {0x23, 1},
    /* _ */
    ['_'] = {0x2D, 1},
    /* ` */
    ['`'] = {0x35, 0},
    /* a-z (lowercase) */
    ['a'] = {0x04, 0},
    ['b'] = {0x05, 0},
    ['c'] = {0x06, 0},
    ['d'] = {0x07, 0},
    ['e'] = {0x08, 0},
    ['f'] = {0x09, 0},
    ['g'] = {0x0A, 0},
    ['h'] = {0x0B, 0},
    ['i'] = {0x0C, 0},
    ['j'] = {0x0D, 0},
    ['k'] = {0x0E, 0},
    ['l'] = {0x0F, 0},
    ['m'] = {0x10, 0},
    ['n'] = {0x11, 0},
    ['o'] = {0x12, 0},
    ['p'] = {0x13, 0},
    ['q'] = {0x14, 0},
    ['r'] = {0x15, 0},
    ['s'] = {0x16, 0},
    ['t'] = {0x17, 0},
    ['u'] = {0x18, 0},
    ['v'] = {0x19, 0},
    ['w'] = {0x1A, 0},
    ['x'] = {0x1B, 0},
    ['y'] = {0x1C, 0},
    ['z'] = {0x1D, 0},
    /* { */
    ['{'] = {0x2F, 1},
    /* | */
    ['|'] = {0x31, 1},
    /* } */
    ['}'] = {0x30, 1},
    /* ~ */
    ['~'] = {0x35, 1},
    /* DEL - not mapped */
    [0x7F] = {0, 0},
};

/**
 * Convert ASCII to HID keycode
 */
bool ascii_to_hid_keycode(uint8_t ascii, uint8_t *keycode, bool *needs_shift)
{
    if (ascii >= 128) {
        return false;
    }

    uint8_t code = ascii_to_hid_table[ascii][0];
    if (code == 0 && ascii != 0) {
        /* Special case: tab, enter, etc. */
        switch (ascii) {
        case '\t':
            *keycode = 0x2B;  /* Tab */
            *needs_shift = false;
            return true;
        case '\n':
        case '\r':
            *keycode = 0x28;  /* Enter */
            *needs_shift = false;
            return true;
        default:
            return false;
        }
    }

    *keycode = code;
    *needs_shift = ascii_to_hid_table[ascii][1] != 0;
    return true;
}

/**
 * Type a string of ASCII characters
 */
bool hid_keyboard_type(const char *text, size_t length)
{
    if (!initialized || text == NULL) {
        return false;
    }

    for (size_t i = 0; i < length; i++) {
        uint8_t keycode;
        bool needs_shift;

        if (!ascii_to_hid_keycode((uint8_t)text[i], &keycode, &needs_shift)) {
            LOG_WRN("Unsupported character: 0x%02X", text[i]);
            continue;
        }

        /* Set up the report */
        memset(&report.keys, 0, sizeof(report.keys));
        report.modifiers = needs_shift ? 0x02 : 0x00;  /* Left Shift */
        report.keys[0] = keycode;

        /* Send key press */
        if (!send_report()) {
            return false;
        }
        k_sleep(K_MSEC(KEY_PRESS_DELAY_MS));

        /* Send key release */
        report.modifiers = 0;
        report.keys[0] = 0;
        if (!send_report()) {
            return false;
        }
        k_sleep(K_MSEC(KEY_RELEASE_DELAY_MS));
    }

    return true;
}

/**
 * Press a single key with modifiers
 */
bool hid_keyboard_key_press(uint8_t keycode, uint8_t modifiers)
{
    if (!initialized) {
        return false;
    }

    memset(&report.keys, 0, sizeof(report.keys));
    report.modifiers = modifiers;
    report.keys[0] = keycode;

    return send_report();
}

/**
 * Release all keys
 */
bool hid_keyboard_release_all(void)
{
    if (!initialized) {
        return false;
    }

    memset(&report.keys, 0, sizeof(report.keys));
    report.modifiers = 0;

    return send_report();
}

/**
 * Send a key combination (press all, release all)
 */
bool hid_keyboard_combo(const uint8_t *keycodes, uint8_t count, uint8_t modifiers)
{
    if (!initialized || keycodes == NULL || count == 0 || count > 6) {
        return false;
    }

    /* Set up combo */
    memset(&report.keys, 0, sizeof(report.keys));
    report.modifiers = modifiers;
    for (uint8_t i = 0; i < count; i++) {
        report.keys[i] = keycodes[i];
    }

    /* Send key press */
    if (!send_report()) {
        return false;
    }
    k_sleep(K_MSEC(KEY_PRESS_DELAY_MS));

    /* Release all */
    return hid_keyboard_release_all();
}

/**
 * Tap a key (press and release)
 */
bool hid_keyboard_tap(uint8_t keycode, uint8_t modifiers)
{
    if (!hid_keyboard_key_press(keycode, modifiers)) {
        return false;
    }
    k_sleep(K_MSEC(KEY_PRESS_DELAY_MS));
    return hid_keyboard_release_all();
}

/* ===========================================================================
 * Unicode injection (CMD_KEYBOARD_UNICODE)
 *
 * Emits the host OS's native Unicode-entry keystrokes. The hard parts the app
 * can't do well live here: holding a modifier continuously across multiple key
 * taps, and splitting supplementary-plane code points into a UTF-16 surrogate
 * pair for macOS. Best-effort: requires an active Unicode input method on the
 * host (see host_os_t docs). Will not work in a bare terminal.
 * ===========================================================================
 */

/* US-layout HID keycode for a hex nibble 0..15. */
static uint8_t hex_nibble_keycode(uint8_t nib)
{
    static const uint8_t digit_kc[10] = {
        0x27, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26 /* 0-9 */
    };
    static const uint8_t alpha_kc[6] = {
        0x04, 0x05, 0x06, 0x07, 0x08, 0x09 /* a-f */
    };
    nib &= 0x0F;
    return (nib < 10) ? digit_kc[nib] : alpha_kc[nib - 10];
}

/* Tap a key while whatever is in report.modifiers stays held down. */
static bool tap_with_held_modifiers(uint8_t keycode)
{
    report.keys[0] = keycode;
    if (!send_report()) {
        return false;
    }
    k_sleep(K_MSEC(KEY_PRESS_DELAY_MS));
    report.keys[0] = 0;
    if (!send_report()) {
        return false;
    }
    k_sleep(K_MSEC(KEY_RELEASE_DELAY_MS));
    return true;
}

/* Hex digits needed to represent cp (1..6), no leading zeros. */
static uint8_t hex_width(uint32_t cp)
{
    uint8_t w = 1;
    while (cp >> (4 * w)) {
        w++;
    }
    return w;
}

/* Linux/IBus: Ctrl+Shift+U, release, hex digits, Space to commit. */
static bool unicode_linux(uint32_t cp)
{
    if (!hid_keyboard_tap(0x18, MOD_LEFT_CTRL | MOD_LEFT_SHIFT)) {  /* U */
        return false;
    }
    k_sleep(K_MSEC(KEY_PRESS_DELAY_MS));
    for (int i = hex_width(cp) - 1; i >= 0; i--) {
        if (!hid_keyboard_tap(hex_nibble_keycode((cp >> (4 * i)) & 0xF), MOD_NONE)) {
            return false;
        }
    }
    return hid_keyboard_tap(0x2C, MOD_NONE);  /* Space commits */
}

/* macOS: one UTF-16 code unit as exactly 4 hex digits, Option already held. */
static bool unicode_macos_unit(uint16_t unit)
{
    for (int i = 3; i >= 0; i--) {
        if (!tap_with_held_modifiers(hex_nibble_keycode((unit >> (4 * i)) & 0xF))) {
            return false;
        }
    }
    return true;
}

/* macOS Unicode Hex Input: Option held throughout; supplementary-plane code
 * points are entered as a UTF-16 surrogate pair (two 4-digit groups). */
static bool unicode_macos(uint32_t cp)
{
    bool ok;
    memset(&report.keys, 0, sizeof(report.keys));
    report.modifiers = MOD_LEFT_ALT;  /* hold Option for the whole sequence */

    if (cp <= 0xFFFF) {
        ok = unicode_macos_unit((uint16_t)cp);
    } else {
        uint32_t v = cp - 0x10000;
        ok = unicode_macos_unit(0xD800 + (uint16_t)(v >> 10)) &&
             unicode_macos_unit(0xDC00 + (uint16_t)(v & 0x3FF));
    }

    report.modifiers = 0;             /* release Option */
    report.keys[0] = 0;
    return send_report() && ok;
}

/* Windows hex input / WinCompose: Alt held, Keypad-+, hex digits, release Alt. */
static bool unicode_windows(uint32_t cp)
{
    memset(&report.keys, 0, sizeof(report.keys));
    report.modifiers = MOD_LEFT_ALT;

    bool ok = tap_with_held_modifiers(0x57);  /* Keypad + */
    for (int i = hex_width(cp) - 1; ok && i >= 0; i--) {
        ok = tap_with_held_modifiers(hex_nibble_keycode((cp >> (4 * i)) & 0xF));
    }

    report.modifiers = 0;             /* release Alt commits the character */
    report.keys[0] = 0;
    return send_report() && ok;
}

bool hid_keyboard_send_unicode(uint8_t os_mode, const uint32_t *codepoints, uint8_t count)
{
    if (!initialized || codepoints == NULL || count == 0) {
        return false;
    }

    for (uint8_t i = 0; i < count; i++) {
        uint32_t cp = codepoints[i];
        bool ok;

        switch (os_mode) {
        case HOST_OS_LINUX_IBUS:  ok = unicode_linux(cp);   break;
        case HOST_OS_MACOS_HEX:   ok = unicode_macos(cp);   break;
        case HOST_OS_WINDOWS_HEX: ok = unicode_windows(cp); break;
        default:
            LOG_WRN("Unicode: unknown os_mode %u", os_mode);
            return false;
        }

        if (!ok) {
            LOG_ERR("Unicode: failed to inject U+%04X (os_mode %u)", cp, os_mode);
            return false;
        }
        /* let the host commit each code point before the next in a sequence */
        k_sleep(K_MSEC(KEY_RELEASE_DELAY_MS));
    }
    return true;
}

/**
 * Get current keyboard LED state from host
 * @return LED state bitmask:
 *         Bit 0: NumLock
 *         Bit 1: CapsLock
 *         Bit 2: ScrollLock
 *         Bit 3: Compose
 *         Bit 4: Kana
 */
uint8_t hid_keyboard_get_led_state(void)
{
    return keyboard_led_state;
}

/**
 * Check if NumLock is active
 */
bool hid_keyboard_numlock_on(void)
{
    return (keyboard_led_state & 0x01) != 0;
}

/**
 * Check if CapsLock is active
 */
bool hid_keyboard_capslock_on(void)
{
    return (keyboard_led_state & 0x02) != 0;
}

/**
 * Check if ScrollLock is active
 */
bool hid_keyboard_scrolllock_on(void)
{
    return (keyboard_led_state & 0x04) != 0;
}

/**
 * Check and clear LED state changed flag
 * @return true if LED state changed since last check
 */
bool hid_keyboard_led_state_changed(void)
{
    bool changed = led_state_changed;
    led_state_changed = false;
    return changed;
}

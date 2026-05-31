/*
 * Brewer BLE HID Bridge - USB HID Keyboard
 */

#ifndef HID_KEYBOARD_H
#define HID_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * Initialize USB HID keyboard device
 * @return true on success
 */
bool hid_keyboard_init(void);

/**
 * Type a string of ASCII characters
 * @param text ASCII text to type
 * @param length Length of text
 * @return true if all characters sent successfully
 */
bool hid_keyboard_type(const char *text, size_t length);

/**
 * Send a single key press with modifiers
 * @param keycode HID keycode
 * @param modifiers Modifier flags
 * @return true on success
 */
bool hid_keyboard_key_press(uint8_t keycode, uint8_t modifiers);

/**
 * Release all keys
 * @return true on success
 */
bool hid_keyboard_release_all(void);

/**
 * Send a key combination (press all, release all)
 * @param keycodes Array of HID keycodes
 * @param count Number of keys (max 6)
 * @param modifiers Modifier flags
 * @return true on success
 */
bool hid_keyboard_combo(const uint8_t *keycodes, uint8_t count, uint8_t modifiers);

/**
 * Tap a key (press and release)
 * @param keycode HID keycode
 * @param modifiers Modifier flags
 * @return true on success
 */
bool hid_keyboard_tap(uint8_t keycode, uint8_t modifiers);

/**
 * Inject Unicode code point(s) using the host OS's native Unicode-entry method.
 * Holds modifiers across multi-key sequences (e.g. Option held while typing the
 * hex digits on macOS) and splits supplementary-plane code points into UTF-16
 * surrogate pairs where the host method requires it (macOS).
 *
 * Best-effort and host-context dependent: requires an active Unicode input
 * method (IBus / Unicode Hex Input layout / WinCompose|EnableHexNumpad). Does
 * NOT work in a bare terminal/TTY.
 *
 * @param os_mode    host_os_t (LINUX_IBUS / MACOS_HEX / WINDOWS_HEX)
 * @param codepoints Array of Unicode scalar values
 * @param count      Number of code points (entered as one grapheme cluster)
 * @return true on success
 */
bool hid_keyboard_send_unicode(uint8_t os_mode, const uint32_t *codepoints, uint8_t count);

/**
 * Convert ASCII character to HID keycode
 * @param ascii ASCII character
 * @param keycode Output keycode
 * @param needs_shift Output: true if shift is needed
 * @return true if character is supported
 */
bool ascii_to_hid_keycode(uint8_t ascii, uint8_t *keycode, bool *needs_shift);

/**
 * Get current keyboard LED state from host PC
 * @return LED state bitmask:
 *         Bit 0: NumLock
 *         Bit 1: CapsLock
 *         Bit 2: ScrollLock
 *         Bit 3: Compose
 *         Bit 4: Kana
 */
uint8_t hid_keyboard_get_led_state(void);

/**
 * Check if NumLock is active on host
 */
bool hid_keyboard_numlock_on(void);

/**
 * Check if CapsLock is active on host
 */
bool hid_keyboard_capslock_on(void);

/**
 * Check if ScrollLock is active on host
 */
bool hid_keyboard_scrolllock_on(void);

/**
 * Check and clear LED state changed flag
 * @return true if LED state changed since last check
 */
bool hid_keyboard_led_state_changed(void);

/* LED state bit definitions */
#define HID_LED_NUMLOCK     (1 << 0)
#define HID_LED_CAPSLOCK    (1 << 1)
#define HID_LED_SCROLLLOCK  (1 << 2)
#define HID_LED_COMPOSE     (1 << 3)
#define HID_LED_KANA        (1 << 4)

#endif /* HID_KEYBOARD_H */

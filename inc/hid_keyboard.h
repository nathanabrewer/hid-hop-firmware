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
 * Convert ASCII character to HID keycode
 * @param ascii ASCII character
 * @param keycode Output keycode
 * @param needs_shift Output: true if shift is needed
 * @return true if character is supported
 */
bool ascii_to_hid_keycode(uint8_t ascii, uint8_t *keycode, bool *needs_shift);

#endif /* HID_KEYBOARD_H */

/*
 * Brewer BLE HID Bridge - USB HID Mouse
 */

#ifndef HID_MOUSE_H
#define HID_MOUSE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Initialize USB HID mouse device
 * @return true on success
 */
bool hid_mouse_init(void);

/**
 * Move mouse cursor by relative amount
 * @param dx X movement (-32768 to 32767)
 * @param dy Y movement (-32768 to 32767)
 * @return true on success
 */
bool hid_mouse_move(int16_t dx, int16_t dy);

/**
 * Press mouse button(s)
 * @param buttons Button mask (bit 0=left, 1=right, 2=middle)
 * @return true on success
 */
bool hid_mouse_button_press(uint8_t buttons);

/**
 * Release mouse button(s)
 * @param buttons Button mask
 * @return true on success
 */
bool hid_mouse_button_release(uint8_t buttons);

/**
 * Click mouse button(s) - press and release
 * @param buttons Button mask
 * @return true on success
 */
bool hid_mouse_click(uint8_t buttons);

/**
 * Scroll the mouse wheel
 * @param vertical Vertical scroll (-127 to 127, positive = up)
 * @param horizontal Horizontal scroll (-127 to 127, positive = right)
 * @return true on success
 */
bool hid_mouse_scroll(int8_t vertical, int8_t horizontal);

/**
 * Start drag operation (press and hold left button)
 * @return true on success
 */
bool hid_mouse_drag_start(void);

/**
 * End drag operation (release left button)
 * @return true on success
 */
bool hid_mouse_drag_end(void);

/**
 * Release all mouse buttons
 * @return true on success
 */
bool hid_mouse_release_all(void);

#endif /* HID_MOUSE_H */

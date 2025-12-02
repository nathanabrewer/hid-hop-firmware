/*
 * Brewer BLE HID Bridge - USB HID Consumer Control (Media Keys)
 */

#ifndef HID_CONSUMER_H
#define HID_CONSUMER_H

#include <stdbool.h>
#include <stdint.h>

/* Consumer Control Usage IDs (from USB HID Usage Tables) */
typedef enum {
    CONSUMER_PLAY_PAUSE     = 0x00CD,
    CONSUMER_NEXT_TRACK     = 0x00B5,
    CONSUMER_PREV_TRACK     = 0x00B6,
    CONSUMER_STOP           = 0x00B7,
    CONSUMER_VOLUME_UP      = 0x00E9,
    CONSUMER_VOLUME_DOWN    = 0x00EA,
    CONSUMER_MUTE           = 0x00E2,
    CONSUMER_BRIGHTNESS_UP  = 0x006F,
    CONSUMER_BRIGHTNESS_DN  = 0x0070,
    CONSUMER_EJECT          = 0x00B8,
} consumer_usage_t;

/**
 * Initialize USB HID consumer control device
 * @return true on success
 */
bool hid_consumer_init(void);

/**
 * Send a consumer control key press (and auto-release)
 * @param usage_id Consumer control usage ID (e.g., CONSUMER_PLAY_PAUSE)
 * @return true on success
 */
bool hid_consumer_send(uint16_t usage_id);

/**
 * Press a consumer control key (hold until release called)
 * @param usage_id Consumer control usage ID
 * @return true on success
 */
bool hid_consumer_press(uint16_t usage_id);

/**
 * Release all consumer control keys
 * @return true on success
 */
bool hid_consumer_release(void);

#endif /* HID_CONSUMER_H */

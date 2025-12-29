/*
 * HID-HOP Device Mode System
 *
 * Runtime-selectable operating modes for different use cases.
 * All modes share common BLE GATT config service for mode switching.
 *
 * Copyright (c) 2025 Nathan Brewer
 */

#ifndef DEVICE_MODE_H
#define DEVICE_MODE_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Device operating modes
 *
 * Each mode optimizes for different use cases:
 * - HID: USB keyboard/mouse with BLE mesh (default, app-compatible)
 * - MESH: Full mesh node with JSONL CLI, no USB HID
 * - TUNNEL: Point-to-point BLE serial bridge (no mesh, max speed)
 * - RC: BLE joystick to PWM outputs (low latency RC control)
 */
typedef enum {
    DEVICE_MODE_HID = 0,      /* Default: USB HID + BLE mesh (current app behavior) */
    DEVICE_MODE_MESH = 1,     /* Mesh node: JSONL CLI + mesh relay, no USB HID */
    DEVICE_MODE_TUNNEL = 2,   /* Serial tunnel: point-to-point BLE, ~115200 baud */
    DEVICE_MODE_RC = 3,       /* RC controller: BLE GATT -> PWM, lowest latency */
    DEVICE_MODE_COUNT
} device_mode_t;

/**
 * Mode feature flags (what each mode supports)
 */
#define MODE_FEAT_USB_HID       (1 << 0)  /* USB HID keyboard/mouse */
#define MODE_FEAT_USB_CDC       (1 << 1)  /* USB CDC serial (JSONL or raw) */
#define MODE_FEAT_BLE_MESH      (1 << 2)  /* BLE Mesh networking */
#define MODE_FEAT_BLE_GATT      (1 << 3)  /* BLE GATT services */
#define MODE_FEAT_GPIO          (1 << 4)  /* GPIO control (LEDs, buttons) */
#define MODE_FEAT_RC_PWM        (1 << 5)  /* RC PWM outputs */
#define MODE_FEAT_JSONL_CLI     (1 << 6)  /* JSONL command interface */
#define MODE_FEAT_CONFIG_GATT   (1 << 7)  /* Config GATT service (always on) */

/**
 * Mode configuration structure
 */
typedef struct {
    device_mode_t mode;
    const char *name;
    const char *description;
    uint8_t features;
} device_mode_info_t;

/**
 * Mode feature matrix
 *
 * MODE_HID:    USB HID + CDC(JSONL) + Mesh + GATT + GPIO + RC
 * MODE_MESH:   CDC(JSONL) + Mesh + GATT + GPIO + RC (no USB HID)
 * MODE_TUNNEL: CDC(raw) + GATT (point-to-point BLE, no mesh)
 * MODE_RC:     GATT + RC PWM (direct BLE->PWM, lowest latency)
 */
static const device_mode_info_t device_modes[DEVICE_MODE_COUNT] = {
    [DEVICE_MODE_HID] = {
        .mode = DEVICE_MODE_HID,
        .name = "hid",
        .description = "USB HID + BLE Mesh (default)",
        .features = MODE_FEAT_USB_HID | MODE_FEAT_USB_CDC | MODE_FEAT_BLE_MESH |
                    MODE_FEAT_BLE_GATT | MODE_FEAT_GPIO | MODE_FEAT_RC_PWM |
                    MODE_FEAT_JSONL_CLI | MODE_FEAT_CONFIG_GATT,
    },
    [DEVICE_MODE_MESH] = {
        .mode = DEVICE_MODE_MESH,
        .name = "mesh",
        .description = "Mesh node with JSONL CLI (no USB HID)",
        .features = MODE_FEAT_USB_CDC | MODE_FEAT_BLE_MESH | MODE_FEAT_BLE_GATT |
                    MODE_FEAT_GPIO | MODE_FEAT_RC_PWM | MODE_FEAT_JSONL_CLI |
                    MODE_FEAT_CONFIG_GATT,
    },
    [DEVICE_MODE_TUNNEL] = {
        .mode = DEVICE_MODE_TUNNEL,
        .name = "tunnel",
        .description = "Serial tunnel over BLE (point-to-point)",
        .features = MODE_FEAT_USB_CDC | MODE_FEAT_BLE_GATT | MODE_FEAT_CONFIG_GATT,
    },
    [DEVICE_MODE_RC] = {
        .mode = DEVICE_MODE_RC,
        .name = "rc",
        .description = "RC controller (BLE -> PWM)",
        .features = MODE_FEAT_BLE_GATT | MODE_FEAT_RC_PWM | MODE_FEAT_CONFIG_GATT,
    },
};

/**
 * Get current device mode
 * @return Current mode
 */
device_mode_t device_mode_get(void);

/**
 * Set device mode (takes effect after reboot)
 * @param mode New mode
 * @return 0 on success, negative on error
 */
int device_mode_set(device_mode_t mode);

/**
 * Get mode info by mode enum
 * @param mode Mode to query
 * @return Pointer to mode info, or NULL if invalid
 */
const device_mode_info_t *device_mode_get_info(device_mode_t mode);

/**
 * Get mode by name string
 * @param name Mode name (e.g., "hid", "tunnel")
 * @return Mode enum, or DEVICE_MODE_HID if not found
 */
device_mode_t device_mode_from_name(const char *name);

/**
 * Check if current mode has a feature
 * @param feature Feature flag (MODE_FEAT_*)
 * @return true if feature is enabled in current mode
 */
bool device_mode_has_feature(uint8_t feature);

/**
 * Initialize device mode system (call early in boot)
 * Loads mode from config storage
 */
void device_mode_init(void);

/**
 * Get mode name string
 * @param mode Mode to query
 * @return Mode name string
 */
static inline const char *device_mode_name(device_mode_t mode) {
    if (mode < DEVICE_MODE_COUNT) {
        return device_modes[mode].name;
    }
    return "unknown";
}

#endif /* DEVICE_MODE_H */

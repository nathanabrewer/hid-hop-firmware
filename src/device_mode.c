/*
 * HID-HOP Device Mode System
 *
 * Copyright (c) 2025 Nathan Brewer
 */

#include "device_mode.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>

LOG_MODULE_REGISTER(device_mode, LOG_LEVEL_INF);

/* Settings key for device mode */
#define SETTINGS_MODE_KEY "mode/current"

/* Current device mode (loaded from settings on init) */
static device_mode_t current_mode = DEVICE_MODE_HID;
static bool mode_loaded = false;

/**
 * Settings handler for device mode
 */
static int mode_settings_set(const char *name, size_t len,
                              settings_read_cb read_cb, void *cb_arg)
{
    const char *next;
    int rc;

    if (settings_name_steq(name, "current", &next) && !next) {
        if (len != sizeof(current_mode)) {
            return -EINVAL;
        }

        rc = read_cb(cb_arg, &current_mode, sizeof(current_mode));
        if (rc < 0) {
            return rc;
        }

        /* Validate mode */
        if (current_mode >= DEVICE_MODE_COUNT) {
            LOG_WRN("Invalid mode %d in settings, defaulting to HID", current_mode);
            current_mode = DEVICE_MODE_HID;
        }

        mode_loaded = true;
        LOG_INF("Loaded device mode: %s", device_mode_name(current_mode));
        return 0;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(device_mode, "mode", NULL, mode_settings_set,
                                NULL, NULL);

void device_mode_init(void)
{
    /* Mode is loaded via settings_load() in main */
    if (!mode_loaded) {
        LOG_INF("No saved mode, defaulting to HID");
        current_mode = DEVICE_MODE_HID;
    }

    LOG_INF("Device mode: %s (%s)",
            device_modes[current_mode].name,
            device_modes[current_mode].description);
}

device_mode_t device_mode_get(void)
{
    return current_mode;
}

int device_mode_set(device_mode_t mode)
{
    if (mode >= DEVICE_MODE_COUNT) {
        LOG_ERR("Invalid mode: %d", mode);
        return -EINVAL;
    }

    if (mode == current_mode) {
        LOG_INF("Mode already set to %s", device_mode_name(mode));
        return 0;
    }

    /* Save to settings */
    int err = settings_save_one(SETTINGS_MODE_KEY, &mode, sizeof(mode));
    if (err) {
        LOG_ERR("Failed to save mode: %d", err);
        return err;
    }

    LOG_INF("Mode set to %s (reboot required)", device_mode_name(mode));
    return 0;
}

const device_mode_info_t *device_mode_get_info(device_mode_t mode)
{
    if (mode >= DEVICE_MODE_COUNT) {
        return NULL;
    }
    return &device_modes[mode];
}

device_mode_t device_mode_from_name(const char *name)
{
    if (!name) {
        return DEVICE_MODE_HID;
    }

    for (int i = 0; i < DEVICE_MODE_COUNT; i++) {
        if (strcmp(device_modes[i].name, name) == 0) {
            return (device_mode_t)i;
        }
    }

    LOG_WRN("Unknown mode name: %s", name);
    return DEVICE_MODE_HID;
}

bool device_mode_has_feature(uint8_t feature)
{
    if (current_mode >= DEVICE_MODE_COUNT) {
        return false;
    }
    return (device_modes[current_mode].features & feature) != 0;
}

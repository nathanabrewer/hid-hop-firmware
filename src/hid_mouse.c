/*
 * Brewer BLE HID Bridge - USB HID Mouse Implementation
 *
 * Implements USB HID mouse functionality using Zephyr's USB subsystem.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

#include "hid_mouse.h"

LOG_MODULE_REGISTER(hid_mouse, LOG_LEVEL_DBG);

/* USB HID Mouse Report Descriptor */
static const uint8_t mouse_report_desc[] = {
    /* Usage Page (Generic Desktop) */
    0x05, 0x01,
    /* Usage (Mouse) */
    0x09, 0x02,
    /* Collection (Application) */
    0xA1, 0x01,
        /* Report ID 2 */
        0x85, 0x02,
        /* Usage (Pointer) */
        0x09, 0x01,
        /* Collection (Physical) */
        0xA1, 0x00,
            /* Usage Page (Buttons) */
            0x05, 0x09,
            /* Usage Minimum (1) */
            0x19, 0x01,
            /* Usage Maximum (3) */
            0x29, 0x03,
            /* Logical Minimum (0) */
            0x15, 0x00,
            /* Logical Maximum (1) */
            0x25, 0x01,
            /* Report Count (3) - 3 buttons */
            0x95, 0x03,
            /* Report Size (1) */
            0x75, 0x01,
            /* Input (Data, Variable, Absolute) */
            0x81, 0x02,
            /* Report Count (1) - padding */
            0x95, 0x01,
            /* Report Size (5) */
            0x75, 0x05,
            /* Input (Constant) */
            0x81, 0x01,
            /* Usage Page (Generic Desktop) */
            0x05, 0x01,
            /* Usage (X) */
            0x09, 0x30,
            /* Usage (Y) */
            0x09, 0x31,
            /* Usage (Wheel) */
            0x09, 0x38,
            /* Logical Minimum (-127) */
            0x15, 0x81,
            /* Logical Maximum (127) */
            0x25, 0x7F,
            /* Report Size (8) */
            0x75, 0x08,
            /* Report Count (3) - X, Y, Wheel */
            0x95, 0x03,
            /* Input (Data, Variable, Relative) */
            0x81, 0x06,
            /* Usage Page (Consumer) for horizontal scroll */
            0x05, 0x0C,
            /* Usage (AC Pan) */
            0x0A, 0x38, 0x02,
            /* Logical Minimum (-127) */
            0x15, 0x81,
            /* Logical Maximum (127) */
            0x25, 0x7F,
            /* Report Size (8) */
            0x75, 0x08,
            /* Report Count (1) */
            0x95, 0x01,
            /* Input (Data, Variable, Relative) */
            0x81, 0x06,
        /* End Collection (Physical) */
        0xC0,
    /* End Collection (Application) */
    0xC0
};

/* Mouse HID report structure */
struct mouse_report {
    uint8_t report_id;
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel;
    int8_t pan;  /* Horizontal scroll */
} __packed;

/* Device and state */
static const struct device *hid_dev;
static struct mouse_report report;
static uint8_t current_buttons = 0;
static K_SEM_DEFINE(hid_sem, 1, 1);
static bool initialized = false;

/* Timing constants */
#define REPORT_DELAY_MS  5
#define CLICK_DELAY_MS   20

/* Maximum movement per report (HID uses signed 8-bit) */
#define MAX_MOVEMENT 127
#define MIN_MOVEMENT -127

/* USB HID callbacks */
static void hid_int_in_ready_cb(const struct device *dev)
{
    ARG_UNUSED(dev);
    k_sem_give(&hid_sem);
}

static const struct hid_ops mouse_ops = {
    .int_in_ready = hid_int_in_ready_cb,
};

/**
 * Send the current mouse report
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
        LOG_ERR("Failed to send mouse report: %d", ret);
        k_sem_give(&hid_sem);
        return false;
    }

    return true;
}

/**
 * Initialize USB HID mouse
 */
bool hid_mouse_init(void)
{
    int ret;

    hid_dev = device_get_binding("HID_1");
    if (hid_dev == NULL) {
        LOG_ERR("Cannot find HID_1 device");
        return false;
    }

    /* Register report descriptor (void in Zephyr 3.x) */
    usb_hid_register_device(hid_dev,
                            mouse_report_desc,
                            sizeof(mouse_report_desc),
                            &mouse_ops);

    ret = usb_hid_init(hid_dev);
    if (ret) {
        LOG_ERR("Failed to init HID mouse: %d", ret);
        return false;
    }

    /* Initialize report */
    memset(&report, 0, sizeof(report));
    report.report_id = 2;

    initialized = true;
    LOG_INF("HID mouse initialized");

    return true;
}

/**
 * Clamp a value to the HID report range
 */
static int8_t clamp_movement(int16_t value)
{
    if (value > MAX_MOVEMENT) {
        return MAX_MOVEMENT;
    }
    if (value < MIN_MOVEMENT) {
        return MIN_MOVEMENT;
    }
    return (int8_t)value;
}

/**
 * Move mouse cursor by relative amount
 * Handles large movements by sending multiple reports
 */
bool hid_mouse_move(int16_t dx, int16_t dy)
{
    if (!initialized) {
        return false;
    }

    /* Handle large movements by chunking */
    while (dx != 0 || dy != 0) {
        int8_t move_x = clamp_movement(dx);
        int8_t move_y = clamp_movement(dy);

        report.buttons = current_buttons;
        report.x = move_x;
        report.y = move_y;
        report.wheel = 0;
        report.pan = 0;

        if (!send_report()) {
            return false;
        }

        dx -= move_x;
        dy -= move_y;

        if (dx != 0 || dy != 0) {
            k_sleep(K_MSEC(REPORT_DELAY_MS));
        }
    }

    /* Clear movement for next report */
    report.x = 0;
    report.y = 0;

    return true;
}

/**
 * Press mouse button(s)
 */
bool hid_mouse_button_press(uint8_t buttons)
{
    if (!initialized) {
        return false;
    }

    current_buttons |= buttons;
    report.buttons = current_buttons;
    report.x = 0;
    report.y = 0;
    report.wheel = 0;
    report.pan = 0;

    return send_report();
}

/**
 * Release mouse button(s)
 */
bool hid_mouse_button_release(uint8_t buttons)
{
    if (!initialized) {
        return false;
    }

    current_buttons &= ~buttons;
    report.buttons = current_buttons;
    report.x = 0;
    report.y = 0;
    report.wheel = 0;
    report.pan = 0;

    return send_report();
}

/**
 * Click mouse button(s) - press and release
 */
bool hid_mouse_click(uint8_t buttons)
{
    if (!hid_mouse_button_press(buttons)) {
        return false;
    }
    k_sleep(K_MSEC(CLICK_DELAY_MS));
    return hid_mouse_button_release(buttons);
}

/**
 * Scroll the mouse wheel
 */
bool hid_mouse_scroll(int8_t vertical, int8_t horizontal)
{
    if (!initialized) {
        LOG_WRN("Scroll called but not initialized");
        return false;
    }

    LOG_INF("SCROLL: v=%d, h=%d", vertical, horizontal);

    report.buttons = current_buttons;
    report.x = 0;
    report.y = 0;
    report.wheel = vertical;
    report.pan = horizontal;

    bool result = send_report();

    LOG_INF("SCROLL send_report: %s", result ? "OK" : "FAIL");

    /* Clear scroll for next report */
    report.wheel = 0;
    report.pan = 0;

    return result;
}

/**
 * Start drag operation
 */
bool hid_mouse_drag_start(void)
{
    return hid_mouse_button_press(0x01);  /* Left button */
}

/**
 * End drag operation
 */
bool hid_mouse_drag_end(void)
{
    return hid_mouse_button_release(0x01);  /* Left button */
}

/**
 * Release all mouse buttons
 */
bool hid_mouse_release_all(void)
{
    if (!initialized) {
        return false;
    }

    current_buttons = 0;
    report.buttons = 0;
    report.x = 0;
    report.y = 0;
    report.wheel = 0;
    report.pan = 0;

    return send_report();
}

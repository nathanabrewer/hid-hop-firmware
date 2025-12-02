/*
 * Brewer BLE HID Bridge - USB HID Consumer Control Implementation
 *
 * Implements USB HID consumer control (media keys) functionality.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

#include "hid_consumer.h"

LOG_MODULE_REGISTER(hid_consumer, LOG_LEVEL_INF);

/* USB HID Consumer Control Report Descriptor */
static const uint8_t consumer_report_desc[] = {
    /* Usage Page (Consumer Devices) */
    0x05, 0x0C,
    /* Usage (Consumer Control) */
    0x09, 0x01,
    /* Collection (Application) */
    0xA1, 0x01,
        /* Report ID 3 */
        0x85, 0x03,
        /* Logical Minimum (0) */
        0x15, 0x00,
        /* Logical Maximum (0x03FF) - 10 bits */
        0x26, 0xFF, 0x03,
        /* Usage Minimum (0) */
        0x19, 0x00,
        /* Usage Maximum (0x03FF) */
        0x2A, 0xFF, 0x03,
        /* Report Size (16) - using 16-bit for usage ID */
        0x75, 0x10,
        /* Report Count (1) */
        0x95, 0x01,
        /* Input (Data, Array, Absolute) */
        0x81, 0x00,
    /* End Collection */
    0xC0
};

/* Consumer Control HID report structure */
struct consumer_report {
    uint8_t report_id;
    uint16_t usage_id;  /* 16-bit usage ID */
} __packed;

/* Device and state */
static const struct device *hid_dev;
static struct consumer_report report;
static K_SEM_DEFINE(hid_sem, 1, 1);
static bool initialized = false;

/* Timing constants */
#define KEY_PRESS_DELAY_MS   50

/* USB HID callbacks */
static void hid_int_in_ready_cb(const struct device *dev)
{
    ARG_UNUSED(dev);
    k_sem_give(&hid_sem);
}

static const struct hid_ops consumer_ops = {
    .int_in_ready = hid_int_in_ready_cb,
};

/**
 * Send the current consumer report
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
        LOG_ERR("Failed to send consumer report: %d", ret);
        k_sem_give(&hid_sem);
        return false;
    }

    return true;
}

/**
 * Initialize USB HID consumer control
 */
bool hid_consumer_init(void)
{
    int ret;

    hid_dev = device_get_binding("HID_2");
    if (hid_dev == NULL) {
        LOG_ERR("Cannot find HID_2 device");
        return false;
    }

    /* Register report descriptor */
    usb_hid_register_device(hid_dev,
                            consumer_report_desc,
                            sizeof(consumer_report_desc),
                            &consumer_ops);

    ret = usb_hid_init(hid_dev);
    if (ret) {
        LOG_ERR("Failed to init HID consumer: %d", ret);
        return false;
    }

    /* Initialize report */
    memset(&report, 0, sizeof(report));
    report.report_id = 3;

    initialized = true;
    LOG_INF("HID consumer control initialized");

    return true;
}

/**
 * Send a consumer control key (press and release)
 */
bool hid_consumer_send(uint16_t usage_id)
{
    if (!initialized) {
        return false;
    }

    /* Send key press */
    report.usage_id = usage_id;
    if (!send_report()) {
        return false;
    }

    k_sleep(K_MSEC(KEY_PRESS_DELAY_MS));

    /* Send key release */
    report.usage_id = 0;
    return send_report();
}

/**
 * Press a consumer control key (hold)
 */
bool hid_consumer_press(uint16_t usage_id)
{
    if (!initialized) {
        return false;
    }

    report.usage_id = usage_id;
    return send_report();
}

/**
 * Release all consumer control keys
 */
bool hid_consumer_release(void)
{
    if (!initialized) {
        return false;
    }

    report.usage_id = 0;
    return send_report();
}

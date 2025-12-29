/*
 * Brewer BLE HID Bridge - Main Entry Point
 *
 * This firmware bridges BLE commands from an iOS app to USB HID
 * keyboard/mouse events on a connected PC.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>

/*
 * Debug boot LED blinks - helps identify which init stage crashes.
 * DISABLED by default (adds ~10+ seconds to boot time).
 * To enable: add CONFIG_DEBUG_BOOT_BLINKS=y to prj.conf
 */
#ifdef CONFIG_DEBUG_BOOT_BLINKS
#define P0_OUTSET    (*(volatile uint32_t *)0x50000508)
#define P0_OUTCLR    (*(volatile uint32_t *)0x5000050C)
#define P0_DIRSET    (*(volatile uint32_t *)0x50000518)
#define P0_PIN_CNF(n) (*(volatile uint32_t *)(0x50000700 + (n)*4))
#define DBG_LED1 6
#define DBG_LED2 8

static void dbg_delay(void) {
    for (volatile int i = 0; i < 800000; i++) { __asm__("nop"); }
}

static void dbg_blink(int count) {
    for (int j = 0; j < count; j++) {
        P0_OUTCLR = (1 << DBG_LED1) | (1 << DBG_LED2);
        dbg_delay();
        P0_OUTSET = (1 << DBG_LED1) | (1 << DBG_LED2);
        dbg_delay();
    }
    /* Pause between blink sets */
    dbg_delay(); dbg_delay();
}

static int dbg_pre_kernel_1(void) {
    P0_PIN_CNF(DBG_LED1) = 3;
    P0_PIN_CNF(DBG_LED2) = 3;
    P0_DIRSET = (1 << DBG_LED1) | (1 << DBG_LED2);
    dbg_blink(1);  /* 1 blink = PRE_KERNEL_1 */
    return 0;
}
SYS_INIT(dbg_pre_kernel_1, PRE_KERNEL_1, 0);

static int dbg_pre_kernel_2(void) {
    dbg_blink(2);  /* 2 blinks = PRE_KERNEL_2 */
    return 0;
}
SYS_INIT(dbg_pre_kernel_2, PRE_KERNEL_2, 99);

static int dbg_post_kernel(void) {
    dbg_blink(3);  /* 3 blinks = POST_KERNEL */
    return 0;
}
SYS_INIT(dbg_post_kernel, POST_KERNEL, 99);

static int dbg_application(void) {
    dbg_blink(4);  /* 4 blinks = APPLICATION */
    return 0;
}
SYS_INIT(dbg_application, APPLICATION, 99);
#endif /* CONFIG_DEBUG_BOOT_BLINKS */
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>
#include <dk_buttons_and_leds.h>

#include "ble_hid_service.h"
#include "hid_keyboard.h"
#include "hid_mouse.h"
#include "hid_consumer.h"
#include "protocol.h"
#include "security.h"
#include "config.h"
#include "gpio_control.h"
#include "jsonl_serial.h"
#include "mesh_hid.h"
#include "device_mode.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* LED definitions */
#define STATUS_LED          DK_LED1  /* General status */
#define BLE_CONNECTED_LED   DK_LED2  /* BLE connection active */
#define USB_ACTIVE_LED      DK_LED3  /* USB HID active */
#define ERROR_LED           DK_LED4  /* Error indicator */

/* Status LED blink intervals */
#define STATUS_BLINK_IDLE_MS     1000
#define STATUS_BLINK_ACTIVE_MS   200

/* Forward declarations */
static void ble_command_handler(const uint8_t *data, size_t length);
static void button_handler(uint32_t button_state, uint32_t has_changed);
static void serial_to_ble_callback(const char *json_line, size_t len);
static void mesh_msg_handler(uint16_t src_addr, mesh_msg_type_t type,
                             const uint8_t *payload, size_t len);

/* Global state */
static bool usb_enabled = false;
static uint32_t uptime_seconds = 0;

/**
 * Get device uptime in seconds
 */
uint32_t get_uptime_seconds(void)
{
    return uptime_seconds;
}

/**
 * Check if USB is ready
 */
bool is_usb_ready(void)
{
    return usb_enabled;
}

/**
 * Initialize GPIO and LEDs
 * Note: Button init failure is non-fatal (dongle may not have buttons)
 */
static int init_gpio(void)
{
    int err;

    err = dk_leds_init();
    if (err) {
        LOG_ERR("Failed to initialize LEDs: %d", err);
        return err;
    }

    err = dk_buttons_init(button_handler);
    if (err) {
        /* Non-fatal - dongle may not have GPIO buttons */
        LOG_WRN("Button init failed: %d (continuing without buttons)", err);
    }

    /* Initial LED state - all off */
    dk_set_leds(0);

    return 0;
}

/**
 * Initialize USB HID subsystem
 */
static int init_usb(void)
{
    int err;

    /* Initialize keyboard HID device */
    if (!hid_keyboard_init()) {
        LOG_ERR("Failed to initialize HID keyboard");
        return -1;
    }

    /* Initialize mouse HID device */
    if (!hid_mouse_init()) {
        LOG_ERR("Failed to initialize HID mouse");
        return -1;
    }

    /* Initialize consumer control HID device (media keys) */
    if (!hid_consumer_init()) {
        LOG_ERR("Failed to initialize HID consumer control");
        return -1;
    }

    /* Enable USB device */
    err = usb_enable(NULL);
    if (err) {
        LOG_ERR("Failed to enable USB: %d", err);
        return err;
    }

    /* Wait for USB to be ready */
    k_sleep(K_MSEC(1000));

    usb_enabled = true;
    dk_set_led_on(USB_ACTIVE_LED);
    LOG_INF("USB HID initialized");

    return 0;
}

/**
 * Handle button presses
 * Button 1: Manual pairing mode / disconnect
 * Button 2: Reserved for future use
 */
static void button_handler(uint32_t button_state, uint32_t has_changed)
{
    if (has_changed & DK_BTN1_MSK) {
        if (button_state & DK_BTN1_MSK) {
            LOG_INF("Button 1 pressed");
            /* Could trigger pairing mode or disconnect */
            if (ble_hid_service_is_connected()) {
                LOG_INF("Disconnecting BLE...");
                ble_hid_service_disconnect();
            }
        }
    }

    if (has_changed & DK_BTN2_MSK) {
        if (button_state & DK_BTN2_MSK) {
            LOG_INF("Button 2 pressed");
            /* Reserved for future use */
        }
    }
}

/**
 * Callback for routing serial messages to BLE phone
 */
static void serial_to_ble_callback(const char *json_line, size_t len)
{
    LOG_DBG("Serial->BLE: %zu bytes", len);
    ble_hid_service_send_jsonl(json_line, len);
}

/**
 * Handle incoming mesh messages and forward to serial
 */
static void mesh_msg_handler(uint16_t src_addr, mesh_msg_type_t type,
                             const uint8_t *payload, size_t len)
{
    LOG_INF("Mesh msg from 0x%04x, type=%d, len=%zu", src_addr, type, len);

    switch (type) {
    case MESH_MSG_DISCOVERY_RESP: {
        /* Discovery response - node announced itself */
        if (len >= sizeof(mesh_discovery_t)) {
            const mesh_discovery_t *disc = (const mesh_discovery_t *)payload;
            char extra[128];
            snprintf(extra, sizeof(extra),
                "\"name\":\"%.*s\",\"caps\":\"0x%02x\"",
                disc->name_len, disc->name, disc->capabilities);
            jsonl_serial_send_mesh_event(src_addr, "mesh_discovered", extra);
        }
        break;
    }
    case MESH_MSG_HID_CMD:
        /* HID command from another node - would execute locally */
        jsonl_serial_send_mesh_event(src_addr, "mesh_hid_cmd", NULL);
        break;

    case MESH_MSG_STATUS:
        jsonl_serial_send_mesh_event(src_addr, "mesh_status_rcvd", NULL);
        break;

    default:
        LOG_DBG("Unhandled mesh msg type: %d", type);
        break;
    }
}

/**
 * Handle incoming BLE commands
 */
static void ble_command_handler(const uint8_t *data, size_t length)
{
    uint8_t response[64];
    size_t response_len;
    status_code_t status;

    LOG_HEXDUMP_DBG(data, length, "Received command");

    /* Validate minimum command size */
    if (length < sizeof(cmd_header_t)) {
        LOG_WRN("Command too short: %zu bytes", length);
        response_len = protocol_build_status(response, STATUS_ERR_INVALID_LEN, 0);
        ble_hid_service_send_response(response, response_len);
        return;
    }

    const cmd_header_t *header = (const cmd_header_t *)data;
    const uint8_t *payload = data + sizeof(cmd_header_t);

    /* Handle commands that need special responses */
    switch (header->type) {
    case CMD_PING:
        /* Send pong response */
        if (length >= sizeof(cmd_header_t) + sizeof(cmd_ping_t)) {
            response_len = protocol_build_pong(response, (const cmd_ping_t *)payload);
            ble_hid_service_send_response(response, response_len);
        }
        return;

    case CMD_GET_INFO:
        /* Send info response */
        response_len = protocol_build_info(response);
        ble_hid_service_send_response(response, response_len);
        return;

    case CMD_VERIFY_PIN:
        /* Handle PIN verification */
        LOG_INF(">>> VERIFY_PIN received, length=%zu", length);
        if (length >= sizeof(cmd_header_t) + 1) {
            const cmd_verify_pin_t *pin_cmd = (const cmd_verify_pin_t *)payload;
            LOG_INF("    PIN length from cmd: %u", pin_cmd->length);
            uint8_t attempts_left = 0;
            bool success = config_verify_pin((const char *)pin_cmd->pin, pin_cmd->length, &attempts_left);
            LOG_INF("    PIN verify result: success=%d, attempts_left=%u", success, attempts_left);
            if (success) {
                /* Start authenticated session on successful PIN */
                security_start_session();
                LOG_INF("PIN verified - session started");
            }
            response_len = protocol_build_pin_result(response, success, attempts_left);
            LOG_INF("    Sending PIN result: %zu bytes", response_len);
            bool sent = ble_hid_service_send_response(response, response_len);
            LOG_INF("    Response sent: %d", sent);
        } else {
            LOG_WRN("    VERIFY_PIN too short!");
        }
        return;

    case CMD_GET_NAME:
        /* Send name response */
        response_len = protocol_build_name_response(response);
        ble_hid_service_send_response(response, response_len);
        return;

    case CMD_GPIO_GET_LED:
    case CMD_GPIO_GET_RELAY:
    case CMD_GPIO_READ_DIN:
    case CMD_GPIO_READ_AIN:
    case CMD_GPIO_GET_ALL:
        /* Send GPIO state response */
        response_len = protocol_build_gpio_state(response);
        ble_hid_service_send_response(response, response_len);
        return;

    default:
        break;
    }

    /* Process the command */
    status = protocol_process_command(data, length);

    /* Build and send response */
    response_len = protocol_build_status(response, status, data[0]);
    ble_hid_service_send_response(response, response_len);

    /* Refresh session on successful command */
    if (status == STATUS_OK) {
        security_refresh_session();
    }
}

/**
 * Status LED update task
 */
static void status_led_task(void)
{
    static bool led_state = false;
    static int64_t last_toggle = 0;
    int64_t now = k_uptime_get();
    int blink_interval;

    /* Determine blink rate based on state */
    if (ble_hid_service_is_connected() && security_session_active()) {
        blink_interval = STATUS_BLINK_ACTIVE_MS;
    } else {
        blink_interval = STATUS_BLINK_IDLE_MS;
    }

    if (now - last_toggle >= blink_interval) {
        led_state = !led_state;
        if (led_state) {
            dk_set_led_on(STATUS_LED);
        } else {
            dk_set_led_off(STATUS_LED);
        }
        last_toggle = now;
    }
}

/**
 * Main application entry point
 */
int main(void)
{
    int err;

#ifdef CONFIG_DEBUG_BOOT_BLINKS
    /* Debug: 5 blinks = reached main() */
    dbg_blink(5);
#endif

    LOG_INF("=== HID-HOP starting ===");
    LOG_INF("Protocol version: %d.%d", PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR);

    /* Initialize GPIO/LEDs first */
    LOG_INF("Step 1: init_gpio...");
    err = init_gpio();
    if (err) {
        LOG_ERR("GPIO init failed");
        /* Can't use dk_set_led_on if init failed */
        return err;
    }
    LOG_INF("Step 1: init_gpio DONE");

    /* Flash LED 1x = passed GPIO init */
    dk_set_led_on(STATUS_LED);
    k_sleep(K_MSEC(200));
    dk_set_led_off(STATUS_LED);
    k_sleep(K_MSEC(200));

    /* Initialize configuration storage */
    LOG_INF("Step 2: config_init...");
    if (!config_init()) {
        LOG_ERR("Config init failed");
        dk_set_led_on(ERROR_LED);
        return -1;
    }
    LOG_INF("Step 2: config_init DONE");

    /* Initialize device mode system (loads saved mode from settings) */
    device_mode_init();
    device_mode_t mode = device_mode_get();
    const device_mode_info_t *mode_info = device_mode_get_info(mode);
    LOG_INF("=== Device Mode: %s ===", mode_info->description);

    /* Flash LED 2x = passed config init */
    for (int i = 0; i < 2; i++) {
        dk_set_led_on(STATUS_LED);
        k_sleep(K_MSEC(200));
        dk_set_led_off(STATUS_LED);
        k_sleep(K_MSEC(200));
    }

    /* Initialize security module (needed for all modes with BLE) */
    LOG_INF("Step 3: security_init...");
    if (!security_init()) {
        LOG_ERR("Security init failed");
        dk_set_led_on(ERROR_LED);
        return -1;
    }
    LOG_INF("Step 3: security_init DONE");

    /* Flash LED 3x = passed security init */
    for (int i = 0; i < 3; i++) {
        dk_set_led_on(STATUS_LED);
        k_sleep(K_MSEC(200));
        dk_set_led_off(STATUS_LED);
        k_sleep(K_MSEC(200));
    }

    /* Initialize GPIO control module (for modes with GPIO or RC PWM) */
    if (device_mode_has_feature(MODE_FEAT_GPIO) || device_mode_has_feature(MODE_FEAT_RC_PWM)) {
        LOG_INF("Step 3b: gpio_control_init...");
        if (!gpio_control_init()) {
            LOG_WRN("GPIO control init failed - continuing without GPIO");
        } else {
            LOG_INF("Step 3b: gpio_control_init DONE");
        }
    }

    /* Initialize USB HID devices (only for modes that support USB HID) */
    if (device_mode_has_feature(MODE_FEAT_USB_HID)) {
        LOG_INF("Step 4: init_usb (HID)...");
        err = init_usb();
        if (err) {
            LOG_ERR("USB init failed");
            dk_set_led_on(ERROR_LED);
            return err;
        }
        LOG_INF("Step 4: init_usb DONE");
    } else if (device_mode_has_feature(MODE_FEAT_USB_CDC)) {
        /* USB CDC only (no HID) - still need to enable USB */
        LOG_INF("Step 4: init_usb (CDC only)...");
        err = usb_enable(NULL);
        if (err) {
            LOG_ERR("Failed to enable USB: %d", err);
            return err;
        }
        k_sleep(K_MSEC(500));
        usb_enabled = true;
        LOG_INF("Step 4: USB CDC enabled");
    } else {
        LOG_INF("Step 4: Skipping USB init (not needed for mode)");
    }

    /* Flash LED 4x = passed USB init */
    for (int i = 0; i < 4; i++) {
        dk_set_led_on(STATUS_LED);
        k_sleep(K_MSEC(200));
        dk_set_led_off(STATUS_LED);
        k_sleep(K_MSEC(200));
    }

    /* Initialize JSONL serial interface (only for modes with JSONL CLI) */
    if (device_mode_has_feature(MODE_FEAT_JSONL_CLI)) {
        LOG_INF("Step 5: jsonl_serial_init...");
        err = jsonl_serial_init();
        if (err) {
            LOG_WRN("JSONL serial init failed: %d (continuing without serial)", err);
        } else {
            /* Set callback for routing serial->BLE messages */
            jsonl_serial_set_ble_callback(serial_to_ble_callback);
            LOG_INF("Step 5: jsonl_serial_init DONE");
        }
    } else {
        LOG_INF("Step 5: Skipping JSONL serial (not needed for mode)");
    }

    /* Initialize BLE GATT services (needed for all modes) */
    if (device_mode_has_feature(MODE_FEAT_BLE_GATT)) {
        LOG_INF("Step 6: ble_hid_service_init...");
        if (!ble_hid_service_init(ble_command_handler)) {
            LOG_ERR("BLE init failed");
            dk_set_led_on(ERROR_LED);
            return -1;
        }
        LOG_INF("Step 6: ble_hid_service_init DONE");
    }

    /* Initialize BLE Mesh (only for modes with mesh support) */
    if (device_mode_has_feature(MODE_FEAT_BLE_MESH)) {
        LOG_INF("Step 7: mesh_hid_init...");
        err = mesh_hid_init();
        if (err) {
            LOG_ERR("Mesh init failed: %d", err);
            /* Non-fatal - continue without mesh */
        } else {
            LOG_INF("Step 7: mesh_hid_init DONE");

            /* Register mesh message callback */
            mesh_hid_set_callback(mesh_msg_handler);

            /* Self-provision as founder if not already provisioned */
            if (!mesh_hid_is_provisioned()) {
                LOG_INF("Not provisioned - self-provisioning as founder...");
                err = mesh_hid_self_provision();
                if (err && err != -EALREADY) {
                    LOG_WRN("Self-provision failed: %d", err);
                }
            } else {
                LOG_INF("Already provisioned, addr=0x%04x", mesh_hid_get_addr());
                /* Ensure app key is bound (may not be restored from NVS) */
                mesh_hid_ensure_app_key();
            }

            /* Sync device name to mesh node name for discovery */
            char device_name[MAX_DEVICE_NAME_LENGTH + 1];
            if (config_get_name(device_name) > 0) {
                mesh_hid_set_name(device_name);
                LOG_INF("Mesh node name set to: %s", device_name);
            }
        }
    } else {
        LOG_INF("Step 7: Skipping mesh init (not needed for mode)");
    }

    LOG_INF("=== Initialization complete - entering main loop ===");

    /* Main loop - just update status and track uptime */
    int64_t last_uptime_update = k_uptime_get();

    while (true) {
        /* Update status LED */
        status_led_task();

        /* Update BLE connected LED */
        if (ble_hid_service_is_connected()) {
            dk_set_led_on(BLE_CONNECTED_LED);
        } else {
            dk_set_led_off(BLE_CONNECTED_LED);
        }

        /* Track uptime */
        int64_t now = k_uptime_get();
        if (now - last_uptime_update >= 1000) {
            uptime_seconds++;
            last_uptime_update = now;
        }

        /* Sleep to avoid busy loop */
        k_sleep(K_MSEC(50));
    }

    return 0;
}

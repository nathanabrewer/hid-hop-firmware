/*
 * JSONL Serial Interface for HID-HOP
 *
 * Copyright (c) 2025 Nathan Brewer
 */

#include "jsonl_serial.h"
#include "mesh_hid.h"
#include "gpio_control.h"
#include "device_mode.h"
#include "config.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(jsonl_serial, LOG_LEVEL_INF);

/* CDC ACM device */
static const struct device *cdc_dev;

/* Ring buffers for RX/TX */
#define RX_RING_BUF_SIZE 512
#define TX_RING_BUF_SIZE 512

static uint8_t rx_ring_buf_data[RX_RING_BUF_SIZE];
static uint8_t tx_ring_buf_data[TX_RING_BUF_SIZE];
static struct ring_buf rx_ring_buf;
static struct ring_buf tx_ring_buf;

/* Line buffer for parsing */
static char line_buf[JSONL_MAX_LINE_LEN];
static size_t line_pos = 0;

/* State */
static bool initialized = false;
static bool enabled = true;
static bool host_connected = false;

/* Callback for BLE routing */
static jsonl_to_ble_callback_t ble_callback = NULL;

/* Work queue for processing */
static struct k_work serial_work;

/* Forward declarations */
static void uart_irq_handler(const struct device *dev, void *user_data);
static void serial_work_handler(struct k_work *work);
static void process_line(const char *line, size_t len);
static void handle_local_command(const char *line, size_t len);
static bool send_raw(const char *data, size_t len);

/**
 * Initialize the JSONL serial interface
 */
int jsonl_serial_init(void)
{
    /* Get CDC ACM device */
    cdc_dev = DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);
    if (!device_is_ready(cdc_dev)) {
        LOG_ERR("CDC ACM device not ready");
        return -ENODEV;
    }

    /* Initialize ring buffers */
    ring_buf_init(&rx_ring_buf, sizeof(rx_ring_buf_data), rx_ring_buf_data);
    ring_buf_init(&tx_ring_buf, sizeof(tx_ring_buf_data), tx_ring_buf_data);

    /* Initialize work queue item */
    k_work_init(&serial_work, serial_work_handler);

    /* Set up UART interrupt */
    uart_irq_callback_set(cdc_dev, uart_irq_handler);
    uart_irq_rx_enable(cdc_dev);

    initialized = true;
    LOG_INF("JSONL serial initialized");

    return 0;
}

/**
 * Register callback for messages destined for BLE/phone
 */
void jsonl_serial_set_ble_callback(jsonl_to_ble_callback_t callback)
{
    ble_callback = callback;
}

/**
 * UART IRQ handler
 */
static void uart_irq_handler(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
        /* Handle RX */
        if (uart_irq_rx_ready(dev)) {
            uint8_t buf[64];
            int len = uart_fifo_read(dev, buf, sizeof(buf));
            if (len > 0) {
                ring_buf_put(&rx_ring_buf, buf, len);
                /* Schedule processing */
                k_work_submit(&serial_work);
            }
        }

        /* Handle TX */
        if (uart_irq_tx_ready(dev)) {
            uint8_t buf[64];
            uint32_t len = ring_buf_get(&tx_ring_buf, buf, sizeof(buf));
            if (len > 0) {
                uart_fifo_fill(dev, buf, len);
            } else {
                uart_irq_tx_disable(dev);
            }
        }
    }
}

/**
 * Work handler for processing received data
 */
static void serial_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    jsonl_serial_process();
}

/**
 * Process pending serial data
 */
void jsonl_serial_process(void)
{
    uint8_t c;

    if (!initialized || !enabled) {
        return;
    }

    /* Check DTR for host connection status */
    uint32_t dtr = 0;
    uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_DTR, &dtr);
    if (dtr && !host_connected) {
        host_connected = true;
        LOG_INF("Serial host connected");
        /* Send welcome message */
        jsonl_serial_send_event("connected", "\"device\":\"HID-HOP\"");
    } else if (!dtr && host_connected) {
        host_connected = false;
        LOG_INF("Serial host disconnected");
    }

    /* Process received bytes */
    while (ring_buf_get(&rx_ring_buf, &c, 1) == 1) {
        if (c == '\n' || c == '\r') {
            if (line_pos > 0) {
                line_buf[line_pos] = '\0';
                process_line(line_buf, line_pos);
                line_pos = 0;
            }
        } else if (line_pos < JSONL_MAX_LINE_LEN - 1) {
            line_buf[line_pos++] = c;
        } else {
            /* Line too long, discard */
            LOG_WRN("JSONL line too long, discarding");
            line_pos = 0;
        }
    }
}

/**
 * Process a complete JSONL line
 */
static void process_line(const char *line, size_t len)
{
    LOG_DBG("RX: %s", line);

    /* Quick validation - must start with { */
    if (len < 2 || line[0] != '{') {
        jsonl_serial_send_error("Invalid JSON", line);
        return;
    }

    /* Check for "to" field to determine routing */
    const char *to_phone = strstr(line, "\"to\":\"phone\"");
    const char *to_local = strstr(line, "\"to\":\"local\"");

    if (to_phone) {
        /* Route to BLE/phone */
        if (ble_callback) {
            ble_callback(line, len);
        } else {
            jsonl_serial_send_error("No BLE connection", NULL);
        }
    } else if (to_local || !strstr(line, "\"to\":")) {
        /* Handle locally (default if no "to" field) */
        handle_local_command(line, len);
    } else {
        /* Unknown target */
        jsonl_serial_send_error("Unknown target", line);
    }
}

/**
 * Handle a command destined for this device
 */
static void handle_local_command(const char *line, size_t len)
{
    /* Parse command type */
    if (strstr(line, "\"cmd\":\"ping\"")) {
        char data[64];
        snprintf(data, sizeof(data), "\"uptime_ms\":%lld", k_uptime_get());
        jsonl_serial_send_event("pong", data);
    }
    else if (strstr(line, "\"cmd\":\"info\"") || strstr(line, "\"cmd\":\"version\"")) {
        char data[192];
        snprintf(data, sizeof(data),
            "\"device\":\"HID-HOP\",\"version\":\"%s\",\"git\":\"%s\",\"build\":\"%s\",\"serial_enabled\":%s",
            APP_VERSION, APP_GIT_HASH, APP_BUILD_TIME,
            enabled ? "true" : "false");
        jsonl_serial_send_event("info", data);
    }
    else if (strstr(line, "\"cmd\":\"status\"")) {
        char data[128];
        snprintf(data, sizeof(data),
            "\"ble_connected\":%s,\"serial_connected\":%s",
            "false", /* TODO: get actual BLE status */
            host_connected ? "true" : "false");
        jsonl_serial_send_event("status", data);
    }
    else if (strstr(line, "\"cmd\":\"mesh_status\"")) {
        mesh_status_t status;
        mesh_hid_get_status(&status);
        char data[256];
        snprintf(data, sizeof(data),
            "\"provisioned\":%s,\"addr\":\"0x%04x\",\"founder\":%s,\"relay\":%s,\"app_key_bound\":%s,\"peer_count\":%d,\"name\":\"%s\"",
            status.provisioned ? "true" : "false",
            status.addr,
            mesh_hid_is_founder() ? "true" : "false",
            status.relay_enabled ? "true" : "false",
            mesh_hid_app_key_bound() ? "true" : "false",
            status.peer_count,
            mesh_hid_get_name());
        jsonl_serial_send_event("mesh_status", data);
    }
    else if (strstr(line, "\"cmd\":\"mesh_discover\"")) {
        int err = mesh_hid_send_discovery();
        if (err) {
            char data[64];
            snprintf(data, sizeof(data), "\"error\":%d", err);
            jsonl_serial_send_event("mesh_discover", data);
        } else {
            jsonl_serial_send_event("mesh_discover", "\"sent\":true");
        }
    }
    else if (strstr(line, "\"cmd\":\"beacon\"")) {
        int err = mesh_hid_send_beacon();
        char data[128];
        if (err) {
            snprintf(data, sizeof(data),
                     "\"error\":%d,\"app_key_bound\":%s,\"provisioned\":%s",
                     err,
                     mesh_hid_app_key_bound() ? "true" : "false",
                     mesh_hid_is_provisioned() ? "true" : "false");
            jsonl_serial_send_event("beacon", data);
        } else {
            snprintf(data, sizeof(data),
                     "\"sent\":true,\"app_key_bound\":%s",
                     mesh_hid_app_key_bound() ? "true" : "false");
            jsonl_serial_send_event("beacon", data);
        }
    }
    else if (strstr(line, "\"cmd\":\"mesh_reset\"")) {
        mesh_hid_reset();
        jsonl_serial_send_event("mesh_reset", "\"done\":true,\"reboot_required\":true");
    }
    else if (strstr(line, "\"cmd\":\"reboot\"")) {
        jsonl_serial_send_event("reboot", "\"rebooting\":true");
        k_msleep(100);  /* Let the message send */
        sys_reboot(SYS_REBOOT_COLD);
    }
    else if (strstr(line, "\"cmd\":\"mesh_ping\"")) {
        /* Parse target address: {"cmd":"mesh_ping","addr":"0x1c04"} */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        if (addr_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            int err = mesh_hid_send_ping(addr);
            char data[64];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"err\":%d", addr, err);
            jsonl_serial_send_event("mesh_ping", data);
        } else {
            jsonl_serial_send_error("missing addr", "mesh_ping");
        }
    }
    else if (strstr(line, "\"cmd\":\"mesh_text\"")) {
        /* Parse: {"cmd":"mesh_text","addr":"0x1c04","text":"hello"} */
        /* addr is optional - omit for broadcast */
        uint16_t addr = 0xFFFF;  /* Default: broadcast */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        if (addr_str) {
            addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
        }
        const char *text_str = strstr(line, "\"text\":\"");
        if (text_str) {
            text_str += 8;  /* Skip past "text":" */
            char text[64];
            int i = 0;
            while (text_str[i] && text_str[i] != '"' && i < 63) {
                text[i] = text_str[i];
                i++;
            }
            text[i] = '\0';
            int err = mesh_hid_send_text(addr, text);
            char data[96];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"text\":\"%s\",\"err\":%d", addr, text, err);
            jsonl_serial_send_event("mesh_text", data);
        } else {
            jsonl_serial_send_error("missing text", "mesh_text");
        }
    }
    else if (strstr(line, "\"cmd\":\"hid_type\"")) {
        /* {"cmd":"hid_type","addr":"0x1c04","text":"hello"} */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        const char *text_str = strstr(line, "\"text\":\"");
        if (addr_str && text_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            text_str += 8;
            char text[64];
            int i = 0;
            while (text_str[i] && text_str[i] != '"' && i < 58) {
                text[i] = text_str[i];
                i++;
            }
            text[i] = '\0';
            int err = mesh_hid_send_keyboard_type(addr, text);
            char data[96];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"text\":\"%s\",\"err\":%d", addr, text, err);
            jsonl_serial_send_event("hid_type", data);
        } else {
            jsonl_serial_send_error("missing addr or text", "hid_type");
        }
    }
    else if (strstr(line, "\"cmd\":\"hid_click\"")) {
        /* {"cmd":"hid_click","addr":"0x1c04","btn":1} - 1=left, 2=right, 4=middle */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        const char *btn_str = strstr(line, "\"btn\":");
        if (addr_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            uint8_t btn = btn_str ? (uint8_t)strtol(btn_str + 6, NULL, 10) : 1;
            int err = mesh_hid_send_mouse_click(addr, btn);
            char data[64];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"btn\":%d,\"err\":%d", addr, btn, err);
            jsonl_serial_send_event("hid_click", data);
        } else {
            jsonl_serial_send_error("missing addr", "hid_click");
        }
    }
    else if (strstr(line, "\"cmd\":\"hid_move\"")) {
        /* {"cmd":"hid_move","addr":"0x1c04","dx":10,"dy":-5} */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        const char *dx_str = strstr(line, "\"dx\":");
        const char *dy_str = strstr(line, "\"dy\":");
        if (addr_str && dx_str && dy_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            int16_t dx = (int16_t)strtol(dx_str + 5, NULL, 10);
            int16_t dy = (int16_t)strtol(dy_str + 5, NULL, 10);
            int err = mesh_hid_send_mouse_move(addr, dx, dy);
            char data[64];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"dx\":%d,\"dy\":%d,\"err\":%d", addr, dx, dy, err);
            jsonl_serial_send_event("hid_move", data);
        } else {
            jsonl_serial_send_error("missing addr, dx, or dy", "hid_move");
        }
    }
    else if (strstr(line, "\"cmd\":\"hid_key\"")) {
        /* {"cmd":"hid_key","addr":"0x1c04","key":40,"mod":0} - key=HID keycode, mod=modifier */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        const char *key_str = strstr(line, "\"key\":");
        if (addr_str && key_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            uint8_t keycode = (uint8_t)strtol(key_str + 6, NULL, 10);
            const char *mod_str = strstr(line, "\"mod\":");
            uint8_t mod = mod_str ? (uint8_t)strtol(mod_str + 6, NULL, 10) : 0;
            int err = mesh_hid_send_keyboard_tap(addr, keycode, mod);
            char data[64];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"key\":%d,\"mod\":%d,\"err\":%d", addr, keycode, mod, err);
            jsonl_serial_send_event("hid_key", data);
        } else {
            jsonl_serial_send_error("missing addr or key", "hid_key");
        }
    }
    else if (strstr(line, "\"cmd\":\"hid_media\"")) {
        /* {"cmd":"hid_media","addr":"0x1c04","usage":205} - usage=consumer usage ID */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        const char *usage_str = strstr(line, "\"usage\":");
        if (addr_str && usage_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            uint16_t usage = (uint16_t)strtol(usage_str + 8, NULL, 10);
            int err = mesh_hid_send_consumer(addr, usage);
            char data[64];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"usage\":%d,\"err\":%d", addr, usage, err);
            jsonl_serial_send_event("hid_media", data);
        } else {
            jsonl_serial_send_error("missing addr or usage", "hid_media");
        }
    }
    else if (strstr(line, "\"cmd\":\"gpio_status\"")) {
        /* Get GPIO status */
        gpio_state_t state;
        gpio_get_state(&state);
        char data[96];
        snprintf(data, sizeof(data),
            "\"led_count\":%d,\"btn_count\":%d,\"leds\":%d,\"btns\":%d",
            GPIO_LED_COUNT, GPIO_BTN_COUNT, state.led_state, state.btn_state);
        jsonl_serial_send_event("gpio_status", data);
    }
    else if (strstr(line, "\"cmd\":\"gpio_led\"")) {
        /* {"cmd":"gpio_led","id":0,"on":true} */
        const char *id_str = strstr(line, "\"id\":");
        const char *on_str = strstr(line, "\"on\":");
        if (id_str) {
            uint8_t id = (uint8_t)strtol(id_str + 5, NULL, 10);
            bool on = on_str && strstr(on_str, "true");
            bool ok = gpio_led_set(id, on);
            char data[64];
            snprintf(data, sizeof(data), "\"id\":%d,\"on\":%s,\"ok\":%s",
                id, on ? "true" : "false", ok ? "true" : "false");
            jsonl_serial_send_event("gpio_led", data);
        } else {
            jsonl_serial_send_error("missing id", "gpio_led");
        }
    }
    else if (strstr(line, "\"cmd\":\"gpio_toggle\"")) {
        /* {"cmd":"gpio_toggle","id":0} */
        const char *id_str = strstr(line, "\"id\":");
        if (id_str) {
            uint8_t id = (uint8_t)strtol(id_str + 5, NULL, 10);
            bool new_state = gpio_led_toggle(id);
            char data[48];
            snprintf(data, sizeof(data), "\"id\":%d,\"on\":%s", id, new_state ? "true" : "false");
            jsonl_serial_send_event("gpio_toggle", data);
        } else {
            jsonl_serial_send_error("missing id", "gpio_toggle");
        }
    }
    else if (strstr(line, "\"cmd\":\"gpio_btn\"")) {
        /* {"cmd":"gpio_btn","id":0} or {"cmd":"gpio_btn"} for all */
        const char *id_str = strstr(line, "\"id\":");
        if (id_str) {
            uint8_t id = (uint8_t)strtol(id_str + 5, NULL, 10);
            bool pressed = gpio_btn_read(id);
            char data[48];
            snprintf(data, sizeof(data), "\"id\":%d,\"pressed\":%s", id, pressed ? "true" : "false");
            jsonl_serial_send_event("gpio_btn", data);
        } else {
            uint8_t state = gpio_btn_read_all();
            char data[32];
            snprintf(data, sizeof(data), "\"btns\":%d", state);
            jsonl_serial_send_event("gpio_btn", data);
        }
    }
    else if (strstr(line, "\"cmd\":\"gpio_blink\"")) {
        /* {"cmd":"gpio_blink","id":0,"count":3,"on":100,"off":100} */
        const char *id_str = strstr(line, "\"id\":");
        const char *count_str = strstr(line, "\"count\":");
        if (id_str) {
            uint8_t id = (uint8_t)strtol(id_str + 5, NULL, 10);
            uint8_t count = count_str ? (uint8_t)strtol(count_str + 8, NULL, 10) : 3;
            const char *on_str = strstr(line, "\"on\":");
            const char *off_str = strstr(line, "\"off\":");
            uint16_t on_ms = on_str ? (uint16_t)strtol(on_str + 5, NULL, 10) : 100;
            uint16_t off_ms = off_str ? (uint16_t)strtol(off_str + 6, NULL, 10) : 100;
            gpio_led_blink(id, count, on_ms, off_ms);
            char data[64];
            snprintf(data, sizeof(data), "\"id\":%d,\"count\":%d,\"on\":%d,\"off\":%d",
                id, count, on_ms, off_ms);
            jsonl_serial_send_event("gpio_blink", data);
        } else {
            jsonl_serial_send_error("missing id", "gpio_blink");
        }
    }
    else if (strstr(line, "\"cmd\":\"rc_status\"")) {
        /* Get RC PWM channel status with mode info */
        char data[256];
        uint8_t mode = mesh_hid_get_rc_mode();
        uint8_t invert = mesh_hid_get_rc_invert();
        snprintf(data, sizeof(data),
            "\"rc_count\":%d,\"ch0_us\":%d,\"ch1_us\":%d,\"center_us\":%d,\"min_us\":%d,\"max_us\":%d,"
            "\"failsafe\":%s,\"mode\":\"%s\",\"invert_ch0\":%s,\"invert_ch1\":%s,\"swap\":%s",
            GPIO_RC_COUNT,
            GPIO_RC_COUNT > 0 ? gpio_rc_get(0) : 0,
            GPIO_RC_COUNT > 1 ? gpio_rc_get(1) : 0,
            RC_PWM_CENTER_US, RC_PWM_MIN_US, RC_PWM_MAX_US,
            gpio_rc_failsafe_enabled() ? "true" : "false",
            mode == MESH_RC_MODE_SKID_STEER ? "skid_steer" : "normal",
            (invert & MESH_RC_INVERT_CH0) ? "true" : "false",
            (invert & MESH_RC_INVERT_CH1) ? "true" : "false",
            (invert & MESH_RC_SWAP_CHANNELS) ? "true" : "false");
        jsonl_serial_send_event("rc_status", data);
    }
    else if (strstr(line, "\"cmd\":\"rc_failsafe\"")) {
        /* {"cmd":"rc_failsafe","enabled":true} - enable/disable failsafe */
        const char *enabled_str = strstr(line, "\"enabled\":");
        if (enabled_str) {
            bool enabled = strstr(enabled_str, "true") != NULL;
            gpio_rc_set_failsafe(enabled);
            char data[48];
            snprintf(data, sizeof(data), "\"failsafe\":%s", enabled ? "true" : "false");
            jsonl_serial_send_event("rc_failsafe", data);
        } else {
            /* No enabled param - just report current state */
            char data[48];
            snprintf(data, sizeof(data), "\"failsafe\":%s",
                gpio_rc_failsafe_enabled() ? "true" : "false");
            jsonl_serial_send_event("rc_failsafe", data);
        }
    }
    else if (strstr(line, "\"cmd\":\"rc_set\"")) {
        /* {"cmd":"rc_set","ch":0,"us":1500} - set RC channel pulse width */
        const char *ch_str = strstr(line, "\"ch\":");
        const char *us_str = strstr(line, "\"us\":");
        if (ch_str && us_str) {
            uint8_t ch = (uint8_t)strtol(ch_str + 5, NULL, 10);
            uint16_t us = (uint16_t)strtol(us_str + 5, NULL, 10);
            bool ok = gpio_rc_set(ch, us);
            char data[64];
            snprintf(data, sizeof(data), "\"ch\":%d,\"us\":%d,\"ok\":%s",
                ch, us, ok ? "true" : "false");
            jsonl_serial_send_event("rc_set", data);
        } else {
            jsonl_serial_send_error("missing ch or us", "rc_set");
        }
    }
    else if (strstr(line, "\"cmd\":\"rc_center\"")) {
        /* Center all RC channels */
        gpio_rc_center_all();
        char data[64];
        snprintf(data, sizeof(data), "\"rc_count\":%d,\"center_us\":%d", GPIO_RC_COUNT, RC_PWM_CENTER_US);
        jsonl_serial_send_event("rc_center", data);
    }
    else if (strstr(line, "\"cmd\":\"rc_disable\"")) {
        /* {"cmd":"rc_disable","ch":0} - disable RC channel */
        const char *ch_str = strstr(line, "\"ch\":");
        if (ch_str) {
            uint8_t ch = (uint8_t)strtol(ch_str + 5, NULL, 10);
            bool ok = gpio_rc_disable(ch);
            char data[48];
            snprintf(data, sizeof(data), "\"ch\":%d,\"ok\":%s", ch, ok ? "true" : "false");
            jsonl_serial_send_event("rc_disable", data);
        } else {
            jsonl_serial_send_error("missing ch", "rc_disable");
        }
    }
    else if (strstr(line, "\"cmd\":\"rc_mode\"")) {
        /* {"cmd":"rc_mode","mode":"skid_steer"} or {"cmd":"rc_mode"} to query */
        const char *mode_str = strstr(line, "\"mode\":\"");
        if (mode_str) {
            /* Set mode */
            if (strstr(mode_str, "skid_steer")) {
                mesh_hid_set_rc_mode(MESH_RC_MODE_SKID_STEER);
            } else if (strstr(mode_str, "normal")) {
                mesh_hid_set_rc_mode(MESH_RC_MODE_NORMAL);
            }
        }
        /* Report current mode */
        char data[96];
        uint8_t mode = mesh_hid_get_rc_mode();
        uint8_t invert = mesh_hid_get_rc_invert();
        snprintf(data, sizeof(data),
            "\"mode\":\"%s\",\"invert_ch0\":%s,\"invert_ch1\":%s,\"swap\":%s",
            mode == MESH_RC_MODE_SKID_STEER ? "skid_steer" : "normal",
            (invert & MESH_RC_INVERT_CH0) ? "true" : "false",
            (invert & MESH_RC_INVERT_CH1) ? "true" : "false",
            (invert & MESH_RC_SWAP_CHANNELS) ? "true" : "false");
        jsonl_serial_send_event("rc_mode", data);
    }
    else if (strstr(line, "\"cmd\":\"rc_invert\"")) {
        /* {"cmd":"rc_invert","ch0":true,"ch1":false,"swap":false} */
        uint8_t invert = 0;
        if (strstr(line, "\"ch0\":true")) invert |= MESH_RC_INVERT_CH0;
        if (strstr(line, "\"ch1\":true")) invert |= MESH_RC_INVERT_CH1;
        if (strstr(line, "\"swap\":true")) invert |= MESH_RC_SWAP_CHANNELS;
        mesh_hid_set_rc_invert(invert);
        char data[64];
        snprintf(data, sizeof(data),
            "\"invert_ch0\":%s,\"invert_ch1\":%s,\"swap\":%s",
            (invert & MESH_RC_INVERT_CH0) ? "true" : "false",
            (invert & MESH_RC_INVERT_CH1) ? "true" : "false",
            (invert & MESH_RC_SWAP_CHANNELS) ? "true" : "false");
        jsonl_serial_send_event("rc_invert", data);
    }
    else if (strstr(line, "\"cmd\":\"mouse_rc\"")) {
        /* {"cmd":"mouse_rc","enabled":true} or {"cmd":"mouse_rc"} to query */
        if (strstr(line, "\"enabled\":true")) {
            config_set_mouse_to_rc(true);
        } else if (strstr(line, "\"enabled\":false")) {
            config_set_mouse_to_rc(false);
        }
        char data[32];
        snprintf(data, sizeof(data), "\"enabled\":%s",
                 config_get_mouse_to_rc() ? "true" : "false");
        jsonl_serial_send_event("mouse_rc", data);
    }
    else if (strstr(line, "\"cmd\":\"peers\"")) {
        /* List all known peers with stale status */
        int count = mesh_hid_get_peer_count();
        uint32_t now = k_uptime_get_32();
        char data[384];
        int pos = snprintf(data, sizeof(data), "\"count\":%d,\"peers\":[", count);
        for (int i = 0; i < count && pos < sizeof(data) - 96; i++) {
            mesh_peer_t peer;
            if (mesh_hid_get_peer(i, &peer) == 0) {
                if (i > 0) data[pos++] = ',';
                bool stale = mesh_hid_is_peer_stale(peer.addr);
                uint32_t age_s = peer.last_seen > 0 ? (now - peer.last_seen) / 1000 : 0;
                pos += snprintf(data + pos, sizeof(data) - pos,
                    "{\"addr\":\"0x%04x\",\"rssi\":%d,\"name\":\"%s\",\"stale\":%s,\"age_s\":%u}",
                    peer.addr, peer.rssi, peer.name, stale ? "true" : "false", age_s);
            }
        }
        snprintf(data + pos, sizeof(data) - pos, "]");
        jsonl_serial_send_event("peers", data);
    }
    else if (strstr(line, "\"cmd\":\"peer_info\"")) {
        /* {"cmd":"peer_info","addr":"0x1c04"} */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        if (addr_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            mesh_peer_t peer;
            if (mesh_hid_get_peer_by_addr(addr, &peer) == 0) {
                char data[128];
                snprintf(data, sizeof(data),
                    "\"addr\":\"0x%04x\",\"rssi\":%d,\"caps\":\"0x%02x\",\"name\":\"%s\",\"last_seen\":%u",
                    peer.addr, peer.rssi, peer.capabilities, peer.name, peer.last_seen);
                jsonl_serial_send_event("peer_info", data);
            } else {
                jsonl_serial_send_error("peer not found", "peer_info");
            }
        } else {
            jsonl_serial_send_error("missing addr", "peer_info");
        }
    }
    else if (strstr(line, "\"cmd\":\"clear_peers\"")) {
        /* Clear peer list */
        mesh_hid_clear_peers();
        jsonl_serial_send_event("clear_peers", "\"done\":true");
    }
    else if (strstr(line, "\"cmd\":\"discovery_settings\"")) {
        /* Get periodic discovery settings */
        char data[128];
        snprintf(data, sizeof(data),
            "\"enabled\":%s,\"interval_ms\":%u,\"stale_timeout_ms\":%u",
            mesh_hid_periodic_discovery_enabled() ? "true" : "false",
            (unsigned)mesh_hid_get_discovery_interval(),
            180000);  /* PEER_STALE_TIMEOUT_MS constant */
        jsonl_serial_send_event("discovery_settings", data);
    }
    else if (strstr(line, "\"cmd\":\"set_discovery_interval\"")) {
        /* {"cmd":"set_discovery_interval","interval":60000} or {"cmd":"set_discovery_interval","enabled":false} */
        const char *interval_str = strstr(line, "\"interval\":");
        const char *enabled_str = strstr(line, "\"enabled\":");
        if (interval_str) {
            uint32_t interval = (uint32_t)strtol(interval_str + 11, NULL, 10);
            mesh_hid_set_discovery_interval(interval);
            char data[64];
            snprintf(data, sizeof(data), "\"interval_ms\":%u", (unsigned)interval);
            jsonl_serial_send_event("set_discovery_interval", data);
        } else if (enabled_str) {
            bool enabled = strstr(enabled_str, "true") != NULL;
            mesh_hid_set_periodic_discovery(enabled);
            char data[64];
            snprintf(data, sizeof(data), "\"enabled\":%s", enabled ? "true" : "false");
            jsonl_serial_send_event("set_discovery_interval", data);
        } else {
            jsonl_serial_send_error("missing interval or enabled", "set_discovery_interval");
        }
    }
    else if (strstr(line, "\"cmd\":\"set_name\"")) {
        /* {"cmd":"set_name","name":"MyNode"} */
        const char *name_str = strstr(line, "\"name\":\"");
        if (name_str) {
            name_str += 8;
            char name[16];
            int i = 0;
            while (name_str[i] && name_str[i] != '"' && i < 15) {
                name[i] = name_str[i];
                i++;
            }
            name[i] = '\0';
            mesh_hid_set_name(name);
            char data[64];
            snprintf(data, sizeof(data), "\"name\":\"%s\"", name);
            jsonl_serial_send_event("set_name", data);
        } else {
            jsonl_serial_send_error("missing name", "set_name");
        }
    }
    else if (strstr(line, "\"cmd\":\"get_name\"")) {
        const char *name = mesh_hid_get_name();
        char data[64];
        snprintf(data, sizeof(data), "\"name\":\"%s\"", name);
        jsonl_serial_send_event("get_name", data);
    }
    else if (strstr(line, "\"cmd\":\"mesh_auth\"")) {
        /* {"cmd":"mesh_auth","addr":"0x1c04","pin":"123456"} */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        const char *pin_str = strstr(line, "\"pin\":\"");
        if (addr_str && pin_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            pin_str += 7;
            char pin[16];
            int i = 0;
            while (pin_str[i] && pin_str[i] != '"' && i < 8) {
                pin[i] = pin_str[i];
                i++;
            }
            pin[i] = '\0';
            int err = mesh_hid_send_pin(addr, pin);
            char data[64];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"err\":%d", addr, err);
            jsonl_serial_send_event("mesh_auth", data);
        } else {
            jsonl_serial_send_error("missing addr or pin", "mesh_auth");
        }
    }
    else if (strstr(line, "\"cmd\":\"set_pin_required\"")) {
        /* {"cmd":"set_pin_required","enabled":true} */
        const char *enabled_str = strstr(line, "\"enabled\":");
        bool enabled = enabled_str && strstr(enabled_str, "true");
        mesh_hid_set_pin_required(enabled);
        char data[48];
        snprintf(data, sizeof(data), "\"pin_required\":%s", enabled ? "true" : "false");
        jsonl_serial_send_event("set_pin_required", data);
    }
    else if (strstr(line, "\"cmd\":\"peer_auth_status\"")) {
        /* {"cmd":"peer_auth_status","addr":"0x1c04"} or just check all */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        if (addr_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            bool auth = mesh_hid_is_peer_authenticated(addr);
            char data[64];
            snprintf(data, sizeof(data), "\"addr\":\"0x%04x\",\"authenticated\":%s",
                addr, auth ? "true" : "false");
            jsonl_serial_send_event("peer_auth_status", data);
        } else {
            /* List all peer auth status */
            char data[48];
            snprintf(data, sizeof(data), "\"pin_required\":%s",
                mesh_hid_pin_required() ? "true" : "false");
            jsonl_serial_send_event("pin_config", data);
        }
    }
    else if (strstr(line, "\"cmd\":\"key_exchange\"")) {
        /* {"cmd":"key_exchange","addr":"0x1c04"} - initiate E2E key exchange */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        if (addr_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            int err = mesh_hid_key_exchange(addr);
            char data[64];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"err\":%d", addr, err);
            jsonl_serial_send_event("key_exchange", data);
        } else {
            jsonl_serial_send_error("missing addr", "key_exchange");
        }
    }
    else if (strstr(line, "\"cmd\":\"set_encryption\"")) {
        /* {"cmd":"set_encryption","required":true} */
        const char *req_str = strstr(line, "\"required\":");
        bool required = req_str && strstr(req_str, "true");
        mesh_hid_set_encryption_required(required);
        char data[48];
        snprintf(data, sizeof(data), "\"encryption_required\":%s", required ? "true" : "false");
        jsonl_serial_send_event("set_encryption", data);
    }
    else if (strstr(line, "\"cmd\":\"encryption_status\"")) {
        /* {"cmd":"encryption_status","addr":"0x1c04"} or just global status */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        if (addr_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            bool has_key = mesh_hid_has_session_key(addr);
            bool auth = mesh_hid_is_peer_authenticated(addr);
            char data[96];
            snprintf(data, sizeof(data),
                "\"addr\":\"0x%04x\",\"has_session_key\":%s,\"authenticated\":%s",
                addr, has_key ? "true" : "false", auth ? "true" : "false");
            jsonl_serial_send_event("encryption_status", data);
        } else {
            char data[64];
            snprintf(data, sizeof(data),
                "\"encryption_required\":%s,\"pin_required\":%s",
                mesh_hid_encryption_required() ? "true" : "false",
                mesh_hid_pin_required() ? "true" : "false");
            jsonl_serial_send_event("encryption_status", data);
        }
    }
    else if (strstr(line, "\"cmd\":\"text_enc\"")) {
        /* {"cmd":"text_enc","addr":"0x1c04","text":"hello"} - encrypted text */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        const char *text_str = strstr(line, "\"text\":\"");
        if (addr_str && text_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            text_str += 8;
            char text[64];
            int i = 0;
            while (text_str[i] && text_str[i] != '"' && i < 48) {
                text[i] = text_str[i];
                i++;
            }
            text[i] = '\0';
            int err = mesh_hid_send_text_encrypted(addr, text);
            char data[96];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"encrypted\":true,\"err\":%d", addr, err);
            jsonl_serial_send_event("text_enc", data);
        } else {
            jsonl_serial_send_error("missing addr or text", "text_enc");
        }
    }
    else if (strstr(line, "\"cmd\":\"hid_type_enc\"")) {
        /* {"cmd":"hid_type_enc","addr":"0x1c04","text":"hello"} - encrypted type */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        const char *text_str = strstr(line, "\"text\":\"");
        if (addr_str && text_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            text_str += 8;
            char text[64];
            int i = 0;
            while (text_str[i] && text_str[i] != '"' && i < 46) {
                text[i] = text_str[i];
                i++;
            }
            text[i] = '\0';
            int err = mesh_hid_send_keyboard_type_encrypted(addr, text);
            char data[96];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"encrypted\":true,\"err\":%d", addr, err);
            jsonl_serial_send_event("hid_type_enc", data);
        } else {
            jsonl_serial_send_error("missing addr or text", "hid_type_enc");
        }
    }
    else if (strstr(line, "\"cmd\":\"hid_key_enc\"")) {
        /* {"cmd":"hid_key_enc","addr":"0x1c04","key":40,"mod":0} - encrypted key tap */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        const char *key_str = strstr(line, "\"key\":");
        if (addr_str && key_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            uint8_t keycode = (uint8_t)strtol(key_str + 6, NULL, 10);
            const char *mod_str = strstr(line, "\"mod\":");
            uint8_t mod = mod_str ? (uint8_t)strtol(mod_str + 6, NULL, 10) : 0;
            int err = mesh_hid_send_keyboard_tap_encrypted(addr, keycode, mod);
            char data[80];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"encrypted\":true,\"err\":%d", addr, err);
            jsonl_serial_send_event("hid_key_enc", data);
        } else {
            jsonl_serial_send_error("missing addr or key", "hid_key_enc");
        }
    }
    else if (strstr(line, "\"cmd\":\"hid_click_enc\"")) {
        /* {"cmd":"hid_click_enc","addr":"0x1c04","btn":1} - encrypted click */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        const char *btn_str = strstr(line, "\"btn\":");
        if (addr_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            uint8_t btn = btn_str ? (uint8_t)strtol(btn_str + 6, NULL, 10) : 1;
            int err = mesh_hid_send_mouse_click_encrypted(addr, btn);
            char data[80];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"encrypted\":true,\"err\":%d", addr, err);
            jsonl_serial_send_event("hid_click_enc", data);
        } else {
            jsonl_serial_send_error("missing addr", "hid_click_enc");
        }
    }
    else if (strstr(line, "\"cmd\":\"mesh_gpio_led_enc\"")) {
        /* {"cmd":"mesh_gpio_led_enc","addr":"0x1c04","id":0,"on":true} - encrypted */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        const char *id_str = strstr(line, "\"id\":");
        const char *on_str = strstr(line, "\"on\":");
        if (addr_str && id_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            uint8_t id = (uint8_t)strtol(id_str + 5, NULL, 10);
            bool on = on_str && strstr(on_str, "true");
            int err = mesh_hid_send_gpio_led_encrypted(addr, id, on);
            char data[80];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"id\":%d,\"on\":%s,\"encrypted\":true,\"err\":%d",
                addr, id, on ? "true" : "false", err);
            jsonl_serial_send_event("mesh_gpio_led_enc", data);
        } else {
            jsonl_serial_send_error("missing addr or id", "mesh_gpio_led_enc");
        }
    }
    else if (strstr(line, "\"cmd\":\"mesh_gpio_led\"")) {
        /* {"cmd":"mesh_gpio_led","addr":"0x1c04","id":0,"on":true} - plaintext */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        const char *id_str = strstr(line, "\"id\":");
        const char *on_str = strstr(line, "\"on\":");
        if (addr_str && id_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            uint8_t id = (uint8_t)strtol(id_str + 5, NULL, 10);
            bool on = on_str && strstr(on_str, "true");
            int err = mesh_hid_send_gpio_led(addr, id, on);
            char data[80];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"id\":%d,\"on\":%s,\"err\":%d",
                addr, id, on ? "true" : "false", err);
            jsonl_serial_send_event("mesh_gpio_led", data);
        } else {
            jsonl_serial_send_error("missing addr or id", "mesh_gpio_led");
        }
    }
    else if (strstr(line, "\"cmd\":\"mesh_gpio_toggle_enc\"")) {
        /* {"cmd":"mesh_gpio_toggle_enc","addr":"0x1c04","id":0} - encrypted */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        const char *id_str = strstr(line, "\"id\":");
        if (addr_str && id_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            uint8_t id = (uint8_t)strtol(id_str + 5, NULL, 10);
            int err = mesh_hid_send_gpio_toggle_encrypted(addr, id);
            char data[64];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"id\":%d,\"encrypted\":true,\"err\":%d",
                addr, id, err);
            jsonl_serial_send_event("mesh_gpio_toggle_enc", data);
        } else {
            jsonl_serial_send_error("missing addr or id", "mesh_gpio_toggle_enc");
        }
    }
    else if (strstr(line, "\"cmd\":\"mesh_gpio_toggle\"")) {
        /* {"cmd":"mesh_gpio_toggle","addr":"0x1c04","id":0} - plaintext */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        const char *id_str = strstr(line, "\"id\":");
        if (addr_str && id_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            uint8_t id = (uint8_t)strtol(id_str + 5, NULL, 10);
            int err = mesh_hid_send_gpio_toggle(addr, id);
            char data[64];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"id\":%d,\"err\":%d",
                addr, id, err);
            jsonl_serial_send_event("mesh_gpio_toggle", data);
        } else {
            jsonl_serial_send_error("missing addr or id", "mesh_gpio_toggle");
        }
    }
    else if (strstr(line, "\"cmd\":\"mesh_gpio_blink_enc\"")) {
        /* {"cmd":"mesh_gpio_blink_enc","addr":"0x1c04","id":0,"count":3} - encrypted */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        const char *id_str = strstr(line, "\"id\":");
        const char *count_str = strstr(line, "\"count\":");
        if (addr_str && id_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            uint8_t id = (uint8_t)strtol(id_str + 5, NULL, 10);
            uint8_t count = count_str ? (uint8_t)strtol(count_str + 8, NULL, 10) : 3;
            int err = mesh_hid_send_gpio_blink_encrypted(addr, id, count);
            char data[80];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"id\":%d,\"count\":%d,\"encrypted\":true,\"err\":%d",
                addr, id, count, err);
            jsonl_serial_send_event("mesh_gpio_blink_enc", data);
        } else {
            jsonl_serial_send_error("missing addr or id", "mesh_gpio_blink_enc");
        }
    }
    else if (strstr(line, "\"cmd\":\"mesh_gpio_blink\"")) {
        /* {"cmd":"mesh_gpio_blink","addr":"0x1c04","id":0,"count":3} - plaintext */
        const char *addr_str = strstr(line, "\"addr\":\"0x");
        const char *id_str = strstr(line, "\"id\":");
        const char *count_str = strstr(line, "\"count\":");
        if (addr_str && id_str) {
            uint16_t addr = (uint16_t)strtol(addr_str + 10, NULL, 16);
            uint8_t id = (uint8_t)strtol(id_str + 5, NULL, 10);
            uint8_t count = count_str ? (uint8_t)strtol(count_str + 8, NULL, 10) : 3;
            int err = mesh_hid_send_gpio_blink(addr, id, count);
            char data[64];
            snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"id\":%d,\"count\":%d,\"err\":%d",
                addr, id, count, err);
            jsonl_serial_send_event("mesh_gpio_blink", data);
        } else {
            jsonl_serial_send_error("missing addr or id", "mesh_gpio_blink");
        }
    }
    else if (strstr(line, "\"cmd\":\"get_mode\"")) {
        /* Return current mode and list available modes */
        device_mode_t mode = device_mode_get();
        const device_mode_info_t *info = device_mode_get_info(mode);
        char data[256];
        snprintf(data, sizeof(data),
            "\"mode\":\"%s\",\"description\":\"%s\",\"modes\":[\"hid\",\"mesh\",\"tunnel\",\"rc\"]",
            info ? info->name : "unknown",
            info ? info->description : "");
        jsonl_serial_send_event("get_mode", data);
    }
    else if (strstr(line, "\"cmd\":\"set_mode\"")) {
        /* {"cmd":"set_mode","mode":"tunnel"} */
        const char *mode_str = strstr(line, "\"mode\":\"");
        if (mode_str) {
            mode_str += 8;
            char mode_name[16];
            int i = 0;
            while (mode_str[i] && mode_str[i] != '"' && i < 15) {
                mode_name[i] = mode_str[i];
                i++;
            }
            mode_name[i] = '\0';
            device_mode_t new_mode = device_mode_from_name(mode_name);
            int err = device_mode_set(new_mode);
            const device_mode_info_t *info = device_mode_get_info(new_mode);
            char data[192];
            snprintf(data, sizeof(data),
                "\"mode\":\"%s\",\"description\":\"%s\",\"err\":%d,\"reboot_required\":%s",
                info ? info->name : mode_name,
                info ? info->description : "",
                err,
                err == 0 ? "true" : "false");
            jsonl_serial_send_event("set_mode", data);
        } else {
            jsonl_serial_send_error("missing mode", "set_mode");
        }
    }
    else if (strstr(line, "\"cmd\":\"help\"")) {
        jsonl_serial_send_event("help", "\"cmds\":[\"ping\",\"status\",\"mesh_status\",\"mesh_discover\",\"mesh_ping\",\"mesh_text\",\"mesh_auth\",\"key_exchange\",\"set_encryption\",\"encryption_status\",\"text_enc\",\"hid_type\",\"hid_type_enc\",\"hid_click\",\"hid_click_enc\",\"hid_move\",\"hid_key\",\"hid_key_enc\",\"hid_media\",\"gpio_status\",\"gpio_led\",\"gpio_toggle\",\"gpio_btn\",\"gpio_blink\",\"rc_status\",\"rc_set\",\"rc_center\",\"rc_disable\",\"mesh_gpio_led\",\"mesh_gpio_toggle\",\"mesh_gpio_blink\",\"mesh_gpio_led_enc\",\"mesh_gpio_toggle_enc\",\"mesh_gpio_blink_enc\",\"peers\",\"peer_info\",\"clear_peers\",\"set_name\",\"get_name\",\"set_pin_required\",\"peer_auth_status\",\"get_mode\",\"set_mode\",\"mesh_reset\",\"help\"]");
    }
    else {
        /* Unknown command - could be HID command, try to parse */
        /* For now, just acknowledge */
        jsonl_serial_send_event("ack", "\"received\":true");
    }
}

/**
 * Send raw data to serial
 */
static bool send_raw(const char *data, size_t len)
{
    if (!initialized || !enabled || !host_connected) {
        return false;
    }

    /* Put data in TX ring buffer */
    uint32_t written = ring_buf_put(&tx_ring_buf, (uint8_t *)data, len);
    if (written < len) {
        LOG_WRN("TX buffer overflow, dropped %zu bytes", len - written);
    }

    /* Enable TX interrupt to send */
    uart_irq_tx_enable(cdc_dev);

    return written == len;
}

/**
 * Send a message from BLE/phone to serial
 */
bool jsonl_serial_send_from_ble(const char *json_line, size_t len)
{
    if (!initialized || !enabled) {
        return false;
    }

    /* Add "from":"phone" wrapper if not present */
    char buf[JSONL_MAX_LINE_LEN];
    if (strstr(json_line, "\"from\":") == NULL) {
        /* Insert from field after opening brace */
        snprintf(buf, sizeof(buf), "{\"from\":\"phone\",%s\r\n", json_line + 1);
    } else {
        snprintf(buf, sizeof(buf), "%s\r\n", json_line);
    }

    return send_raw(buf, strlen(buf));
}

/**
 * Send an event to serial
 */
bool jsonl_serial_send_event(const char *event_type, const char *json_data)
{
    char buf[JSONL_MAX_LINE_LEN];
    int len;

    if (json_data && strlen(json_data) > 0) {
        len = snprintf(buf, sizeof(buf), "{\"event\":\"%s\",%s}\r\n",
                      event_type, json_data);
    } else {
        len = snprintf(buf, sizeof(buf), "{\"event\":\"%s\"}\r\n", event_type);
    }

    if (len < 0 || len >= sizeof(buf)) {
        LOG_ERR("Event message too long");
        return false;
    }

    LOG_DBG("TX: %s", buf);
    return send_raw(buf, len);
}

/**
 * Send an error to serial
 */
bool jsonl_serial_send_error(const char *error_msg, const char *cmd)
{
    char buf[JSONL_MAX_LINE_LEN];
    int len;

    if (cmd) {
        len = snprintf(buf, sizeof(buf),
            "{\"error\":\"%s\",\"cmd\":\"%s\"}\r\n", error_msg, cmd);
    } else {
        len = snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}\r\n", error_msg);
    }

    if (len < 0 || len >= sizeof(buf)) {
        return false;
    }

    return send_raw(buf, len);
}

/**
 * Check if serial port is connected
 */
bool jsonl_serial_is_connected(void)
{
    return host_connected;
}

/**
 * Enable or disable the serial interface
 */
void jsonl_serial_set_enabled(bool enable)
{
    enabled = enable;
    LOG_INF("JSONL serial %s", enable ? "enabled" : "disabled");
}

/**
 * Check if serial interface is enabled
 */
bool jsonl_serial_is_enabled(void)
{
    return enabled;
}

/**
 * Send mesh event to serial (called from mesh callback)
 */
void jsonl_serial_send_mesh_event(uint16_t src_addr, const char *event_type,
                                   const char *extra_data)
{
    char data[192];
    if (extra_data && strlen(extra_data) > 0) {
        snprintf(data, sizeof(data), "\"addr\":\"0x%04x\",%s", src_addr, extra_data);
    } else {
        snprintf(data, sizeof(data), "\"addr\":\"0x%04x\"", src_addr);
    }
    jsonl_serial_send_event(event_type, data);
}

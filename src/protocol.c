/*
 * Brewer BLE HID Bridge - Protocol Implementation
 *
 * Parses incoming BLE commands and dispatches to appropriate handlers.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "protocol.h"
#include "hid_keyboard.h"
#include "hid_mouse.h"
#include "hid_consumer.h"
#include "security.h"
#include "config.h"
#include "gpio_control.h"
#include "device_mode.h"
#include "mesh_hid.h"

LOG_MODULE_REGISTER(protocol, LOG_LEVEL_DBG);

/* External functions from main.c */
extern uint32_t get_uptime_seconds(void);
extern bool is_usb_ready(void);

/* Mouse-to-RC virtual joystick accumulator */
#define MOUSE_RC_SCALE       3     /* Multiply mouse deltas by this */
#define MOUSE_RC_DECAY_MS    50    /* Decay interval */
#define MOUSE_RC_DECAY_RATE  30    /* Units to decay per interval toward center */

static int32_t mouse_rc_x = 0;
static int32_t mouse_rc_y = 0;
static int64_t mouse_rc_last_input = 0;

/**
 * Decay the virtual joystick toward center based on elapsed time
 */
static void mouse_rc_decay(void)
{
    int64_t now = k_uptime_get();
    int64_t elapsed = now - mouse_rc_last_input;

    if (elapsed < MOUSE_RC_DECAY_MS) {
        return;
    }

    /* How many decay steps have passed */
    int steps = (int)(elapsed / MOUSE_RC_DECAY_MS);
    int decay = steps * MOUSE_RC_DECAY_RATE;

    if (mouse_rc_x > 0) {
        mouse_rc_x = (mouse_rc_x > decay) ? mouse_rc_x - decay : 0;
    } else if (mouse_rc_x < 0) {
        mouse_rc_x = (mouse_rc_x < -decay) ? mouse_rc_x + decay : 0;
    }

    if (mouse_rc_y > 0) {
        mouse_rc_y = (mouse_rc_y > decay) ? mouse_rc_y - decay : 0;
    } else if (mouse_rc_y < 0) {
        mouse_rc_y = (mouse_rc_y < -decay) ? mouse_rc_y + decay : 0;
    }
}

/**
 * Handle mouse move command
 * When mouse-to-RC is enabled, accumulates dx/dy into a virtual joystick
 * that decays back to center when input stops.
 */
static status_code_t handle_mouse_move(const uint8_t *data, size_t length)
{
    if (length < sizeof(cmd_mouse_move_t)) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_mouse_move_t *cmd = (const cmd_mouse_move_t *)data;

    LOG_DBG("Mouse move: dx=%d, dy=%d", cmd->dx, cmd->dy);

    /* Route to RC PWM if mouse-to-RC is enabled and RC channels exist */
    if (config_get_mouse_to_rc() && GPIO_RC_COUNT > 0) {
        /* Decay toward center based on time since last input */
        mouse_rc_decay();

        /* Accumulate scaled deltas */
        mouse_rc_x += (int32_t)cmd->dx * MOUSE_RC_SCALE;
        mouse_rc_y += (int32_t)cmd->dy * MOUSE_RC_SCALE;

        /* Clamp to RC vector range */
        if (mouse_rc_x < MESH_RC_VECTOR_MIN) mouse_rc_x = MESH_RC_VECTOR_MIN;
        if (mouse_rc_x > MESH_RC_VECTOR_MAX) mouse_rc_x = MESH_RC_VECTOR_MAX;
        if (mouse_rc_y < MESH_RC_VECTOR_MIN) mouse_rc_y = MESH_RC_VECTOR_MIN;
        if (mouse_rc_y > MESH_RC_VECTOR_MAX) mouse_rc_y = MESH_RC_VECTOR_MAX;

        mouse_rc_last_input = k_uptime_get();

        /* Rotate 45° CW so "up" on screen = forward
         * x' = 0.707*(x - y),  y' = 0.707*(x + y)
         * Use fixed-point: 707/1000 ≈ 0.707 */
        int32_t rx = (707 * (mouse_rc_x + mouse_rc_y)) / 1000;
        int32_t ry = (707 * (-mouse_rc_x + mouse_rc_y)) / 1000;
        if (rx < MESH_RC_VECTOR_MIN) rx = MESH_RC_VECTOR_MIN;
        if (rx > MESH_RC_VECTOR_MAX) rx = MESH_RC_VECTOR_MAX;
        if (ry < MESH_RC_VECTOR_MIN) ry = MESH_RC_VECTOR_MIN;
        if (ry > MESH_RC_VECTOR_MAX) ry = MESH_RC_VECTOR_MAX;

        mesh_hid_apply_rc_vector((int16_t)rx, (int16_t)ry);
        return STATUS_OK;
    }

    if (!is_usb_ready()) {
        return STATUS_ERR_USB_BUSY;
    }

    if (!hid_mouse_move(cmd->dx, cmd->dy)) {
        return STATUS_ERR_USB_FAILED;
    }

    return STATUS_OK;
}

/**
 * Handle absolute joystick position command
 * Direct mapping to RC PWM — no accumulation, no decay.
 */
static status_code_t handle_joystick_xy(const uint8_t *data, size_t length)
{
    if (length < sizeof(cmd_joystick_xy_t)) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_joystick_xy_t *cmd = (const cmd_joystick_xy_t *)data;

    LOG_DBG("Joystick: x=%d, y=%d", cmd->x, cmd->y);

    if (GPIO_RC_COUNT == 0) {
        return STATUS_ERR_INVALID_DATA;
    }

    /* Rotate 45° CCW so controls feel natural
     * x' = 0.707*(x + y),  y' = 0.707*(-x + y) */
    int32_t rx = (707 * ((int32_t)cmd->x + (int32_t)cmd->y)) / 1000;
    int32_t ry = (707 * (-(int32_t)cmd->x + (int32_t)cmd->y)) / 1000;
    if (rx < MESH_RC_VECTOR_MIN) rx = MESH_RC_VECTOR_MIN;
    if (rx > MESH_RC_VECTOR_MAX) rx = MESH_RC_VECTOR_MAX;
    if (ry < MESH_RC_VECTOR_MIN) ry = MESH_RC_VECTOR_MIN;
    if (ry > MESH_RC_VECTOR_MAX) ry = MESH_RC_VECTOR_MAX;

    mesh_hid_apply_rc_vector((int16_t)rx, (int16_t)ry);
    return STATUS_OK;
}

/**
 * Handle mouse click command
 */
static status_code_t handle_mouse_click(const uint8_t *data, size_t length)
{
    if (length < sizeof(cmd_mouse_click_t)) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_mouse_click_t *cmd = (const cmd_mouse_click_t *)data;

    LOG_DBG("Mouse click: buttons=0x%02X, action=%d", cmd->buttons, cmd->action);

    if (!is_usb_ready()) {
        return STATUS_ERR_USB_BUSY;
    }

    bool success = false;
    switch (cmd->action) {
    case 0:  /* Release */
        success = hid_mouse_button_release(cmd->buttons);
        break;
    case 1:  /* Press */
        success = hid_mouse_button_press(cmd->buttons);
        break;
    case 2:  /* Click (press + release) */
        success = hid_mouse_click(cmd->buttons);
        break;
    default:
        return STATUS_ERR_INVALID_DATA;
    }

    return success ? STATUS_OK : STATUS_ERR_USB_FAILED;
}

/**
 * Handle mouse scroll command
 */
static status_code_t handle_mouse_scroll(const uint8_t *data, size_t length)
{
    if (length < sizeof(cmd_mouse_scroll_t)) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_mouse_scroll_t *cmd = (const cmd_mouse_scroll_t *)data;

    LOG_DBG("Mouse scroll: v=%d, h=%d", cmd->vertical, cmd->horizontal);

    if (!is_usb_ready()) {
        return STATUS_ERR_USB_BUSY;
    }

    if (!hid_mouse_scroll(cmd->vertical, cmd->horizontal)) {
        return STATUS_ERR_USB_FAILED;
    }

    return STATUS_OK;
}

/**
 * Handle mouse drag start command
 */
static status_code_t handle_mouse_drag_start(void)
{
    if (!is_usb_ready()) {
        return STATUS_ERR_USB_BUSY;
    }

    if (!hid_mouse_drag_start()) {
        return STATUS_ERR_USB_FAILED;
    }

    return STATUS_OK;
}

/**
 * Handle mouse drag end command
 */
static status_code_t handle_mouse_drag_end(void)
{
    if (!is_usb_ready()) {
        return STATUS_ERR_USB_BUSY;
    }

    if (!hid_mouse_drag_end()) {
        return STATUS_ERR_USB_FAILED;
    }

    return STATUS_OK;
}

/**
 * Handle keyboard type command
 */
static status_code_t handle_keyboard_type(const uint8_t *data, size_t length)
{
    if (length < 2) {  /* modifiers + length */
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_keyboard_type_t *cmd = (const cmd_keyboard_type_t *)data;

    if (length < 2 + cmd->length) {
        return STATUS_ERR_INVALID_LEN;
    }

    if (cmd->length > MAX_KEYBOARD_PAYLOAD) {
        return STATUS_ERR_INVALID_LEN;
    }

    LOG_DBG("Keyboard type: %u chars, mods=0x%02X", cmd->length, cmd->modifiers);

    if (!is_usb_ready()) {
        return STATUS_ERR_USB_BUSY;
    }

    /* Type each character */
    if (!hid_keyboard_type((const char *)cmd->text, cmd->length)) {
        return STATUS_ERR_USB_FAILED;
    }

    return STATUS_OK;
}

/**
 * Handle keyboard key command
 */
static status_code_t handle_keyboard_key(const uint8_t *data, size_t length)
{
    if (length < sizeof(cmd_keyboard_key_t)) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_keyboard_key_t *cmd = (const cmd_keyboard_key_t *)data;

    LOG_DBG("Keyboard key: code=0x%02X, mods=0x%02X, action=%d",
            cmd->keycode, cmd->modifiers, cmd->action);

    if (!is_usb_ready()) {
        return STATUS_ERR_USB_BUSY;
    }

    bool success = false;
    switch (cmd->action) {
    case 0:  /* Release */
        success = hid_keyboard_release_all();
        break;
    case 1:  /* Press */
        success = hid_keyboard_key_press(cmd->keycode, cmd->modifiers);
        break;
    case 2:  /* Tap (press + release) */
        success = hid_keyboard_tap(cmd->keycode, cmd->modifiers);
        break;
    default:
        return STATUS_ERR_INVALID_DATA;
    }

    return success ? STATUS_OK : STATUS_ERR_USB_FAILED;
}

/**
 * Handle keyboard combo command
 */
static status_code_t handle_keyboard_combo(const uint8_t *data, size_t length)
{
    if (length < 2) {  /* modifiers + count */
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_keyboard_combo_t *cmd = (const cmd_keyboard_combo_t *)data;

    if (cmd->key_count > 6 || length < 2 + cmd->key_count) {
        return STATUS_ERR_INVALID_LEN;
    }

    LOG_DBG("Keyboard combo: %u keys, mods=0x%02X", cmd->key_count, cmd->modifiers);

    if (!is_usb_ready()) {
        return STATUS_ERR_USB_BUSY;
    }

    if (!hid_keyboard_combo(cmd->keycodes, cmd->key_count, cmd->modifiers)) {
        return STATUS_ERR_USB_FAILED;
    }

    return STATUS_OK;
}

/**
 * Handle media key command
 */
static status_code_t handle_media_key(const uint8_t *data, size_t length)
{
    if (length < sizeof(cmd_media_key_t)) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_media_key_t *cmd = (const cmd_media_key_t *)data;

    LOG_DBG("Media key: usage=0x%04X, action=%d", cmd->usage_id, cmd->action);

    if (!is_usb_ready()) {
        return STATUS_ERR_USB_BUSY;
    }

    bool success = false;
    switch (cmd->action) {
    case 0:  /* Release */
        success = hid_consumer_release();
        break;
    case 1:  /* Press (hold) */
        success = hid_consumer_press(cmd->usage_id);
        break;
    case 2:  /* Tap (press + release) */
        success = hid_consumer_send(cmd->usage_id);
        break;
    default:
        return STATUS_ERR_INVALID_DATA;
    }

    return success ? STATUS_OK : STATUS_ERR_USB_FAILED;
}

/**
 * Handle ping command
 */
static status_code_t handle_ping(const uint8_t *data, size_t length)
{
    ARG_UNUSED(data);
    ARG_UNUSED(length);

    /* Pong response is built separately */
    return STATUS_OK;
}

/**
 * Handle get info command
 */
static status_code_t handle_get_info(void)
{
    /* Info response is built separately */
    return STATUS_OK;
}

/**
 * Handle set device name command
 */
static status_code_t handle_set_name(const uint8_t *data, size_t length)
{
    if (length < 1) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_set_name_t *cmd = (const cmd_set_name_t *)data;

    if (cmd->length > MAX_DEVICE_NAME_LENGTH || length < 1 + cmd->length) {
        return STATUS_ERR_INVALID_LEN;
    }

    if (!config_set_name((const char *)cmd->name, cmd->length)) {
        return STATUS_ERR_INVALID_DATA;
    }

    LOG_INF("Device name changed");
    return STATUS_OK;
}

/**
 * Build name response
 */
size_t protocol_build_name_response(uint8_t *buffer)
{
    char name[MAX_DEVICE_NAME_LENGTH + 1];
    uint8_t len = config_get_name(name);

    buffer[0] = CMD_NAME_RESPONSE;
    buffer[1] = 1 + len;  /* length byte + name */
    buffer[2] = len;
    memcpy(&buffer[3], name, len);

    return 3 + len;
}

/**
 * Handle GPIO set LED command
 */
static status_code_t handle_gpio_set_led(const uint8_t *data, size_t length)
{
    if (length < sizeof(cmd_gpio_set_led_t)) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_gpio_set_led_t *cmd = (const cmd_gpio_set_led_t *)data;

    if (cmd->led_index == 0xFF) {
        /* Set all LEDs using bitmask */
        gpio_led_set_all(cmd->state);
        LOG_INF("Set all LEDs: 0x%02X", cmd->state);
    } else if (cmd->led_index < GPIO_LED_COUNT) {
        /* Set single LED */
        if (!gpio_led_set(cmd->led_index, cmd->state != 0)) {
            return STATUS_ERR_INVALID_DATA;
        }
        LOG_INF("Set LED%d: %s", cmd->led_index, cmd->state ? "ON" : "OFF");
    } else {
        return STATUS_ERR_INVALID_DATA;
    }

    return STATUS_OK;
}

/**
 * Handle GPIO set relay command
 */
static status_code_t handle_gpio_set_relay(const uint8_t *data, size_t length)
{
    if (length < sizeof(cmd_gpio_set_relay_t)) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_gpio_set_relay_t *cmd = (const cmd_gpio_set_relay_t *)data;

    if (cmd->relay_index == 0xFF) {
        /* Set all relays using bitmask */
        gpio_relay_set_all(cmd->state);
        LOG_INF("Set all relays: 0x%02X", cmd->state);
    } else if (cmd->relay_index < GPIO_RELAY_COUNT) {
        /* Set single relay */
        if (!gpio_relay_set(cmd->relay_index, cmd->state != 0)) {
            return STATUS_ERR_INVALID_DATA;
        }
        LOG_INF("Set relay%d: %s", cmd->relay_index, cmd->state ? "ON" : "OFF");
    } else {
        return STATUS_ERR_INVALID_DATA;
    }

    return STATUS_OK;
}

/**
 * Handle RC set command - set PWM pulse width for RC channel
 */
static status_code_t handle_rc_set(const uint8_t *data, size_t length)
{
    if (length < sizeof(cmd_rc_set_t)) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_rc_set_t *cmd = (const cmd_rc_set_t *)data;

    /* Validate pulse width range */
    if (cmd->pulse_us < RC_PWM_MIN_US || cmd->pulse_us > RC_PWM_MAX_US) {
        LOG_WRN("RC pulse out of range: %u (valid: %u-%u)",
                cmd->pulse_us, RC_PWM_MIN_US, RC_PWM_MAX_US);
        return STATUS_ERR_INVALID_DATA;
    }

    if (!gpio_rc_set(cmd->channel, cmd->pulse_us)) {
        LOG_WRN("RC set failed: channel=%u, pulse=%u", cmd->channel, cmd->pulse_us);
        return STATUS_ERR_INVALID_DATA;
    }

    LOG_DBG("RC set: ch=%u, pulse=%uus", cmd->channel, cmd->pulse_us);
    return STATUS_OK;
}

/**
 * Handle RC disable command - stop PWM output on channel
 */
static status_code_t handle_rc_disable(const uint8_t *data, size_t length)
{
    if (length < sizeof(cmd_rc_disable_t)) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_rc_disable_t *cmd = (const cmd_rc_disable_t *)data;

    if (!gpio_rc_disable(cmd->channel)) {
        LOG_WRN("RC disable failed: channel=%u", cmd->channel);
        return STATUS_ERR_INVALID_DATA;
    }

    LOG_INF("RC disabled: ch=%u", cmd->channel);
    return STATUS_OK;
}

/**
 * Handle RC center all command
 */
static status_code_t handle_rc_center_all(void)
{
    gpio_rc_center_all();
    LOG_INF("RC all channels centered");
    return STATUS_OK;
}

/**
 * Handle RC set failsafe command
 */
static status_code_t handle_rc_set_failsafe(const uint8_t *data, size_t length)
{
    if (length < sizeof(cmd_rc_set_failsafe_t)) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_rc_set_failsafe_t *cmd = (const cmd_rc_set_failsafe_t *)data;

    gpio_rc_set_failsafe(cmd->enabled != 0);
    LOG_INF("RC failsafe: %s", cmd->enabled ? "enabled" : "disabled");
    return STATUS_OK;
}

/**
 * Handle RC set mouse-to-RC routing command
 */
static status_code_t handle_rc_set_mouse_rc(const uint8_t *data, size_t length)
{
    if (length < sizeof(cmd_rc_set_mouse_rc_t)) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_rc_set_mouse_rc_t *cmd = (const cmd_rc_set_mouse_rc_t *)data;

    if (!config_set_mouse_to_rc(cmd->enabled != 0)) {
        return STATUS_ERR_INVALID_DATA;
    }

    LOG_INF("Mouse-to-RC: %s", cmd->enabled ? "enabled" : "disabled");
    return STATUS_OK;
}

/**
 * Handle RC set channel inversion command
 */
static status_code_t handle_rc_set_invert(const uint8_t *data, size_t length)
{
    if (length < sizeof(cmd_rc_set_invert_t)) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_rc_set_invert_t *cmd = (const cmd_rc_set_invert_t *)data;
    mesh_hid_set_rc_invert(cmd->invert);

    LOG_INF("RC invert set: 0x%02x (ch0=%d ch1=%d swap=%d)",
            cmd->invert,
            (cmd->invert & 0x01) ? 1 : 0,
            (cmd->invert & 0x02) ? 1 : 0,
            (cmd->invert & 0x04) ? 1 : 0);
    return STATUS_OK;
}

/**
 * Handle set device mode command
 * Note: Mode change requires reboot to take effect
 */
static status_code_t handle_set_mode(const uint8_t *data, size_t length)
{
    if (length < sizeof(cmd_set_mode_t)) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_set_mode_t *cmd = (const cmd_set_mode_t *)data;

    /* Validate mode value */
    if (cmd->mode >= DEVICE_MODE_COUNT) {
        LOG_WRN("Invalid mode: %u (max=%u)", cmd->mode, DEVICE_MODE_COUNT - 1);
        return STATUS_ERR_INVALID_DATA;
    }

    int err = device_mode_set((device_mode_t)cmd->mode);
    if (err) {
        LOG_ERR("Failed to set mode: %d", err);
        return STATUS_ERR_INVALID_DATA;
    }

    LOG_INF("Device mode set to %u (%s) - reboot required",
            cmd->mode, device_mode_name((device_mode_t)cmd->mode));
    return STATUS_OK;
}

/**
 * Handle set PIN command
 */
static status_code_t handle_set_pin(const uint8_t *data, size_t length)
{
    if (length < 1) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_set_pin_t *cmd = (const cmd_set_pin_t *)data;

    /* Length 0 means clear PIN */
    if (cmd->length == 0) {
        if (!config_clear_pin()) {
            return STATUS_ERR_INVALID_DATA;
        }
        LOG_INF("PIN cleared");
        return STATUS_OK;
    }

    if (cmd->length < MIN_PIN_LENGTH || cmd->length > MAX_PIN_LENGTH ||
        length < 1 + cmd->length) {
        return STATUS_ERR_INVALID_LEN;
    }

    if (!config_set_pin((const char *)cmd->pin, cmd->length)) {
        return STATUS_ERR_INVALID_DATA;
    }

    LOG_INF("PIN set");
    return STATUS_OK;
}

/**
 * Process incoming command
 */
status_code_t protocol_process_command(const uint8_t *data, size_t length)
{
    if (data == NULL || length < sizeof(cmd_header_t)) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_header_t *header = (const cmd_header_t *)data;
    const uint8_t *payload = data + sizeof(cmd_header_t);
    size_t payload_len = length - sizeof(cmd_header_t);

    /* Validate payload length matches header */
    if (header->length != payload_len) {
        LOG_WRN("Length mismatch: header=%u, actual=%zu", header->length, payload_len);
        return STATUS_ERR_INVALID_LEN;
    }

    /* Check if authentication is required */
    if (security_requires_auth(header->type)) {
        LOG_WRN("Auth required for command 0x%02X", header->type);
        return STATUS_ERR_AUTH_REQUIRED;
    }

    /* Dispatch based on command type */
    switch (header->type) {
    /* Mouse commands */
    case CMD_MOUSE_MOVE:
        return handle_mouse_move(payload, payload_len);
    case CMD_MOUSE_CLICK:
        return handle_mouse_click(payload, payload_len);
    case CMD_MOUSE_SCROLL:
        return handle_mouse_scroll(payload, payload_len);
    case CMD_MOUSE_DRAG_START:
        return handle_mouse_drag_start();
    case CMD_MOUSE_DRAG_END:
        return handle_mouse_drag_end();
    case CMD_JOYSTICK_XY:
        return handle_joystick_xy(payload, payload_len);

    /* Keyboard commands */
    case CMD_KEYBOARD_TYPE:
        return handle_keyboard_type(payload, payload_len);
    case CMD_KEYBOARD_KEY:
        return handle_keyboard_key(payload, payload_len);
    case CMD_KEYBOARD_COMBO:
        return handle_keyboard_combo(payload, payload_len);
    case CMD_KEYBOARD_SPECIAL:
        /* Use same handler as key */
        return handle_keyboard_key(payload, payload_len);
    case CMD_MEDIA_KEY:
        return handle_media_key(payload, payload_len);

    /* Control commands */
    case CMD_PING:
        return handle_ping(payload, payload_len);
    case CMD_GET_INFO:
        return handle_get_info();
    case CMD_SET_NAME:
        return handle_set_name(payload, payload_len);
    case CMD_GET_NAME:
        /* Name response is built separately in main.c */
        return STATUS_OK;
    case CMD_SET_PIN:
        return handle_set_pin(payload, payload_len);
    case CMD_VERIFY_PIN:
        /* PIN verification response is built separately */
        return STATUS_OK;

    /* Security commands handled by security module */
    case CMD_AUTH_CHALLENGE:
    case CMD_AUTH_RESPONSE:
    case CMD_SESSION_START:
    case CMD_SESSION_END:
        /* These are handled at a higher level */
        return STATUS_OK;

    /* GPIO commands */
    case CMD_GPIO_SET_LED:
        return handle_gpio_set_led(payload, payload_len);
    case CMD_GPIO_GET_LED:
    case CMD_GPIO_SET_RELAY:
        return handle_gpio_set_relay(payload, payload_len);
    case CMD_GPIO_GET_RELAY:
    case CMD_GPIO_READ_DIN:
    case CMD_GPIO_READ_AIN:
    case CMD_GPIO_GET_ALL:
        /* GPIO state response is built separately in main.c */
        return STATUS_OK;

    /* Keyboard LED state */
    case CMD_GET_KBD_LEDS:
        /* Response is built separately in main.c */
        return STATUS_OK;

    /* RC PWM/Joystick commands */
    case CMD_RC_SET:
        return handle_rc_set(payload, payload_len);
    case CMD_RC_GET:
        /* Response is built separately in main.c */
        return STATUS_OK;
    case CMD_RC_CENTER_ALL:
        return handle_rc_center_all();
    case CMD_RC_DISABLE:
        return handle_rc_disable(payload, payload_len);
    case CMD_RC_SET_FAILSAFE:
        return handle_rc_set_failsafe(payload, payload_len);
    case CMD_RC_GET_ALL:
        /* Response is built separately in main.c */
        return STATUS_OK;
    case CMD_RC_SET_MOUSE_RC:
        return handle_rc_set_mouse_rc(payload, payload_len);
    case CMD_RC_SET_INVERT:
        return handle_rc_set_invert(payload, payload_len);
    case CMD_RC_GET_MOUSE_RC:
        /* Response is built separately in main.c */
        return STATUS_OK;

    /* Device mode commands */
    case CMD_GET_MODE:
        /* Response is built separately in main.c */
        return STATUS_OK;
    case CMD_SET_MODE:
        return handle_set_mode(payload, payload_len);

    default:
        LOG_WRN("Unknown command: 0x%02X", header->type);
        return STATUS_ERR_UNKNOWN_CMD;
    }
}

/**
 * Build status response
 */
size_t protocol_build_status(uint8_t *buffer, status_code_t status, uint8_t original_cmd)
{
    cmd_header_t *header = (cmd_header_t *)buffer;
    cmd_status_t *resp = (cmd_status_t *)(buffer + sizeof(cmd_header_t));

    header->type = CMD_STATUS;
    header->length = sizeof(cmd_status_t);

    resp->status_code = status;
    resp->original_cmd = original_cmd;

    return sizeof(cmd_header_t) + sizeof(cmd_status_t);
}

/**
 * Build info response
 */
size_t protocol_build_info(uint8_t *buffer)
{
    cmd_header_t *header = (cmd_header_t *)buffer;
    cmd_info_response_t *info = (cmd_info_response_t *)(buffer + sizeof(cmd_header_t));

    header->type = CMD_INFO_RESPONSE;
    header->length = sizeof(cmd_info_response_t);

    info->version_major = PROTOCOL_VERSION_MAJOR;
    info->version_minor = PROTOCOL_VERSION_MINOR;
    info->usb_connected = is_usb_ready() ? 1 : 0;
    info->session_active = security_session_active() ? 1 : 0;
    info->uptime_sec = get_uptime_seconds();

    return sizeof(cmd_header_t) + sizeof(cmd_info_response_t);
}

/**
 * Build pong response
 */
size_t protocol_build_pong(uint8_t *buffer, const cmd_ping_t *ping_data)
{
    cmd_header_t *header = (cmd_header_t *)buffer;
    cmd_ping_t *pong = (cmd_ping_t *)(buffer + sizeof(cmd_header_t));

    header->type = CMD_PONG;
    header->length = sizeof(cmd_ping_t);

    /* Echo back the ping data */
    pong->timestamp = ping_data->timestamp;
    pong->sequence = ping_data->sequence;

    return sizeof(cmd_header_t) + sizeof(cmd_ping_t);
}

/**
 * Build PIN verification result response
 */
size_t protocol_build_pin_result(uint8_t *buffer, bool success, uint8_t attempts_left)
{
    cmd_header_t *header = (cmd_header_t *)buffer;
    cmd_pin_result_t *result = (cmd_pin_result_t *)(buffer + sizeof(cmd_header_t));

    header->type = CMD_PIN_RESULT;
    header->length = sizeof(cmd_pin_result_t);

    result->success = success ? 1 : 0;
    result->attempts_left = attempts_left;

    return sizeof(cmd_header_t) + sizeof(cmd_pin_result_t);
}

/**
 * Build GPIO state response
 */
size_t protocol_build_gpio_state(uint8_t *buffer)
{
    cmd_header_t *header = (cmd_header_t *)buffer;
    cmd_gpio_state_t *state = (cmd_gpio_state_t *)(buffer + sizeof(cmd_header_t));

    header->type = CMD_GPIO_STATE;
    header->length = sizeof(cmd_gpio_state_t);

    state->led_state = gpio_led_get_all();
    state->relay_state = gpio_relay_get_all();
    state->din_state = gpio_din_read_all();
    state->ain0_value = gpio_ain_read(0);
    state->ain1_value = gpio_ain_read(1);

    LOG_DBG("GPIO state: LEDs=0x%02X, Relays=0x%02X, DIN=0x%02X, AIN0=%u, AIN1=%u",
            state->led_state, state->relay_state, state->din_state,
            state->ain0_value, state->ain1_value);

    return sizeof(cmd_header_t) + sizeof(cmd_gpio_state_t);
}

/**
 * Build keyboard LED state response
 * Reports NumLock, CapsLock, ScrollLock state from host PC
 */
size_t protocol_build_kbd_leds_state(uint8_t *buffer)
{
    cmd_header_t *header = (cmd_header_t *)buffer;
    cmd_kbd_leds_state_t *state = (cmd_kbd_leds_state_t *)(buffer + sizeof(cmd_header_t));

    header->type = CMD_KBD_LEDS_STATE;
    header->length = sizeof(cmd_kbd_leds_state_t);

    state->led_state = hid_keyboard_get_led_state();

    LOG_DBG("Keyboard LEDs: Num=%d Caps=%d Scroll=%d",
            (state->led_state >> 0) & 1,
            (state->led_state >> 1) & 1,
            (state->led_state >> 2) & 1);

    return sizeof(cmd_header_t) + sizeof(cmd_kbd_leds_state_t);
}

/**
 * Build RC single channel state response
 */
size_t protocol_build_rc_state(uint8_t *buffer, uint8_t channel)
{
    cmd_header_t *header = (cmd_header_t *)buffer;
    cmd_rc_state_t *state = (cmd_rc_state_t *)(buffer + sizeof(cmd_header_t));

    header->type = CMD_RC_STATE;
    header->length = sizeof(cmd_rc_state_t);

    state->channel = channel;
    state->pulse_us = gpio_rc_get(channel);

    LOG_DBG("RC state: ch=%u, pulse=%uus", channel, state->pulse_us);

    return sizeof(cmd_header_t) + sizeof(cmd_rc_state_t);
}

/**
 * Build RC all channels state response
 */
size_t protocol_build_rc_state_all(uint8_t *buffer)
{
    cmd_header_t *header = (cmd_header_t *)buffer;
    cmd_rc_state_all_t *state = (cmd_rc_state_all_t *)(buffer + sizeof(cmd_header_t));

    header->type = CMD_RC_STATE;
    header->length = sizeof(cmd_rc_state_all_t);

    state->channel_count = GPIO_RC_COUNT;
    state->failsafe = gpio_rc_failsafe_enabled() ? 1 : 0;

    for (uint8_t i = 0; i < MAX_RC_CHANNELS; i++) {
        if (i < GPIO_RC_COUNT) {
            state->pulse_us[i] = gpio_rc_get(i);
        } else {
            state->pulse_us[i] = 0;
        }
    }

    LOG_DBG("RC state all: count=%u, failsafe=%u", state->channel_count, state->failsafe);

    return sizeof(cmd_header_t) + sizeof(cmd_rc_state_all_t);
}

/**
 * Build mouse-to-RC state response
 */
size_t protocol_build_mouse_rc_state(uint8_t *buffer)
{
    cmd_header_t *header = (cmd_header_t *)buffer;
    cmd_rc_mouse_rc_state_t *state = (cmd_rc_mouse_rc_state_t *)(buffer + sizeof(cmd_header_t));

    header->type = CMD_RC_GET_MOUSE_RC;
    header->length = sizeof(cmd_rc_mouse_rc_state_t);

    state->enabled = config_get_mouse_to_rc() ? 1 : 0;

    LOG_DBG("Mouse-to-RC state: %s", state->enabled ? "enabled" : "disabled");

    return sizeof(cmd_header_t) + sizeof(cmd_rc_mouse_rc_state_t);
}

/**
 * Build device mode state response
 */
size_t protocol_build_mode_state(uint8_t *buffer)
{
    cmd_header_t *header = (cmd_header_t *)buffer;
    cmd_mode_state_t *state = (cmd_mode_state_t *)(buffer + sizeof(cmd_header_t));

    device_mode_t mode = device_mode_get();
    const device_mode_info_t *info = device_mode_get_info(mode);

    header->type = CMD_MODE_STATE;
    header->length = sizeof(cmd_mode_state_t);

    state->current_mode = (uint8_t)mode;
    state->features = info->features;

    /* Copy mode name */
    size_t name_len = strlen(info->name);
    if (name_len > MODE_NAME_MAX_LEN) {
        name_len = MODE_NAME_MAX_LEN;
    }
    state->name_len = (uint8_t)name_len;
    memcpy(state->name, info->name, name_len);

    LOG_DBG("Mode state: mode=%u (%s), features=0x%02x",
            state->current_mode, info->name, state->features);

    return sizeof(cmd_header_t) + sizeof(cmd_mode_state_t);
}

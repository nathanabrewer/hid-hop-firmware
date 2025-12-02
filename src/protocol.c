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

LOG_MODULE_REGISTER(protocol, LOG_LEVEL_DBG);

/* External functions from main.c */
extern uint32_t get_uptime_seconds(void);
extern bool is_usb_ready(void);

/**
 * Handle mouse move command
 */
static status_code_t handle_mouse_move(const uint8_t *data, size_t length)
{
    if (length < sizeof(cmd_mouse_move_t)) {
        return STATUS_ERR_INVALID_LEN;
    }

    const cmd_mouse_move_t *cmd = (const cmd_mouse_move_t *)data;

    LOG_DBG("Mouse move: dx=%d, dy=%d", cmd->dx, cmd->dy);

    if (!is_usb_ready()) {
        return STATUS_ERR_USB_BUSY;
    }

    if (!hid_mouse_move(cmd->dx, cmd->dy)) {
        return STATUS_ERR_USB_FAILED;
    }

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

/*
 * Brewer BLE HID Bridge - Communication Protocol
 *
 * Binary protocol for efficient BLE communication between iOS app and dongle.
 * All multi-byte values are little-endian.
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Protocol version for compatibility checking */
#define PROTOCOL_VERSION_MAJOR 1
#define PROTOCOL_VERSION_MINOR 0

/* Maximum payload sizes */
#define MAX_KEYBOARD_PAYLOAD 64
#define MAX_MOUSE_PAYLOAD 8
#define MAX_COMMAND_SIZE 128

/* ==========================================
 * Command Types (1 byte)
 * ==========================================
 */
typedef enum {
    /* Mouse commands: 0x01 - 0x1F */
    CMD_MOUSE_MOVE          = 0x01,  /* Relative mouse movement */
    CMD_MOUSE_CLICK         = 0x02,  /* Button click */
    CMD_MOUSE_SCROLL        = 0x03,  /* Scroll wheel */
    CMD_MOUSE_DRAG_START    = 0x04,  /* Start drag operation */
    CMD_MOUSE_DRAG_END      = 0x05,  /* End drag operation */

    /* Keyboard commands: 0x20 - 0x3F */
    CMD_KEYBOARD_TYPE       = 0x20,  /* Type ASCII text */
    CMD_KEYBOARD_KEY        = 0x21,  /* Single key press/release */
    CMD_KEYBOARD_COMBO      = 0x22,  /* Key combination (e.g., Ctrl+C) */
    CMD_KEYBOARD_SPECIAL    = 0x23,  /* Special keys (F1-F12, etc.) */
    CMD_MEDIA_KEY           = 0x24,  /* Media/consumer control keys */

    /* Control commands: 0x40 - 0x5F */
    CMD_PING                = 0x40,  /* Keepalive/latency check */
    CMD_PONG                = 0x41,  /* Ping response */
    CMD_GET_INFO            = 0x42,  /* Get device info */
    CMD_INFO_RESPONSE       = 0x43,  /* Device info response */
    CMD_SET_CONFIG          = 0x44,  /* Set configuration */
    CMD_RESET               = 0x45,  /* Reset to defaults */
    CMD_SET_NAME            = 0x46,  /* Set device BLE name */
    CMD_GET_NAME            = 0x4A,  /* Get current device name */
    CMD_NAME_RESPONSE       = 0x4B,  /* Device name response */
    CMD_SET_PIN             = 0x47,  /* Set access PIN */
    CMD_VERIFY_PIN          = 0x48,  /* Verify access PIN */
    CMD_PIN_RESULT          = 0x49,  /* PIN verification result */

    /* Security commands: 0x60 - 0x7F */
    CMD_AUTH_CHALLENGE      = 0x60,  /* Authentication challenge */
    CMD_AUTH_RESPONSE       = 0x61,  /* Authentication response */
    CMD_SESSION_START       = 0x62,  /* Start authenticated session */
    CMD_SESSION_END         = 0x63,  /* End session */

    /* GPIO commands: 0x80 - 0x9F */
    CMD_GPIO_SET_LED        = 0x80,  /* Set LED state */
    CMD_GPIO_GET_LED        = 0x81,  /* Get LED state */
    CMD_GPIO_SET_RELAY      = 0x82,  /* Set relay/output state */
    CMD_GPIO_GET_RELAY      = 0x83,  /* Get relay/output state */
    CMD_GPIO_READ_DIN       = 0x84,  /* Read digital inputs */
    CMD_GPIO_READ_AIN       = 0x85,  /* Read analog inputs */
    CMD_GPIO_GET_ALL        = 0x86,  /* Get all GPIO states */
    CMD_GPIO_STATE          = 0x87,  /* GPIO state response */

    /* Status/Error: 0xE0 - 0xFF */
    CMD_STATUS              = 0xE0,  /* Status response */
    CMD_ERROR               = 0xFF,  /* Error response */
} command_type_t;

/* ==========================================
 * Mouse Button Flags
 * ==========================================
 */
typedef enum {
    MOUSE_BTN_LEFT      = (1 << 0),
    MOUSE_BTN_RIGHT     = (1 << 1),
    MOUSE_BTN_MIDDLE    = (1 << 2),
} mouse_button_t;

/* ==========================================
 * Keyboard Modifier Flags
 * ==========================================
 */
typedef enum {
    MOD_NONE        = 0x00,
    MOD_LEFT_CTRL   = (1 << 0),
    MOD_LEFT_SHIFT  = (1 << 1),
    MOD_LEFT_ALT    = (1 << 2),
    MOD_LEFT_GUI    = (1 << 3),  /* Command on Mac */
    MOD_RIGHT_CTRL  = (1 << 4),
    MOD_RIGHT_SHIFT = (1 << 5),
    MOD_RIGHT_ALT   = (1 << 6),
    MOD_RIGHT_GUI   = (1 << 7),
} keyboard_modifier_t;

/* ==========================================
 * Special Keys
 * ==========================================
 */
typedef enum {
    KEY_ESCAPE      = 0x29,
    KEY_BACKSPACE   = 0x2A,
    KEY_TAB         = 0x2B,
    KEY_ENTER       = 0x28,
    KEY_CAPS_LOCK   = 0x39,
    KEY_DELETE      = 0x4C,
    KEY_INSERT      = 0x49,
    KEY_HOME        = 0x4A,
    KEY_END         = 0x4D,
    KEY_PAGE_UP     = 0x4B,
    KEY_PAGE_DOWN   = 0x4E,
    KEY_ARROW_RIGHT = 0x4F,
    KEY_ARROW_LEFT  = 0x50,
    KEY_ARROW_DOWN  = 0x51,
    KEY_ARROW_UP    = 0x52,
    KEY_F1          = 0x3A,
    KEY_F2          = 0x3B,
    KEY_F3          = 0x3C,
    KEY_F4          = 0x3D,
    KEY_F5          = 0x3E,
    KEY_F6          = 0x3F,
    KEY_F7          = 0x40,
    KEY_F8          = 0x41,
    KEY_F9          = 0x42,
    KEY_F10         = 0x43,
    KEY_F11         = 0x44,
    KEY_F12         = 0x45,
} special_key_t;

/* ==========================================
 * Status Codes
 * ==========================================
 */
typedef enum {
    STATUS_OK               = 0x00,
    STATUS_ERR_UNKNOWN_CMD  = 0x01,
    STATUS_ERR_INVALID_LEN  = 0x02,
    STATUS_ERR_AUTH_REQUIRED= 0x03,
    STATUS_ERR_USB_BUSY     = 0x04,
    STATUS_ERR_USB_FAILED   = 0x05,
    STATUS_ERR_INVALID_DATA = 0x06,
} status_code_t;

/* ==========================================
 * Command Structures
 * ==========================================
 */

/* Generic command header */
typedef struct __attribute__((packed)) {
    uint8_t type;       /* command_type_t */
    uint8_t length;     /* Length of payload (excluding header) */
} cmd_header_t;

/* Mouse move command payload */
typedef struct __attribute__((packed)) {
    int16_t dx;         /* X movement (-32768 to 32767) */
    int16_t dy;         /* Y movement (-32768 to 32767) */
} cmd_mouse_move_t;

/* Mouse click command payload */
typedef struct __attribute__((packed)) {
    uint8_t buttons;    /* Button mask (mouse_button_t flags) */
    uint8_t action;     /* 0 = release, 1 = press, 2 = click */
} cmd_mouse_click_t;

/* Mouse scroll command payload */
typedef struct __attribute__((packed)) {
    int8_t vertical;    /* Vertical scroll (-127 to 127) */
    int8_t horizontal;  /* Horizontal scroll (-127 to 127) */
} cmd_mouse_scroll_t;

/* Keyboard type command payload - variable length */
typedef struct __attribute__((packed)) {
    uint8_t modifiers;  /* Modifier keys held during typing */
    uint8_t length;     /* Number of characters */
    uint8_t text[];     /* UTF-8 text (up to MAX_KEYBOARD_PAYLOAD bytes) */
} cmd_keyboard_type_t;

/* Keyboard single key command payload */
typedef struct __attribute__((packed)) {
    uint8_t modifiers;  /* Modifier keys */
    uint8_t keycode;    /* HID keycode */
    uint8_t action;     /* 0 = release, 1 = press, 2 = tap */
} cmd_keyboard_key_t;

/* Keyboard combo command payload - multiple keys at once */
typedef struct __attribute__((packed)) {
    uint8_t modifiers;  /* Modifier keys */
    uint8_t key_count;  /* Number of keys */
    uint8_t keycodes[]; /* HID keycodes (up to 6) */
} cmd_keyboard_combo_t;

/* Media key command payload */
typedef struct __attribute__((packed)) {
    uint16_t usage_id;  /* Consumer control usage ID (little-endian) */
    uint8_t action;     /* 0 = release, 1 = press, 2 = tap (press+release) */
} cmd_media_key_t;

/* Ping/Pong payload */
typedef struct __attribute__((packed)) {
    uint32_t timestamp; /* Sender's timestamp in ms */
    uint32_t sequence;  /* Sequence number */
} cmd_ping_t;

/* Device info response payload */
typedef struct __attribute__((packed)) {
    uint8_t version_major;
    uint8_t version_minor;
    uint8_t usb_connected;  /* 1 if USB HID is active */
    uint8_t session_active; /* 1 if authenticated session */
    uint32_t uptime_sec;    /* Device uptime in seconds */
} cmd_info_response_t;

/* Status response payload */
typedef struct __attribute__((packed)) {
    uint8_t status_code;    /* status_code_t */
    uint8_t original_cmd;   /* Command this is responding to */
} cmd_status_t;

/* Set device name command payload */
typedef struct __attribute__((packed)) {
    uint8_t length;         /* Name length (max 20) */
    uint8_t name[];         /* Device name (UTF-8) */
} cmd_set_name_t;

/* Set PIN command payload */
typedef struct __attribute__((packed)) {
    uint8_t length;         /* PIN length (4-8 digits) */
    uint8_t pin[];          /* PIN digits as ASCII */
} cmd_set_pin_t;

/* Verify PIN command payload */
typedef struct __attribute__((packed)) {
    uint8_t length;         /* PIN length */
    uint8_t pin[];          /* PIN to verify */
} cmd_verify_pin_t;

/* PIN result response payload */
typedef struct __attribute__((packed)) {
    uint8_t success;        /* 1 if PIN correct, 0 if wrong */
    uint8_t attempts_left;  /* Remaining attempts before lockout */
} cmd_pin_result_t;

/* Max device name length */
#define MAX_DEVICE_NAME_LENGTH 20

/* PIN constraints */
#define MIN_PIN_LENGTH 4
#define MAX_PIN_LENGTH 8
#define MAX_PIN_ATTEMPTS 3   /* SECURITY: Reduced from 5 to 3 */

/* GPIO constraints */
#define GPIO_LED_COUNT      3
#define GPIO_RELAY_COUNT    7
#define GPIO_DIN_COUNT      2
#define GPIO_AIN_COUNT      2

/* GPIO set LED command payload */
typedef struct __attribute__((packed)) {
    uint8_t led_index;      /* LED index (0-2) or 0xFF for all */
    uint8_t state;          /* 0 = off, 1 = on, or bitmask if index=0xFF */
} cmd_gpio_set_led_t;

/* GPIO set relay command payload */
typedef struct __attribute__((packed)) {
    uint8_t relay_index;    /* Relay index (0-6) or 0xFF for all */
    uint8_t state;          /* 0 = off, 1 = on, or bitmask if index=0xFF */
} cmd_gpio_set_relay_t;

/* GPIO state response payload */
typedef struct __attribute__((packed)) {
    uint8_t led_state;      /* Bitmask of LED states (3 bits) */
    uint8_t relay_state;    /* Bitmask of relay states (7 bits) */
    uint8_t din_state;      /* Bitmask of digital input states (2 bits) */
    uint16_t ain0_value;    /* Analog input 0 value (12-bit, little-endian) */
    uint16_t ain1_value;    /* Analog input 1 value (12-bit, little-endian) */
} cmd_gpio_state_t;

/* ==========================================
 * Protocol Functions
 * ==========================================
 */

/**
 * Process incoming command from BLE
 * @param data Raw command data
 * @param length Length of data
 * @return Status code
 */
status_code_t protocol_process_command(const uint8_t *data, size_t length);

/**
 * Build a status response
 * @param buffer Output buffer
 * @param status Status code
 * @param original_cmd Command being responded to
 * @return Length of response
 */
size_t protocol_build_status(uint8_t *buffer, status_code_t status, uint8_t original_cmd);

/**
 * Build an info response
 * @param buffer Output buffer
 * @return Length of response
 */
size_t protocol_build_info(uint8_t *buffer);

/**
 * Build a pong response
 * @param buffer Output buffer
 * @param ping_data Original ping data
 * @return Length of response
 */
size_t protocol_build_pong(uint8_t *buffer, const cmd_ping_t *ping_data);

/**
 * Build a PIN verification result response
 * @param buffer Output buffer
 * @param success true if PIN was correct
 * @param attempts_left Remaining attempts before lockout
 * @return Length of response
 */
size_t protocol_build_pin_result(uint8_t *buffer, bool success, uint8_t attempts_left);

/**
 * Build a device name response
 * @param buffer Output buffer
 * @return Length of response
 */
size_t protocol_build_name_response(uint8_t *buffer);

/**
 * Build a GPIO state response
 * @param buffer Output buffer
 * @return Length of response
 */
size_t protocol_build_gpio_state(uint8_t *buffer);

#endif /* PROTOCOL_H */

/*
 * BLE Mesh HID Bridge for HID-HOP
 *
 * Provides mesh networking between HID-HOP nodes with:
 * - Node discovery and announcements
 * - Message relay (multi-hop)
 * - Vendor Model for HID commands
 *
 * Copyright (c) 2025 Nathan Brewer
 */

#ifndef MESH_HID_H
#define MESH_HID_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Vendor Model identifiers */
#define MESH_HID_COMPANY_ID     0x1915  /* Nordic Semiconductor */
#define MESH_HID_MODEL_ID       0x0001  /* HID-HOP Vendor Model */

/* Message opcodes (3-byte vendor opcode) */
#define MESH_HID_OP_DISCOVERY       BT_MESH_MODEL_OP_3(0x01, MESH_HID_COMPANY_ID)
#define MESH_HID_OP_DISCOVERY_RESP  BT_MESH_MODEL_OP_3(0x02, MESH_HID_COMPANY_ID)
#define MESH_HID_OP_BEACON          BT_MESH_MODEL_OP_3(0x03, MESH_HID_COMPANY_ID)  /* One-way presence broadcast */
#define MESH_HID_OP_HID_CMD         BT_MESH_MODEL_OP_3(0x10, MESH_HID_COMPANY_ID)
#define MESH_HID_OP_HID_RESP        BT_MESH_MODEL_OP_3(0x11, MESH_HID_COMPANY_ID)
#define MESH_HID_OP_STATUS          BT_MESH_MODEL_OP_3(0x20, MESH_HID_COMPANY_ID)
#define MESH_HID_OP_PIN_AUTH        BT_MESH_MODEL_OP_3(0x24, MESH_HID_COMPANY_ID)
#define MESH_HID_OP_PIN_RESP        BT_MESH_MODEL_OP_3(0x25, MESH_HID_COMPANY_ID)
#define MESH_HID_OP_TEXT            BT_MESH_MODEL_OP_3(0x30, MESH_HID_COMPANY_ID)

/* Encrypted message opcodes
 * NOTE: BT_MESH_MODEL_OP_3 only uses lower 6 bits (0x00-0x3F), so opcodes must not collide!
 * Previous 0x4X/0x5X opcodes collided with 0x0X/0x1X after masking with 0x3F.
 */
#define MESH_HID_OP_HID_CMD_ENC     BT_MESH_MODEL_OP_3(0x04, MESH_HID_COMPANY_ID)
#define MESH_HID_OP_TEXT_ENC        BT_MESH_MODEL_OP_3(0x05, MESH_HID_COMPANY_ID)
#define MESH_HID_OP_KEY_EXCHANGE    BT_MESH_MODEL_OP_3(0x06, MESH_HID_COMPANY_ID)
#define MESH_HID_OP_KEY_CONFIRM     BT_MESH_MODEL_OP_3(0x07, MESH_HID_COMPANY_ID)

/* GPIO command opcodes (require PIN auth like HID) */
#define MESH_HID_OP_GPIO_CMD        BT_MESH_MODEL_OP_3(0x08, MESH_HID_COMPANY_ID)
#define MESH_HID_OP_GPIO_CMD_ENC    BT_MESH_MODEL_OP_3(0x09, MESH_HID_COMPANY_ID)

/* RC PWM control opcodes (for servo/ESC control) */
#define MESH_HID_OP_RC_VECTOR       BT_MESH_MODEL_OP_3(0x0A, MESH_HID_COMPANY_ID)
#define MESH_HID_OP_RC_VECTOR_ENC   BT_MESH_MODEL_OP_3(0x0B, MESH_HID_COMPANY_ID)
#define MESH_HID_OP_RC_CONFIG       BT_MESH_MODEL_OP_3(0x0C, MESH_HID_COMPANY_ID)

/* Maximum mesh message payload */
#define MESH_HID_MAX_PAYLOAD    64

/* HID command types */
#define MESH_HID_TYPE_KEYBOARD  0x00
#define MESH_HID_TYPE_MOUSE     0x01
#define MESH_HID_TYPE_CONSUMER  0x02

/* Keyboard actions */
#define MESH_HID_KB_TYPE        0x00  /* Type ASCII string */
#define MESH_HID_KB_TAP         0x01  /* Tap key (press+release) */
#define MESH_HID_KB_PRESS       0x02  /* Press key (hold) */
#define MESH_HID_KB_RELEASE     0x03  /* Release all keys */

/* Mouse actions */
#define MESH_HID_MOUSE_MOVE     0x00  /* Move cursor */
#define MESH_HID_MOUSE_CLICK    0x01  /* Click button */
#define MESH_HID_MOUSE_SCROLL   0x02  /* Scroll wheel */
#define MESH_HID_MOUSE_PRESS    0x03  /* Press button (hold) */
#define MESH_HID_MOUSE_RELEASE  0x04  /* Release buttons */

/* Consumer actions */
#define MESH_HID_CONSUMER_SEND  0x00  /* Send consumer key */

/* GPIO actions */
#define MESH_GPIO_LED_SET       0x00  /* Set LED on/off */
#define MESH_GPIO_LED_TOGGLE    0x01  /* Toggle LED */
#define MESH_GPIO_LED_BLINK     0x02  /* Blink LED */
#define MESH_GPIO_BTN_READ      0x10  /* Read button state */

/* RC drive modes */
#define MESH_RC_MODE_NORMAL     0x00  /* Normal: CH0=steering, CH1=throttle */
#define MESH_RC_MODE_SKID_STEER 0x01  /* Skid steer: left = Y+X, right = Y-X */

/* RC channel inversion flags (bitmask) */
#define MESH_RC_INVERT_CH0      0x01  /* Invert channel 0 */
#define MESH_RC_INVERT_CH1      0x02  /* Invert channel 1 */
#define MESH_RC_SWAP_CHANNELS   0x04  /* Swap CH0 and CH1 output */

/* RC vector control constants */
#define MESH_RC_VECTOR_MIN      (-1000)  /* Min joystick value */
#define MESH_RC_VECTOR_MAX      1000     /* Max joystick value */
#define MESH_RC_VECTOR_CENTER   0        /* Center/neutral */

/**
 * HID command packet format (sent over mesh)
 */
typedef struct __attribute__((packed)) {
    uint8_t type;       /* MESH_HID_TYPE_* */
    uint8_t action;     /* Type-specific action */
    uint8_t data[60];   /* Variable payload */
} mesh_hid_cmd_t;

/**
 * RC vector control packet format
 * Sent from mobile app, chip does mixing based on drive mode
 *
 * Wire format: 5 bytes
 *   [0-1] x: int16_t LE, joystick X axis (-1000 to +1000)
 *   [2-3] y: int16_t LE, joystick Y axis (-1000 to +1000)
 *   [4]   flags: uint8_t, reserved (set to 0)
 */
typedef struct __attribute__((packed)) {
    int16_t x;          /* X axis: -1000 (left) to +1000 (right) */
    int16_t y;          /* Y axis: -1000 (back) to +1000 (forward) */
    uint8_t flags;      /* Reserved for future use */
} mesh_rc_vector_t;

/**
 * RC configuration packet format
 * Sets drive mode and channel inversion
 *
 * Wire format: 2 bytes
 *   [0] mode: uint8_t, MESH_RC_MODE_*
 *   [1] invert: uint8_t, MESH_RC_INVERT_* flags
 */
typedef struct __attribute__((packed)) {
    uint8_t mode;       /* MESH_RC_MODE_NORMAL or MESH_RC_MODE_SKID_STEER */
    uint8_t invert;     /* Inversion flags (MESH_RC_INVERT_*) */
} mesh_rc_config_t;

/* Node capabilities flags */
#define MESH_CAP_RELAY          0x01
#define MESH_CAP_PROXY          0x02
#define MESH_CAP_USB_HOST       0x04
#define MESH_CAP_PROVISIONER    0x08

/* Peer tracking */
#define MESH_MAX_PEERS          16
#define MESH_NODE_NAME_LEN      16

/* Encryption constants */
#define MESH_E2E_KEY_LEN        16   /* AES-128 key */
#define MESH_E2E_NONCE_LEN      13   /* CCM nonce */
#define MESH_E2E_TAG_LEN        8    /* CCM auth tag (truncated) */

/**
 * Peer node info
 */
typedef struct {
    uint16_t addr;              /* Mesh address */
    int8_t rssi;                /* Last received RSSI */
    uint8_t capabilities;       /* Capability flags */
    uint32_t last_seen;         /* Uptime ms when last seen */
    char name[MESH_NODE_NAME_LEN]; /* Optional friendly name */
    bool authenticated;         /* PIN authenticated for HID */
    uint32_t auth_expires;      /* Auth expiration time (uptime_ms) */
    /* E2E encryption state */
    bool has_session_key;       /* Session key established */
    uint8_t session_key[MESH_E2E_KEY_LEN];  /* AES-128 session key */
    uint32_t tx_counter;        /* TX message counter (for nonce) */
    uint32_t rx_counter;        /* RX message counter (for replay protection) */
} mesh_peer_t;

/**
 * Mesh message types (for routing/identification)
 */
typedef enum {
    /* Open messages (mesh encryption only) */
    MESH_MSG_DISCOVERY      = 0x01,
    MESH_MSG_DISCOVERY_RESP = 0x02,
    MESH_MSG_STATUS         = 0x03,
    MESH_MSG_RSSI_REPORT    = 0x04,

    /* E2E encrypted messages (PIN auth required) - Phase 3 */
    MESH_MSG_HID_CMD        = 0x20,
    MESH_MSG_HID_RESP       = 0x21,
    MESH_MSG_PIN_EXCHANGE   = 0x24,
    MESH_MSG_SESSION_KEY    = 0x25,
} mesh_msg_type_t;

/**
 * Discovery announcement payload
 */
typedef struct __attribute__((packed)) {
    uint8_t uuid[16];           /* Device UUID */
    uint8_t name_len;           /* Device name length */
    char name[20];              /* Device name (null-terminated) */
    uint8_t capabilities;       /* Capability flags */
    int8_t tx_power;            /* TX power dBm */
} mesh_discovery_t;

/**
 * Status message payload
 */
typedef struct __attribute__((packed)) {
    uint16_t addr;              /* Mesh address */
    uint8_t provisioned;        /* Is provisioned? */
    uint8_t relay_enabled;      /* Is relay enabled? */
    uint8_t peer_count;         /* Number of known peers */
    int8_t rssi;                /* Last RSSI (if applicable) */
} mesh_status_t;

/**
 * Callback for received mesh messages
 */
typedef void (*mesh_msg_callback_t)(uint16_t src_addr,
                                     mesh_msg_type_t type,
                                     const uint8_t *payload,
                                     size_t len);

/**
 * Initialize the mesh HID subsystem
 *
 * @return 0 on success, negative error code on failure
 */
int mesh_hid_init(void);

/**
 * Check if this node is provisioned into a mesh network
 *
 * @return true if provisioned
 */
bool mesh_hid_is_provisioned(void);

/**
 * Check if this node is the mesh founder (created the network)
 *
 * @return true if founder
 */
bool mesh_hid_is_founder(void);

/**
 * Check if app key is bound to vendor model
 *
 * @return true if bound
 */
bool mesh_hid_app_key_bound(void);

/**
 * Get this node's mesh address
 *
 * @return Mesh address, or 0 if not provisioned
 */
uint16_t mesh_hid_get_addr(void);

/**
 * Send a discovery broadcast to find nearby nodes (request/response mode)
 *
 * @return 0 on success
 */
int mesh_hid_send_discovery(void);

/**
 * Send a beacon broadcast to announce presence (one-way, no response)
 * More reliable than discovery since there's no response collision
 *
 * @return 0 on success
 */
int mesh_hid_send_beacon(void);

/**
 * Send a ping to a specific node
 *
 * @param dst_addr  Destination mesh address
 * @return 0 on success
 */
int mesh_hid_send_ping(uint16_t dst_addr);

/**
 * Send a text message to a specific node
 *
 * @param dst_addr  Destination mesh address (0xFFFF for broadcast)
 * @param text      Text message (max 60 chars)
 * @return 0 on success
 */
int mesh_hid_send_text(uint16_t dst_addr, const char *text);

/**
 * Send HID keyboard type command (types a string)
 *
 * @param dst_addr  Destination mesh address
 * @param text      ASCII text to type (max 58 chars)
 * @return 0 on success
 */
int mesh_hid_send_keyboard_type(uint16_t dst_addr, const char *text);

/**
 * Send HID keyboard tap command
 *
 * @param dst_addr  Destination mesh address
 * @param keycode   HID keycode
 * @param modifiers Modifier flags (shift, ctrl, etc)
 * @return 0 on success
 */
int mesh_hid_send_keyboard_tap(uint16_t dst_addr, uint8_t keycode, uint8_t modifiers);

/**
 * Send HID mouse move command
 *
 * @param dst_addr  Destination mesh address
 * @param dx        X movement (-32768 to 32767)
 * @param dy        Y movement (-32768 to 32767)
 * @return 0 on success
 */
int mesh_hid_send_mouse_move(uint16_t dst_addr, int16_t dx, int16_t dy);

/**
 * Send HID mouse click command
 *
 * @param dst_addr  Destination mesh address
 * @param buttons   Button mask (1=left, 2=right, 4=middle)
 * @return 0 on success
 */
int mesh_hid_send_mouse_click(uint16_t dst_addr, uint8_t buttons);

/**
 * Send HID consumer key command
 *
 * @param dst_addr  Destination mesh address
 * @param usage_id  Consumer control usage ID
 * @return 0 on success
 */
int mesh_hid_send_consumer(uint16_t dst_addr, uint16_t usage_id);

/**
 * Send a message to a specific mesh node
 *
 * @param dst_addr  Destination mesh address
 * @param type      Message type
 * @param payload   Message payload
 * @param len       Payload length
 * @return 0 on success
 */
int mesh_hid_send_msg(uint16_t dst_addr, mesh_msg_type_t type,
                      const uint8_t *payload, size_t len);

/**
 * Broadcast a message to all nodes
 *
 * @param type      Message type
 * @param payload   Message payload
 * @param len       Payload length
 * @return 0 on success
 */
int mesh_hid_broadcast(mesh_msg_type_t type,
                       const uint8_t *payload, size_t len);

/**
 * Register callback for received mesh messages
 *
 * @param callback  Function to call on message receipt
 */
void mesh_hid_set_callback(mesh_msg_callback_t callback);

/**
 * Start scanning for unprovisioned nodes (provisioner mode)
 *
 * @return 0 on success
 */
int mesh_hid_scan_unprovisioned(void);

/**
 * Approve an unprovisioned node to join the mesh
 *
 * @param uuid  UUID of the node to approve
 * @return 0 on success
 */
int mesh_hid_approve_node(const uint8_t *uuid);

/**
 * Self-provision as mesh founder (creates new network)
 * Used when no existing mesh is found
 *
 * @return 0 on success
 */
int mesh_hid_self_provision(void);

/**
 * Get mesh status for reporting
 *
 * @param status  Pointer to status struct to fill
 */
void mesh_hid_get_status(mesh_status_t *status);

/**
 * Reset mesh provisioning (clears NVS, requires reboot)
 */
void mesh_hid_reset(void);

/**
 * Ensure app key is bound (call after loading from NVS)
 *
 * @return 0 on success
 */
int mesh_hid_ensure_app_key(void);

/**
 * Get number of known peers
 *
 * @return Number of peers in list
 */
int mesh_hid_get_peer_count(void);

/**
 * Get peer info by index
 *
 * @param index  Peer index (0-based)
 * @param peer   Output peer struct
 * @return 0 on success, -ENOENT if not found
 */
int mesh_hid_get_peer(int index, mesh_peer_t *peer);

/**
 * Get peer info by address
 *
 * @param addr   Mesh address
 * @param peer   Output peer struct
 * @return 0 on success, -ENOENT if not found
 */
int mesh_hid_get_peer_by_addr(uint16_t addr, mesh_peer_t *peer);

/**
 * Set this node's name
 *
 * @param name  Node name (max 15 chars)
 */
void mesh_hid_set_name(const char *name);

/**
 * Get this node's name
 *
 * @return Node name
 */
const char *mesh_hid_get_name(void);

/**
 * Clear peer list
 */
void mesh_hid_clear_peers(void);

/**
 * Send PIN authentication request to a peer
 *
 * @param dst_addr  Destination mesh address
 * @param pin       PIN string
 * @return 0 on success
 */
int mesh_hid_send_pin(uint16_t dst_addr, const char *pin);

/**
 * Check if a peer is authenticated for HID commands
 *
 * @param addr  Peer mesh address
 * @return true if authenticated
 */
bool mesh_hid_is_peer_authenticated(uint16_t addr);

/**
 * Enable/disable PIN requirement for HID commands
 *
 * @param required  true to require PIN auth
 */
void mesh_hid_set_pin_required(bool required);

/**
 * Check if PIN is required for HID commands
 *
 * @return true if PIN auth is required
 */
bool mesh_hid_pin_required(void);

/**
 * Initiate key exchange with a peer
 * Sends random challenge and derives session key using PIN
 *
 * @param dst_addr  Destination mesh address
 * @return 0 on success
 */
int mesh_hid_key_exchange(uint16_t dst_addr);

/**
 * Check if peer has an established session key
 *
 * @param addr  Peer mesh address
 * @return true if session key exists
 */
bool mesh_hid_has_session_key(uint16_t addr);

/**
 * Clear session key for a peer
 *
 * @param addr  Peer mesh address
 */
void mesh_hid_clear_session_key(uint16_t addr);

/**
 * Send encrypted text message
 *
 * @param dst_addr  Destination mesh address
 * @param text      Text message (max 48 chars due to overhead)
 * @return 0 on success, -ENOENT if no session key
 */
int mesh_hid_send_text_encrypted(uint16_t dst_addr, const char *text);

/**
 * Send encrypted HID keyboard type command
 *
 * @param dst_addr  Destination mesh address
 * @param text      ASCII text to type (max 46 chars due to overhead)
 * @return 0 on success, -ENOENT if no session key
 */
int mesh_hid_send_keyboard_type_encrypted(uint16_t dst_addr, const char *text);

/**
 * Send encrypted HID keyboard tap command
 *
 * @param dst_addr  Destination mesh address
 * @param keycode   HID keycode
 * @param modifiers Modifier flags
 * @return 0 on success, -ENOENT if no session key
 */
int mesh_hid_send_keyboard_tap_encrypted(uint16_t dst_addr, uint8_t keycode, uint8_t modifiers);

/**
 * Send encrypted HID mouse move command
 *
 * @param dst_addr  Destination mesh address
 * @param dx        X movement
 * @param dy        Y movement
 * @return 0 on success, -ENOENT if no session key
 */
int mesh_hid_send_mouse_move_encrypted(uint16_t dst_addr, int16_t dx, int16_t dy);

/**
 * Send encrypted HID mouse click command
 *
 * @param dst_addr  Destination mesh address
 * @param buttons   Button mask
 * @return 0 on success, -ENOENT if no session key
 */
int mesh_hid_send_mouse_click_encrypted(uint16_t dst_addr, uint8_t buttons);

/**
 * Send encrypted HID consumer key command
 *
 * @param dst_addr  Destination mesh address
 * @param usage_id  Consumer control usage ID
 * @return 0 on success, -ENOENT if no session key
 */
int mesh_hid_send_consumer_encrypted(uint16_t dst_addr, uint16_t usage_id);

/**
 * Enable/disable encryption requirement for HID commands
 *
 * @param required  true to reject non-encrypted HID commands
 */
void mesh_hid_set_encryption_required(bool required);

/**
 * Check if encryption is required for HID commands
 *
 * @return true if encryption is required
 */
bool mesh_hid_encryption_required(void);

/**
 * Send GPIO LED set command to a remote node (plaintext, requires auth)
 *
 * @param dst_addr  Destination mesh address
 * @param led_id    LED index (0-based)
 * @param on        true to turn on, false to turn off
 * @return 0 on success
 */
int mesh_hid_send_gpio_led(uint16_t dst_addr, uint8_t led_id, bool on);

/**
 * Send GPIO LED toggle command to a remote node (plaintext, requires auth)
 *
 * @param dst_addr  Destination mesh address
 * @param led_id    LED index (0-based)
 * @return 0 on success
 */
int mesh_hid_send_gpio_toggle(uint16_t dst_addr, uint8_t led_id);

/**
 * Send GPIO LED blink command to a remote node (plaintext, requires auth)
 *
 * @param dst_addr  Destination mesh address
 * @param led_id    LED index (0-based)
 * @param count     Number of blinks
 * @return 0 on success
 */
int mesh_hid_send_gpio_blink(uint16_t dst_addr, uint8_t led_id, uint8_t count);

/**
 * Send encrypted GPIO LED set command to a remote node
 *
 * @param dst_addr  Destination mesh address
 * @param led_id    LED index (0-based)
 * @param on        true to turn on, false to turn off
 * @return 0 on success, -ENOENT if no session key
 */
int mesh_hid_send_gpio_led_encrypted(uint16_t dst_addr, uint8_t led_id, bool on);

/**
 * Send encrypted GPIO LED toggle command to a remote node
 *
 * @param dst_addr  Destination mesh address
 * @param led_id    LED index (0-based)
 * @return 0 on success, -ENOENT if no session key
 */
int mesh_hid_send_gpio_toggle_encrypted(uint16_t dst_addr, uint8_t led_id);

/**
 * Send encrypted GPIO LED blink command to a remote node
 *
 * @param dst_addr  Destination mesh address
 * @param led_id    LED index (0-based)
 * @param count     Number of blinks
 * @return 0 on success, -ENOENT if no session key
 */
int mesh_hid_send_gpio_blink_encrypted(uint16_t dst_addr, uint8_t led_id, uint8_t count);

/* ============ RC PWM Control Functions ============ */

/**
 * Send RC vector control command (plaintext, requires auth)
 * Chip applies mixing based on configured drive mode
 *
 * @param dst_addr  Destination mesh address
 * @param x         X axis value (-1000 to +1000)
 * @param y         Y axis value (-1000 to +1000)
 * @return 0 on success
 */
int mesh_hid_send_rc_vector(uint16_t dst_addr, int16_t x, int16_t y);

/**
 * Send encrypted RC vector control command
 *
 * @param dst_addr  Destination mesh address
 * @param x         X axis value (-1000 to +1000)
 * @param y         Y axis value (-1000 to +1000)
 * @return 0 on success, -ENOENT if no session key
 */
int mesh_hid_send_rc_vector_encrypted(uint16_t dst_addr, int16_t x, int16_t y);

/**
 * Send RC configuration command (sets drive mode and inversion)
 *
 * @param dst_addr  Destination mesh address
 * @param mode      Drive mode (MESH_RC_MODE_*)
 * @param invert    Inversion flags (MESH_RC_INVERT_*)
 * @return 0 on success
 */
int mesh_hid_send_rc_config(uint16_t dst_addr, uint8_t mode, uint8_t invert);

/**
 * Set local RC drive mode
 *
 * @param mode  Drive mode (MESH_RC_MODE_*)
 */
void mesh_hid_set_rc_mode(uint8_t mode);

/**
 * Get current RC drive mode
 *
 * @return Current drive mode
 */
uint8_t mesh_hid_get_rc_mode(void);

/**
 * Set local RC channel inversion
 *
 * @param invert  Inversion flags (MESH_RC_INVERT_*)
 */
void mesh_hid_set_rc_invert(uint8_t invert);

/**
 * Get current RC channel inversion flags
 *
 * @return Current inversion flags
 */
uint8_t mesh_hid_get_rc_invert(void);

/**
 * Set periodic discovery interval
 *
 * @param interval_ms  Interval in milliseconds (0 to disable)
 */
void mesh_hid_set_discovery_interval(uint32_t interval_ms);

/**
 * Get current periodic discovery interval
 *
 * @return Interval in milliseconds
 */
uint32_t mesh_hid_get_discovery_interval(void);

/**
 * Enable/disable periodic discovery
 *
 * @param enabled  true to enable, false to disable
 */
void mesh_hid_set_periodic_discovery(bool enabled);

/**
 * Check if periodic discovery is enabled
 *
 * @return true if enabled
 */
bool mesh_hid_periodic_discovery_enabled(void);

/**
 * Check if a peer is stale (not seen recently)
 *
 * @param addr  Peer mesh address
 * @return true if stale, false if active or unknown
 */
bool mesh_hid_is_peer_stale(uint16_t addr);

/**
 * Get peer's last seen time in ms since boot
 *
 * @param addr  Peer mesh address
 * @return last_seen timestamp, or 0 if not found
 */
uint32_t mesh_hid_get_peer_last_seen(uint16_t addr);

#endif /* MESH_HID_H */

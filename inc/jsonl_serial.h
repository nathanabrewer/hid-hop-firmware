/*
 * JSONL Serial Interface for HID-HOP
 *
 * Provides bidirectional JSONL messaging over USB CDC ACM.
 * Routes messages between serial port and BLE.
 *
 * Copyright (c) 2025 Nathan Brewer
 */

#ifndef JSONL_SERIAL_H
#define JSONL_SERIAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Maximum JSONL line length */
#define JSONL_MAX_LINE_LEN 256

/* Message targets */
#define JSONL_TARGET_LOCAL   "local"   /* This device */
#define JSONL_TARGET_PHONE   "phone"   /* Connected BLE phone */
#define JSONL_TARGET_MESH    "mesh"    /* Future: mesh broadcast */

/**
 * Message types for routing
 */
typedef enum {
    JSONL_MSG_CMD,      /* Command to execute */
    JSONL_MSG_EVENT,    /* Event notification */
    JSONL_MSG_RESPONSE, /* Response to command */
    JSONL_MSG_ERROR,    /* Error response */
} jsonl_msg_type_t;

/**
 * Callback for messages received from serial destined for BLE
 *
 * @param json_line  The raw JSON line (null-terminated)
 * @param len        Length of the JSON line
 */
typedef void (*jsonl_to_ble_callback_t)(const char *json_line, size_t len);

/**
 * Initialize the JSONL serial interface
 *
 * @return 0 on success, negative error code on failure
 */
int jsonl_serial_init(void);

/**
 * Register callback for messages destined for BLE/phone
 *
 * @param callback  Function to call when serial sends to phone
 */
void jsonl_serial_set_ble_callback(jsonl_to_ble_callback_t callback);

/**
 * Send a message from BLE/phone to serial
 * Called when phone sends a message that should appear on serial.
 *
 * @param json_line  The JSON line to send (will add newline)
 * @param len        Length of the JSON string
 * @return true on success
 */
bool jsonl_serial_send_from_ble(const char *json_line, size_t len);

/**
 * Send an event to serial
 *
 * @param event_type  Event type string (e.g., "connected", "hid", "sensor")
 * @param json_data   Additional JSON data (without outer braces), can be NULL
 * @return true on success
 */
bool jsonl_serial_send_event(const char *event_type, const char *json_data);

/**
 * Send an error to serial
 *
 * @param error_msg  Error message
 * @param cmd        Original command that caused error, can be NULL
 * @return true on success
 */
bool jsonl_serial_send_error(const char *error_msg, const char *cmd);

/**
 * Check if serial port is connected (DTR asserted by host)
 *
 * @return true if host has opened the serial port
 */
bool jsonl_serial_is_connected(void);

/**
 * Enable or disable the serial interface
 * When disabled, no data is sent/received.
 *
 * @param enabled  true to enable, false to disable
 */
void jsonl_serial_set_enabled(bool enabled);

/**
 * Check if serial interface is enabled
 *
 * @return true if enabled
 */
bool jsonl_serial_is_enabled(void);

/**
 * Process pending serial data (call from main loop or work queue)
 * Parses complete JSONL lines and dispatches them.
 */
void jsonl_serial_process(void);

/**
 * Send mesh event to serial (called from mesh callback)
 *
 * @param src_addr    Source mesh address
 * @param event_type  Event type string (e.g., "mesh_discovered", "mesh_hid")
 * @param extra_data  Additional JSON data (without outer braces), can be NULL
 */
void jsonl_serial_send_mesh_event(uint16_t src_addr, const char *event_type,
                                   const char *extra_data);

#endif /* JSONL_SERIAL_H */

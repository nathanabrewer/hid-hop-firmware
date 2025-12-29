/*
 * Brewer BLE HID Bridge - BLE GATT Service
 */

#ifndef BLE_HID_SERVICE_H
#define BLE_HID_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Custom service UUID: f8b34000-XXXX-XXXX-XXXX-XXXXXXXXXXXX */
#define BLE_HID_SERVICE_UUID \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0xf8b34000, 0x6e8b, 0x4b5a, 0x9f3e, 0x2c1d4a8e7f00))

/* Command characteristic - write to send commands */
#define BLE_HID_CMD_CHAR_UUID \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0xf8b34001, 0x6e8b, 0x4b5a, 0x9f3e, 0x2c1d4a8e7f00))

/* Response characteristic - notify for responses */
#define BLE_HID_RESP_CHAR_UUID \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0xf8b34002, 0x6e8b, 0x4b5a, 0x9f3e, 0x2c1d4a8e7f00))

/**
 * Callback type for received commands
 * @param data Command data
 * @param length Data length
 */
typedef void (*ble_hid_cmd_callback_t)(const uint8_t *data, size_t length);

/**
 * Initialize BLE subsystem and start advertising
 * @param cmd_callback Callback for received commands
 * @return true on success
 */
bool ble_hid_service_init(ble_hid_cmd_callback_t cmd_callback);

/**
 * Send response/notification to connected device
 * @param data Response data
 * @param length Data length
 * @return true on success
 */
bool ble_hid_service_send_response(const uint8_t *data, size_t length);

/**
 * Check if a device is connected
 * @return true if connected
 */
bool ble_hid_service_is_connected(void);

/**
 * Get connection RSSI (signal strength)
 * @return RSSI value or 0 if not connected
 */
int8_t ble_hid_service_get_rssi(void);

/**
 * Disconnect from current device
 */
void ble_hid_service_disconnect(void);

/**
 * Update the BLE device name
 * Note: Takes effect on next advertising cycle (after disconnect)
 * @param name New device name (max 20 chars)
 * @return true on success
 */
bool ble_hid_service_set_name(const char *name);

/**
 * Send a JSONL message to the connected phone
 * Used for routing serial messages to BLE
 * @param json_line The JSON line to send (null-terminated)
 * @param len Length of the JSON string
 * @return true on success
 */
bool ble_hid_service_send_jsonl(const char *json_line, size_t len);

#endif /* BLE_HID_SERVICE_H */

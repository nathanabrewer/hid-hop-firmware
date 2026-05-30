/*
 * Brewer BLE HID Bridge - BLE GATT Service Implementation
 *
 * Implements a custom GATT service for receiving HID commands
 * from the iOS app and sending responses.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>

#include "ble_hid_service.h"
#include "config.h"
#include "hid_keyboard.h"
#include "security.h"

LOG_MODULE_REGISTER(ble_hid_service, LOG_LEVEL_INF);

/* Buffer for dynamic device name */
static char device_name_buf[MAX_DEVICE_NAME_LENGTH + 1];

/* Service and characteristic UUIDs */
static struct bt_uuid_128 hid_service_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0xf8b34000, 0x6e8b, 0x4b5a, 0x9f3e, 0x2c1d4a8e7f00));

static struct bt_uuid_128 cmd_char_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0xf8b34001, 0x6e8b, 0x4b5a, 0x9f3e, 0x2c1d4a8e7f00));

static struct bt_uuid_128 resp_char_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0xf8b34002, 0x6e8b, 0x4b5a, 0x9f3e, 0x2c1d4a8e7f00));

/* Keyboard LED state characteristic - notifications for NumLock/CapsLock/ScrollLock */
static struct bt_uuid_128 kbd_leds_char_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0xf8b34003, 0x6e8b, 0x4b5a, 0x9f3e, 0x2c1d4a8e7f00));

/* State */
static ble_hid_cmd_callback_t cmd_callback = NULL;
static struct bt_conn *current_conn = NULL;
static bool resp_notifications_enabled = false;
static bool kbd_leds_notifications_enabled = false;
static uint8_t last_kbd_led_state = 0xFF;  /* Invalid initial value to force first notification */
static bool initialized = false;

/*
 * NOTE: Advertising data is built dynamically in start_advertising_with_name()
 * to include the device name in BOTH ad data AND scan response.
 * This ensures iOS picks up the correct name (iOS caches BLE names aggressively).
 */

/* Forward declarations */
static int start_advertising_with_name(void);
static ssize_t cmd_write_handler(struct bt_conn *conn,
                                  const struct bt_gatt_attr *attr,
                                  const void *buf, uint16_t len,
                                  uint16_t offset, uint8_t flags);
static void resp_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
static void kbd_leds_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t kbd_leds_read_handler(struct bt_conn *conn,
                                      const struct bt_gatt_attr *attr,
                                      void *buf, uint16_t len, uint16_t offset);

/* GATT Service Definition */
BT_GATT_SERVICE_DEFINE(hid_bridge_svc,
    /* Primary Service */
    BT_GATT_PRIMARY_SERVICE(&hid_service_uuid),

    /* Command Characteristic - Write without response for low latency */
    /* SECURITY: TODO - Re-enable BT_GATT_PERM_WRITE_ENCRYPT after debugging */
    /* Temporarily using BT_GATT_PERM_WRITE for debugging */
    BT_GATT_CHARACTERISTIC(&cmd_char_uuid.uuid,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE,
                           NULL, cmd_write_handler, NULL),

    /* Response Characteristic - Notify for async responses */
    BT_GATT_CHARACTERISTIC(&resp_char_uuid.uuid,
                           BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE,
                           NULL, NULL, NULL),
    BT_GATT_CCC(resp_ccc_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* Keyboard LED State Characteristic - Read + Notify for NumLock/CapsLock/ScrollLock */
    BT_GATT_CHARACTERISTIC(&kbd_leds_char_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ,
                           kbd_leds_read_handler, NULL, NULL),
    BT_GATT_CCC(kbd_leds_ccc_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/**
 * Handle writes to command characteristic
 */
static ssize_t cmd_write_handler(struct bt_conn *conn,
                                  const struct bt_gatt_attr *attr,
                                  const void *buf, uint16_t len,
                                  uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(attr);
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);

    LOG_INF(">>> CMD RECEIVED: %u bytes", len);

    /* Log first few bytes for debugging */
    const uint8_t *data = (const uint8_t *)buf;
    if (len >= 2) {
        LOG_INF("    CMD type=0x%02X, len=%u", data[0], data[1]);
    }

    if (cmd_callback != NULL) {
        cmd_callback(buf, len);
    } else {
        LOG_WRN("    No callback registered!");
    }

    return len;
}

/**
 * Handle CCC changes for response characteristic
 */
static void resp_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);

    resp_notifications_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Response notifications %s",
            resp_notifications_enabled ? "enabled" : "disabled");
}

/**
 * Handle reads of keyboard LED state characteristic
 */
static ssize_t kbd_leds_read_handler(struct bt_conn *conn,
                                      const struct bt_gatt_attr *attr,
                                      void *buf, uint16_t len, uint16_t offset)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);

    uint8_t led_state = hid_keyboard_get_led_state();
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &led_state, sizeof(led_state));
}

/**
 * Handle CCC changes for keyboard LED state characteristic
 */
static void kbd_leds_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);

    kbd_leds_notifications_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Keyboard LED notifications %s",
            kbd_leds_notifications_enabled ? "enabled" : "disabled");

    /* Send initial state when notifications are enabled */
    if (kbd_leds_notifications_enabled && current_conn != NULL) {
        uint8_t led_state = hid_keyboard_get_led_state();
        last_kbd_led_state = led_state;

        /*
         * GATT attribute layout for kbd_leds characteristic:
         * [0] Primary Service declaration
         * [1] Command characteristic declaration
         * [2] Command characteristic value
         * [3] Response characteristic declaration
         * [4] Response characteristic value
         * [5] Response CCC descriptor
         * [6] Keyboard LED characteristic declaration
         * [7] Keyboard LED characteristic value  <-- This is what we need
         * [8] Keyboard LED CCC descriptor
         */
        const struct bt_gatt_attr *led_attr = &hid_bridge_svc.attrs[7];

        int err = bt_gatt_notify(current_conn, led_attr, &led_state, sizeof(led_state));
        if (err) {
            LOG_ERR("Failed to send initial LED state (err %d)", err);
        } else {
            LOG_INF("Sent initial LED state: Num=%d Caps=%d Scroll=%d",
                    (led_state >> 0) & 0x01,
                    (led_state >> 1) & 0x01,
                    (led_state >> 2) & 0x01);
        }
    }
}

/**
 * Connection callback
 */
static void connected_cb(struct bt_conn *conn, uint8_t err)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (err) {
        LOG_ERR("Connection failed (err %u): %s", err, addr);
        return;
    }

    LOG_INF("Connected: %s", addr);
    current_conn = bt_conn_ref(conn);

    /* SECURITY: TODO - Re-enable after debugging */
    /* Temporarily disabled security level request for debugging */
    #if 0
    int sec_err = bt_conn_set_security(conn, BT_SECURITY_L2);
    if (sec_err) {
        LOG_ERR("Failed to request security level 2: %d", sec_err);
    }
    #endif

    /* Request higher connection parameters for better latency */
    struct bt_le_conn_param param = {
        .interval_min = 6,   /* 7.5ms */
        .interval_max = 12,  /* 15ms */
        .latency = 0,
        .timeout = 400,      /* 4s */
    };
    bt_conn_le_param_update(conn, &param);
}

/**
 * Disconnection callback
 */
static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Disconnected: %s (reason %u)", addr, reason);

    if (current_conn == conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }

    resp_notifications_enabled = false;
    kbd_leds_notifications_enabled = false;

    /* End security session on disconnect - requires re-auth on reconnect */
    security_end_session();

    /* Restart advertising with current name in AD and scan response */
    start_advertising_with_name();
}

/**
 * Security changed callback
 * SECURITY: Disconnect if encryption not established
 */
static void security_changed_cb(struct bt_conn *conn, bt_security_t level,
                                 enum bt_security_err err)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (err) {
        LOG_WRN("Security failed: %s (err %d) - disconnecting", addr, err);
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
        return;
    }

    LOG_INF("Security changed: %s level %u", addr, level);

    /* SECURITY: TODO - Re-enable security level check after debugging */
    /* Temporarily disabled disconnection for low security level */
    #if 0
    if (level < BT_SECURITY_L2) {
        LOG_WRN("Insufficient security level %d - disconnecting", level);
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
    }
    #endif
}

/* Connection callbacks struct */
BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected_cb,
    .disconnected = disconnected_cb,
    .security_changed = security_changed_cb,
};

/**
 * Pairing callbacks
 */
static void auth_cancel(struct bt_conn *conn)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Pairing cancelled: %s", addr);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Pairing complete: %s, bonded: %d", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_WRN("Pairing failed: %s, reason %d", addr, reason);
}

static struct bt_conn_auth_cb auth_cb = {
    .cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb auth_info_cb = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed,
};

/**
 * Start advertising with the current device name.
 * Includes name in BOTH advertising data AND scan response for iOS compatibility.
 * iOS caches names aggressively, so we include the name in ad data where iOS
 * is more likely to pick it up fresh.
 */
static int start_advertising_with_name(void)
{
    size_t name_len = strlen(device_name_buf);

    /* Safety: if name is empty, use default */
    if (name_len == 0) {
        LOG_WRN("Empty device name, using default");
        strncpy(device_name_buf, CONFIG_BT_DEVICE_NAME, MAX_DEVICE_NAME_LENGTH);
        device_name_buf[MAX_DEVICE_NAME_LENGTH] = '\0';
        name_len = strlen(device_name_buf);
    }

    /*
     * Build advertising data with flags and name
     * AD packet has limited space (~31 bytes), so we need to be careful.
     * Flags = 3 bytes, Name = 2 + name_len bytes
     */
    struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
        BT_DATA(BT_DATA_NAME_COMPLETE, device_name_buf, name_len),
    };

    /*
     * Build scan response with service UUID and name again
     * Having the name in both places maximizes iOS pickup
     */
    struct bt_data sd[] = {
        BT_DATA_BYTES(BT_DATA_UUID128_ALL,
            BT_UUID_128_ENCODE(0xf8b34000, 0x6e8b, 0x4b5a, 0x9f3e, 0x2c1d4a8e7f00)),
        BT_DATA(BT_DATA_NAME_COMPLETE, device_name_buf, name_len),
    };

    int err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        LOG_ERR("Failed to start advertising (err %d)", err);
    } else {
        LOG_INF("Advertising started with name '%s' in AD and SD", device_name_buf);
    }

    return err;
}

/**
 * Initialize BLE subsystem
 */
bool ble_hid_service_init(ble_hid_cmd_callback_t callback)
{
    int err;

    if (initialized) {
        LOG_WRN("BLE already initialized");
        return true;
    }

    cmd_callback = callback;

    /* Enable Bluetooth */
    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return false;
    }

    LOG_INF("Bluetooth initialized");

    /* Load settings (bonding info) */
    if (IS_ENABLED(CONFIG_SETTINGS)) {
        settings_load();
    }

    /* Get saved device name from config and apply it */
    uint8_t name_len = config_get_name(device_name_buf);
    LOG_INF("Got name from config: '%s' (len=%d)", device_name_buf, name_len);

    if (name_len > 0) {
        err = bt_set_name(device_name_buf);
        if (err) {
            LOG_ERR("bt_set_name failed (err %d)", err);
        } else {
            LOG_INF("bt_set_name SUCCESS: '%s'", device_name_buf);
        }
    } else {
        LOG_WRN("No name from config, using default");
        strncpy(device_name_buf, CONFIG_BT_DEVICE_NAME, MAX_DEVICE_NAME_LENGTH);
    }

    /* Register auth callbacks */
    err = bt_conn_auth_cb_register(&auth_cb);
    if (err) {
        LOG_ERR("Failed to register auth callbacks (err %d)", err);
        return false;
    }

    err = bt_conn_auth_info_cb_register(&auth_info_cb);
    if (err) {
        LOG_ERR("Failed to register auth info callbacks (err %d)", err);
        return false;
    }

    /* Start advertising with the device name in both AD and scan response */
    err = start_advertising_with_name();
    if (err) {
        return false;
    }

    initialized = true;
    return true;
}

/**
 * Send response to connected device
 */
bool ble_hid_service_send_response(const uint8_t *data, size_t length)
{
    if (!initialized || current_conn == NULL) {
        LOG_DBG("Cannot send response: not ready");
        return false;
    }

    if (!resp_notifications_enabled) {
        LOG_DBG("Response notifications not enabled");
        return false;
    }

    /*
     * GATT attribute layout:
     * [0] Primary Service declaration
     * [1] Command characteristic declaration
     * [2] Command characteristic value
     * [3] Response characteristic declaration
     * [4] Response characteristic value  <-- This is what we need
     * [5] Response CCC descriptor
     */
    const struct bt_gatt_attr *attr = &hid_bridge_svc.attrs[4];

    int err = bt_gatt_notify(current_conn, attr, data, length);
    if (err) {
        LOG_ERR("Failed to send notification (err %d)", err);
        return false;
    }

    LOG_DBG("Response sent: %zu bytes", length);
    return true;
}

/**
 * Check if connected
 */
bool ble_hid_service_is_connected(void)
{
    return current_conn != NULL;
}

/**
 * Get connection RSSI
 */
int8_t ble_hid_service_get_rssi(void)
{
    if (current_conn == NULL) {
        return 0;
    }

    /* Note: Getting RSSI requires vendor-specific implementation on nRF */
    return 0;
}

/**
 * Disconnect from current device
 */
void ble_hid_service_disconnect(void)
{
    if (current_conn != NULL) {
        bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

/**
 * Update the BLE device name
 * Updates immediately if not connected, otherwise takes effect after disconnect
 */
bool ble_hid_service_set_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    int err = bt_set_name(name);
    if (err) {
        LOG_ERR("Failed to set BLE name (err %d)", err);
        return false;
    }

    /* Store in our buffer for advertising */
    strncpy(device_name_buf, name, MAX_DEVICE_NAME_LENGTH);
    device_name_buf[MAX_DEVICE_NAME_LENGTH] = '\0';

    LOG_INF("BLE name set to: '%s'", name);

    /* If not connected, restart advertising with new name immediately */
    if (current_conn == NULL) {
        bt_le_adv_stop();
        err = start_advertising_with_name();
        if (err) {
            return false;
        }
    } else {
        LOG_INF("Name will take effect after disconnect");
    }

    return true;
}

/**
 * Send a JSONL message to the connected phone
 * Used for routing serial messages to BLE
 */
bool ble_hid_service_send_jsonl(const char *json_line, size_t len)
{
    if (!initialized || current_conn == NULL) {
        LOG_DBG("Cannot send JSONL: not connected");
        return false;
    }

    if (!resp_notifications_enabled) {
        LOG_DBG("Cannot send JSONL: notifications not enabled");
        return false;
    }

    /* Send as notification on response characteristic */
    const struct bt_gatt_attr *attr = &hid_bridge_svc.attrs[4];

    int err = bt_gatt_notify(current_conn, attr, json_line, len);
    if (err) {
        LOG_ERR("Failed to send JSONL notification (err %d)", err);
        return false;
    }

    LOG_DBG("JSONL sent to phone: %zu bytes", len);
    return true;
}

/**
 * Check and notify keyboard LED state changes
 * Call this periodically from main loop to send notifications on state change
 * Returns true if a notification was sent
 */
bool ble_hid_service_check_kbd_leds(void)
{
    if (!initialized || current_conn == NULL || !kbd_leds_notifications_enabled) {
        return false;
    }

    uint8_t current_state = hid_keyboard_get_led_state();

    /* Only notify if state changed */
    if (current_state == last_kbd_led_state) {
        return false;
    }

    last_kbd_led_state = current_state;

    /* Send notification */
    const struct bt_gatt_attr *led_attr = &hid_bridge_svc.attrs[7];

    int err = bt_gatt_notify(current_conn, led_attr, &current_state, sizeof(current_state));
    if (err) {
        LOG_ERR("Failed to notify LED state change (err %d)", err);
        return false;
    }

    LOG_INF("LED state changed: Num=%d Caps=%d Scroll=%d",
            (current_state >> 0) & 0x01,
            (current_state >> 1) & 0x01,
            (current_state >> 2) & 0x01);

    return true;
}

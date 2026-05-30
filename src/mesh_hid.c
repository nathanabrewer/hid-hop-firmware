/*
 * BLE Mesh HID Bridge for HID-HOP
 *
 * Copyright (c) 2025 Nathan Brewer
 */

#include "mesh_hid.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/bluetooth/mesh/cfg_cli.h>
#include <zephyr/settings/settings.h>
#include <zephyr/random/random.h>
#include <string.h>

/* TinyCrypt for E2E encryption */
#include <tinycrypt/aes.h>
#include <tinycrypt/ccm_mode.h>
#include <tinycrypt/sha256.h>
#include <tinycrypt/constants.h>

#include "config.h"
#include "hid_keyboard.h"
#include "hid_mouse.h"
#include "hid_consumer.h"
#include "gpio_control.h"
#include "protocol.h"

LOG_MODULE_REGISTER(mesh_hid, LOG_LEVEL_INF);

/* Settings key for mesh node name */
#define SETTINGS_MESH_NAME "mesh/name"

/* Forward declarations for serial events */
extern void jsonl_serial_send_event(const char *event_type, const char *json_data);

/* Mesh configuration */
#define MESH_TTL_DEFAULT        5
#define MESH_GROUP_ADDR         0xC000  /* All HID-HOP nodes */

/* Static network/app keys for self-provisioning */
static const uint8_t net_key[16] = {
    0x48, 0x49, 0x44, 0x2D, 0x48, 0x4F, 0x50, 0x2D,  /* "HID-HOP-" */
    0x4E, 0x45, 0x54, 0x4B, 0x45, 0x59, 0x30, 0x31   /* "NETKEY01" */
};

static const uint8_t app_key[16] = {
    0x48, 0x49, 0x44, 0x2D, 0x48, 0x4F, 0x50, 0x2D,  /* "HID-HOP-" */
    0x41, 0x50, 0x50, 0x4B, 0x45, 0x59, 0x30, 0x31   /* "APPKEY01" */
};

/* State */
static bool initialized = false;
static bool is_founder = false;
static bool app_key_bound = false;
static uint8_t dev_uuid[16];
static mesh_msg_callback_t msg_callback = NULL;

/* Peer tracking */
static mesh_peer_t peers[MESH_MAX_PEERS];
static int peer_count = 0;
static char node_name[MESH_NODE_NAME_LEN] = "HID-HOP";
static bool node_name_loaded = false;

/* PIN authentication */
static bool pin_required = true;  /* Require PIN auth by default */
#define AUTH_TIMEOUT_MS  (30 * 60 * 1000)  /* 30 minute auth timeout */

/* Periodic discovery */
#define DISCOVERY_INTERVAL_DEFAULT_MS  (60 * 1000)  /* 60 seconds */
#define PEER_STALE_TIMEOUT_MS          (180 * 1000) /* 3 minutes without contact = stale */
static uint32_t discovery_interval_ms = DISCOVERY_INTERVAL_DEFAULT_MS;
static bool periodic_discovery_enabled = true;

/* E2E encryption state */
static bool encryption_required = true;  /* Require encryption for HID by default */
static uint8_t local_challenge[16];      /* Our challenge for key exchange */
static bool pending_key_exchange = false;
static uint16_t pending_key_peer = 0;

/* RC PWM control configuration */
static uint8_t rc_drive_mode = MESH_RC_MODE_SKID_STEER;  /* Default: skid steer */
static uint8_t rc_invert_flags = 0;                       /* No inversion by default */

/* Delayed work for app key binding and auto-discovery */
static struct k_work_delayable app_key_work;
static struct k_work_delayable discovery_work;
static void app_key_work_handler(struct k_work *work);
static void discovery_work_handler(struct k_work *work);

/* Forward declarations */
static int discovery_handler(const struct bt_mesh_model *model,
                             struct bt_mesh_msg_ctx *ctx,
                             struct net_buf_simple *buf);
static int discovery_resp_handler(const struct bt_mesh_model *model,
                                   struct bt_mesh_msg_ctx *ctx,
                                   struct net_buf_simple *buf);
static int beacon_handler(const struct bt_mesh_model *model,
                          struct bt_mesh_msg_ctx *ctx,
                          struct net_buf_simple *buf);
static int hid_cmd_handler(const struct bt_mesh_model *model,
                           struct bt_mesh_msg_ctx *ctx,
                           struct net_buf_simple *buf);
static int status_handler(const struct bt_mesh_model *model,
                          struct bt_mesh_msg_ctx *ctx,
                          struct net_buf_simple *buf);
static int text_handler(const struct bt_mesh_model *model,
                        struct bt_mesh_msg_ctx *ctx,
                        struct net_buf_simple *buf);
static int pin_auth_handler(const struct bt_mesh_model *model,
                            struct bt_mesh_msg_ctx *ctx,
                            struct net_buf_simple *buf);
static int pin_resp_handler(const struct bt_mesh_model *model,
                            struct bt_mesh_msg_ctx *ctx,
                            struct net_buf_simple *buf);
static int hid_cmd_enc_handler(const struct bt_mesh_model *model,
                                struct bt_mesh_msg_ctx *ctx,
                                struct net_buf_simple *buf);
static int text_enc_handler(const struct bt_mesh_model *model,
                            struct bt_mesh_msg_ctx *ctx,
                            struct net_buf_simple *buf);
static int key_exchange_handler(const struct bt_mesh_model *model,
                                struct bt_mesh_msg_ctx *ctx,
                                struct net_buf_simple *buf);
static int key_confirm_handler(const struct bt_mesh_model *model,
                               struct bt_mesh_msg_ctx *ctx,
                               struct net_buf_simple *buf);
static int gpio_cmd_handler(const struct bt_mesh_model *model,
                            struct bt_mesh_msg_ctx *ctx,
                            struct net_buf_simple *buf);
static int gpio_cmd_enc_handler(const struct bt_mesh_model *model,
                                struct bt_mesh_msg_ctx *ctx,
                                struct net_buf_simple *buf);
static int rc_vector_handler(const struct bt_mesh_model *model,
                             struct bt_mesh_msg_ctx *ctx,
                             struct net_buf_simple *buf);
static int rc_vector_enc_handler(const struct bt_mesh_model *model,
                                 struct bt_mesh_msg_ctx *ctx,
                                 struct net_buf_simple *buf);
static int rc_config_handler(const struct bt_mesh_model *model,
                             struct bt_mesh_msg_ctx *ctx,
                             struct net_buf_simple *buf);
static void update_peer(uint16_t addr, int8_t rssi, uint8_t caps);
static void update_peer_name(uint16_t addr, const char *name, size_t name_len);
static mesh_peer_t *get_peer_ptr(uint16_t addr);
static int derive_session_key(const char *pin, const uint8_t *our_challenge,
                              const uint8_t *their_challenge, uint8_t *key_out);
static int encrypt_message(mesh_peer_t *peer, const uint8_t *plaintext, size_t plain_len,
                           uint8_t *ciphertext, size_t *cipher_len);
static int decrypt_message(mesh_peer_t *peer, const uint8_t *ciphertext, size_t cipher_len,
                           uint8_t *plaintext, size_t *plain_len);

/* Vendor Model operation handlers */
static const struct bt_mesh_model_op mesh_hid_ops[] = {
    { MESH_HID_OP_DISCOVERY, 0, discovery_handler },
    { MESH_HID_OP_DISCOVERY_RESP, 0, discovery_resp_handler },
    { MESH_HID_OP_BEACON, 0, beacon_handler },  /* One-way presence broadcast */
    { MESH_HID_OP_HID_CMD, 0, hid_cmd_handler },
    { MESH_HID_OP_STATUS, 0, status_handler },
    { MESH_HID_OP_TEXT, 0, text_handler },
    { MESH_HID_OP_PIN_AUTH, 0, pin_auth_handler },
    { MESH_HID_OP_PIN_RESP, 0, pin_resp_handler },
    /* Encrypted message handlers */
    { MESH_HID_OP_HID_CMD_ENC, 0, hid_cmd_enc_handler },
    { MESH_HID_OP_TEXT_ENC, 0, text_enc_handler },
    { MESH_HID_OP_KEY_EXCHANGE, 0, key_exchange_handler },
    { MESH_HID_OP_KEY_CONFIRM, 0, key_confirm_handler },
    /* GPIO command handlers (require PIN auth like HID) */
    { MESH_HID_OP_GPIO_CMD, 0, gpio_cmd_handler },
    { MESH_HID_OP_GPIO_CMD_ENC, 0, gpio_cmd_enc_handler },
    /* RC PWM control handlers */
    { MESH_HID_OP_RC_VECTOR, 0, rc_vector_handler },
    { MESH_HID_OP_RC_VECTOR_ENC, 0, rc_vector_enc_handler },
    { MESH_HID_OP_RC_CONFIG, 0, rc_config_handler },
    BT_MESH_MODEL_OP_END,
};

/* Publication buffer */
NET_BUF_SIMPLE_DEFINE_STATIC(mesh_hid_pub_msg, 64);

/* Publication context for vendor model */
static struct bt_mesh_model_pub mesh_hid_pub = {
    .msg = &mesh_hid_pub_msg,
};

/* Config client instance */
static struct bt_mesh_cfg_cli cfg_cli = {};

/* Model definitions */
static struct bt_mesh_model root_models[] = {
    BT_MESH_MODEL_CFG_SRV,
    BT_MESH_MODEL_CFG_CLI(&cfg_cli),
};

/* Vendor model using standard macro for proper initialization */
static struct bt_mesh_model vendor_models[] = {
    BT_MESH_MODEL_VND_CB(MESH_HID_COMPANY_ID, MESH_HID_MODEL_ID,
                         mesh_hid_ops, &mesh_hid_pub, NULL, NULL),
};

/* Element definition */
static struct bt_mesh_elem elements[] = {
    BT_MESH_ELEM(0, root_models, vendor_models),
};

/* Composition */
static const struct bt_mesh_comp comp = {
    .cid = MESH_HID_COMPANY_ID,
    .elem = elements,
    .elem_count = ARRAY_SIZE(elements),
};

/* Provisioning */
static int output_number(bt_mesh_output_action_t action, uint32_t number)
{
    LOG_INF("OOB Number: %u", number);
    return 0;
}

static void prov_complete(uint16_t net_idx, uint16_t addr)
{
    LOG_INF("Provisioning complete! Net idx: 0x%04x, Addr: 0x%04x", net_idx, addr);
    /* Schedule app key binding now that provisioning is done */
    k_work_schedule(&app_key_work, K_SECONDS(1));
}

static void prov_reset(void)
{
    LOG_INF("Provisioning reset");
    bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);
}

static const struct bt_mesh_prov prov = {
    .uuid = dev_uuid,
    .output_size = 4,
    .output_actions = BT_MESH_DISPLAY_NUMBER,
    .output_number = output_number,
    .complete = prov_complete,
    .reset = prov_reset,
};

/**
 * Generate device UUID from hardware ID
 */
static void generate_uuid(void)
{
    /* Use nRF device ID as base for UUID */
    uint32_t dev_id[2];
    dev_id[0] = NRF_FICR->DEVICEID[0];
    dev_id[1] = NRF_FICR->DEVICEID[1];

    /* Format: HID-HOP-XXXXXXXX */
    memset(dev_uuid, 0, sizeof(dev_uuid));
    dev_uuid[0] = 'H';
    dev_uuid[1] = 'I';
    dev_uuid[2] = 'D';
    dev_uuid[3] = '-';
    memcpy(&dev_uuid[4], dev_id, 8);

    /* Random bytes for uniqueness */
    sys_csrand_get(&dev_uuid[12], 4);
}

/**
 * Handle discovery request
 */
static int discovery_handler(const struct bt_mesh_model *model,
                             struct bt_mesh_msg_ctx *ctx,
                             struct net_buf_simple *buf)
{
    char data[128];

    /* Ignore our own broadcasts */
    if (ctx->addr == mesh_hid_get_addr()) {
        return 0;
    }

    LOG_INF("Discovery request from 0x%04x", ctx->addr);

    /* Track this peer */
    update_peer(ctx->addr, ctx->recv_rssi, 0);

    snprintf(data, sizeof(data),
        "\"from\":\"0x%04x\",\"rssi\":%d,\"net_idx\":%d,\"app_idx\":%d,\"recv_ttl\":%d",
        ctx->addr, ctx->recv_rssi, ctx->net_idx, ctx->app_idx, ctx->recv_ttl);
    jsonl_serial_send_event("discovery_rcvd", data);

    /* Prepare response */
    BT_MESH_MODEL_BUF_DEFINE(msg, MESH_HID_OP_DISCOVERY_RESP,
                             sizeof(mesh_discovery_t));
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_DISCOVERY_RESP);

    static mesh_discovery_t disc;  /* Static to avoid stack overflow */
    memcpy(disc.uuid, dev_uuid, 16);
    strncpy(disc.name, node_name, sizeof(disc.name) - 1);
    disc.name[sizeof(disc.name) - 1] = '\0';
    disc.name_len = strlen(disc.name);
    disc.capabilities = MESH_CAP_RELAY | MESH_CAP_USB_HOST;
    if (is_founder) {
        disc.capabilities |= MESH_CAP_PROVISIONER;
    }
    disc.tx_power = 0;  /* TODO: Get actual TX power */

    net_buf_simple_add_mem(&msg, &disc, sizeof(disc));

    /* Send response */
    struct bt_mesh_msg_ctx reply_ctx = {
        .net_idx = ctx->net_idx,
        .app_idx = ctx->app_idx,
        .addr = ctx->addr,
        .send_ttl = MESH_TTL_DEFAULT,
    };

    int err = bt_mesh_model_send(model, &reply_ctx, &msg, NULL, NULL);
    snprintf(data, sizeof(data), "\"to\":\"0x%04x\",\"err\":%d", ctx->addr, err);
    jsonl_serial_send_event("discovery_resp_sent", data);
    return err;
}

/**
 * Handle discovery response
 */
static int discovery_resp_handler(const struct bt_mesh_model *model,
                                   struct bt_mesh_msg_ctx *ctx,
                                   struct net_buf_simple *buf)
{
    /* Ignore our own responses (shouldn't happen but be safe) */
    if (ctx->addr == mesh_hid_get_addr()) {
        return 0;
    }

    /* DEBUG v2: Print first bytes to identify if this is a misrouted KEY_EXCHANGE */
    char data[192];
    uint8_t b0 = buf->len > 0 ? buf->data[0] : 0;
    uint8_t b1 = buf->len > 1 ? buf->data[1] : 0;
    snprintf(data, sizeof(data), "\"from\":\"0x%04x\",\"rssi\":%d,\"len\":%d,\"b0\":%d,\"b1\":%d,\"v\":2",
        ctx->addr, ctx->recv_rssi, buf->len, b0, b1);
    jsonl_serial_send_event("discovery_resp_rcvd", data);

    if (buf->len < sizeof(mesh_discovery_t)) {
        LOG_WRN("Discovery response too short");
        /* Still track the peer even with short response */
        update_peer(ctx->addr, ctx->recv_rssi, 0);
        return -EINVAL;
    }

    mesh_discovery_t *disc = (mesh_discovery_t *)buf->data;

    /* Track this peer with capabilities and name */
    update_peer(ctx->addr, ctx->recv_rssi, disc->capabilities);
    if (disc->name_len > 0) {
        update_peer_name(ctx->addr, disc->name, disc->name_len);
    }

    LOG_INF("Discovered node 0x%04x: %s (caps=0x%02x)", ctx->addr, disc->name, disc->capabilities);

    /* Notify callback */
    if (msg_callback) {
        msg_callback(ctx->addr, MESH_MSG_DISCOVERY_RESP, buf->data, buf->len);
    }

    return 0;
}

/**
 * Handle beacon - one-way presence broadcast, just add sender as peer
 * No response needed - simpler and more reliable than request/response discovery
 */
static int beacon_handler(const struct bt_mesh_model *model,
                          struct bt_mesh_msg_ctx *ctx,
                          struct net_buf_simple *buf)
{
    /* Ignore our own beacons */
    if (ctx->addr == mesh_hid_get_addr()) {
        return 0;
    }

    char data[128];
    snprintf(data, sizeof(data), "\"from\":\"0x%04x\",\"rssi\":%d",
        ctx->addr, ctx->recv_rssi);
    jsonl_serial_send_event("beacon_rcvd", data);

    if (buf->len < sizeof(mesh_discovery_t)) {
        /* Short beacon - still add peer with minimal info */
        update_peer(ctx->addr, ctx->recv_rssi, 0);
        return 0;
    }

    mesh_discovery_t *beacon = (mesh_discovery_t *)buf->data;

    /* Add/update peer */
    update_peer(ctx->addr, ctx->recv_rssi, beacon->capabilities);
    if (beacon->name_len > 0) {
        update_peer_name(ctx->addr, beacon->name, beacon->name_len);
    }

    LOG_INF("Beacon from 0x%04x: %s (rssi=%d)", ctx->addr, beacon->name, ctx->recv_rssi);

    return 0;
}

/**
 * Handle HID command - execute keyboard/mouse/consumer actions
 * NOTE: Plaintext HID commands are rejected when encryption_required is true
 */
static int hid_cmd_handler(const struct bt_mesh_model *model,
                           struct bt_mesh_msg_ctx *ctx,
                           struct net_buf_simple *buf)
{
    if (buf->len < 2) {
        LOG_WRN("HID command too short from 0x%04x", ctx->addr);
        return -EINVAL;
    }

    /* Update peer tracking */
    update_peer(ctx->addr, ctx->recv_rssi, 0);

    /* SECURITY: Reject plaintext HID commands when encryption is required */
    if (encryption_required) {
        LOG_WRN("Plaintext HID command rejected from 0x%04x - encryption required", ctx->addr);
        char event_data[128];
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"rssi\":%d,\"error\":\"encryption_required\"",
            ctx->addr, ctx->recv_rssi);
        jsonl_serial_send_event("hid_cmd_rejected", event_data);
        return -EACCES;
    }

    uint8_t type = net_buf_simple_pull_u8(buf);
    uint8_t action = net_buf_simple_pull_u8(buf);

    LOG_INF("HID cmd from 0x%04x (rssi=%d): type=%d action=%d len=%d",
            ctx->addr, ctx->recv_rssi, type, action, buf->len);

    /* Check authentication if PIN is required */
    if (pin_required && !mesh_hid_is_peer_authenticated(ctx->addr)) {
        LOG_WRN("HID command rejected - peer 0x%04x not authenticated", ctx->addr);
        char event_data[96];
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"rssi\":%d,\"error\":\"not_authenticated\"",
            ctx->addr, ctx->recv_rssi);
        jsonl_serial_send_event("hid_cmd_rejected", event_data);
        return -EACCES;
    }

    char event_data[160];
    bool success = false;

    switch (type) {
    case MESH_HID_TYPE_KEYBOARD:
        switch (action) {
        case MESH_HID_KB_TYPE:
            if (buf->len > 0) {
                char text[64];
                size_t len = buf->len < sizeof(text) - 1 ? buf->len : sizeof(text) - 1;
                memcpy(text, buf->data, len);
                text[len] = '\0';
                success = hid_keyboard_type(text, len);
                snprintf(event_data, sizeof(event_data),
                    "\"from\":\"0x%04x\",\"type\":\"keyboard\",\"action\":\"type\",\"text\":\"%s\",\"ok\":%s",
                    ctx->addr, text, success ? "true" : "false");
            }
            break;
        case MESH_HID_KB_TAP:
            if (buf->len >= 2) {
                uint8_t keycode = buf->data[0];
                uint8_t modifiers = buf->data[1];
                success = hid_keyboard_tap(keycode, modifiers);
                snprintf(event_data, sizeof(event_data),
                    "\"from\":\"0x%04x\",\"type\":\"keyboard\",\"action\":\"tap\",\"keycode\":%d,\"mod\":%d,\"ok\":%s",
                    ctx->addr, keycode, modifiers, success ? "true" : "false");
            }
            break;
        case MESH_HID_KB_RELEASE:
            success = hid_keyboard_release_all();
            snprintf(event_data, sizeof(event_data),
                "\"from\":\"0x%04x\",\"type\":\"keyboard\",\"action\":\"release\",\"ok\":%s",
                ctx->addr, success ? "true" : "false");
            break;
        default:
            snprintf(event_data, sizeof(event_data),
                "\"from\":\"0x%04x\",\"type\":\"keyboard\",\"error\":\"unknown_action\"", ctx->addr);
        }
        break;

    case MESH_HID_TYPE_MOUSE:
        switch (action) {
        case MESH_HID_MOUSE_MOVE:
            if (buf->len >= 4) {
                int16_t dx = (int16_t)(buf->data[0] | (buf->data[1] << 8));
                int16_t dy = (int16_t)(buf->data[2] | (buf->data[3] << 8));
                success = hid_mouse_move(dx, dy);
                snprintf(event_data, sizeof(event_data),
                    "\"from\":\"0x%04x\",\"type\":\"mouse\",\"action\":\"move\",\"dx\":%d,\"dy\":%d,\"ok\":%s",
                    ctx->addr, dx, dy, success ? "true" : "false");
            }
            break;
        case MESH_HID_MOUSE_CLICK:
            if (buf->len >= 1) {
                uint8_t buttons = buf->data[0];
                success = hid_mouse_click(buttons);
                snprintf(event_data, sizeof(event_data),
                    "\"from\":\"0x%04x\",\"type\":\"mouse\",\"action\":\"click\",\"buttons\":%d,\"ok\":%s",
                    ctx->addr, buttons, success ? "true" : "false");
            }
            break;
        case MESH_HID_MOUSE_SCROLL:
            if (buf->len >= 2) {
                int8_t v = (int8_t)buf->data[0];
                int8_t h = (int8_t)buf->data[1];
                success = hid_mouse_scroll(v, h);
                snprintf(event_data, sizeof(event_data),
                    "\"from\":\"0x%04x\",\"type\":\"mouse\",\"action\":\"scroll\",\"v\":%d,\"h\":%d,\"ok\":%s",
                    ctx->addr, v, h, success ? "true" : "false");
            }
            break;
        default:
            snprintf(event_data, sizeof(event_data),
                "\"from\":\"0x%04x\",\"type\":\"mouse\",\"error\":\"unknown_action\"", ctx->addr);
        }
        break;

    case MESH_HID_TYPE_CONSUMER:
        if (action == MESH_HID_CONSUMER_SEND && buf->len >= 2) {
            uint16_t usage = buf->data[0] | (buf->data[1] << 8);
            success = hid_consumer_send(usage);
            snprintf(event_data, sizeof(event_data),
                "\"from\":\"0x%04x\",\"type\":\"consumer\",\"usage\":%d,\"ok\":%s",
                ctx->addr, usage, success ? "true" : "false");
        } else {
            snprintf(event_data, sizeof(event_data),
                "\"from\":\"0x%04x\",\"type\":\"consumer\",\"error\":\"invalid\"", ctx->addr);
        }
        break;

    default:
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"error\":\"unknown_type\",\"type\":%d", ctx->addr, type);
    }

    jsonl_serial_send_event("hid_cmd_rcvd", event_data);

    if (msg_callback) {
        msg_callback(ctx->addr, MESH_MSG_HID_CMD, buf->data, buf->len);
    }

    return 0;
}

/**
 * Handle status/ping message
 */
static int status_handler(const struct bt_mesh_model *model,
                          struct bt_mesh_msg_ctx *ctx,
                          struct net_buf_simple *buf)
{
    char data[96];
    snprintf(data, sizeof(data), "\"from\":\"0x%04x\",\"rssi\":%d,\"len\":%d",
        ctx->addr, ctx->recv_rssi, buf->len);
    jsonl_serial_send_event("mesh_ping_rcvd", data);
    LOG_INF("Ping/status from 0x%04x (rssi=%d)", ctx->addr, ctx->recv_rssi);

    /* Update peer tracking */
    update_peer(ctx->addr, ctx->recv_rssi, 0);

    if (msg_callback) {
        msg_callback(ctx->addr, MESH_MSG_STATUS, buf->data, buf->len);
    }

    return 0;
}

/**
 * Handle text message
 */
static int text_handler(const struct bt_mesh_model *model,
                        struct bt_mesh_msg_ctx *ctx,
                        struct net_buf_simple *buf)
{
    char text[64];
    size_t len = buf->len < sizeof(text) - 1 ? buf->len : sizeof(text) - 1;
    memcpy(text, buf->data, len);
    text[len] = '\0';

    char data[160];
    snprintf(data, sizeof(data), "\"from\":\"0x%04x\",\"rssi\":%d,\"text\":\"%s\"",
        ctx->addr, ctx->recv_rssi, text);
    jsonl_serial_send_event("mesh_text_rcvd", data);
    LOG_INF("Text from 0x%04x (rssi=%d): %s", ctx->addr, ctx->recv_rssi, text);

    /* Update peer tracking */
    update_peer(ctx->addr, ctx->recv_rssi, 0);

    return 0;
}

/**
 * Handle PIN authentication request
 */
static int pin_auth_handler(const struct bt_mesh_model *model,
                            struct bt_mesh_msg_ctx *ctx,
                            struct net_buf_simple *buf)
{
    if (buf->len < 4 || buf->len > 8) {
        LOG_WRN("Invalid PIN length from 0x%04x: %d", ctx->addr, buf->len);
        return -EINVAL;
    }

    char pin[16];
    size_t len = buf->len < sizeof(pin) - 1 ? buf->len : sizeof(pin) - 1;
    memcpy(pin, buf->data, len);
    pin[len] = '\0';

    LOG_INF("PIN auth from 0x%04x", ctx->addr);

    /* Verify against configured PIN */
    uint8_t attempts_left = 0;
    bool success = config_verify_pin(pin, len, &attempts_left);

    char event_data[96];
    snprintf(event_data, sizeof(event_data),
        "\"from\":\"0x%04x\",\"rssi\":%d,\"success\":%s",
        ctx->addr, ctx->recv_rssi, success ? "true" : "false");
    jsonl_serial_send_event("mesh_pin_auth", event_data);

    /* Send response */
    BT_MESH_MODEL_BUF_DEFINE(msg, MESH_HID_OP_PIN_RESP, 2);
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_PIN_RESP);
    net_buf_simple_add_u8(&msg, success ? 1 : 0);
    net_buf_simple_add_u8(&msg, attempts_left);

    struct bt_mesh_msg_ctx reply_ctx = {
        .net_idx = ctx->net_idx,
        .app_idx = ctx->app_idx,
        .addr = ctx->addr,
        .send_ttl = MESH_TTL_DEFAULT,
    };

    bt_mesh_model_send(model, &reply_ctx, &msg, NULL, NULL);

    /* If successful, mark peer as authenticated */
    if (success) {
        for (int i = 0; i < peer_count; i++) {
            if (peers[i].addr == ctx->addr) {
                peers[i].authenticated = true;
                peers[i].auth_expires = k_uptime_get_32() + AUTH_TIMEOUT_MS;
                LOG_INF("Peer 0x%04x authenticated", ctx->addr);
                break;
            }
        }
        /* If peer not in list, add them first */
        if (!mesh_hid_is_peer_authenticated(ctx->addr)) {
            update_peer(ctx->addr, ctx->recv_rssi, 0);
            for (int i = 0; i < peer_count; i++) {
                if (peers[i].addr == ctx->addr) {
                    peers[i].authenticated = true;
                    peers[i].auth_expires = k_uptime_get_32() + AUTH_TIMEOUT_MS;
                    break;
                }
            }
        }
    }

    return 0;
}

/**
 * Handle PIN authentication response
 */
static int pin_resp_handler(const struct bt_mesh_model *model,
                            struct bt_mesh_msg_ctx *ctx,
                            struct net_buf_simple *buf)
{
    if (buf->len < 2) {
        LOG_WRN("PIN response too short from 0x%04x", ctx->addr);
        return -EINVAL;
    }

    uint8_t success = buf->data[0];
    uint8_t attempts_left = buf->data[1];

    LOG_INF("PIN response from 0x%04x: success=%d, attempts=%d",
        ctx->addr, success, attempts_left);

    char event_data[96];
    snprintf(event_data, sizeof(event_data),
        "\"from\":\"0x%04x\",\"rssi\":%d,\"success\":%s,\"attempts_left\":%d",
        ctx->addr, ctx->recv_rssi, success ? "true" : "false", attempts_left);
    jsonl_serial_send_event("mesh_pin_resp", event_data);

    return 0;
}

/**
 * Settings handler for mesh node name
 */
static int mesh_settings_set(const char *name, size_t len,
                              settings_read_cb read_cb, void *cb_arg)
{
    if (!strcmp(name, "name")) {
        if (len >= MESH_NODE_NAME_LEN) {
            len = MESH_NODE_NAME_LEN - 1;
        }
        int rc = read_cb(cb_arg, node_name, len);
        if (rc >= 0) {
            node_name[rc] = '\0';
            node_name_loaded = true;
            LOG_INF("Loaded mesh node name from NVS: '%s'", node_name);
        }
        return 0;
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(mesh, "mesh", NULL, mesh_settings_set, NULL, NULL);

/**
 * Initialize mesh HID subsystem
 */
int mesh_hid_init(void)
{
    int err;

    if (initialized) {
        return 0;
    }

    /* Initialize delayed work for app key binding and auto-discovery */
    k_work_init_delayable(&app_key_work, app_key_work_handler);
    k_work_init_delayable(&discovery_work, discovery_work_handler);

    /* Generate device UUID */
    generate_uuid();

    LOG_INF("Mesh HID initializing...");
    LOG_HEXDUMP_INF(dev_uuid, 16, "Device UUID");

    /* Initialize mesh */
    err = bt_mesh_init(&prov, &comp);
    if (err) {
        LOG_ERR("Mesh init failed: %d", err);
        return err;
    }

    /* Load settings (if previously provisioned) */
    if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
        settings_load();
    }

    /* Check if already provisioned */
    if (bt_mesh_is_provisioned()) {
        LOG_INF("Already provisioned");
        /* Enable beacons and GATT proxy */
        bt_mesh_beacon_set(true);
        bt_mesh_proxy_gatt_enable();
        /* Always bind app key on boot (may not be restored from NVS) */
        k_work_schedule(&app_key_work, K_MSEC(500));
    } else {
        LOG_INF("Not provisioned - enabling PB-ADV/GATT");
        bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);
    }

    initialized = true;
    LOG_INF("Mesh HID initialized");

    return 0;
}

/**
 * Check if provisioned
 */
bool mesh_hid_is_provisioned(void)
{
    return bt_mesh_is_provisioned();
}

/**
 * Check if founder
 */
bool mesh_hid_is_founder(void)
{
    return is_founder;
}

/**
 * Check if app key is bound
 */
bool mesh_hid_app_key_bound(void)
{
    return app_key_bound;
}

/**
 * Get mesh address
 */
uint16_t mesh_hid_get_addr(void)
{
    if (!bt_mesh_is_provisioned()) {
        return 0;
    }
    /* Primary element address */
    return bt_mesh_primary_addr();
}

/**
 * Send discovery broadcast (request/response mode - triggers responses)
 */
int mesh_hid_send_discovery(void)
{
    if (!bt_mesh_is_provisioned()) {
        LOG_WRN("Not provisioned");
        return -ENOENT;
    }

    BT_MESH_MODEL_BUF_DEFINE(msg, MESH_HID_OP_DISCOVERY, 0);
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_DISCOVERY);

    struct bt_mesh_msg_ctx ctx = {
        .net_idx = 0,
        .app_idx = 0,
        .addr = MESH_GROUP_ADDR,  /* Use group address that models subscribe to */
        .send_ttl = MESH_TTL_DEFAULT,
    };

    LOG_INF("Sending discovery to group 0x%04x", MESH_GROUP_ADDR);
    return bt_mesh_model_send(&vendor_models[0], &ctx, &msg, NULL, NULL);
}

/**
 * Send beacon broadcast (one-way presence announcement - no responses)
 * More reliable than request/response since there's no response collision
 */
int mesh_hid_send_beacon(void)
{
    int err;

    if (!bt_mesh_is_provisioned()) {
        LOG_WRN("Not provisioned");
        return -ENOENT;
    }

    /* Log model configuration for debugging */
    LOG_INF("Beacon send - model keys[0]=%u (expect 0), groups[0]=0x%04x (expect 0x%04x)",
            vendor_models[0].keys[0], vendor_models[0].groups[0], MESH_GROUP_ADDR);
    LOG_INF("Beacon send - app_key_bound=%d, app_key_exists=%d",
            app_key_bound, bt_mesh_app_key_exists(0));

    /* Build beacon with our node info (same format as discovery response) */
    BT_MESH_MODEL_BUF_DEFINE(msg, MESH_HID_OP_BEACON, sizeof(mesh_discovery_t));
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_BEACON);

    static mesh_discovery_t beacon;  /* Static to avoid stack overflow */
    memcpy(beacon.uuid, dev_uuid, 16);
    beacon.capabilities = MESH_CAP_RELAY | MESH_CAP_USB_HOST;
    beacon.tx_power = 0;  /* TODO: get actual TX power */

    const char *name = mesh_hid_get_name();
    beacon.name_len = strlen(name);
    if (beacon.name_len > sizeof(beacon.name) - 1) {
        beacon.name_len = sizeof(beacon.name) - 1;
    }
    memcpy(beacon.name, name, beacon.name_len);
    beacon.name[beacon.name_len] = '\0';

    net_buf_simple_add_mem(&msg, &beacon, sizeof(beacon));

    struct bt_mesh_msg_ctx ctx = {
        .net_idx = 0,
        .app_idx = 0,
        .addr = MESH_GROUP_ADDR,  /* Use group address that models subscribe to */
        .send_ttl = MESH_TTL_DEFAULT,
    };

    LOG_INF("Sending beacon to group 0x%04x", MESH_GROUP_ADDR);
    err = bt_mesh_model_send(&vendor_models[0], &ctx, &msg, NULL, NULL);
    if (err) {
        LOG_ERR("bt_mesh_model_send failed: %d", err);
    }
    return err;
}

/**
 * Send message to specific node
 */
int mesh_hid_send_msg(uint16_t dst_addr, mesh_msg_type_t type,
                      const uint8_t *payload, size_t len)
{
    if (!bt_mesh_is_provisioned()) {
        return -ENOENT;
    }

    uint32_t opcode;
    switch (type) {
    case MESH_MSG_HID_CMD:
        opcode = MESH_HID_OP_HID_CMD;
        break;
    case MESH_MSG_STATUS:
        opcode = MESH_HID_OP_STATUS;
        break;
    default:
        opcode = MESH_HID_OP_STATUS;
        break;
    }

    BT_MESH_MODEL_BUF_DEFINE(msg, opcode, len);
    bt_mesh_model_msg_init(&msg, opcode);

    if (payload && len > 0) {
        net_buf_simple_add_mem(&msg, payload, len);
    }

    struct bt_mesh_msg_ctx ctx = {
        .net_idx = 0,
        .app_idx = 0,
        .addr = dst_addr,
        .send_ttl = MESH_TTL_DEFAULT,
    };

    return bt_mesh_model_send(&vendor_models[0], &ctx, &msg, NULL, NULL);
}

/**
 * Broadcast message
 */
int mesh_hid_broadcast(mesh_msg_type_t type,
                       const uint8_t *payload, size_t len)
{
    return mesh_hid_send_msg(BT_MESH_ADDR_ALL_NODES, type, payload, len);
}

/**
 * Send ping to specific node (uses STATUS opcode)
 */
int mesh_hid_send_ping(uint16_t dst_addr)
{
    if (!bt_mesh_is_provisioned()) {
        return -ENOENT;
    }

    /* Send our status as a ping */
    mesh_status_t status;
    mesh_hid_get_status(&status);

    BT_MESH_MODEL_BUF_DEFINE(msg, MESH_HID_OP_STATUS, sizeof(mesh_status_t));
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_STATUS);
    net_buf_simple_add_mem(&msg, &status, sizeof(status));

    struct bt_mesh_msg_ctx ctx = {
        .net_idx = 0,
        .app_idx = 0,
        .addr = dst_addr,
        .send_ttl = MESH_TTL_DEFAULT,
    };

    LOG_INF("Sending ping to 0x%04x", dst_addr);
    return bt_mesh_model_send(&vendor_models[0], &ctx, &msg, NULL, NULL);
}

/**
 * Send text message to specific node (or broadcast with 0xFFFF)
 */
int mesh_hid_send_text(uint16_t dst_addr, const char *text)
{
    if (!bt_mesh_is_provisioned()) {
        return -ENOENT;
    }

    size_t len = strlen(text);
    if (len > 60) {
        len = 60;  /* Max text length */
    }

    BT_MESH_MODEL_BUF_DEFINE(msg, MESH_HID_OP_TEXT, 64);
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_TEXT);
    net_buf_simple_add_mem(&msg, text, len);

    struct bt_mesh_msg_ctx ctx = {
        .net_idx = 0,
        .app_idx = 0,
        .addr = dst_addr,
        .send_ttl = MESH_TTL_DEFAULT,
    };

    LOG_INF("Sending text to 0x%04x: %s", dst_addr, text);
    return bt_mesh_model_send(&vendor_models[0], &ctx, &msg, NULL, NULL);
}

/**
 * Send HID command helper
 */
static int send_hid_cmd(uint16_t dst_addr, uint8_t type, uint8_t action,
                        const void *data, size_t data_len)
{
    if (!bt_mesh_is_provisioned()) {
        return -ENOENT;
    }

    BT_MESH_MODEL_BUF_DEFINE(msg, MESH_HID_OP_HID_CMD, 64);
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_HID_CMD);
    net_buf_simple_add_u8(&msg, type);
    net_buf_simple_add_u8(&msg, action);
    if (data && data_len > 0) {
        net_buf_simple_add_mem(&msg, data, data_len);
    }

    struct bt_mesh_msg_ctx ctx = {
        .net_idx = 0,
        .app_idx = 0,
        .addr = dst_addr,
        .send_ttl = MESH_TTL_DEFAULT,
    };

    return bt_mesh_model_send(&vendor_models[0], &ctx, &msg, NULL, NULL);
}

/**
 * Send keyboard type command
 */
int mesh_hid_send_keyboard_type(uint16_t dst_addr, const char *text)
{
    size_t len = strlen(text);
    if (len > 58) len = 58;
    LOG_INF("Sending keyboard type to 0x%04x: %s", dst_addr, text);
    return send_hid_cmd(dst_addr, MESH_HID_TYPE_KEYBOARD, MESH_HID_KB_TYPE, text, len);
}

/**
 * Send keyboard tap command
 */
int mesh_hid_send_keyboard_tap(uint16_t dst_addr, uint8_t keycode, uint8_t modifiers)
{
    uint8_t data[2] = { keycode, modifiers };
    LOG_INF("Sending keyboard tap to 0x%04x: key=0x%02x mod=0x%02x", dst_addr, keycode, modifiers);
    return send_hid_cmd(dst_addr, MESH_HID_TYPE_KEYBOARD, MESH_HID_KB_TAP, data, sizeof(data));
}

/**
 * Send mouse move command
 */
int mesh_hid_send_mouse_move(uint16_t dst_addr, int16_t dx, int16_t dy)
{
    uint8_t data[4] = {
        dx & 0xFF, (dx >> 8) & 0xFF,
        dy & 0xFF, (dy >> 8) & 0xFF
    };
    LOG_INF("Sending mouse move to 0x%04x: dx=%d dy=%d", dst_addr, dx, dy);
    return send_hid_cmd(dst_addr, MESH_HID_TYPE_MOUSE, MESH_HID_MOUSE_MOVE, data, sizeof(data));
}

/**
 * Send mouse click command
 */
int mesh_hid_send_mouse_click(uint16_t dst_addr, uint8_t buttons)
{
    LOG_INF("Sending mouse click to 0x%04x: buttons=%d", dst_addr, buttons);
    return send_hid_cmd(dst_addr, MESH_HID_TYPE_MOUSE, MESH_HID_MOUSE_CLICK, &buttons, 1);
}

/**
 * Send consumer key command
 */
int mesh_hid_send_consumer(uint16_t dst_addr, uint16_t usage_id)
{
    uint8_t data[2] = { usage_id & 0xFF, (usage_id >> 8) & 0xFF };
    LOG_INF("Sending consumer to 0x%04x: usage=%d", dst_addr, usage_id);
    return send_hid_cmd(dst_addr, MESH_HID_TYPE_CONSUMER, MESH_HID_CONSUMER_SEND, data, sizeof(data));
}

/**
 * Set message callback
 */
void mesh_hid_set_callback(mesh_msg_callback_t callback)
{
    msg_callback = callback;
}

/**
 * Bind app key to model.
 * Uses public bt_mesh_app_key_add for key database, then direct array
 * modification for model binding (same approach as Config Server).
 */
static void bind_app_key_direct(void)
{
    uint8_t status;

    if (!bt_mesh_is_provisioned()) {
        LOG_ERR("Not provisioned yet, will retry");
        jsonl_serial_send_event("app_key_binding", "\"state\":\"not_provisioned\",\"retry\":true");
        k_work_schedule(&app_key_work, K_SECONDS(5));
        return;
    }

    /* Step 1: Add app key to mesh key database using public API
     * Returns status: 0=success, others=error per Mesh spec */
    status = bt_mesh_app_key_add(0, 0, app_key);  /* app_idx=0, net_idx=0 */
    if (status != 0x00 && status != 0x02) {  /* 0x02 = key already exists */
        LOG_ERR("App key add failed: status=0x%02x, will retry", status);
        char data[64];
        snprintf(data, sizeof(data), "\"state\":\"add_failed\",\"status\":\"0x%02x\",\"retry\":true", status);
        jsonl_serial_send_event("app_key_binding", data);
        k_work_schedule(&app_key_work, K_SECONDS(5));
        return;
    }
    LOG_INF("App key added to database (status=0x%02x)", status);

    /* Step 2: Bind app key to vendor model (direct array modification)
     * This is the same approach used by Config Server's mod_bind() */
    vendor_models[0].keys[0] = 0;  /* App key index 0 */
    LOG_INF("App key 0 bound to vendor model");

    /* Step 3: Subscribe model to group address (direct array modification)
     * This is the same approach used by Config Server's mod_sub_add() */
    vendor_models[0].groups[0] = MESH_GROUP_ADDR;  /* 0xC000 */
    LOG_INF("Model subscribed to group 0x%04x", MESH_GROUP_ADDR);

    app_key_bound = true;
    LOG_INF("Direct app key binding complete");
    jsonl_serial_send_event("app_key_binding", "\"state\":\"success\"");

    /* Schedule auto-discovery after app key is bound */
    k_work_schedule(&discovery_work, K_SECONDS(3));
}

/**
 * Work handler for delayed app key binding
 */
static void app_key_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!bt_mesh_is_provisioned()) {
        LOG_WRN("Not provisioned yet, will retry app key bind");
        /* Retry in 5 seconds - provisioning might still be in progress */
        k_work_schedule(&app_key_work, K_SECONDS(5));
        return;
    }

    if (app_key_bound) {
        LOG_INF("App key already bound");
        /* Still schedule discovery even if already bound */
        k_work_schedule(&discovery_work, K_SECONDS(2));
        return;
    }

    /* Use direct binding - Config Client doesn't work for local config */
    bind_app_key_direct();
}

/**
 * Work handler for auto-discovery on boot
 */
static void discovery_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!bt_mesh_is_provisioned()) {
        LOG_WRN("Not provisioned, will retry discovery");
        if (periodic_discovery_enabled && discovery_interval_ms > 0) {
            k_work_schedule(&discovery_work, K_SECONDS(10));
        }
        return;
    }

    if (!app_key_bound) {
        LOG_WRN("App key not bound - attempting self-heal");
        jsonl_serial_send_event("self_heal", "\"action\":\"rebind_app_key\"");
        /* Try to bind app key, then discovery will run after */
        k_work_schedule(&app_key_work, K_MSEC(100));
        /* Also reschedule discovery in case binding still fails */
        if (periodic_discovery_enabled && discovery_interval_ms > 0) {
            k_work_schedule(&discovery_work, K_SECONDS(15));
        }
        return;
    }

    LOG_INF("Sending periodic beacon...");
    int err = mesh_hid_send_beacon();
    if (err) {
        LOG_WRN("Beacon failed: %d", err);
    } else {
        jsonl_serial_send_event("beacon", "\"sent\":true");
    }

    /* Mark stale peers */
    uint32_t now = k_uptime_get_32();
    int stale_count = 0;
    for (int i = 0; i < peer_count; i++) {
        if (peers[i].last_seen > 0 &&
            (now - peers[i].last_seen) > PEER_STALE_TIMEOUT_MS) {
            stale_count++;
        }
    }
    if (stale_count > 0) {
        char data[64];
        snprintf(data, sizeof(data), "\"stale_peers\":%d", stale_count);
        jsonl_serial_send_event("peer_check", data);
    }

    /* Schedule next periodic discovery */
    if (periodic_discovery_enabled && discovery_interval_ms > 0) {
        k_work_schedule(&discovery_work, K_MSEC(discovery_interval_ms));
    }
}

/**
 * Generate unique mesh address from device ID
 */
static uint16_t generate_mesh_addr(void)
{
    uint32_t dev_id = NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1];
    /* Use lower 14 bits, add 1 to avoid 0, stay in unicast range (0x0001-0x7FFF) */
    uint16_t addr = (dev_id & 0x3FFF) + 1;
    if (addr > 0x7FFF) {
        addr = 0x7FFF;
    }
    return addr;
}

/**
 * Self-provision as mesh founder
 */
int mesh_hid_self_provision(void)
{
    int err;
    uint16_t my_addr;

    if (bt_mesh_is_provisioned()) {
        LOG_WRN("Already provisioned");
        return -EALREADY;
    }

    /* Generate unique address from device ID */
    my_addr = generate_mesh_addr();
    LOG_INF("Self-provisioning with addr=0x%04x...", my_addr);

    /* Disable provisioning beacons */
    bt_mesh_prov_disable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);

    /* Provision with static network key and unique address */
    err = bt_mesh_provision(net_key, 0, 0, 0, my_addr, NULL);
    if (err) {
        LOG_ERR("Provisioning failed: %d", err);
        return err;
    }

    is_founder = true;
    LOG_INF("Self-provisioned, addr=0x%04x", my_addr);

    /* Enable secure network beacons for node discovery */
    bt_mesh_beacon_set(true);
    LOG_INF("Beacons enabled");

    /* Enable GATT proxy */
    bt_mesh_proxy_gatt_enable();
    LOG_INF("GATT proxy enabled");

    /* Schedule app key binding after mesh settles */
    k_work_schedule(&app_key_work, K_SECONDS(2));

    return 0;
}

/**
 * Scan for unprovisioned nodes
 */
int mesh_hid_scan_unprovisioned(void)
{
    /* This will be called when user wants to add new nodes */
    LOG_INF("Scanning for unprovisioned nodes...");
    /* TODO: Implement unprovisioned beacon scanning */
    return 0;
}

/**
 * Approve node to join mesh
 */
int mesh_hid_approve_node(const uint8_t *uuid)
{
    /* Phase 1: Just log */
    /* Full implementation needs provisioner role */
    LOG_INF("Approve node requested");
    LOG_HEXDUMP_INF(uuid, 16, "UUID");
    return -ENOTSUP;
}

/**
 * Get mesh status
 */
void mesh_hid_get_status(mesh_status_t *status)
{
    if (!status) return;

    memset(status, 0, sizeof(*status));
    status->addr = mesh_hid_get_addr();
    status->provisioned = bt_mesh_is_provisioned();
    status->relay_enabled = 1;  /* TODO: Get actual relay state */
    status->peer_count = peer_count;
    status->rssi = 0;  /* Self RSSI not applicable */
}

/**
 * Reset mesh provisioning
 */
void mesh_hid_reset(void)
{
    LOG_INF("Resetting mesh provisioning...");
    bt_mesh_reset();
    is_founder = false;
    LOG_INF("Mesh reset complete - reboot required");
}

/**
 * Ensure app key is bound after loading from NVS
 */
int mesh_hid_ensure_app_key(void)
{
    if (!bt_mesh_is_provisioned()) {
        return -ENOENT;
    }

    /* Schedule app key binding */
    k_work_schedule(&app_key_work, K_SECONDS(2));
    LOG_INF("Scheduled app key binding");

    return 0;
}

/**
 * Add or update peer in list
 */
static void update_peer(uint16_t addr, int8_t rssi, uint8_t caps)
{
    /* Check if already in list */
    for (int i = 0; i < peer_count; i++) {
        if (peers[i].addr == addr) {
            peers[i].rssi = rssi;
            peers[i].last_seen = k_uptime_get_32();
            if (caps) peers[i].capabilities = caps;
            return;
        }
    }

    /* Add new peer if room */
    if (peer_count < MESH_MAX_PEERS) {
        peers[peer_count].addr = addr;
        peers[peer_count].rssi = rssi;
        peers[peer_count].capabilities = caps;
        peers[peer_count].last_seen = k_uptime_get_32();
        peers[peer_count].name[0] = '\0';
        peers[peer_count].authenticated = false;
        peers[peer_count].auth_expires = 0;
        /* Initialize E2E encryption state */
        peers[peer_count].has_session_key = false;
        memset(peers[peer_count].session_key, 0, sizeof(peers[peer_count].session_key));
        peers[peer_count].tx_counter = 0;
        peers[peer_count].rx_counter = 0;
        peer_count++;
        LOG_INF("Added peer 0x%04x (count=%d)", addr, peer_count);
    }
}

/**
 * Update peer name
 */
static void update_peer_name(uint16_t addr, const char *name, size_t name_len)
{
    for (int i = 0; i < peer_count; i++) {
        if (peers[i].addr == addr) {
            if (name_len >= MESH_NODE_NAME_LEN) {
                name_len = MESH_NODE_NAME_LEN - 1;
            }
            memcpy(peers[i].name, name, name_len);
            peers[i].name[name_len] = '\0';
            LOG_INF("Updated peer 0x%04x name: '%s'", addr, peers[i].name);
            return;
        }
    }
}

/**
 * Get number of known peers
 */
int mesh_hid_get_peer_count(void)
{
    return peer_count;
}

/**
 * Get peer info by index
 */
int mesh_hid_get_peer(int index, mesh_peer_t *peer)
{
    if (index < 0 || index >= peer_count || peer == NULL) {
        return -ENOENT;
    }
    memcpy(peer, &peers[index], sizeof(mesh_peer_t));
    return 0;
}

/**
 * Get peer info by address
 */
int mesh_hid_get_peer_by_addr(uint16_t addr, mesh_peer_t *peer)
{
    for (int i = 0; i < peer_count; i++) {
        if (peers[i].addr == addr) {
            if (peer) memcpy(peer, &peers[i], sizeof(mesh_peer_t));
            return 0;
        }
    }
    return -ENOENT;
}

/**
 * Set this node's name (persists to NVS)
 */
void mesh_hid_set_name(const char *name)
{
    strncpy(node_name, name, MESH_NODE_NAME_LEN - 1);
    node_name[MESH_NODE_NAME_LEN - 1] = '\0';

    /* Save to NVS */
    int rc = settings_save_one(SETTINGS_MESH_NAME, node_name, strlen(node_name));
    if (rc) {
        LOG_ERR("Failed to save mesh name to NVS: %d", rc);
    } else {
        LOG_INF("Mesh node name saved: '%s'", node_name);
    }
}

/**
 * Get this node's name
 */
const char *mesh_hid_get_name(void)
{
    return node_name;
}

/**
 * Clear peer list
 */
void mesh_hid_clear_peers(void)
{
    peer_count = 0;
    memset(peers, 0, sizeof(peers));
    LOG_INF("Peer list cleared");
}

/**
 * Send PIN authentication to a peer
 */
int mesh_hid_send_pin(uint16_t dst_addr, const char *pin)
{
    if (!bt_mesh_is_provisioned()) {
        return -ENOENT;
    }

    size_t len = strlen(pin);
    if (len < 4 || len > 8) {
        return -EINVAL;
    }

    BT_MESH_MODEL_BUF_DEFINE(msg, MESH_HID_OP_PIN_AUTH, 8);
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_PIN_AUTH);
    net_buf_simple_add_mem(&msg, pin, len);

    struct bt_mesh_msg_ctx ctx = {
        .net_idx = 0,
        .app_idx = 0,
        .addr = dst_addr,
        .send_ttl = MESH_TTL_DEFAULT,
    };

    LOG_INF("Sending PIN auth to 0x%04x", dst_addr);
    return bt_mesh_model_send(&vendor_models[0], &ctx, &msg, NULL, NULL);
}

/**
 * Check if a peer is authenticated (with expiration check)
 */
bool mesh_hid_is_peer_authenticated(uint16_t addr)
{
    uint32_t now = k_uptime_get_32();

    for (int i = 0; i < peer_count; i++) {
        if (peers[i].addr == addr) {
            if (!peers[i].authenticated) {
                return false;
            }
            /* Check expiration */
            if (peers[i].auth_expires != 0 && now > peers[i].auth_expires) {
                peers[i].authenticated = false;
                LOG_INF("Peer 0x%04x auth expired", addr);
                return false;
            }
            return true;
        }
    }
    return false;
}

/**
 * Enable/disable PIN requirement for HID commands
 */
void mesh_hid_set_pin_required(bool required)
{
    pin_required = required;
    LOG_INF("PIN auth %s for HID commands", required ? "required" : "disabled");
}

/**
 * Check if PIN is required for HID commands
 */
bool mesh_hid_pin_required(void)
{
    return pin_required;
}

/* ========== E2E ENCRYPTION IMPLEMENTATION ========== */

/**
 * Get pointer to peer struct (internal helper)
 */
static mesh_peer_t *get_peer_ptr(uint16_t addr)
{
    for (int i = 0; i < peer_count; i++) {
        if (peers[i].addr == addr) {
            return &peers[i];
        }
    }
    return NULL;
}

/**
 * Derive session key from PIN and both challenges using SHA-256
 * Key = SHA256(PIN || our_challenge || their_challenge)[0:16]
 */
static int derive_session_key(const char *pin, const uint8_t *our_challenge,
                              const uint8_t *their_challenge, uint8_t *key_out)
{
    struct tc_sha256_state_struct sha_state;
    uint8_t hash[32];

    if (tc_sha256_init(&sha_state) != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    /* Hash PIN */
    size_t pin_len = strlen(pin);
    if (tc_sha256_update(&sha_state, (const uint8_t *)pin, pin_len) != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    /* Hash our challenge */
    if (tc_sha256_update(&sha_state, our_challenge, 16) != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    /* Hash their challenge */
    if (tc_sha256_update(&sha_state, their_challenge, 16) != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    if (tc_sha256_final(hash, &sha_state) != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    /* Use first 16 bytes as AES-128 key */
    memcpy(key_out, hash, MESH_E2E_KEY_LEN);
    return 0;
}

/**
 * Encrypt message using AES-128-CCM
 * Input:  plaintext of plain_len bytes
 * Output: [4-byte counter][encrypted data][8-byte tag]
 */
static int encrypt_message(mesh_peer_t *peer, const uint8_t *plaintext, size_t plain_len,
                           uint8_t *ciphertext, size_t *cipher_len)
{
    if (!peer || !peer->has_session_key || plain_len == 0) {
        return -EINVAL;
    }

    struct tc_ccm_mode_struct ccm;
    struct tc_aes_key_sched_struct sched;
    uint8_t nonce[MESH_E2E_NONCE_LEN];

    /* Build nonce: [4-byte counter][8-byte addr pair][1-byte direction] */
    memset(nonce, 0, sizeof(nonce));
    nonce[0] = (peer->tx_counter >> 24) & 0xFF;
    nonce[1] = (peer->tx_counter >> 16) & 0xFF;
    nonce[2] = (peer->tx_counter >> 8) & 0xFF;
    nonce[3] = peer->tx_counter & 0xFF;
    nonce[4] = (mesh_hid_get_addr() >> 8) & 0xFF;
    nonce[5] = mesh_hid_get_addr() & 0xFF;
    nonce[6] = (peer->addr >> 8) & 0xFF;
    nonce[7] = peer->addr & 0xFF;
    nonce[8] = 0x01;  /* TX direction */

    /* Initialize AES */
    if (tc_aes128_set_encrypt_key(&sched, peer->session_key) != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    /* Initialize CCM */
    if (tc_ccm_config(&ccm, &sched, nonce, sizeof(nonce), MESH_E2E_TAG_LEN) != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    /* Output format: [4-byte counter][ciphertext][8-byte tag] */
    ciphertext[0] = nonce[0];
    ciphertext[1] = nonce[1];
    ciphertext[2] = nonce[2];
    ciphertext[3] = nonce[3];

    /* Encrypt and authenticate */
    if (tc_ccm_generation_encryption(&ciphertext[4], plain_len + MESH_E2E_TAG_LEN,
                                      NULL, 0, /* No associated data */
                                      plaintext, plain_len, &ccm) != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    *cipher_len = 4 + plain_len + MESH_E2E_TAG_LEN;
    peer->tx_counter++;

    LOG_DBG("Encrypted %d bytes -> %d bytes (counter=%u)", (int)plain_len, (int)*cipher_len, peer->tx_counter - 1);
    return 0;
}

/**
 * Decrypt message using AES-128-CCM
 * Input:  [4-byte counter][encrypted data][8-byte tag]
 * Output: plaintext
 */
static int decrypt_message(mesh_peer_t *peer, const uint8_t *ciphertext, size_t cipher_len,
                           uint8_t *plaintext, size_t *plain_len)
{
    if (!peer || !peer->has_session_key || cipher_len < 4 + MESH_E2E_TAG_LEN + 1) {
        return -EINVAL;
    }

    struct tc_ccm_mode_struct ccm;
    struct tc_aes_key_sched_struct sched;
    uint8_t nonce[MESH_E2E_NONCE_LEN];

    /* Extract counter from message */
    uint32_t msg_counter = ((uint32_t)ciphertext[0] << 24) |
                           ((uint32_t)ciphertext[1] << 16) |
                           ((uint32_t)ciphertext[2] << 8) |
                           ciphertext[3];

    /* Replay protection: counter must be >= last seen */
    if (msg_counter < peer->rx_counter) {
        LOG_WRN("Replay attack? counter=%u, expected >= %u", msg_counter, peer->rx_counter);
        return -EACCES;
    }

    /* Build nonce: [4-byte counter][8-byte addr pair][1-byte direction] */
    memset(nonce, 0, sizeof(nonce));
    nonce[0] = ciphertext[0];
    nonce[1] = ciphertext[1];
    nonce[2] = ciphertext[2];
    nonce[3] = ciphertext[3];
    nonce[4] = (peer->addr >> 8) & 0xFF;  /* Sender's addr */
    nonce[5] = peer->addr & 0xFF;
    nonce[6] = (mesh_hid_get_addr() >> 8) & 0xFF;  /* Our addr */
    nonce[7] = mesh_hid_get_addr() & 0xFF;
    nonce[8] = 0x01;  /* TX direction from sender's perspective */

    /* Initialize AES */
    if (tc_aes128_set_encrypt_key(&sched, peer->session_key) != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    /* Initialize CCM */
    if (tc_ccm_config(&ccm, &sched, nonce, sizeof(nonce), MESH_E2E_TAG_LEN) != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    size_t encrypted_len = cipher_len - 4;  /* Remove counter prefix */
    *plain_len = encrypted_len - MESH_E2E_TAG_LEN;

    /* Decrypt and verify */
    if (tc_ccm_decryption_verification(plaintext, *plain_len,
                                        NULL, 0, /* No associated data */
                                        &ciphertext[4], encrypted_len, &ccm) != TC_CRYPTO_SUCCESS) {
        LOG_WRN("Decryption failed - bad key or tampered message");
        return -EBADMSG;
    }

    /* Update replay counter */
    peer->rx_counter = msg_counter + 1;

    LOG_DBG("Decrypted %d bytes (counter=%u)", (int)*plain_len, msg_counter);
    return 0;
}

/**
 * Handle key exchange request
 * Received: [16-byte challenge from initiator]
 * Response: [16-byte our challenge]
 * Both sides derive: key = SHA256(PIN || initiator_challenge || responder_challenge)
 */
static int key_exchange_handler(const struct bt_mesh_model *model,
                                struct bt_mesh_msg_ctx *ctx,
                                struct net_buf_simple *buf)
{
    /* Debug: confirm handler is being called */
    char dbg[64];
    snprintf(dbg, sizeof(dbg), "\"from\":\"0x%04x\",\"len\":%d", ctx->addr, buf->len);
    jsonl_serial_send_event("key_exchange_rcvd", dbg);

    if (buf->len != 16) {
        LOG_WRN("Invalid key exchange from 0x%04x: len=%d", ctx->addr, buf->len);
        return -EINVAL;
    }

    uint8_t their_challenge[16];
    memcpy(their_challenge, buf->data, 16);

    LOG_INF("Key exchange request from 0x%04x", ctx->addr);

    /* Update peer tracking */
    update_peer(ctx->addr, ctx->recv_rssi, 0);

    /* NOTE: Key exchange is always allowed - PIN auth is only for HID/GPIO commands.
     * The PIN is still used as the pre-shared secret for key derivation,
     * so both peers must have the same PIN configured to derive matching keys. */

    /* Generate our challenge */
    uint8_t our_challenge[16];
    sys_csrand_get(our_challenge, sizeof(our_challenge));

    /* Derive session key: SHA256(PIN || their_challenge || our_challenge) */
    /* Note: As responder, their challenge comes first */
    mesh_peer_t *peer = get_peer_ptr(ctx->addr);
    if (!peer) {
        return -ENOENT;
    }

    /* Get current PIN for key derivation */
    char pin[16];
    uint8_t pin_len = config_get_pin(pin);
    if (pin_len == 0) {
        LOG_WRN("No PIN configured for key derivation");
        return -ENOENT;
    }

    /* Derive key: responder uses (their_challenge, our_challenge) order */
    if (derive_session_key(pin, their_challenge, our_challenge, peer->session_key) != 0) {
        LOG_ERR("Failed to derive session key");
        return -1;
    }

    peer->has_session_key = true;
    peer->tx_counter = 0;
    peer->rx_counter = 0;

    /* Send our challenge back */
    BT_MESH_MODEL_BUF_DEFINE(msg, MESH_HID_OP_KEY_CONFIRM, 16);
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_KEY_CONFIRM);
    net_buf_simple_add_mem(&msg, our_challenge, 16);

    struct bt_mesh_msg_ctx reply_ctx = {
        .net_idx = ctx->net_idx,
        .app_idx = ctx->app_idx,
        .addr = ctx->addr,
        .send_ttl = MESH_TTL_DEFAULT,
    };

    int err = bt_mesh_model_send(model, &reply_ctx, &msg, NULL, NULL);

    /* Debug: confirm KEY_CONFIRM was sent */
    char event_data[96];
    snprintf(event_data, sizeof(event_data),
        "\"to\":\"0x%04x\",\"err\":%d,\"key_established\":true",
        ctx->addr, err);
    jsonl_serial_send_event("key_confirm_sent", event_data);

    LOG_INF("Session key established with 0x%04x (responder), send_err=%d", ctx->addr, err);
    return err;
}

/**
 * Handle key exchange confirmation (response to our key exchange request)
 * Received: [16-byte challenge from responder]
 */
static int key_confirm_handler(const struct bt_mesh_model *model,
                               struct bt_mesh_msg_ctx *ctx,
                               struct net_buf_simple *buf)
{
    /* Debug: emit event immediately to confirm handler is reached */
    char dbg[96];
    snprintf(dbg, sizeof(dbg),
        "\"from\":\"0x%04x\",\"len\":%d,\"pending\":%s,\"peer\":\"0x%04x\"",
        ctx->addr, buf->len, pending_key_exchange ? "true" : "false", pending_key_peer);
    jsonl_serial_send_event("key_confirm_rcvd", dbg);

    if (buf->len != 16) {
        LOG_WRN("Invalid key confirm from 0x%04x: len=%d", ctx->addr, buf->len);
        return -EINVAL;
    }

    if (!pending_key_exchange || pending_key_peer != ctx->addr) {
        LOG_WRN("Unexpected key confirm from 0x%04x", ctx->addr);
        return -EINVAL;
    }

    uint8_t their_challenge[16];
    memcpy(their_challenge, buf->data, 16);

    LOG_INF("Key exchange confirm from 0x%04x", ctx->addr);

    mesh_peer_t *peer = get_peer_ptr(ctx->addr);
    if (!peer) {
        return -ENOENT;
    }

    /* Get current PIN for key derivation */
    char pin[16];
    uint8_t pin_len = config_get_pin(pin);
    if (pin_len == 0) {
        LOG_WRN("No PIN configured for key derivation");
        pending_key_exchange = false;
        return -ENOENT;
    }

    /* Derive key: initiator uses (our_challenge, their_challenge) order */
    if (derive_session_key(pin, local_challenge, their_challenge, peer->session_key) != 0) {
        LOG_ERR("Failed to derive session key");
        pending_key_exchange = false;
        return -1;
    }

    peer->has_session_key = true;
    peer->tx_counter = 0;
    peer->rx_counter = 0;
    pending_key_exchange = false;

    char event_data[96];
    snprintf(event_data, sizeof(event_data),
        "\"from\":\"0x%04x\",\"rssi\":%d,\"key_established\":true",
        ctx->addr, ctx->recv_rssi);
    jsonl_serial_send_event("key_exchange_complete", event_data);

    LOG_INF("Session key established with 0x%04x (initiator)", ctx->addr);
    return 0;
}

/**
 * Handle encrypted HID command
 */
static int hid_cmd_enc_handler(const struct bt_mesh_model *model,
                                struct bt_mesh_msg_ctx *ctx,
                                struct net_buf_simple *buf)
{
    /* Minimum: 4-byte counter + 2-byte payload + 8-byte tag */
    if (buf->len < 14) {
        LOG_WRN("Encrypted HID command too short from 0x%04x", ctx->addr);
        return -EINVAL;
    }

    /* Update peer tracking */
    update_peer(ctx->addr, ctx->recv_rssi, 0);

    /* Get peer and check session key */
    mesh_peer_t *peer = get_peer_ptr(ctx->addr);
    if (!peer || !peer->has_session_key) {
        LOG_WRN("No session key for peer 0x%04x", ctx->addr);
        char event_data[96];
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"error\":\"no_session_key\"", ctx->addr);
        jsonl_serial_send_event("hid_cmd_rejected", event_data);
        return -ENOENT;
    }

    /* Check PIN authentication (still required even with encryption) */
    if (pin_required && !mesh_hid_is_peer_authenticated(ctx->addr)) {
        LOG_WRN("Encrypted HID rejected - peer 0x%04x not authenticated", ctx->addr);
        char event_data[96];
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"error\":\"not_authenticated\"", ctx->addr);
        jsonl_serial_send_event("hid_cmd_rejected", event_data);
        return -EACCES;
    }

    /* Decrypt */
    uint8_t plaintext[64];
    size_t plain_len;
    if (decrypt_message(peer, buf->data, buf->len, plaintext, &plain_len) != 0) {
        LOG_WRN("Decryption failed from 0x%04x", ctx->addr);
        char event_data[96];
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"error\":\"decryption_failed\"", ctx->addr);
        jsonl_serial_send_event("hid_cmd_rejected", event_data);
        return -EBADMSG;
    }

    if (plain_len < 2) {
        return -EINVAL;
    }

    uint8_t type = plaintext[0];
    uint8_t action = plaintext[1];

    LOG_INF("Encrypted HID cmd from 0x%04x: type=%d action=%d", ctx->addr, type, action);

    char event_data[160];
    bool success = false;

    /* Execute HID command (same logic as plaintext handler) */
    switch (type) {
    case MESH_HID_TYPE_KEYBOARD:
        switch (action) {
        case MESH_HID_KB_TYPE:
            if (plain_len > 2) {
                char text[64];
                size_t len = plain_len - 2;
                if (len >= sizeof(text)) len = sizeof(text) - 1;
                memcpy(text, &plaintext[2], len);
                text[len] = '\0';
                success = hid_keyboard_type(text, len);
                snprintf(event_data, sizeof(event_data),
                    "\"from\":\"0x%04x\",\"type\":\"keyboard\",\"action\":\"type\",\"encrypted\":true,\"ok\":%s",
                    ctx->addr, success ? "true" : "false");
            }
            break;
        case MESH_HID_KB_TAP:
            if (plain_len >= 4) {
                success = hid_keyboard_tap(plaintext[2], plaintext[3]);
                snprintf(event_data, sizeof(event_data),
                    "\"from\":\"0x%04x\",\"type\":\"keyboard\",\"action\":\"tap\",\"encrypted\":true,\"ok\":%s",
                    ctx->addr, success ? "true" : "false");
            }
            break;
        case MESH_HID_KB_RELEASE:
            success = hid_keyboard_release_all();
            snprintf(event_data, sizeof(event_data),
                "\"from\":\"0x%04x\",\"type\":\"keyboard\",\"action\":\"release\",\"encrypted\":true,\"ok\":%s",
                ctx->addr, success ? "true" : "false");
            break;
        default:
            snprintf(event_data, sizeof(event_data),
                "\"from\":\"0x%04x\",\"encrypted\":true,\"error\":\"unknown_action\"", ctx->addr);
        }
        break;

    case MESH_HID_TYPE_MOUSE:
        switch (action) {
        case MESH_HID_MOUSE_MOVE:
            if (plain_len >= 6) {
                int16_t dx = (int16_t)(plaintext[2] | (plaintext[3] << 8));
                int16_t dy = (int16_t)(plaintext[4] | (plaintext[5] << 8));
                success = hid_mouse_move(dx, dy);
                snprintf(event_data, sizeof(event_data),
                    "\"from\":\"0x%04x\",\"type\":\"mouse\",\"action\":\"move\",\"encrypted\":true,\"ok\":%s",
                    ctx->addr, success ? "true" : "false");
            }
            break;
        case MESH_HID_MOUSE_CLICK:
            if (plain_len >= 3) {
                success = hid_mouse_click(plaintext[2]);
                snprintf(event_data, sizeof(event_data),
                    "\"from\":\"0x%04x\",\"type\":\"mouse\",\"action\":\"click\",\"encrypted\":true,\"ok\":%s",
                    ctx->addr, success ? "true" : "false");
            }
            break;
        case MESH_HID_MOUSE_SCROLL:
            if (plain_len >= 4) {
                success = hid_mouse_scroll((int8_t)plaintext[2], (int8_t)plaintext[3]);
                snprintf(event_data, sizeof(event_data),
                    "\"from\":\"0x%04x\",\"type\":\"mouse\",\"action\":\"scroll\",\"encrypted\":true,\"ok\":%s",
                    ctx->addr, success ? "true" : "false");
            }
            break;
        default:
            snprintf(event_data, sizeof(event_data),
                "\"from\":\"0x%04x\",\"encrypted\":true,\"error\":\"unknown_action\"", ctx->addr);
        }
        break;

    case MESH_HID_TYPE_CONSUMER:
        if (action == MESH_HID_CONSUMER_SEND && plain_len >= 4) {
            uint16_t usage = plaintext[2] | (plaintext[3] << 8);
            success = hid_consumer_send(usage);
            snprintf(event_data, sizeof(event_data),
                "\"from\":\"0x%04x\",\"type\":\"consumer\",\"encrypted\":true,\"ok\":%s",
                ctx->addr, success ? "true" : "false");
        }
        break;

    default:
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"encrypted\":true,\"error\":\"unknown_type\"", ctx->addr);
    }

    jsonl_serial_send_event("hid_cmd_rcvd", event_data);
    return 0;
}

/**
 * Handle encrypted text message
 */
static int text_enc_handler(const struct bt_mesh_model *model,
                            struct bt_mesh_msg_ctx *ctx,
                            struct net_buf_simple *buf)
{
    /* Debug: confirm handler is called */
    char dbg[64];
    snprintf(dbg, sizeof(dbg), "\"from\":\"0x%04x\",\"len\":%d", ctx->addr, buf->len);
    jsonl_serial_send_event("text_enc_handler", dbg);

    if (buf->len < 13) {  /* 4-byte counter + 1 char + 8-byte tag minimum */
        LOG_WRN("Encrypted text too short from 0x%04x", ctx->addr);
        return -EINVAL;
    }

    update_peer(ctx->addr, ctx->recv_rssi, 0);

    mesh_peer_t *peer = get_peer_ptr(ctx->addr);
    if (!peer || !peer->has_session_key) {
        LOG_WRN("No session key for peer 0x%04x", ctx->addr);
        return -ENOENT;
    }

    uint8_t plaintext[64];
    size_t plain_len;
    if (decrypt_message(peer, buf->data, buf->len, plaintext, &plain_len) != 0) {
        LOG_WRN("Text decryption failed from 0x%04x", ctx->addr);
        return -EBADMSG;
    }

    plaintext[plain_len] = '\0';

    char data[160];
    snprintf(data, sizeof(data), "\"from\":\"0x%04x\",\"rssi\":%d,\"text\":\"%s\"",
        ctx->addr, ctx->recv_rssi, (char *)plaintext);
    jsonl_serial_send_event("mesh_text_enc_rcvd", data);

    LOG_INF("Encrypted text from 0x%04x: %s", ctx->addr, (char *)plaintext);
    return 0;
}

/* ========== ENCRYPTED SEND FUNCTIONS ========== */

/**
 * Initiate key exchange with a peer
 * NOTE: Key exchange is always allowed - PIN auth is only for HID/GPIO commands
 */
int mesh_hid_key_exchange(uint16_t dst_addr)
{
    if (!bt_mesh_is_provisioned()) {
        return -ENOENT;
    }

    /* Ensure peer is in our table so we can store the session key when KEY_CONFIRM arrives */
    update_peer(dst_addr, 0, 0);

    /* Generate our challenge */
    sys_csrand_get(local_challenge, sizeof(local_challenge));
    pending_key_exchange = true;
    pending_key_peer = dst_addr;

    /* Send key exchange request */
    BT_MESH_MODEL_BUF_DEFINE(msg, MESH_HID_OP_KEY_EXCHANGE, 16);
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_KEY_EXCHANGE);
    net_buf_simple_add_mem(&msg, local_challenge, 16);

    struct bt_mesh_msg_ctx ctx = {
        .net_idx = 0,
        .app_idx = 0,
        .addr = dst_addr,
        .send_ttl = MESH_TTL_DEFAULT,
    };

    LOG_INF("Initiating key exchange with 0x%04x", dst_addr);
    return bt_mesh_model_send(&vendor_models[0], &ctx, &msg, NULL, NULL);
}

/**
 * Check if peer has session key
 */
bool mesh_hid_has_session_key(uint16_t addr)
{
    mesh_peer_t *peer = get_peer_ptr(addr);
    return peer && peer->has_session_key;
}

/**
 * Clear session key for peer
 */
void mesh_hid_clear_session_key(uint16_t addr)
{
    mesh_peer_t *peer = get_peer_ptr(addr);
    if (peer) {
        peer->has_session_key = false;
        memset(peer->session_key, 0, sizeof(peer->session_key));
        peer->tx_counter = 0;
        peer->rx_counter = 0;
        LOG_INF("Cleared session key for 0x%04x", addr);
    }
}

/**
 * Helper to send encrypted HID command
 */
static int send_hid_cmd_encrypted(uint16_t dst_addr, uint8_t type, uint8_t action,
                                   const void *data, size_t data_len)
{
    if (!bt_mesh_is_provisioned()) {
        return -ENOENT;
    }

    mesh_peer_t *peer = get_peer_ptr(dst_addr);
    if (!peer || !peer->has_session_key) {
        LOG_WRN("No session key for 0x%04x", dst_addr);
        return -ENOENT;
    }

    /* Build plaintext: [type][action][data] */
    uint8_t plaintext[64];
    plaintext[0] = type;
    plaintext[1] = action;
    if (data && data_len > 0) {
        memcpy(&plaintext[2], data, data_len);
    }
    size_t plain_len = 2 + data_len;

    /* Encrypt */
    uint8_t ciphertext[80];
    size_t cipher_len;
    if (encrypt_message(peer, plaintext, plain_len, ciphertext, &cipher_len) != 0) {
        LOG_ERR("Encryption failed");
        return -1;
    }

    /* Send encrypted message */
    BT_MESH_MODEL_BUF_DEFINE(msg, MESH_HID_OP_HID_CMD_ENC, 80);
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_HID_CMD_ENC);
    net_buf_simple_add_mem(&msg, ciphertext, cipher_len);

    struct bt_mesh_msg_ctx ctx = {
        .net_idx = 0,
        .app_idx = 0,
        .addr = dst_addr,
        .send_ttl = MESH_TTL_DEFAULT,
    };

    return bt_mesh_model_send(&vendor_models[0], &ctx, &msg, NULL, NULL);
}

/**
 * Send encrypted text message
 */
int mesh_hid_send_text_encrypted(uint16_t dst_addr, const char *text)
{
    if (!bt_mesh_is_provisioned()) {
        return -ENOENT;
    }

    mesh_peer_t *peer = get_peer_ptr(dst_addr);
    if (!peer || !peer->has_session_key) {
        return -ENOENT;
    }

    size_t len = strlen(text);
    if (len > 48) len = 48;  /* Leave room for overhead */

    uint8_t ciphertext[64];
    size_t cipher_len;
    if (encrypt_message(peer, (const uint8_t *)text, len, ciphertext, &cipher_len) != 0) {
        return -1;
    }

    BT_MESH_MODEL_BUF_DEFINE(msg, MESH_HID_OP_TEXT_ENC, 64);
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_TEXT_ENC);
    net_buf_simple_add_mem(&msg, ciphertext, cipher_len);

    struct bt_mesh_msg_ctx ctx = {
        .net_idx = 0,
        .app_idx = 0,
        .addr = dst_addr,
        .send_ttl = MESH_TTL_DEFAULT,
    };

    LOG_INF("Sending encrypted text to 0x%04x", dst_addr);
    return bt_mesh_model_send(&vendor_models[0], &ctx, &msg, NULL, NULL);
}

/**
 * Send encrypted keyboard type command
 */
int mesh_hid_send_keyboard_type_encrypted(uint16_t dst_addr, const char *text)
{
    size_t len = strlen(text);
    if (len > 46) len = 46;  /* Account for encryption overhead */
    LOG_INF("Sending encrypted keyboard type to 0x%04x", dst_addr);
    return send_hid_cmd_encrypted(dst_addr, MESH_HID_TYPE_KEYBOARD, MESH_HID_KB_TYPE, text, len);
}

/**
 * Send encrypted keyboard tap command
 */
int mesh_hid_send_keyboard_tap_encrypted(uint16_t dst_addr, uint8_t keycode, uint8_t modifiers)
{
    uint8_t data[2] = { keycode, modifiers };
    LOG_INF("Sending encrypted keyboard tap to 0x%04x", dst_addr);
    return send_hid_cmd_encrypted(dst_addr, MESH_HID_TYPE_KEYBOARD, MESH_HID_KB_TAP, data, sizeof(data));
}

/**
 * Send encrypted mouse move command
 */
int mesh_hid_send_mouse_move_encrypted(uint16_t dst_addr, int16_t dx, int16_t dy)
{
    uint8_t data[4] = {
        dx & 0xFF, (dx >> 8) & 0xFF,
        dy & 0xFF, (dy >> 8) & 0xFF
    };
    return send_hid_cmd_encrypted(dst_addr, MESH_HID_TYPE_MOUSE, MESH_HID_MOUSE_MOVE, data, sizeof(data));
}

/**
 * Send encrypted mouse click command
 */
int mesh_hid_send_mouse_click_encrypted(uint16_t dst_addr, uint8_t buttons)
{
    return send_hid_cmd_encrypted(dst_addr, MESH_HID_TYPE_MOUSE, MESH_HID_MOUSE_CLICK, &buttons, 1);
}

/**
 * Send encrypted consumer key command
 */
int mesh_hid_send_consumer_encrypted(uint16_t dst_addr, uint16_t usage_id)
{
    uint8_t data[2] = { usage_id & 0xFF, (usage_id >> 8) & 0xFF };
    return send_hid_cmd_encrypted(dst_addr, MESH_HID_TYPE_CONSUMER, MESH_HID_CONSUMER_SEND, data, sizeof(data));
}

/**
 * Enable/disable encryption requirement for HID commands
 */
void mesh_hid_set_encryption_required(bool required)
{
    encryption_required = required;
    LOG_INF("E2E encryption %s for HID commands", required ? "required" : "disabled");
}

/**
 * Check if encryption is required for HID commands
 */
bool mesh_hid_encryption_required(void)
{
    return encryption_required;
}

/* ========== GPIO MESH COMMANDS ========== */

/**
 * Handle GPIO command (plaintext) - requires PIN auth like HID
 * Format: [action][led_id][data...]
 */
static int gpio_cmd_handler(const struct bt_mesh_model *model,
                            struct bt_mesh_msg_ctx *ctx,
                            struct net_buf_simple *buf)
{
    if (buf->len < 2) {
        LOG_WRN("GPIO command too short from 0x%04x", ctx->addr);
        return -EINVAL;
    }

    /* Update peer tracking */
    update_peer(ctx->addr, ctx->recv_rssi, 0);

    /* SECURITY: Reject plaintext GPIO commands when encryption is required */
    if (encryption_required) {
        LOG_WRN("Plaintext GPIO command rejected from 0x%04x - encryption required", ctx->addr);
        char event_data[128];
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"rssi\":%d,\"error\":\"encryption_required\"",
            ctx->addr, ctx->recv_rssi);
        jsonl_serial_send_event("gpio_cmd_rejected", event_data);
        return -EACCES;
    }

    /* Check PIN authentication */
    if (pin_required && !mesh_hid_is_peer_authenticated(ctx->addr)) {
        LOG_WRN("GPIO command rejected - peer 0x%04x not authenticated", ctx->addr);
        char event_data[96];
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"rssi\":%d,\"error\":\"not_authenticated\"",
            ctx->addr, ctx->recv_rssi);
        jsonl_serial_send_event("gpio_cmd_rejected", event_data);
        return -EACCES;
    }

    uint8_t action = net_buf_simple_pull_u8(buf);
    uint8_t led_id = net_buf_simple_pull_u8(buf);

    LOG_INF("GPIO cmd from 0x%04x: action=%d led=%d", ctx->addr, action, led_id);

    char event_data[128];
    bool success = false;

    switch (action) {
    case MESH_GPIO_LED_SET:
        if (buf->len >= 1) {
            bool on = buf->data[0] != 0;
            success = gpio_led_set(led_id, on);
            snprintf(event_data, sizeof(event_data),
                "\"from\":\"0x%04x\",\"action\":\"led_set\",\"id\":%d,\"on\":%s,\"ok\":%s",
                ctx->addr, led_id, on ? "true" : "false", success ? "true" : "false");
        }
        break;

    case MESH_GPIO_LED_TOGGLE:
        success = true;
        bool new_state = gpio_led_toggle(led_id);
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"action\":\"led_toggle\",\"id\":%d,\"on\":%s",
            ctx->addr, led_id, new_state ? "true" : "false");
        break;

    case MESH_GPIO_LED_BLINK:
        if (buf->len >= 1) {
            uint8_t count = buf->data[0];
            gpio_led_blink(led_id, count, 100, 100);
            success = true;
            snprintf(event_data, sizeof(event_data),
                "\"from\":\"0x%04x\",\"action\":\"led_blink\",\"id\":%d,\"count\":%d",
                ctx->addr, led_id, count);
        }
        break;

    default:
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"error\":\"unknown_gpio_action\",\"action\":%d", ctx->addr, action);
    }

    jsonl_serial_send_event("gpio_cmd_rcvd", event_data);
    return 0;
}

/**
 * Handle encrypted GPIO command
 */
static int gpio_cmd_enc_handler(const struct bt_mesh_model *model,
                                struct bt_mesh_msg_ctx *ctx,
                                struct net_buf_simple *buf)
{
    /* Minimum: 4-byte counter + 2-byte payload + 8-byte tag */
    if (buf->len < 14) {
        LOG_WRN("Encrypted GPIO command too short from 0x%04x", ctx->addr);
        return -EINVAL;
    }

    /* Update peer tracking */
    update_peer(ctx->addr, ctx->recv_rssi, 0);

    /* Get peer and check session key */
    mesh_peer_t *peer = get_peer_ptr(ctx->addr);
    if (!peer || !peer->has_session_key) {
        LOG_WRN("No session key for peer 0x%04x", ctx->addr);
        char event_data[96];
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"error\":\"no_session_key\"", ctx->addr);
        jsonl_serial_send_event("gpio_cmd_rejected", event_data);
        return -ENOENT;
    }

    /* Check PIN authentication */
    if (pin_required && !mesh_hid_is_peer_authenticated(ctx->addr)) {
        LOG_WRN("Encrypted GPIO rejected - peer 0x%04x not authenticated", ctx->addr);
        char event_data[96];
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"error\":\"not_authenticated\"", ctx->addr);
        jsonl_serial_send_event("gpio_cmd_rejected", event_data);
        return -EACCES;
    }

    /* Decrypt */
    uint8_t plaintext[32];
    size_t plain_len;
    if (decrypt_message(peer, buf->data, buf->len, plaintext, &plain_len) != 0) {
        LOG_WRN("GPIO decryption failed from 0x%04x", ctx->addr);
        char event_data[96];
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"error\":\"decryption_failed\"", ctx->addr);
        jsonl_serial_send_event("gpio_cmd_rejected", event_data);
        return -EBADMSG;
    }

    if (plain_len < 2) {
        return -EINVAL;
    }

    uint8_t action = plaintext[0];
    uint8_t led_id = plaintext[1];

    LOG_INF("Encrypted GPIO cmd from 0x%04x: action=%d led=%d", ctx->addr, action, led_id);

    char event_data[128];
    bool success = false;

    switch (action) {
    case MESH_GPIO_LED_SET:
        if (plain_len >= 3) {
            bool on = plaintext[2] != 0;
            success = gpio_led_set(led_id, on);
            snprintf(event_data, sizeof(event_data),
                "\"from\":\"0x%04x\",\"action\":\"led_set\",\"id\":%d,\"on\":%s,\"encrypted\":true,\"ok\":%s",
                ctx->addr, led_id, on ? "true" : "false", success ? "true" : "false");
        }
        break;

    case MESH_GPIO_LED_TOGGLE:
        success = true;
        bool new_state = gpio_led_toggle(led_id);
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"action\":\"led_toggle\",\"id\":%d,\"on\":%s,\"encrypted\":true",
            ctx->addr, led_id, new_state ? "true" : "false");
        break;

    case MESH_GPIO_LED_BLINK:
        if (plain_len >= 3) {
            uint8_t count = plaintext[2];
            gpio_led_blink(led_id, count, 100, 100);
            success = true;
            snprintf(event_data, sizeof(event_data),
                "\"from\":\"0x%04x\",\"action\":\"led_blink\",\"id\":%d,\"count\":%d,\"encrypted\":true",
                ctx->addr, led_id, count);
        }
        break;

    default:
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"encrypted\":true,\"error\":\"unknown_gpio_action\"", ctx->addr);
    }

    jsonl_serial_send_event("gpio_cmd_rcvd", event_data);
    return 0;
}

/* ========== GPIO SEND FUNCTIONS ========== */

/**
 * Send GPIO command helper (plaintext)
 */
static int send_gpio_cmd(uint16_t dst_addr, uint8_t action, uint8_t led_id,
                         const void *data, size_t data_len)
{
    if (!bt_mesh_is_provisioned()) {
        return -ENOENT;
    }

    BT_MESH_MODEL_BUF_DEFINE(msg, MESH_HID_OP_GPIO_CMD, 16);
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_GPIO_CMD);
    net_buf_simple_add_u8(&msg, action);
    net_buf_simple_add_u8(&msg, led_id);
    if (data && data_len > 0) {
        net_buf_simple_add_mem(&msg, data, data_len);
    }

    struct bt_mesh_msg_ctx ctx = {
        .net_idx = 0,
        .app_idx = 0,
        .addr = dst_addr,
        .send_ttl = MESH_TTL_DEFAULT,
    };

    return bt_mesh_model_send(&vendor_models[0], &ctx, &msg, NULL, NULL);
}

/**
 * Send encrypted GPIO command helper
 */
static int send_gpio_cmd_encrypted(uint16_t dst_addr, uint8_t action, uint8_t led_id,
                                   const void *data, size_t data_len)
{
    if (!bt_mesh_is_provisioned()) {
        return -ENOENT;
    }

    mesh_peer_t *peer = get_peer_ptr(dst_addr);
    if (!peer || !peer->has_session_key) {
        LOG_WRN("No session key for 0x%04x", dst_addr);
        return -ENOENT;
    }

    /* Build plaintext: [action][led_id][data] */
    uint8_t plaintext[16];
    plaintext[0] = action;
    plaintext[1] = led_id;
    if (data && data_len > 0) {
        memcpy(&plaintext[2], data, data_len);
    }
    size_t plain_len = 2 + data_len;

    /* Encrypt */
    uint8_t ciphertext[32];
    size_t cipher_len;
    if (encrypt_message(peer, plaintext, plain_len, ciphertext, &cipher_len) != 0) {
        LOG_ERR("GPIO encryption failed");
        return -1;
    }

    /* Send encrypted message */
    BT_MESH_MODEL_BUF_DEFINE(msg, MESH_HID_OP_GPIO_CMD_ENC, 32);
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_GPIO_CMD_ENC);
    net_buf_simple_add_mem(&msg, ciphertext, cipher_len);

    struct bt_mesh_msg_ctx ctx = {
        .net_idx = 0,
        .app_idx = 0,
        .addr = dst_addr,
        .send_ttl = MESH_TTL_DEFAULT,
    };

    return bt_mesh_model_send(&vendor_models[0], &ctx, &msg, NULL, NULL);
}

/**
 * Send GPIO LED set command (plaintext)
 */
int mesh_hid_send_gpio_led(uint16_t dst_addr, uint8_t led_id, bool on)
{
    uint8_t data = on ? 1 : 0;
    LOG_INF("Sending GPIO LED set to 0x%04x: led=%d on=%d", dst_addr, led_id, on);
    return send_gpio_cmd(dst_addr, MESH_GPIO_LED_SET, led_id, &data, 1);
}

/**
 * Send GPIO LED toggle command (plaintext)
 */
int mesh_hid_send_gpio_toggle(uint16_t dst_addr, uint8_t led_id)
{
    LOG_INF("Sending GPIO LED toggle to 0x%04x: led=%d", dst_addr, led_id);
    return send_gpio_cmd(dst_addr, MESH_GPIO_LED_TOGGLE, led_id, NULL, 0);
}

/**
 * Send GPIO LED blink command (plaintext)
 */
int mesh_hid_send_gpio_blink(uint16_t dst_addr, uint8_t led_id, uint8_t count)
{
    LOG_INF("Sending GPIO LED blink to 0x%04x: led=%d count=%d", dst_addr, led_id, count);
    return send_gpio_cmd(dst_addr, MESH_GPIO_LED_BLINK, led_id, &count, 1);
}

/**
 * Send encrypted GPIO LED set command
 */
int mesh_hid_send_gpio_led_encrypted(uint16_t dst_addr, uint8_t led_id, bool on)
{
    uint8_t data = on ? 1 : 0;
    LOG_INF("Sending encrypted GPIO LED set to 0x%04x: led=%d on=%d", dst_addr, led_id, on);
    return send_gpio_cmd_encrypted(dst_addr, MESH_GPIO_LED_SET, led_id, &data, 1);
}

/**
 * Send encrypted GPIO LED toggle command
 */
int mesh_hid_send_gpio_toggle_encrypted(uint16_t dst_addr, uint8_t led_id)
{
    LOG_INF("Sending encrypted GPIO LED toggle to 0x%04x: led=%d", dst_addr, led_id);
    return send_gpio_cmd_encrypted(dst_addr, MESH_GPIO_LED_TOGGLE, led_id, NULL, 0);
}

/**
 * Send encrypted GPIO LED blink command
 */
int mesh_hid_send_gpio_blink_encrypted(uint16_t dst_addr, uint8_t led_id, uint8_t count)
{
    LOG_INF("Sending encrypted GPIO LED blink to 0x%04x: led=%d count=%d", dst_addr, led_id, count);
    return send_gpio_cmd_encrypted(dst_addr, MESH_GPIO_LED_BLINK, led_id, &count, 1);
}

/* ========== RC PWM CONTROL ========== */

/**
 * Convert joystick value (-1000 to +1000) to PWM microseconds (1000-2000)
 */
static inline uint16_t rc_value_to_pwm(int16_t value)
{
    /* Clamp input */
    if (value < MESH_RC_VECTOR_MIN) value = MESH_RC_VECTOR_MIN;
    if (value > MESH_RC_VECTOR_MAX) value = MESH_RC_VECTOR_MAX;

    /* Convert: -1000 -> 1000us, 0 -> 1500us, +1000 -> 2000us */
    return (uint16_t)(1500 + (value / 2));
}

/**
 * Apply drive mode mixing and inversion, then set PWM outputs
 */
static void rc_apply_vector(int16_t x, int16_t y)
{
    int16_t ch0_value, ch1_value;

    if (rc_drive_mode == MESH_RC_MODE_SKID_STEER) {
        /* Skid steer: left = Y + X, right = Y - X */
        ch0_value = y + x;  /* Left motor */
        ch1_value = y - x;  /* Right motor */

        /* Clamp to valid range */
        if (ch0_value < MESH_RC_VECTOR_MIN) ch0_value = MESH_RC_VECTOR_MIN;
        if (ch0_value > MESH_RC_VECTOR_MAX) ch0_value = MESH_RC_VECTOR_MAX;
        if (ch1_value < MESH_RC_VECTOR_MIN) ch1_value = MESH_RC_VECTOR_MIN;
        if (ch1_value > MESH_RC_VECTOR_MAX) ch1_value = MESH_RC_VECTOR_MAX;
    } else {
        /* Normal mode: CH0 = X (steering), CH1 = Y (throttle) */
        ch0_value = x;
        ch1_value = y;
    }

    /* Apply channel inversion */
    if (rc_invert_flags & MESH_RC_INVERT_CH0) {
        ch0_value = -ch0_value;
    }
    if (rc_invert_flags & MESH_RC_INVERT_CH1) {
        ch1_value = -ch1_value;
    }

    /* Swap channels if requested */
    if (rc_invert_flags & MESH_RC_SWAP_CHANNELS) {
        int16_t tmp = ch0_value;
        ch0_value = ch1_value;
        ch1_value = tmp;
    }

    /* Convert to PWM and set outputs */
    uint16_t pwm0 = rc_value_to_pwm(ch0_value);
    uint16_t pwm1 = rc_value_to_pwm(ch1_value);

    gpio_rc_set(0, pwm0);
    gpio_rc_set(1, pwm1);

    LOG_DBG("RC vector x=%d y=%d -> ch0=%d ch1=%d (pwm %d, %d us)",
            x, y, ch0_value, ch1_value, pwm0, pwm1);
}

/**
 * Handle RC vector command (plaintext) - requires PIN auth
 * Format: [x_lo][x_hi][y_lo][y_hi][flags] (5 bytes)
 */
static int rc_vector_handler(const struct bt_mesh_model *model,
                             struct bt_mesh_msg_ctx *ctx,
                             struct net_buf_simple *buf)
{
    if (buf->len < sizeof(mesh_rc_vector_t)) {
        LOG_WRN("RC vector command too short from 0x%04x", ctx->addr);
        return -EINVAL;
    }

    update_peer(ctx->addr, ctx->recv_rssi, 0);

    /* SECURITY: Reject plaintext when encryption is required */
    if (encryption_required) {
        LOG_WRN("Plaintext RC command rejected from 0x%04x - encryption required", ctx->addr);
        return -EACCES;
    }

    /* Check PIN authentication */
    if (pin_required && !mesh_hid_is_peer_authenticated(ctx->addr)) {
        LOG_WRN("RC command rejected - peer 0x%04x not authenticated", ctx->addr);
        return -EACCES;
    }

    /* Parse vector */
    int16_t x = (int16_t)net_buf_simple_pull_le16(buf);
    int16_t y = (int16_t)net_buf_simple_pull_le16(buf);
    uint8_t flags = net_buf_simple_pull_u8(buf);
    (void)flags; /* Reserved for future use */

    /* Apply mixing and set PWM */
    rc_apply_vector(x, y);

    /* Send event (but not too often - RC commands come fast) */
    static uint32_t last_event_time = 0;
    uint32_t now = k_uptime_get_32();
    if (now - last_event_time > 500) {  /* Max 2 events/sec */
        char event_data[96];
        snprintf(event_data, sizeof(event_data),
            "\"from\":\"0x%04x\",\"x\":%d,\"y\":%d,\"mode\":\"%s\"",
            ctx->addr, x, y,
            rc_drive_mode == MESH_RC_MODE_SKID_STEER ? "skid_steer" : "normal");
        jsonl_serial_send_event("rc_vector", event_data);
        last_event_time = now;
    }

    return 0;
}

/**
 * Handle encrypted RC vector command
 */
static int rc_vector_enc_handler(const struct bt_mesh_model *model,
                                 struct bt_mesh_msg_ctx *ctx,
                                 struct net_buf_simple *buf)
{
    /* Minimum: 4-byte counter + 5-byte payload + 8-byte tag = 17 bytes */
    if (buf->len < 17) {
        LOG_WRN("Encrypted RC vector too short from 0x%04x", ctx->addr);
        return -EINVAL;
    }

    update_peer(ctx->addr, ctx->recv_rssi, 0);

    /* Get peer and check session key */
    mesh_peer_t *peer = get_peer_ptr(ctx->addr);
    if (!peer || !peer->has_session_key) {
        LOG_WRN("No session key for peer 0x%04x", ctx->addr);
        return -ENOENT;
    }

    /* Check PIN authentication */
    if (pin_required && !mesh_hid_is_peer_authenticated(ctx->addr)) {
        LOG_WRN("Encrypted RC rejected - peer 0x%04x not authenticated", ctx->addr);
        return -EACCES;
    }

    /* Decrypt */
    uint8_t plaintext[16];
    size_t plain_len;
    if (decrypt_message(peer, buf->data, buf->len, plaintext, &plain_len) != 0) {
        LOG_WRN("RC decryption failed from 0x%04x", ctx->addr);
        return -EBADMSG;
    }

    if (plain_len < sizeof(mesh_rc_vector_t)) {
        return -EINVAL;
    }

    /* Parse vector */
    int16_t x = (int16_t)(plaintext[0] | (plaintext[1] << 8));
    int16_t y = (int16_t)(plaintext[2] | (plaintext[3] << 8));

    /* Apply mixing and set PWM */
    rc_apply_vector(x, y);

    return 0;
}

/**
 * Handle RC configuration command
 * Format: [mode][invert_flags] (2 bytes)
 */
static int rc_config_handler(const struct bt_mesh_model *model,
                             struct bt_mesh_msg_ctx *ctx,
                             struct net_buf_simple *buf)
{
    if (buf->len < sizeof(mesh_rc_config_t)) {
        LOG_WRN("RC config command too short from 0x%04x", ctx->addr);
        return -EINVAL;
    }

    update_peer(ctx->addr, ctx->recv_rssi, 0);

    /* Check PIN authentication */
    if (pin_required && !mesh_hid_is_peer_authenticated(ctx->addr)) {
        LOG_WRN("RC config rejected - peer 0x%04x not authenticated", ctx->addr);
        return -EACCES;
    }

    uint8_t mode = net_buf_simple_pull_u8(buf);
    uint8_t invert = net_buf_simple_pull_u8(buf);

    /* Validate mode */
    if (mode > MESH_RC_MODE_SKID_STEER) {
        LOG_WRN("Invalid RC mode %d from 0x%04x", mode, ctx->addr);
        return -EINVAL;
    }

    rc_drive_mode = mode;
    rc_invert_flags = invert;

    LOG_INF("RC config from 0x%04x: mode=%s invert=0x%02x",
            ctx->addr,
            mode == MESH_RC_MODE_SKID_STEER ? "skid_steer" : "normal",
            invert);

    char event_data[128];
    snprintf(event_data, sizeof(event_data),
        "\"from\":\"0x%04x\",\"mode\":\"%s\",\"invert_ch0\":%s,\"invert_ch1\":%s,\"swap\":%s",
        ctx->addr,
        mode == MESH_RC_MODE_SKID_STEER ? "skid_steer" : "normal",
        (invert & MESH_RC_INVERT_CH0) ? "true" : "false",
        (invert & MESH_RC_INVERT_CH1) ? "true" : "false",
        (invert & MESH_RC_SWAP_CHANNELS) ? "true" : "false");
    jsonl_serial_send_event("rc_config", event_data);

    return 0;
}

/* ========== RC SEND FUNCTIONS ========== */

/**
 * Send RC vector command (plaintext)
 */
int mesh_hid_send_rc_vector(uint16_t dst_addr, int16_t x, int16_t y)
{
    if (!bt_mesh_is_provisioned()) {
        return -ENOENT;
    }

    struct bt_mesh_msg_ctx ctx = {
        .net_idx = 0,
        .app_idx = 0,
        .addr = dst_addr,
        .send_ttl = MESH_TTL_DEFAULT,
    };

    NET_BUF_SIMPLE_DEFINE(msg, 16);
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_RC_VECTOR);
    net_buf_simple_add_le16(&msg, (uint16_t)x);
    net_buf_simple_add_le16(&msg, (uint16_t)y);
    net_buf_simple_add_u8(&msg, 0);  /* flags reserved */

    return bt_mesh_model_send(&vendor_models[0], &ctx, &msg, NULL, NULL);
}

/**
 * Send encrypted RC vector command
 */
int mesh_hid_send_rc_vector_encrypted(uint16_t dst_addr, int16_t x, int16_t y)
{
    if (!bt_mesh_is_provisioned()) {
        return -ENOENT;
    }

    mesh_peer_t *peer = get_peer_ptr(dst_addr);
    if (!peer || !peer->has_session_key) {
        return -ENOENT;
    }

    /* Build plaintext */
    uint8_t plaintext[5];
    plaintext[0] = (uint8_t)(x & 0xFF);
    plaintext[1] = (uint8_t)((x >> 8) & 0xFF);
    plaintext[2] = (uint8_t)(y & 0xFF);
    plaintext[3] = (uint8_t)((y >> 8) & 0xFF);
    plaintext[4] = 0;  /* flags */

    uint8_t ciphertext[32];
    size_t cipher_len;
    if (encrypt_message(peer, plaintext, sizeof(plaintext), ciphertext, &cipher_len) != 0) {
        return -EIO;
    }

    struct bt_mesh_msg_ctx ctx = {
        .net_idx = 0,
        .app_idx = 0,
        .addr = dst_addr,
        .send_ttl = MESH_TTL_DEFAULT,
    };

    NET_BUF_SIMPLE_DEFINE(msg, 48);
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_RC_VECTOR_ENC);
    net_buf_simple_add_mem(&msg, ciphertext, cipher_len);

    return bt_mesh_model_send(&vendor_models[0], &ctx, &msg, NULL, NULL);
}

/**
 * Send RC configuration command
 */
int mesh_hid_send_rc_config(uint16_t dst_addr, uint8_t mode, uint8_t invert)
{
    if (!bt_mesh_is_provisioned()) {
        return -ENOENT;
    }

    struct bt_mesh_msg_ctx ctx = {
        .net_idx = 0,
        .app_idx = 0,
        .addr = dst_addr,
        .send_ttl = MESH_TTL_DEFAULT,
    };

    NET_BUF_SIMPLE_DEFINE(msg, 12);
    bt_mesh_model_msg_init(&msg, MESH_HID_OP_RC_CONFIG);
    net_buf_simple_add_u8(&msg, mode);
    net_buf_simple_add_u8(&msg, invert);

    return bt_mesh_model_send(&vendor_models[0], &ctx, &msg, NULL, NULL);
}

/* ========== RC CONFIG GETTERS/SETTERS ========== */

void mesh_hid_set_rc_mode(uint8_t mode)
{
    if (mode <= MESH_RC_MODE_SKID_STEER) {
        rc_drive_mode = mode;
        LOG_INF("RC mode set to %s",
                mode == MESH_RC_MODE_SKID_STEER ? "skid_steer" : "normal");
    }
}

uint8_t mesh_hid_get_rc_mode(void)
{
    return rc_drive_mode;
}

void mesh_hid_set_rc_invert(uint8_t invert)
{
    rc_invert_flags = invert;
    LOG_INF("RC invert flags set to 0x%02x", invert);
}

uint8_t mesh_hid_get_rc_invert(void)
{
    return rc_invert_flags;
}

void mesh_hid_apply_rc_vector(int16_t x, int16_t y)
{
    rc_apply_vector(x, y);
}

/**
 * Set periodic discovery interval
 */
void mesh_hid_set_discovery_interval(uint32_t interval_ms)
{
    discovery_interval_ms = interval_ms;
    LOG_INF("Periodic discovery interval set to %u ms", interval_ms);

    /* If enabling and we're ready, schedule now */
    if (interval_ms > 0 && periodic_discovery_enabled &&
        bt_mesh_is_provisioned() && app_key_bound) {
        k_work_schedule(&discovery_work, K_MSEC(interval_ms));
    }
}

/**
 * Get current periodic discovery interval
 */
uint32_t mesh_hid_get_discovery_interval(void)
{
    return discovery_interval_ms;
}

/**
 * Enable/disable periodic discovery
 */
void mesh_hid_set_periodic_discovery(bool enabled)
{
    periodic_discovery_enabled = enabled;
    LOG_INF("Periodic discovery %s", enabled ? "enabled" : "disabled");

    if (enabled && discovery_interval_ms > 0 &&
        bt_mesh_is_provisioned() && app_key_bound) {
        /* Schedule next discovery */
        k_work_schedule(&discovery_work, K_MSEC(discovery_interval_ms));
    } else if (!enabled) {
        /* Cancel pending discovery */
        k_work_cancel_delayable(&discovery_work);
    }
}

/**
 * Check if periodic discovery is enabled
 */
bool mesh_hid_periodic_discovery_enabled(void)
{
    return periodic_discovery_enabled;
}

/**
 * Check if a peer is stale (not seen recently)
 */
bool mesh_hid_is_peer_stale(uint16_t addr)
{
    uint32_t now = k_uptime_get_32();

    for (int i = 0; i < peer_count; i++) {
        if (peers[i].addr == addr) {
            if (peers[i].last_seen == 0) {
                return false;  /* Never seen = not stale, just unknown */
            }
            return (now - peers[i].last_seen) > PEER_STALE_TIMEOUT_MS;
        }
    }
    return false;  /* Unknown peer */
}

/**
 * Get peer's last seen time in ms since boot
 */
uint32_t mesh_hid_get_peer_last_seen(uint16_t addr)
{
    for (int i = 0; i < peer_count; i++) {
        if (peers[i].addr == addr) {
            return peers[i].last_seen;
        }
    }
    return 0;
}

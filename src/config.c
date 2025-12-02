/*
 * Brewer BLE HID Bridge - Configuration Storage Implementation
 *
 * Uses Zephyr NVS (Non-Volatile Storage) for persistent settings.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>

#include "config.h"
#include "protocol.h"
#include "ble_hid_service.h"

LOG_MODULE_REGISTER(config, LOG_LEVEL_INF);

/* Flag to track if settings subsystem is ready */
static bool settings_ready = false;

/* Settings keys */
#define SETTINGS_DEVICE_NAME    "hid/name"
#define SETTINGS_PIN            "hid/pin"
#define SETTINGS_ATTEMPTS       "hid/attempts"

/* Hip-hop inspired default names - picked randomly on first boot */
/* Keep names SHORT (<12 chars) to fit in BLE advertising packet */
static const char * const hiphop_names[] = {
    "DJ Click",
    "MC Keys",
    "Lil Cursor",
    "Big HID",
    "2Pac-kets",
    "Run-DMA",
    "Ctrl-Alt",
    "Wu-Tang",
    "Snoop Dog",
    "Dr. BT",
    "Ice Type",
    "Busta B",
    "Jay-USB",
    "Em-emory",
    "50 Cent",
};
#define NUM_HIPHOP_NAMES (sizeof(hiphop_names) / sizeof(hiphop_names[0]))

/* Configuration state */
static struct {
    char device_name[MAX_DEVICE_NAME_LENGTH + 1];
    uint8_t name_length;
    char pin[MAX_PIN_LENGTH + 1];
    uint8_t pin_length;
    uint8_t failed_attempts;
    bool initialized;
} config;

/* SECURITY: Stricter lockout - 3 attempts, 5 minute lockout */
#define LOCKOUT_TIME_SEC 300
static int64_t lockout_until = 0;

/* Default PIN for new devices */
#define DEFAULT_PIN "123456"
#define DEFAULT_PIN_LENGTH 6

/**
 * Settings load handler
 */
static int config_settings_set(const char *name, size_t len,
                                settings_read_cb read_cb, void *cb_arg)
{
    LOG_INF("Settings load callback: key='%s', len=%d", name, (int)len);

    if (!strcmp(name, "name")) {
        if (len > MAX_DEVICE_NAME_LENGTH) {
            len = MAX_DEVICE_NAME_LENGTH;
        }
        int rc = read_cb(cb_arg, config.device_name, len);
        if (rc >= 0) {
            config.device_name[rc] = '\0';
            config.name_length = rc;
            LOG_INF("*** LOADED device name from NVS: '%s' ***", config.device_name);
        } else {
            LOG_ERR("Failed to read device name: %d", rc);
        }
        return 0;
    }

    if (!strcmp(name, "pin")) {
        if (len > MAX_PIN_LENGTH) {
            len = MAX_PIN_LENGTH;
        }
        int rc = read_cb(cb_arg, config.pin, len);
        if (rc >= 0) {
            config.pin[rc] = '\0';
            config.pin_length = rc;
            LOG_INF("Loaded PIN (%d digits)", config.pin_length);
        }
        return 0;
    }

    if (!strcmp(name, "attempts")) {
        if (len == sizeof(config.failed_attempts)) {
            read_cb(cb_arg, &config.failed_attempts, len);
        }
        return 0;
    }

    return -ENOENT;
}

/* Settings handler struct */
SETTINGS_STATIC_HANDLER_DEFINE(hid, "hid", NULL, config_settings_set, NULL, NULL);

/**
 * Generate a random hip-hop name with 4-char suffix for uniqueness
 * Format: "Name-XXXX" where X is alphanumeric
 */
static void generate_random_hiphop_name(char *buf, size_t buf_size)
{
    static const char alphanum[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ"; /* No I/O to avoid confusion */
    uint32_t rand_val = k_cycle_get_32();  /* Use cycle counter as entropy */

    /* Pick a random base name */
    const char *base = hiphop_names[rand_val % NUM_HIPHOP_NAMES];

    /* Generate 4 random alphanumeric chars */
    char suffix[5];
    for (int i = 0; i < 4; i++) {
        rand_val = rand_val * 1103515245 + 12345;  /* Simple LCG for more randomness */
        suffix[i] = alphanum[rand_val % (sizeof(alphanum) - 1)];
    }
    suffix[4] = '\0';

    /* Combine: "Name XXXX" */
    snprintf(buf, buf_size, "%s %s", base, suffix);
}

/**
 * Initialize configuration
 */
bool config_init(void)
{
    int rc;

    if (config.initialized) {
        return true;
    }

    LOG_INF("Config init starting...");

    /* Set temporary defaults (will be replaced if saved name exists) */
    config.device_name[0] = '\0';
    config.name_length = 0;
    config.pin[0] = '\0';
    config.pin_length = 0;
    config.failed_attempts = 0;

    LOG_INF("Calling settings_subsys_init...");

    /* Initialize settings subsystem */
    rc = settings_subsys_init();
    if (rc) {
        LOG_WRN("Settings subsys init failed: %d (continuing with defaults)", rc);
        /* Generate a random hip-hop name */
        generate_random_hiphop_name(config.device_name, sizeof(config.device_name));
        config.name_length = strlen(config.device_name);
        config.initialized = true;
        LOG_INF("Config init done (no settings): name='%s'", config.device_name);
        return true;
    }
    settings_ready = true;
    LOG_INF("Settings subsystem ready");

    /* Load from storage */
    LOG_INF("Loading settings...");
    rc = settings_load();
    if (rc) {
        LOG_WRN("Failed to load settings: %d (using defaults)", rc);
    }
    LOG_INF("Settings load done");

    /* If no name was loaded, generate a random hip-hop name and save it */
    if (config.name_length == 0) {
        generate_random_hiphop_name(config.device_name, sizeof(config.device_name));
        config.name_length = strlen(config.device_name);
        LOG_INF("First boot - generated name: %s", config.device_name);

        /* Save it so we keep the same name on reboot */
        if (settings_ready) {
            LOG_INF("Saving name to NVS...");
            settings_save_one(SETTINGS_DEVICE_NAME, config.device_name, config.name_length);
            LOG_INF("Name saved");
        }
    }

    /* SECURITY: If no PIN was loaded, set default PIN */
    if (config.pin_length == 0) {
        memcpy(config.pin, DEFAULT_PIN, DEFAULT_PIN_LENGTH);
        config.pin[DEFAULT_PIN_LENGTH] = '\0';
        config.pin_length = DEFAULT_PIN_LENGTH;
        LOG_INF("First boot - default PIN set (change it!)");

        /* Save default PIN to NVS */
        if (settings_ready) {
            settings_save_one(SETTINGS_PIN, config.pin, config.pin_length);
        }
    }

    config.initialized = true;
    LOG_INF("Config initialized: name='%s', PIN enabled (%d digits)",
            config.device_name, config.pin_length);

    return true;
}

/**
 * Get device name
 */
uint8_t config_get_name(char *name)
{
    if (name == NULL) {
        return 0;
    }

    strncpy(name, config.device_name, MAX_DEVICE_NAME_LENGTH + 1);
    return config.name_length;
}

/**
 * Set device name
 */
bool config_set_name(const char *name, uint8_t length)
{
    LOG_INF("config_set_name called: len=%d, settings_ready=%d", length, settings_ready);

    if (name == NULL || length == 0 || length > MAX_DEVICE_NAME_LENGTH) {
        LOG_ERR("Invalid name parameters");
        return false;
    }

    memcpy(config.device_name, name, length);
    config.device_name[length] = '\0';
    config.name_length = length;

    LOG_INF("Name in RAM: '%s'", config.device_name);

    /* Update BLE advertising name */
    bool ble_ok = ble_hid_service_set_name(config.device_name);
    LOG_INF("BLE set_name returned: %d", ble_ok);

    /* Save to persistent storage if available */
    if (settings_ready) {
        int rc = settings_save_one(SETTINGS_DEVICE_NAME, config.device_name, length);
        if (rc) {
            LOG_ERR("Failed to save device name: %d", rc);
        } else {
            LOG_INF("Name saved to NVS successfully");
        }
    } else {
        LOG_WRN("Settings not ready - name NOT persisted!");
    }

    LOG_INF("Device name set to: %s", config.device_name);
    return true;
}

/**
 * Check if PIN is enabled
 */
bool config_pin_enabled(void)
{
    return config.pin_length >= MIN_PIN_LENGTH;
}

/**
 * Set PIN
 */
bool config_set_pin(const char *pin, uint8_t length)
{
    if (pin == NULL || length < MIN_PIN_LENGTH || length > MAX_PIN_LENGTH) {
        return false;
    }

    /* Validate PIN contains only digits */
    for (uint8_t i = 0; i < length; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            LOG_WRN("Invalid PIN character");
            return false;
        }
    }

    memcpy(config.pin, pin, length);
    config.pin[length] = '\0';
    config.pin_length = length;
    config.failed_attempts = 0;

    /* Save to persistent storage if available */
    if (settings_ready) {
        int rc = settings_save_one(SETTINGS_PIN, config.pin, length);
        if (rc) {
            LOG_ERR("Failed to save PIN: %d", rc);
            /* Continue anyway - PIN is set in RAM */
        }
    }

    LOG_INF("PIN set (%d digits)", length);
    return true;
}

/**
 * Clear PIN
 */
bool config_clear_pin(void)
{
    config.pin[0] = '\0';
    config.pin_length = 0;
    config.failed_attempts = 0;

    /* Remove from storage if available */
    if (settings_ready) {
        int rc = settings_delete(SETTINGS_PIN);
        if (rc && rc != -ENOENT) {
            LOG_ERR("Failed to delete PIN: %d", rc);
            /* Continue anyway - PIN is cleared in RAM */
        }
    }

    LOG_INF("PIN cleared");
    return true;
}

/**
 * Verify PIN
 */
bool config_verify_pin(const char *pin, uint8_t length, uint8_t *attempts_left)
{
    /* Check lockout */
    if (config_is_locked_out()) {
        if (attempts_left) {
            *attempts_left = 0;
        }
        return false;
    }

    /* If no PIN set, always succeed */
    if (!config_pin_enabled()) {
        if (attempts_left) {
            *attempts_left = MAX_PIN_ATTEMPTS;
        }
        return true;
    }

    /* Validate input */
    if (pin == NULL || length != config.pin_length) {
        config.failed_attempts++;
        if (attempts_left) {
            *attempts_left = MAX_PIN_ATTEMPTS > config.failed_attempts ?
                             MAX_PIN_ATTEMPTS - config.failed_attempts : 0;
        }

        /* Check for lockout */
        if (config.failed_attempts >= MAX_PIN_ATTEMPTS) {
            lockout_until = k_uptime_get() + (LOCKOUT_TIME_SEC * 1000);
            LOG_WRN("Too many failed attempts, locked out for %d seconds", LOCKOUT_TIME_SEC);
        }

        return false;
    }

    /* Compare PIN (constant time to prevent timing attacks) */
    uint8_t diff = 0;
    for (uint8_t i = 0; i < length; i++) {
        diff |= pin[i] ^ config.pin[i];
    }

    if (diff == 0) {
        /* Success */
        config.failed_attempts = 0;
        if (attempts_left) {
            *attempts_left = MAX_PIN_ATTEMPTS;
        }
        return true;
    }

    /* Failed */
    config.failed_attempts++;
    if (attempts_left) {
        *attempts_left = MAX_PIN_ATTEMPTS > config.failed_attempts ?
                         MAX_PIN_ATTEMPTS - config.failed_attempts : 0;
    }

    /* Check for lockout */
    if (config.failed_attempts >= MAX_PIN_ATTEMPTS) {
        lockout_until = k_uptime_get() + (LOCKOUT_TIME_SEC * 1000);
        LOG_WRN("Too many failed attempts, locked out for %d seconds", LOCKOUT_TIME_SEC);
    }

    return false;
}

/**
 * Reset failed attempt counter
 */
void config_reset_attempts(void)
{
    config.failed_attempts = 0;
    lockout_until = 0;
}

/**
 * Check if locked out
 */
bool config_is_locked_out(void)
{
    if (lockout_until == 0) {
        return false;
    }

    if (k_uptime_get() >= lockout_until) {
        lockout_until = 0;
        config.failed_attempts = 0;
        return false;
    }

    return true;
}

/**
 * Reset to defaults
 */
void config_reset_defaults(void)
{
    /* Generate a new random hip-hop name */
    generate_random_hiphop_name(config.device_name, sizeof(config.device_name));
    config.name_length = strlen(config.device_name);
    config.pin[0] = '\0';
    config.pin_length = 0;
    config.failed_attempts = 0;
    lockout_until = 0;

    /* Clear persistent storage (name will be saved fresh on next boot) */
    settings_delete(SETTINGS_DEVICE_NAME);
    settings_delete(SETTINGS_PIN);
    settings_delete(SETTINGS_ATTEMPTS);

    LOG_INF("Configuration reset to defaults, new name: %s", config.device_name);
}

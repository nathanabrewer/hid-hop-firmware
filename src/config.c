/*
 * Brewer BLE HID Bridge - Configuration Storage Implementation
 *
 * Uses raw flash with CRC32 validation for robust config storage.
 * Bypasses Zephyr settings subsystem for full control over data integrity.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <stddef.h>

#include "config.h"
#include "protocol.h"
#include "ble_hid_service.h"

LOG_MODULE_REGISTER(config, LOG_LEVEL_INF);

/* Config partition ID - separate from NVS storage for isolation */
#define CONFIG_PARTITION        config_partition
#define CONFIG_PARTITION_ID     FIXED_PARTITION_ID(CONFIG_PARTITION)

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
    bool mouse_to_rc;       /* Route mouse commands to RC PWM outputs */
    uint8_t default_host_os; /* host_os_t default for CMD_KEYBOARD_UNICODE */
    bool initialized;
} config;

/* SECURITY: Stricter lockout - 3 attempts, 5 minute lockout */
#define LOCKOUT_TIME_SEC 300
static int64_t lockout_until = 0;

/* Default PIN for new devices */
#define DEFAULT_PIN "123456"
#define DEFAULT_PIN_LENGTH 6

/**
 * Erase the storage partition (factory reset flash)
 */
static int erase_storage_partition(void)
{
    const struct flash_area *fa;
    int rc;

    LOG_WRN("Erasing storage partition...");

    rc = flash_area_open(CONFIG_PARTITION_ID, &fa);
    if (rc) {
        LOG_ERR("Failed to open storage partition: %d", rc);
        return rc;
    }

    rc = flash_area_erase(fa, 0, fa->fa_size);
    if (rc) {
        LOG_ERR("Failed to erase storage partition: %d", rc);
    } else {
        LOG_INF("Storage partition erased (%u bytes)", fa->fa_size);
    }

    flash_area_close(fa);
    return rc;
}

/*
 * Raw flash config storage with CRC validation.
 * Bypasses Zephyr settings subsystem for full control over data integrity.
 * Uses a dedicated area in the storage partition.
 */
#define CONFIG_FLASH_OFFSET     0x0000  /* Start of storage partition */
#define CONFIG_MAGIC_VALUE      0x48494448  /* "HIDH" */
#define CONFIG_VERSION          0x0002

/**
 * Config data stored in flash - includes CRC of entire struct
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;                              /* Must be CONFIG_MAGIC_VALUE */
    uint16_t version;                            /* CONFIG_VERSION */
    uint16_t struct_size;                        /* Size of this struct for future compat */
    char device_name[MAX_DEVICE_NAME_LENGTH + 1];
    uint8_t name_length;
    char pin[MAX_PIN_LENGTH + 1];
    uint8_t pin_length;
    uint8_t failed_attempts;
    uint8_t _reserved[16];                       /* Future expansion */
    uint32_t crc32;                              /* CRC32 of all bytes before this field */
} config_flash_t;

/**
 * CRC32 lookup table (polynomial 0xEDB88320)
 */
static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F,
    0xE963A535, 0x9E6495A3, 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
    0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2,
    0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9,
    0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
    0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C,
    0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423,
    0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
    0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D, 0x76DC4190, 0x01DB7106,
    0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D,
    0x91646C97, 0xE6635C01, 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
    0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
    0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7,
    0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
    0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7A9B, 0x5005713C, 0x270241AA,
    0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
    0xB7BD5C3B, 0xC0BA6CAD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
    0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683, 0xE3630B12, 0x94643B84,
    0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB,
    0x196C3671, 0x6E6B06E7, 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
    0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
    0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55,
    0x316E8EEF, 0x4669BE79, 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
    0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F, 0xC5BA3BBE, 0xB2BD0B28,
    0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F,
    0x72076785, 0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
    0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242,
    0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69,
    0x616BFFD3, 0x166CCF45, 0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
    0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAED16A4A, 0xD9D65ADC,
    0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD706B3,
    0x54DE5729, 0x23D967BF, 0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
    0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
};

/**
 * Calculate CRC32 of data
 */
static uint32_t calc_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

/**
 * Read config from flash and validate CRC
 * Returns true if valid config was loaded
 */
static bool read_config_from_flash(void)
{
    const struct flash_area *fa;
    int rc;
    config_flash_t flash_data;

    rc = flash_area_open(CONFIG_PARTITION_ID, &fa);
    if (rc) {
        LOG_ERR("Cannot open storage partition: %d", rc);
        return false;
    }

    rc = flash_area_read(fa, CONFIG_FLASH_OFFSET, &flash_data, sizeof(flash_data));
    flash_area_close(fa);

    if (rc) {
        LOG_ERR("Cannot read config from flash: %d", rc);
        return false;
    }

    /* Check magic */
    if (flash_data.magic != CONFIG_MAGIC_VALUE) {
        LOG_WRN("Invalid config magic: 0x%08x", flash_data.magic);
        return false;
    }

    /* Check struct size matches */
    if (flash_data.struct_size != sizeof(config_flash_t)) {
        LOG_WRN("Config struct size mismatch: %d vs %d",
                flash_data.struct_size, sizeof(config_flash_t));
        return false;
    }

    /* Validate CRC32 of everything except the CRC field itself */
    size_t crc_data_len = offsetof(config_flash_t, crc32);
    uint32_t computed_crc = calc_crc32((uint8_t *)&flash_data, crc_data_len);

    if (computed_crc != flash_data.crc32) {
        LOG_ERR("Config CRC mismatch: computed 0x%08x, stored 0x%08x",
                computed_crc, flash_data.crc32);
        return false;
    }

    /* CRC valid - copy data to runtime config */
    memcpy(config.device_name, flash_data.device_name, sizeof(config.device_name));
    config.name_length = flash_data.name_length;
    memcpy(config.pin, flash_data.pin, sizeof(config.pin));
    config.pin_length = flash_data.pin_length;
    config.failed_attempts = flash_data.failed_attempts;
    config.mouse_to_rc = (flash_data._reserved[0] == 1);
    config.default_host_os = flash_data._reserved[1];  /* 0 == HOST_OS_LINUX_IBUS */

    LOG_INF("Config loaded from flash: name='%s', version=%d, mouse_to_rc=%d",
            config.device_name, flash_data.version, config.mouse_to_rc);
    return true;
}

/**
 * Write config to flash with CRC
 */
static int write_config_to_flash(void)
{
    const struct flash_area *fa;
    int rc;
    config_flash_t flash_data;

    /* Prepare flash data struct */
    memset(&flash_data, 0, sizeof(flash_data));
    flash_data.magic = CONFIG_MAGIC_VALUE;
    flash_data.version = CONFIG_VERSION;
    flash_data.struct_size = sizeof(config_flash_t);
    memcpy(flash_data.device_name, config.device_name, sizeof(flash_data.device_name));
    flash_data.name_length = config.name_length;
    memcpy(flash_data.pin, config.pin, sizeof(flash_data.pin));
    flash_data.pin_length = config.pin_length;
    flash_data.failed_attempts = config.failed_attempts;
    flash_data._reserved[0] = config.mouse_to_rc ? 1 : 0;
    flash_data._reserved[1] = config.default_host_os;

    /* Compute CRC of everything except the CRC field */
    size_t crc_data_len = offsetof(config_flash_t, crc32);
    flash_data.crc32 = calc_crc32((uint8_t *)&flash_data, crc_data_len);

    rc = flash_area_open(CONFIG_PARTITION_ID, &fa);
    if (rc) {
        LOG_ERR("Cannot open storage partition: %d", rc);
        return rc;
    }

    /* Erase first sector (4KB is typical minimum erase size) */
    rc = flash_area_erase(fa, 0, 4096);
    if (rc) {
        LOG_ERR("Cannot erase config area: %d", rc);
        flash_area_close(fa);
        return rc;
    }

    /* Write config */
    rc = flash_area_write(fa, CONFIG_FLASH_OFFSET, &flash_data, sizeof(flash_data));
    flash_area_close(fa);

    if (rc) {
        LOG_ERR("Cannot write config: %d", rc);
        return rc;
    }

    LOG_INF("Config saved to flash (CRC: 0x%08x)", flash_data.crc32);
    return 0;
}

/* No longer using Zephyr settings subsystem - we use raw flash with CRC */

/**
 * Generate a random hip-hop name with 4-char suffix for uniqueness
 * Format: "Name XXXX" where X is alphanumeric
 * Uses hardware RNG for true randomness (not cycle counter which is deterministic)
 */
static void generate_random_hiphop_name(char *buf, size_t buf_size)
{
    static const char alphanum[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ"; /* No I/O to avoid confusion */

    /* Use hardware RNG for proper entropy - nRF52840 has true RNG */
    uint32_t rand_val;
    if (sys_csrand_get(&rand_val, sizeof(rand_val)) != 0) {
        /* Fallback: use BLE address XOR'd with cycle count for uniqueness */
        bt_addr_le_t addr;
        bt_id_get(&addr, NULL);
        rand_val = (addr.a.val[0] | (addr.a.val[1] << 8) |
                    (addr.a.val[2] << 16) | (addr.a.val[3] << 24)) ^ k_cycle_get_32();
    }

    /* Pick a random base name */
    const char *base = hiphop_names[rand_val % NUM_HIPHOP_NAMES];

    /* Generate 4 random alphanumeric chars using LCG seeded by true random */
    char suffix[5];
    for (int i = 0; i < 4; i++) {
        rand_val = rand_val * 1103515245 + 12345;  /* LCG seeded by HW RNG */
        suffix[i] = alphanum[rand_val % (sizeof(alphanum) - 1)];
    }
    suffix[4] = '\0';

    /* Combine: "Name XXXX" */
    snprintf(buf, buf_size, "%s %s", base, suffix);
}

/**
 * Set up defaults (used when settings unavailable or reset)
 */
static void setup_defaults(void)
{
    generate_random_hiphop_name(config.device_name, sizeof(config.device_name));
    config.name_length = strlen(config.device_name);

    /* Set default PIN */
    memcpy(config.pin, DEFAULT_PIN, DEFAULT_PIN_LENGTH);
    config.pin[DEFAULT_PIN_LENGTH] = '\0';
    config.pin_length = DEFAULT_PIN_LENGTH;

    config.failed_attempts = 0;
    config.mouse_to_rc = true;
    config.default_host_os = HOST_OS_LINUX_IBUS;
    config.initialized = true;

    LOG_INF("Using defaults: name='%s', PIN=%d digits, mouse_to_rc=ON",
            config.device_name, config.pin_length);
}

/* Debug blink helper for config init diagnosis */
#ifdef CONFIG_DEBUG_BOOT_BLINKS
#define P0_OUTSET    (*(volatile uint32_t *)0x50000508)
#define P0_OUTCLR    (*(volatile uint32_t *)0x5000050C)
#define DBG_LED1 6
#define DBG_LED2 8
static void cfg_dbg_delay(void) {
    for (volatile int i = 0; i < 800000; i++) { __asm__("nop"); }  /* Same speed as main blinks */
}
static void cfg_dbg_blink(int count) {
    for (int j = 0; j < count; j++) {
        P0_OUTCLR = (1 << DBG_LED1) | (1 << DBG_LED2);
        cfg_dbg_delay();
        P0_OUTSET = (1 << DBG_LED1) | (1 << DBG_LED2);
        cfg_dbg_delay();
    }
    cfg_dbg_delay(); cfg_dbg_delay();
}
#else
#define cfg_dbg_blink(x)
#endif

/**
 * Initialize configuration using raw flash with CRC validation.
 * No Zephyr settings subsystem - we have full control.
 */
bool config_init(void)
{
    if (config.initialized) {
        return true;
    }

    LOG_INF("Config init starting...");
    cfg_dbg_blink(1);  /* 1 blink = config_init entered */

    /* Clear config struct */
    memset(&config, 0, sizeof(config));

    /*
     * Try to load config from flash.
     * read_config_from_flash() validates:
     * - Magic value (must be CONFIG_MAGIC_VALUE)
     * - Version (must match CONFIG_VERSION exactly)
     * - Struct size (must match sizeof(config_flash_t))
     * - CRC32 of entire struct
     *
     * If ANY check fails, we erase and start fresh.
     */
    cfg_dbg_blink(2);  /* 2 blinks = about to read flash */

    bool config_valid = read_config_from_flash();

    cfg_dbg_blink(3);  /* 3 blinks = read returned */

    if (config_valid) {
        /* Valid config loaded */
        cfg_dbg_blink(4);  /* 4 blinks = config was valid */
        LOG_INF("Config loaded successfully");
    } else {
        /* Invalid or missing config - use defaults */
        LOG_WRN("Config invalid or missing - using defaults");
        cfg_dbg_blink(5);  /* 5 blinks = using defaults */
        setup_defaults();

        /* Save defaults to flash */
        cfg_dbg_blink(6);  /* 6 blinks = about to save */
        int rc = write_config_to_flash();
        cfg_dbg_blink(7);  /* 7 blinks = save done */
        if (rc != 0) {
            LOG_ERR("Failed to save default config: %d", rc);
        }
    }

    cfg_dbg_blink(8);  /* 8 blinks = config_init done */
    config.initialized = true;
    LOG_INF("Config initialized: name='%s', PIN enabled (%d digits)",
            config.device_name, config.pin_length);

    return true;
}

/**
 * Factory reset - erase all settings and restart
 */
void config_factory_reset(void)
{
    LOG_WRN("=== FACTORY RESET ===");

    /* Erase the entire storage partition */
    erase_storage_partition();

    /* Reset in-memory config */
    memset(&config, 0, sizeof(config));

    LOG_WRN("Factory reset complete - rebooting...");
    k_sleep(K_MSEC(100));

    /* Trigger system reset */
    NVIC_SystemReset();
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
    LOG_INF("config_set_name called: len=%d", length);

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

    /* Save to flash with CRC */
    int rc = write_config_to_flash();
    if (rc) {
        LOG_ERR("Failed to save config: %d", rc);
    } else {
        LOG_INF("Config saved to flash");
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
 * Get current PIN (for key derivation)
 */
uint8_t config_get_pin(char *pin)
{
    if (pin == NULL || config.pin_length == 0) {
        return 0;
    }
    memcpy(pin, config.pin, config.pin_length);
    pin[config.pin_length] = '\0';
    return config.pin_length;
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

    /* Save to flash with CRC */
    int rc = write_config_to_flash();
    if (rc) {
        LOG_ERR("Failed to save config: %d", rc);
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

    /* Save to flash with CRC (PIN now empty) */
    int rc = write_config_to_flash();
    if (rc) {
        LOG_ERR("Failed to save config: %d", rc);
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
 * Reset to defaults and save to flash
 */
void config_reset_defaults(void)
{
    /* Generate a new random hip-hop name */
    generate_random_hiphop_name(config.device_name, sizeof(config.device_name));
    config.name_length = strlen(config.device_name);

    /* Reset PIN to default */
    memcpy(config.pin, DEFAULT_PIN, DEFAULT_PIN_LENGTH);
    config.pin[DEFAULT_PIN_LENGTH] = '\0';
    config.pin_length = DEFAULT_PIN_LENGTH;

    config.failed_attempts = 0;
    lockout_until = 0;

    /* Save new defaults to flash */
    write_config_to_flash();

    LOG_INF("Configuration reset to defaults, new name: %s", config.device_name);
}

/**
 * Get mouse-to-RC routing state
 */
bool config_get_mouse_to_rc(void)
{
    return config.mouse_to_rc;
}

/**
 * Set mouse-to-RC routing state
 */
bool config_set_mouse_to_rc(bool enabled)
{
    config.mouse_to_rc = enabled;

    int rc = write_config_to_flash();
    if (rc) {
        LOG_ERR("Failed to save mouse_to_rc config: %d", rc);
        return false;
    }

    LOG_INF("Mouse-to-RC routing: %s", enabled ? "enabled" : "disabled");
    return true;
}

/**
 * Get the default host OS for Unicode injection
 */
uint8_t config_get_default_host_os(void)
{
    return config.default_host_os;
}

/**
 * Set the default host OS for Unicode injection
 */
bool config_set_default_host_os(uint8_t os_mode)
{
    config.default_host_os = os_mode;

    int rc = write_config_to_flash();
    if (rc) {
        LOG_ERR("Failed to save default_host_os config: %d", rc);
        return false;
    }

    LOG_INF("Default host OS for Unicode: %u", os_mode);
    return true;
}

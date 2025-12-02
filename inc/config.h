/*
 * Brewer BLE HID Bridge - Configuration Storage
 *
 * Persistent storage for device name, PIN, and other settings.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

/**
 * Initialize configuration module
 * Loads settings from persistent storage
 * @return true on success
 */
bool config_init(void);

/**
 * Get device name
 * @param name Buffer to store name (must be at least MAX_DEVICE_NAME_LENGTH + 1)
 * @return Length of name, or 0 if using default
 */
uint8_t config_get_name(char *name);

/**
 * Set device name
 * @param name New device name
 * @param length Length of name
 * @return true on success
 */
bool config_set_name(const char *name, uint8_t length);

/**
 * Check if PIN is enabled
 * @return true if PIN protection is active
 */
bool config_pin_enabled(void);

/**
 * Set PIN
 * @param pin PIN digits (ASCII)
 * @param length PIN length
 * @return true on success
 */
bool config_set_pin(const char *pin, uint8_t length);

/**
 * Clear PIN (disable PIN protection)
 * @return true on success
 */
bool config_clear_pin(void);

/**
 * Verify PIN
 * @param pin PIN to verify
 * @param length PIN length
 * @param attempts_left Output: remaining attempts before lockout
 * @return true if PIN matches
 */
bool config_verify_pin(const char *pin, uint8_t length, uint8_t *attempts_left);

/**
 * Reset failed attempt counter (called after successful PIN entry)
 */
void config_reset_attempts(void);

/**
 * Check if device is locked out due to too many failed attempts
 * @return true if locked out
 */
bool config_is_locked_out(void);

/**
 * Reset all configuration to defaults
 */
void config_reset_defaults(void);

#endif /* CONFIG_H */

/*
 * Brewer BLE HID Bridge - Security Module
 *
 * Simple challenge-response authentication to verify the iOS app.
 * Uses BLE bonding as primary security, with optional app-level auth.
 */

#ifndef SECURITY_H
#define SECURITY_H

#include <stdbool.h>
#include <stdint.h>

/* Challenge size in bytes */
#define SECURITY_CHALLENGE_SIZE 16

/* Session timeout in seconds (0 = no timeout) */
#define SECURITY_SESSION_TIMEOUT 300

/**
 * Initialize security module
 * @return true on success
 */
bool security_init(void);

/**
 * Generate authentication challenge
 * @param challenge Output buffer (SECURITY_CHALLENGE_SIZE bytes)
 * @return true on success
 */
bool security_generate_challenge(uint8_t *challenge);

/**
 * Verify authentication response
 * @param response Response from client
 * @param response_len Length of response
 * @return true if authentication successful
 */
bool security_verify_response(const uint8_t *response, size_t response_len);

/**
 * Start authenticated session
 * @return true on success
 */
bool security_start_session(void);

/**
 * End current session
 */
void security_end_session(void);

/**
 * Check if session is active
 * @return true if authenticated session is active
 */
bool security_session_active(void);

/**
 * Refresh session timeout
 * Call this on each valid command to prevent timeout
 */
void security_refresh_session(void);

/**
 * Check if command requires authentication
 * @param cmd_type Command type byte
 * @return true if auth required and no session active
 */
bool security_requires_auth(uint8_t cmd_type);

#endif /* SECURITY_H */

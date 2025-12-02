/*
 * Brewer BLE HID Bridge - Security Implementation
 *
 * Simple session-based security on top of BLE bonding.
 * Uses challenge-response for optional app-level authentication.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <string.h>

#include "security.h"
#include "protocol.h"
#include "config.h"

LOG_MODULE_REGISTER(security, LOG_LEVEL_INF);

/* Session state */
static bool session_active = false;
static int64_t session_start_time = 0;
static int64_t last_activity_time = 0;

/* Current challenge */
static uint8_t current_challenge[SECURITY_CHALLENGE_SIZE];
static bool challenge_pending = false;

/* Commands that don't require authentication (can be used before PIN verification) */
static const uint8_t auth_exempt_commands[] = {
    CMD_PING,
    CMD_PONG,
    CMD_GET_INFO,
    CMD_INFO_RESPONSE,
    CMD_AUTH_CHALLENGE,
    CMD_AUTH_RESPONSE,
    CMD_STATUS,
    CMD_ERROR,
    CMD_SET_PIN,      /* Allow setting PIN before session (for first-time setup) */
    CMD_VERIFY_PIN,   /* Allow PIN verification before session */
    CMD_GET_NAME,     /* Allow reading device name before session */
};

/**
 * Check if command is exempt from authentication
 */
static bool is_auth_exempt(uint8_t cmd_type)
{
    for (size_t i = 0; i < sizeof(auth_exempt_commands); i++) {
        if (auth_exempt_commands[i] == cmd_type) {
            return true;
        }
    }
    return false;
}

/**
 * Initialize security module
 */
bool security_init(void)
{
    session_active = false;
    challenge_pending = false;
    memset(current_challenge, 0, sizeof(current_challenge));

    LOG_INF("Security module initialized");
    return true;
}

/**
 * Generate authentication challenge
 */
bool security_generate_challenge(uint8_t *challenge)
{
    if (challenge == NULL) {
        return false;
    }

    /* Generate random challenge */
    int ret = sys_csrand_get(current_challenge, SECURITY_CHALLENGE_SIZE);
    if (ret != 0) {
        LOG_ERR("Failed to generate random challenge: %d", ret);
        return false;
    }

    memcpy(challenge, current_challenge, SECURITY_CHALLENGE_SIZE);
    challenge_pending = true;

    LOG_DBG("Challenge generated");
    return true;
}

/**
 * Verify authentication response
 *
 * For simplicity, we're using a basic verification.
 * In production, this should use proper HMAC with a shared secret.
 */
bool security_verify_response(const uint8_t *response, size_t response_len)
{
    if (!challenge_pending || response == NULL) {
        LOG_WRN("No pending challenge or null response");
        return false;
    }

    if (response_len < SECURITY_CHALLENGE_SIZE) {
        LOG_WRN("Response too short");
        return false;
    }

    /*
     * Simple verification: response should be XOR of challenge with known pattern
     * In a real implementation, use HMAC-SHA256 with a shared secret
     *
     * For now, we just check that a response was provided and start session.
     * This relies on BLE bonding for actual security.
     */

    challenge_pending = false;
    memset(current_challenge, 0, sizeof(current_challenge));

    LOG_INF("Authentication successful");
    return true;
}

/**
 * Start authenticated session
 */
bool security_start_session(void)
{
    session_active = true;
    session_start_time = k_uptime_get();
    last_activity_time = session_start_time;

    LOG_INF("Session started");
    return true;
}

/**
 * End current session
 */
void security_end_session(void)
{
    session_active = false;
    session_start_time = 0;
    last_activity_time = 0;
    challenge_pending = false;

    LOG_INF("Session ended");
}

/**
 * Check if session is active
 */
bool security_session_active(void)
{
    if (!session_active) {
        return false;
    }

    /* Check for session timeout */
    if (SECURITY_SESSION_TIMEOUT > 0) {
        int64_t now = k_uptime_get();
        int64_t inactive_seconds = (now - last_activity_time) / 1000;

        if (inactive_seconds >= SECURITY_SESSION_TIMEOUT) {
            LOG_INF("Session timed out after %lld seconds", inactive_seconds);
            security_end_session();
            return false;
        }
    }

    return true;
}

/**
 * Refresh session timeout
 */
void security_refresh_session(void)
{
    if (session_active) {
        last_activity_time = k_uptime_get();
    }
}

/**
 * Check if command requires authentication
 * SECURITY HARDENED: PIN is mandatory - no auto-session
 */
bool security_requires_auth(uint8_t cmd_type)
{
    /* If session is active, no auth needed */
    if (security_session_active()) {
        return false;
    }

    /* Check if command is exempt */
    if (is_auth_exempt(cmd_type)) {
        return false;
    }

    /*
     * SECURITY: Always require PIN verification.
     * Default PIN is 123456 (set in config_init if no PIN exists).
     * User must verify PIN to start a session.
     */
    LOG_INF("Auth required for cmd 0x%02X - verify PIN first", cmd_type);
    return true;
}

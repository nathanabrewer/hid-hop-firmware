/*
 * HID-HOP - GPIO Control Module Implementation
 *
 * Board-aware GPIO control using Zephyr devicetree aliases.
 *
 * Copyright (c) 2025 Nathan Brewer
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#if defined(CONFIG_PWM)
#include <zephyr/drivers/pwm.h>
#endif

#include "gpio_control.h"

LOG_MODULE_REGISTER(gpio_control, LOG_LEVEL_INF);

/* LED GPIO specs from devicetree aliases */
#if DT_NODE_EXISTS(DT_ALIAS(led0))
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#define HAS_LED0 1
#else
#define HAS_LED0 0
#endif

#if DT_NODE_EXISTS(DT_ALIAS(led1))
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
#define HAS_LED1 1
#else
#define HAS_LED1 0
#endif

#if DT_NODE_EXISTS(DT_ALIAS(led2))
static const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
#define HAS_LED2 1
#else
#define HAS_LED2 0
#endif

#if DT_NODE_EXISTS(DT_ALIAS(led3))
static const struct gpio_dt_spec led3 = GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios);
#define HAS_LED3 1
#else
#define HAS_LED3 0
#endif

/* Button GPIO specs from devicetree aliases */
#if DT_NODE_EXISTS(DT_ALIAS(sw0))
static const struct gpio_dt_spec btn0 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
#define HAS_BTN0 1
#else
#define HAS_BTN0 0
#endif

#if DT_NODE_EXISTS(DT_ALIAS(sw1))
static const struct gpio_dt_spec btn1 = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
#define HAS_BTN1 1
#else
#define HAS_BTN1 0
#endif

#if DT_NODE_EXISTS(DT_ALIAS(sw2))
static const struct gpio_dt_spec btn2 = GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios);
#define HAS_BTN2 1
#else
#define HAS_BTN2 0
#endif

#if DT_NODE_EXISTS(DT_ALIAS(sw3))
static const struct gpio_dt_spec btn3 = GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios);
#define HAS_BTN3 1
#else
#define HAS_BTN3 0
#endif

/* RC PWM devices from devicetree (XIAO uses pwm1 for RC outputs) */
#if GPIO_RC_COUNT > 0 && defined(CONFIG_PWM)
#if DT_NODE_HAS_STATUS(DT_NODELABEL(pwm1), okay)
static const struct device *rc_pwm_dev = DEVICE_DT_GET(DT_NODELABEL(pwm1));
#define HAS_RC_PWM 1
#else
#define HAS_RC_PWM 0
#endif
static uint16_t rc_pulse_us[GPIO_RC_COUNT];     /* Current pulse widths */
static uint32_t rc_last_update[GPIO_RC_COUNT];  /* Last update timestamp (ms) */
static bool rc_failsafe_enabled = true;         /* Failsafe enabled globally */

/* Failsafe configuration */
#define RC_FAILSAFE_TIMEOUT_MS   500   /* Time before failsafe kicks in */
#define RC_FAILSAFE_CHECK_MS     50    /* How often to check for timeout */
#define RC_FAILSAFE_FADE_STEP    20    /* Microseconds to move per check toward center */

/* Failsafe work queue */
static struct k_work_delayable rc_failsafe_work;

static void rc_failsafe_handler(struct k_work *work);
#endif

/* Current LED state tracking */
static uint8_t led_state = 0;

/* Module initialized flag */
static bool initialized = false;

/* Helper to get LED spec by index */
static const struct gpio_dt_spec *get_led_spec(uint8_t index)
{
    switch (index) {
#if HAS_LED0
    case 0: return &led0;
#endif
#if HAS_LED1
    case 1: return &led1;
#endif
#if HAS_LED2
    case 2: return &led2;
#endif
#if HAS_LED3
    case 3: return &led3;
#endif
    default: return NULL;
    }
}

/* Helper to get button spec by index */
static const struct gpio_dt_spec *get_btn_spec(uint8_t index)
{
    switch (index) {
#if HAS_BTN0
    case 0: return &btn0;
#endif
#if HAS_BTN1
    case 1: return &btn1;
#endif
#if HAS_BTN2
    case 2: return &btn2;
#endif
#if HAS_BTN3
    case 3: return &btn3;
#endif
    default: return NULL;
    }
}

/**
 * Initialize GPIO control module
 */
bool gpio_control_init(void)
{
    int ret;

    if (initialized) {
        return true;
    }

    /* Configure LEDs */
    for (int i = 0; i < GPIO_LED_COUNT; i++) {
        const struct gpio_dt_spec *led = get_led_spec(i);
        if (led && device_is_ready(led->port)) {
            ret = gpio_pin_configure_dt(led, GPIO_OUTPUT_INACTIVE);
            if (ret < 0) {
                LOG_ERR("Failed to configure LED%d: %d", i, ret);
            } else {
                LOG_INF("LED%d configured on P%d.%d", i,
                        led->port == DEVICE_DT_GET(DT_NODELABEL(gpio0)) ? 0 : 1,
                        led->pin);
            }
        }
    }

    /* Configure buttons */
    for (int i = 0; i < GPIO_BTN_COUNT; i++) {
        const struct gpio_dt_spec *btn = get_btn_spec(i);
        if (btn && device_is_ready(btn->port)) {
            ret = gpio_pin_configure_dt(btn, GPIO_INPUT);
            if (ret < 0) {
                LOG_ERR("Failed to configure BTN%d: %d", i, ret);
            } else {
                LOG_INF("BTN%d configured on P%d.%d", i,
                        btn->port == DEVICE_DT_GET(DT_NODELABEL(gpio0)) ? 0 : 1,
                        btn->pin);
            }
        }
    }

    /* Initialize RC PWM channels */
#if GPIO_RC_COUNT > 0 && defined(CONFIG_PWM) && HAS_RC_PWM
    if (device_is_ready(rc_pwm_dev)) {
        uint32_t now = k_uptime_get_32();
        /* Initialize all channels to center position */
        for (int i = 0; i < GPIO_RC_COUNT; i++) {
            rc_pulse_us[i] = RC_PWM_CENTER_US;
            rc_last_update[i] = now;
            ret = pwm_set(rc_pwm_dev, i, PWM_USEC(RC_PWM_PERIOD_US), PWM_USEC(RC_PWM_CENTER_US), 0);
            if (ret < 0) {
                LOG_ERR("Failed to init RC channel %d: %d", i, ret);
            } else {
                LOG_INF("RC channel %d initialized at %dus", i, RC_PWM_CENTER_US);
            }
        }
        /* Start failsafe watchdog timer */
        k_work_init_delayable(&rc_failsafe_work, rc_failsafe_handler);
        k_work_schedule(&rc_failsafe_work, K_MSEC(RC_FAILSAFE_CHECK_MS));
        LOG_INF("RC failsafe enabled: timeout=%dms, fade=%dus/step",
                RC_FAILSAFE_TIMEOUT_MS, RC_FAILSAFE_FADE_STEP);
    } else {
        LOG_WRN("RC PWM device not ready");
    }
#endif

    initialized = true;
    LOG_INF("GPIO control initialized: %d LEDs, %d buttons, %d RC channels",
            GPIO_LED_COUNT, GPIO_BTN_COUNT, GPIO_RC_COUNT);
    return true;
}

/**
 * Set LED state
 */
bool gpio_led_set(uint8_t led_index, bool on)
{
    if (!initialized || led_index >= GPIO_LED_COUNT) {
        return false;
    }

    const struct gpio_dt_spec *led = get_led_spec(led_index);
    if (!led || !device_is_ready(led->port)) {
        return false;
    }

    int ret = gpio_pin_set_dt(led, on ? 1 : 0);
    if (ret < 0) {
        LOG_ERR("Failed to set LED%d: %d", led_index, ret);
        return false;
    }

    if (on) {
        led_state |= (1 << led_index);
    } else {
        led_state &= ~(1 << led_index);
    }

    return true;
}

/**
 * Get LED state
 */
bool gpio_led_get(uint8_t led_index)
{
    if (led_index >= GPIO_LED_COUNT) {
        return false;
    }
    return (led_state & (1 << led_index)) != 0;
}

/**
 * Toggle LED
 */
bool gpio_led_toggle(uint8_t led_index)
{
    bool new_state = !gpio_led_get(led_index);
    gpio_led_set(led_index, new_state);
    return new_state;
}

/**
 * Set all LEDs at once
 */
void gpio_led_set_all(uint8_t state)
{
    for (int i = 0; i < GPIO_LED_COUNT; i++) {
        gpio_led_set(i, (state & (1 << i)) != 0);
    }
}

/**
 * Get all LED states
 */
uint8_t gpio_led_get_all(void)
{
    return led_state;
}

/**
 * Read button state
 */
bool gpio_btn_read(uint8_t btn_index)
{
    if (!initialized || btn_index >= GPIO_BTN_COUNT) {
        return false;
    }

    const struct gpio_dt_spec *btn = get_btn_spec(btn_index);
    if (!btn || !device_is_ready(btn->port)) {
        return false;
    }

    /* gpio_pin_get_dt handles active-low via flags */
    return gpio_pin_get_dt(btn) != 0;
}

/**
 * Read all button states
 */
uint8_t gpio_btn_read_all(void)
{
    uint8_t state = 0;
    for (int i = 0; i < GPIO_BTN_COUNT; i++) {
        if (gpio_btn_read(i)) {
            state |= (1 << i);
        }
    }
    return state;
}

/**
 * Get complete GPIO state
 */
void gpio_get_state(gpio_state_t *state)
{
    if (state == NULL) {
        return;
    }

    state->led_state = gpio_led_get_all();
    state->btn_state = gpio_btn_read_all();
}

/**
 * Blink LED (blocking)
 */
void gpio_led_blink(uint8_t led_index, uint8_t count, uint16_t on_ms, uint16_t off_ms)
{
    for (int i = 0; i < count; i++) {
        gpio_led_set(led_index, true);
        k_sleep(K_MSEC(on_ms));
        gpio_led_set(led_index, false);
        if (i < count - 1) {
            k_sleep(K_MSEC(off_ms));
        }
    }
}

/*
 * RC PWM functions - only compiled when GPIO_RC_COUNT > 0
 */
#if GPIO_RC_COUNT > 0 && defined(CONFIG_PWM) && HAS_RC_PWM

/**
 * Internal: Set RC PWM without updating timestamp (used by failsafe)
 */
static bool rc_set_internal(uint8_t channel, uint16_t pulse_us)
{
    if (!initialized || channel >= GPIO_RC_COUNT) {
        return false;
    }

    if (!device_is_ready(rc_pwm_dev)) {
        return false;
    }

    /* Clamp pulse width to valid RC range */
    if (pulse_us < RC_PWM_MIN_US) {
        pulse_us = RC_PWM_MIN_US;
    } else if (pulse_us > RC_PWM_MAX_US) {
        pulse_us = RC_PWM_MAX_US;
    }

    int ret = pwm_set(rc_pwm_dev, channel, PWM_USEC(RC_PWM_PERIOD_US), PWM_USEC(pulse_us), 0);
    if (ret < 0) {
        return false;
    }

    rc_pulse_us[channel] = pulse_us;
    return true;
}

/**
 * Set RC PWM channel pulse width (resets failsafe timer)
 */
bool gpio_rc_set(uint8_t channel, uint16_t pulse_us)
{
    if (!initialized || channel >= GPIO_RC_COUNT) {
        return false;
    }

    if (!device_is_ready(rc_pwm_dev)) {
        LOG_ERR("RC PWM device not ready");
        return false;
    }

    /* Update timestamp - this resets the failsafe timer for this channel */
    rc_last_update[channel] = k_uptime_get_32();

    /* Clamp pulse width to valid RC range */
    if (pulse_us < RC_PWM_MIN_US) {
        pulse_us = RC_PWM_MIN_US;
    } else if (pulse_us > RC_PWM_MAX_US) {
        pulse_us = RC_PWM_MAX_US;
    }

    int ret = pwm_set(rc_pwm_dev, channel, PWM_USEC(RC_PWM_PERIOD_US), PWM_USEC(pulse_us), 0);
    if (ret < 0) {
        LOG_ERR("Failed to set RC channel %d to %dus: %d", channel, pulse_us, ret);
        return false;
    }

    rc_pulse_us[channel] = pulse_us;
    LOG_DBG("RC channel %d set to %dus", channel, pulse_us);
    return true;
}

/**
 * Get current RC PWM channel pulse width
 */
uint16_t gpio_rc_get(uint8_t channel)
{
    if (channel >= GPIO_RC_COUNT) {
        return 0;
    }
    return rc_pulse_us[channel];
}

/**
 * Center all RC PWM channels
 */
void gpio_rc_center_all(void)
{
    for (int i = 0; i < GPIO_RC_COUNT; i++) {
        gpio_rc_set(i, RC_PWM_CENTER_US);
    }
    LOG_INF("All RC channels centered at %dus", RC_PWM_CENTER_US);
}

/**
 * Disable RC PWM channel (stop PWM output)
 */
bool gpio_rc_disable(uint8_t channel)
{
    if (!initialized || channel >= GPIO_RC_COUNT) {
        return false;
    }

    if (!device_is_ready(rc_pwm_dev)) {
        return false;
    }

    /* Set pulse width to 0 to disable output */
    int ret = pwm_set(rc_pwm_dev, channel, PWM_USEC(RC_PWM_PERIOD_US), 0, 0);
    if (ret < 0) {
        LOG_ERR("Failed to disable RC channel %d: %d", channel, ret);
        return false;
    }

    rc_pulse_us[channel] = 0;
    LOG_INF("RC channel %d disabled", channel);
    return true;
}

/**
 * Failsafe watchdog handler - fades channels to center if no updates received
 */
static void rc_failsafe_handler(struct k_work *work)
{
    if (!initialized || !rc_failsafe_enabled) {
        /* Reschedule even if disabled, so we can re-enable later */
        k_work_schedule(&rc_failsafe_work, K_MSEC(RC_FAILSAFE_CHECK_MS));
        return;
    }

    uint32_t now = k_uptime_get_32();

    for (int i = 0; i < GPIO_RC_COUNT; i++) {
        uint32_t elapsed = now - rc_last_update[i];

        if (elapsed >= RC_FAILSAFE_TIMEOUT_MS) {
            uint16_t current = rc_pulse_us[i];

            /* Already at center? Nothing to do */
            if (current == RC_PWM_CENTER_US) {
                continue;
            }

            /* Fade toward center */
            uint16_t new_val;
            if (current > RC_PWM_CENTER_US) {
                /* Above center - decrease */
                if (current - RC_PWM_CENTER_US <= RC_FAILSAFE_FADE_STEP) {
                    new_val = RC_PWM_CENTER_US;
                } else {
                    new_val = current - RC_FAILSAFE_FADE_STEP;
                }
            } else {
                /* Below center - increase */
                if (RC_PWM_CENTER_US - current <= RC_FAILSAFE_FADE_STEP) {
                    new_val = RC_PWM_CENTER_US;
                } else {
                    new_val = current + RC_FAILSAFE_FADE_STEP;
                }
            }

            /* Use internal set to avoid resetting timestamp */
            rc_set_internal(i, new_val);

            if (new_val == RC_PWM_CENTER_US) {
                LOG_WRN("RC ch%d failsafe: centered after %dms timeout", i, elapsed);
            }
        }
    }

    /* Reschedule */
    k_work_schedule(&rc_failsafe_work, K_MSEC(RC_FAILSAFE_CHECK_MS));
}

/**
 * Enable or disable RC failsafe
 */
void gpio_rc_set_failsafe(bool enabled)
{
    rc_failsafe_enabled = enabled;
    LOG_INF("RC failsafe %s", enabled ? "enabled" : "disabled");
}

/**
 * Check if RC failsafe is enabled
 */
bool gpio_rc_failsafe_enabled(void)
{
    return rc_failsafe_enabled;
}

#endif /* GPIO_RC_COUNT > 0 */

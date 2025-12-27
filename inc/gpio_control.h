/*
 * HID-HOP - GPIO Control Module
 *
 * Board-aware GPIO control for available pins.
 * Raytac MDBT50Q-CX-40: 2 LEDs (P0.6, P0.8), 1 Button (P0.18)
 * XIAO nRF52840: 3 LEDs, 2 RC PWM channels (D0, D1), 9 GPIO pins
 *
 * Copyright (c) 2025 Nathan Brewer
 */

#ifndef GPIO_CONTROL_H
#define GPIO_CONTROL_H

#include <stdint.h>
#include <stdbool.h>

/* GPIO counts depend on board */
#ifdef CONFIG_BOARD_RAYTAC_MDBT50Q_CX_40
#define GPIO_LED_COUNT    2
#define GPIO_BTN_COUNT    1
#define GPIO_RELAY_COUNT  0
#define GPIO_DIN_COUNT    0
#define GPIO_AIN_COUNT    0
#define GPIO_RC_COUNT     0
#elif defined(CONFIG_BOARD_XIAO_NRF52840)
#define GPIO_LED_COUNT    3
#define GPIO_BTN_COUNT    0
#define GPIO_RELAY_COUNT  0
#define GPIO_DIN_COUNT    0
#define GPIO_AIN_COUNT    0
#define GPIO_RC_COUNT     2   /* RC PWM channels on D0, D1 */
#else
/* Default/DK board */
#define GPIO_LED_COUNT    4
#define GPIO_BTN_COUNT    4
#define GPIO_RELAY_COUNT  7
#define GPIO_DIN_COUNT    2
#define GPIO_AIN_COUNT    2
#define GPIO_RC_COUNT     0
#endif

/* RC Servo/ESC PWM timing constants */
#define RC_PWM_PERIOD_US    20000   /* 20ms = 50Hz */
#define RC_PWM_CENTER_US    1500    /* Center/neutral position */
#define RC_PWM_MIN_US       1000    /* Minimum pulse width */
#define RC_PWM_MAX_US       2000    /* Maximum pulse width */

/* State structure for all GPIO */
typedef struct {
    uint8_t led_state;      /* Bitmask of LED states */
    uint8_t btn_state;      /* Bitmask of button states */
} gpio_state_t;

/**
 * Initialize GPIO control module
 * @return true on success
 */
bool gpio_control_init(void);

/**
 * Set LED state
 * @param led_index LED index (0-based)
 * @param on true to turn on, false to turn off
 * @return true on success
 */
bool gpio_led_set(uint8_t led_index, bool on);

/**
 * Get LED state
 * @param led_index LED index
 * @return true if LED is on
 */
bool gpio_led_get(uint8_t led_index);

/**
 * Toggle LED
 * @param led_index LED index
 * @return new state
 */
bool gpio_led_toggle(uint8_t led_index);

/**
 * Set all LEDs at once
 * @param state Bitmask (bit 0 = LED0, etc)
 */
void gpio_led_set_all(uint8_t state);

/**
 * Get all LED states
 * @return Bitmask of LED states
 */
uint8_t gpio_led_get_all(void);

/**
 * Read button state
 * @param btn_index Button index (0-based)
 * @return true if pressed
 */
bool gpio_btn_read(uint8_t btn_index);

/**
 * Read all button states
 * @return Bitmask of button states
 */
uint8_t gpio_btn_read_all(void);

/**
 * Get complete GPIO state
 * @param state Pointer to state structure to fill
 */
void gpio_get_state(gpio_state_t *state);

/**
 * Blink LED (blocking)
 * @param led_index LED index
 * @param count Number of blinks
 * @param on_ms On time in ms
 * @param off_ms Off time in ms
 */
void gpio_led_blink(uint8_t led_index, uint8_t count, uint16_t on_ms, uint16_t off_ms);

/* RC PWM functions - only available when GPIO_RC_COUNT > 0 */
#if GPIO_RC_COUNT > 0
/**
 * Set RC PWM channel pulse width (for servo/ESC control)
 * @param channel RC channel (0-based)
 * @param pulse_us Pulse width in microseconds (1000-2000, center=1500)
 * @return true on success
 */
bool gpio_rc_set(uint8_t channel, uint16_t pulse_us);

/**
 * Get current RC PWM channel pulse width
 * @param channel RC channel (0-based)
 * @return Current pulse width in microseconds (0 if invalid channel)
 */
uint16_t gpio_rc_get(uint8_t channel);

/**
 * Center all RC PWM channels (set to 1500us)
 */
void gpio_rc_center_all(void);

/**
 * Disable RC PWM channel (stop PWM output)
 * @param channel RC channel (0-based)
 * @return true on success
 */
bool gpio_rc_disable(uint8_t channel);
#else
/* Stub functions when no RC channels available */
static inline bool gpio_rc_set(uint8_t channel, uint16_t pulse_us) { (void)channel; (void)pulse_us; return false; }
static inline uint16_t gpio_rc_get(uint8_t channel) { (void)channel; return 0; }
static inline void gpio_rc_center_all(void) { }
static inline bool gpio_rc_disable(uint8_t channel) { (void)channel; return false; }
#endif

/* Stub functions for boards without relay/din/ain */
#if GPIO_RELAY_COUNT == 0
static inline bool gpio_relay_set(uint8_t index, bool on) { (void)index; (void)on; return false; }
static inline bool gpio_relay_get(uint8_t index) { (void)index; return false; }
static inline void gpio_relay_set_all(uint8_t state) { (void)state; }
static inline uint8_t gpio_relay_get_all(void) { return 0; }
#endif

#if GPIO_DIN_COUNT == 0
static inline bool gpio_din_read(uint8_t index) { (void)index; return false; }
static inline uint8_t gpio_din_read_all(void) { return 0; }
#endif

#if GPIO_AIN_COUNT == 0
static inline uint16_t gpio_ain_read(uint8_t index) { (void)index; return 0; }
static inline void gpio_ain_read_all(uint16_t *values) { (void)values; }
#endif

#endif /* GPIO_CONTROL_H */

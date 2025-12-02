/*
 * HID-HOP - GPIO Control Module
 *
 * Manages GPIO pins for:
 * - LEDs (P0.22, P0.23, P0.24) - Output
 * - Relays (P0.4-P0.10) - Output
 * - Digital inputs (P0.20, P0.21) - Input
 * - Analog inputs (P0.2, P0.3) - ADC
 */

#ifndef GPIO_CONTROL_H
#define GPIO_CONTROL_H

#include <stdint.h>
#include <stdbool.h>

/* LED indices */
#define GPIO_LED_0      0   /* P0.23 - LED0 on DK */
#define GPIO_LED_1      1   /* P0.22 - LED1 on DK */
#define GPIO_LED_2      2   /* P0.24 - LED2 on DK */
#define GPIO_LED_COUNT  3

/* Relay/output indices (P0.4 - P0.10) */
#define GPIO_RELAY_0    0   /* P0.4 */
#define GPIO_RELAY_1    1   /* P0.5 */
#define GPIO_RELAY_2    2   /* P0.6 */
#define GPIO_RELAY_3    3   /* P0.7 */
#define GPIO_RELAY_4    4   /* P0.8 */
#define GPIO_RELAY_5    5   /* P0.9 */
#define GPIO_RELAY_6    6   /* P0.10 */
#define GPIO_RELAY_COUNT 7

/* Digital input indices */
#define GPIO_DIN_0      0   /* P0.20 */
#define GPIO_DIN_1      1   /* P0.21 */
#define GPIO_DIN_COUNT  2

/* Analog input indices */
#define GPIO_AIN_0      0   /* P0.2 / AIN0 */
#define GPIO_AIN_1      1   /* P0.3 / AIN1 */
#define GPIO_AIN_COUNT  2

/* State structure for all GPIO */
typedef struct {
    uint8_t led_state;      /* Bitmask of LED states */
    uint8_t relay_state;    /* Bitmask of relay states */
    uint8_t din_state;      /* Bitmask of digital input states */
    uint16_t ain_values[GPIO_AIN_COUNT];  /* Analog values (12-bit) */
} gpio_state_t;

/**
 * Initialize GPIO control module
 * @return true on success
 */
bool gpio_control_init(void);

/**
 * Set LED state
 * @param led_index LED index (0-2)
 * @param on true to turn on, false to turn off
 * @return true on success
 */
bool gpio_led_set(uint8_t led_index, bool on);

/**
 * Get LED state
 * @param led_index LED index (0-2)
 * @return true if LED is on
 */
bool gpio_led_get(uint8_t led_index);

/**
 * Set all LEDs at once
 * @param state Bitmask (bit 0 = LED0, bit 1 = LED1, bit 2 = LED2)
 */
void gpio_led_set_all(uint8_t state);

/**
 * Get all LED states
 * @return Bitmask of LED states
 */
uint8_t gpio_led_get_all(void);

/**
 * Set relay/output state
 * @param relay_index Relay index (0-6)
 * @param on true to turn on, false to turn off
 * @return true on success
 */
bool gpio_relay_set(uint8_t relay_index, bool on);

/**
 * Get relay/output state
 * @param relay_index Relay index (0-6)
 * @return true if relay is on
 */
bool gpio_relay_get(uint8_t relay_index);

/**
 * Set all relays at once
 * @param state Bitmask (bit 0 = relay 0, etc.)
 */
void gpio_relay_set_all(uint8_t state);

/**
 * Get all relay states
 * @return Bitmask of relay states
 */
uint8_t gpio_relay_get_all(void);

/**
 * Read digital input
 * @param din_index Digital input index (0-1)
 * @return true if input is high
 */
bool gpio_din_read(uint8_t din_index);

/**
 * Read all digital inputs
 * @return Bitmask of digital input states
 */
uint8_t gpio_din_read_all(void);

/**
 * Read analog input
 * @param ain_index Analog input index (0-1)
 * @return 12-bit ADC value (0-4095)
 */
uint16_t gpio_ain_read(uint8_t ain_index);

/**
 * Read all analog inputs
 * @param values Array to store values (at least GPIO_AIN_COUNT elements)
 */
void gpio_ain_read_all(uint16_t *values);

/**
 * Get complete GPIO state
 * @param state Pointer to state structure to fill
 */
void gpio_get_state(gpio_state_t *state);

#endif /* GPIO_CONTROL_H */

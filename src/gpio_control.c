/*
 * HID-HOP - GPIO Control Module Implementation
 *
 * Manages GPIO pins for LEDs, relays, and inputs.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>

#if CONFIG_ADC
#include <zephyr/drivers/adc.h>
#include <hal/nrf_saadc.h>
#endif

#include "gpio_control.h"

LOG_MODULE_REGISTER(gpio_control, LOG_LEVEL_INF);

/* GPIO device */
static const struct device *gpio0_dev;

#if CONFIG_ADC
/* ADC device and configuration */
static const struct device *adc_dev;

/* ADC channel configuration for P0.2 (AIN0) and P0.3 (AIN1) */
static struct adc_channel_cfg adc_channel_cfg[GPIO_AIN_COUNT] = {
    {
        .gain = ADC_GAIN_1_6,
        .reference = ADC_REF_INTERNAL,
        .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 10),
        .channel_id = 0,  /* AIN0 = P0.02 */
        .input_positive = SAADC_CH_PSELP_PSELP_AnalogInput0,
    },
    {
        .gain = ADC_GAIN_1_6,
        .reference = ADC_REF_INTERNAL,
        .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 10),
        .channel_id = 1,  /* AIN1 = P0.03 */
        .input_positive = SAADC_CH_PSELP_PSELP_AnalogInput1,
    },
};
#endif

/* Pin definitions - nRF52840 DK */
#define LED0_PIN    23  /* P0.23 */
#define LED1_PIN    22  /* P0.22 */
#define LED2_PIN    24  /* P0.24 */

#define RELAY0_PIN  4   /* P0.4 */
#define RELAY1_PIN  5   /* P0.5 */
#define RELAY2_PIN  6   /* P0.6 */
#define RELAY3_PIN  7   /* P0.7 */
#define RELAY4_PIN  8   /* P0.8 */
#define RELAY5_PIN  9   /* P0.9 */
#define RELAY6_PIN  10  /* P0.10 */

#define DIN0_PIN    20  /* P0.20 */
#define DIN1_PIN    21  /* P0.21 */

/* Pin arrays for easy iteration */
static const uint8_t led_pins[GPIO_LED_COUNT] = {LED0_PIN, LED1_PIN, LED2_PIN};
static const uint8_t relay_pins[GPIO_RELAY_COUNT] = {
    RELAY0_PIN, RELAY1_PIN, RELAY2_PIN, RELAY3_PIN,
    RELAY4_PIN, RELAY5_PIN, RELAY6_PIN
};
static const uint8_t din_pins[GPIO_DIN_COUNT] = {DIN0_PIN, DIN1_PIN};

/* Current output states (LEDs active low on DK, relays active high) */
static uint8_t led_state = 0;
static uint8_t relay_state = 0;

/* Module initialized flag */
static bool initialized = false;

/**
 * Initialize GPIO control module
 */
bool gpio_control_init(void)
{
    int ret;

    if (initialized) {
        return true;
    }

    /* Get GPIO device */
    gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio0_dev)) {
        LOG_ERR("GPIO0 device not ready");
        return false;
    }

    /* Configure LED pins as outputs (active low on DK) */
    for (int i = 0; i < GPIO_LED_COUNT; i++) {
        ret = gpio_pin_configure(gpio0_dev, led_pins[i], GPIO_OUTPUT_HIGH);
        if (ret < 0) {
            LOG_ERR("Failed to configure LED%d pin: %d", i, ret);
            return false;
        }
    }
    LOG_INF("LEDs configured: P0.%d, P0.%d, P0.%d", LED0_PIN, LED1_PIN, LED2_PIN);

    /* Configure relay pins as outputs (start off) */
    for (int i = 0; i < GPIO_RELAY_COUNT; i++) {
        ret = gpio_pin_configure(gpio0_dev, relay_pins[i], GPIO_OUTPUT_LOW);
        if (ret < 0) {
            LOG_ERR("Failed to configure relay%d pin: %d", i, ret);
            return false;
        }
    }
    LOG_INF("Relays configured: P0.4 - P0.10");

    /* Configure digital input pins */
    for (int i = 0; i < GPIO_DIN_COUNT; i++) {
        ret = gpio_pin_configure(gpio0_dev, din_pins[i], GPIO_INPUT | GPIO_PULL_UP);
        if (ret < 0) {
            LOG_ERR("Failed to configure DIN%d pin: %d", i, ret);
            return false;
        }
    }
    LOG_INF("Digital inputs configured: P0.%d, P0.%d", DIN0_PIN, DIN1_PIN);

#if CONFIG_ADC
    /* Get ADC device */
    adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));
    if (!device_is_ready(adc_dev)) {
        LOG_WRN("ADC device not ready - analog inputs disabled");
        adc_dev = NULL;
    } else {
        /* Configure ADC channels */
        for (int i = 0; i < GPIO_AIN_COUNT; i++) {
            ret = adc_channel_setup(adc_dev, &adc_channel_cfg[i]);
            if (ret < 0) {
                LOG_ERR("Failed to configure ADC channel %d: %d", i, ret);
                adc_dev = NULL;
                break;
            }
        }
        if (adc_dev) {
            LOG_INF("Analog inputs configured: P0.2 (AIN0), P0.3 (AIN1)");
        }
    }
#else
    LOG_INF("ADC disabled - analog inputs not available");
#endif

    initialized = true;
    LOG_INF("GPIO control module initialized");
    return true;
}

/**
 * Set LED state (LEDs are active LOW on nRF52840 DK)
 */
bool gpio_led_set(uint8_t led_index, bool on)
{
    if (!initialized || led_index >= GPIO_LED_COUNT) {
        return false;
    }

    /* Active low - set pin LOW to turn LED ON */
    int ret = gpio_pin_set(gpio0_dev, led_pins[led_index], on ? 0 : 1);
    if (ret < 0) {
        LOG_ERR("Failed to set LED%d: %d", led_index, ret);
        return false;
    }

    if (on) {
        led_state |= (1 << led_index);
    } else {
        led_state &= ~(1 << led_index);
    }

    LOG_DBG("LED%d %s", led_index, on ? "ON" : "OFF");
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
 * Set relay/output state (active HIGH)
 */
bool gpio_relay_set(uint8_t relay_index, bool on)
{
    if (!initialized || relay_index >= GPIO_RELAY_COUNT) {
        return false;
    }

    int ret = gpio_pin_set(gpio0_dev, relay_pins[relay_index], on ? 1 : 0);
    if (ret < 0) {
        LOG_ERR("Failed to set relay%d: %d", relay_index, ret);
        return false;
    }

    if (on) {
        relay_state |= (1 << relay_index);
    } else {
        relay_state &= ~(1 << relay_index);
    }

    LOG_DBG("Relay%d %s", relay_index, on ? "ON" : "OFF");
    return true;
}

/**
 * Get relay/output state
 */
bool gpio_relay_get(uint8_t relay_index)
{
    if (relay_index >= GPIO_RELAY_COUNT) {
        return false;
    }
    return (relay_state & (1 << relay_index)) != 0;
}

/**
 * Set all relays at once
 */
void gpio_relay_set_all(uint8_t state)
{
    for (int i = 0; i < GPIO_RELAY_COUNT; i++) {
        gpio_relay_set(i, (state & (1 << i)) != 0);
    }
}

/**
 * Get all relay states
 */
uint8_t gpio_relay_get_all(void)
{
    return relay_state;
}

/**
 * Read digital input (inputs have pull-up, active low)
 */
bool gpio_din_read(uint8_t din_index)
{
    if (!initialized || din_index >= GPIO_DIN_COUNT) {
        return false;
    }

    /* With pull-up, pin reads HIGH when open, LOW when grounded */
    int val = gpio_pin_get(gpio0_dev, din_pins[din_index]);
    return val == 0;  /* Active low - return true when grounded */
}

/**
 * Read all digital inputs
 */
uint8_t gpio_din_read_all(void)
{
    uint8_t state = 0;
    for (int i = 0; i < GPIO_DIN_COUNT; i++) {
        if (gpio_din_read(i)) {
            state |= (1 << i);
        }
    }
    return state;
}

/**
 * Read analog input
 */
uint16_t gpio_ain_read(uint8_t ain_index)
{
#if CONFIG_ADC
    if (!initialized || !adc_dev || ain_index >= GPIO_AIN_COUNT) {
        return 0;
    }

    int16_t sample_buffer;
    struct adc_sequence sequence = {
        .channels = BIT(adc_channel_cfg[ain_index].channel_id),
        .buffer = &sample_buffer,
        .buffer_size = sizeof(sample_buffer),
        .resolution = 12,
    };

    int ret = adc_read(adc_dev, &sequence);
    if (ret < 0) {
        LOG_ERR("ADC read failed: %d", ret);
        return 0;
    }

    /* Convert signed to unsigned (SAADC can return negative values) */
    if (sample_buffer < 0) {
        sample_buffer = 0;
    }

    return (uint16_t)sample_buffer;
#else
    ARG_UNUSED(ain_index);
    return 0;
#endif
}

/**
 * Read all analog inputs
 */
void gpio_ain_read_all(uint16_t *values)
{
    if (values == NULL) {
        return;
    }

    for (int i = 0; i < GPIO_AIN_COUNT; i++) {
        values[i] = gpio_ain_read(i);
    }
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
    state->relay_state = gpio_relay_get_all();
    state->din_state = gpio_din_read_all();
    gpio_ain_read_all(state->ain_values);
}

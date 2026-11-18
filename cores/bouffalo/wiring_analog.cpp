/*
 * analogRead on the BL616CL GPADC (LHAL adc_v2). Only the Arduino pins listed in the variant's
 * g_analog_pins are accepted; others return -1. Each call switches the pin to analog input,
 * converts once in the 16-bit (oversampled) mode, applies the driver calibration and returns the
 * value scaled to analogReadResolution() bits. Single-ended full scale is 3.2 V from the internal
 * reference; inputs at or above it read the top calibrated code. Not for interrupt context.
 */
#include "bflb_adc_v2.h"
#include "bflb_gpio.h"
#include "pin_keep.h"
#include "pin_map.h"

#include "Arduino.h"

/*
 * Effective bits measured on the 16-bit mode (datasheet ENOB 12.3); requests above this
 * resolution are padded with zero low bits, as the Arduino reference specifies.
 */
static constexpr int EFFECTIVE_BITS = 12;

static int read_bits = 10;

/**
 * @brief Return the BL616CL external ADC channel of a GPIO (datasheet pin table, function 10),
 * or -1 if the GPIO has none.
 */
static int gpio_adc_channel(int gpio)
{
    if (gpio >= 0 && gpio <= 7) {
        return gpio;
    }
    switch (gpio) {
        case 12:
            return 8;
        case 13:
            return 9;
        case 32: /* USB_DP */
            return 10;
        case 33: /* USB_DM */
            return 11;
        default:
            return -1;
    }
}

/**
 * @brief Return the GPIO of a variant-declared analog pin, or -1.
 */
static int analog_pin_gpio(pin_size_t pin)
{
#if NUM_ANALOG_INPUTS > 0
    for (uint8_t analog_pin : g_analog_pins) {
        if (analog_pin == pin) {
            return pin_to_gpio(pin);
        }
    }
#else
    (void)pin;
#endif
    return -1;
}

/**
 * @brief Initialize the GPADC once: single conversion, 16-bit mode, internal reference. The
 * driver reads the efuse trims and measures its internal ground and 20 mV points the first time.
 */
static struct bflb_device_s *adc_dev(void)
{
    static struct bflb_device_s *dev;

    if (dev == nullptr) {
        struct bflb_device_s *adc = bflb_device_get_by_name("adc_v2_0");
        struct bflb_adc_config_s cfg = {};

        cfg.resolution = ADC_RESOLUTION_16B;
        cfg.vref = ADC_VREF_INTERNAL_1P25;
        if (adc != nullptr && bflb_adc_init(adc, &cfg) == 0) {
            dev = adc;
        }
    }
    return dev;
}

/**
 * @brief Convert a declared analog pin once; return -1 for any other pin.
 */
int analogRead(pin_size_t pin)
{
    int gpio = analog_pin_gpio(pin);
    int channel = gpio < 0 ? -1 : gpio_adc_channel(gpio);
    struct bflb_device_s *adc = channel < 0 ? nullptr : adc_dev();

    if (adc == nullptr) {
        return -1;
    }
    pin_release_keep(gpio);
    bflb_gpio_init(bflb_device_get_by_name("gpio"), gpio, GPIO_ANALOG | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_0);

    struct bflb_adc_channel_s chan = {(uint8_t)channel, ADC_EXTERNAL_CHANNEL_NULL};
    bflb_adc_channel_config_external(adc, &chan, 1);
    bflb_adc_clear_fifo(adc);
    bflb_adc_start_conversion(adc); /* The driver waits 100 us here before starting. */
    while (bflb_adc_get_count(adc) == 0) {
    }
    uint32_t raw = bflb_adc_read_raw(adc);
    bflb_adc_stop_conversion(adc);

    struct bflb_adc_result_s result;
    bflb_adc_parse_result(adc, &raw, &result, 1); /* Calibrated 16-bit value. */
    uint32_t value = (uint32_t)result.value >> (16 - EFFECTIVE_BITS);
    if (read_bits <= EFFECTIVE_BITS) {
        return value >> (EFFECTIVE_BITS - read_bits);
    }
    return value << (read_bits - EFFECTIVE_BITS);
}

/**
 * @brief The internal 3.2 V full-scale reference is the only one, so AR_DEFAULT is always in
 * effect and other modes are ignored.
 */
void analogReference(uint8_t mode)
{
    (void)mode;
}

/**
 * @brief Set the analogRead result width to 1–31 bits; other values leave it unchanged. 32 is
 * rejected because the result would not fit a non-negative int.
 */
void analogReadResolution(int bits)
{
    if (bits >= 1 && bits <= 31) {
        read_bits = bits;
    }
}

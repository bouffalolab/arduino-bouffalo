/*
 * analogRead on the BL616CL GPADC (LHAL adc_v2). Only the Arduino pins listed in the variant's
 * g_analog_pins are accepted; others return -1. Each call switches the pin to analog input,
 * converts once in the 16-bit (oversampled) mode, applies the driver calibration and returns the
 * value scaled to analogReadResolution() bits. Single-ended full scale is 3.2 V from the internal
 * reference; inputs at or above it read the top calibrated code. Not for interrupt context.
 *
 * analogWrite on the BL616CL PWM0 controller (LHAL pwm_v2): four channels share one counter, 1 kHz
 * unless analogWriteFrequency() changes it. In the single-ended routing selected here, GPIO n
 * carries the active-high output of channel n % 4. Each channel drives one pin at a time; see
 * analogWrite() for rejected requests.
 */
#include "bflb_adc_v2.h"
#include "bflb_clock.h"
#include "bflb_gpio.h"
#include "bflb_pwm_v2.h"
#include "hardware/pwm_v2_reg.h"
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
    pin_pwm_release(pin);
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

static constexpr int PWM_CHANNELS = 4;
static constexpr uint8_t PWM_NO_PIN = 0xFF;
static constexpr uint32_t PWM_MAX_HZ = 1000000; /* 40 duty steps at the 40 MHz XCLK. */

static int write_bits = 8;
static uint32_t write_hz = 1000;
static uint16_t pwm_period; /* Counts per period of the running counter. */
static uint8_t pwm_owner[PWM_CHANNELS] = {PWM_NO_PIN, PWM_NO_PIN, PWM_NO_PIN, PWM_NO_PIN};
static uint32_t pwm_level[PWM_CHANNELS], pwm_full[PWM_CHANNELS]; /* Duty of each owned channel: level / full. */

/**
 * @brief Stop the counter and set it to write_hz from XCLK. The smallest divider whose period fits
 * 16 bits keeps the most duty steps: 1 kHz at 40 MHz is divider 1 and 40000 counts (25 ns steps).
 */
static void pwm_set_counter(struct bflb_device_s *pwm)
{
    uint64_t src = bflb_clk_get_system_clock(BFLB_SYSTEM_XCLK);
    uint32_t div = (uint32_t)((src + (uint64_t)write_hz * 65535 - 1) / ((uint64_t)write_hz * 65535));
    uint64_t per_div = (uint64_t)div * write_hz;
    struct bflb_pwm_v2_config_s cfg = {};
    uint32_t cfg0 = pwm->reg_base + PWM_MC0_CONFIG0_OFFSET;

    /* The counter stops at the end of its period, up to about 1 s at 1 Hz, but bflb_pwm_v2_init()
     * gives up after 100 ms without applying the new setting, so wait for the stop here. */
    putreg32(getreg32(cfg0) | PWM_STOP_EN, cfg0);
    while ((getreg32(cfg0) & PWM_STS_STOP) == 0) {
    }
    pwm_period = (uint16_t)((src + per_div / 2) / per_div);
    cfg.counter_mode = PWM_COUNTER_MODE_UP;
    cfg.clk_source = BFLB_SYSTEM_XCLK;
    cfg.clk_div = (uint16_t)div;
    cfg.period = pwm_period;
    bflb_pwm_v2_init(pwm, &cfg);
}

/**
 * @brief Set channel ch's output active while the counter is in [0, round(level / full * period)):
 * 0 stays low and a whole period stays high.
 */
static void pwm_apply(struct bflb_device_s *pwm, uint8_t ch)
{
    uint64_t high = ((uint64_t)pwm_level[ch] * pwm_period + pwm_full[ch] / 2) / pwm_full[ch];
    bflb_pwm_v2_channel_set_threshold(pwm, ch, 0, (uint16_t)high);
}

/**
 * @brief Initialize PWM0 once: single-ended pin routing, counter at write_hz, active-high outputs.
 */
static struct bflb_device_s *pwm_dev(void)
{
    static struct bflb_device_s *dev;

    if (dev == nullptr) {
        struct bflb_device_s *pwm = bflb_device_get_by_name("pwm_v2_0");
        struct bflb_pwm_v2_channel_config_s ch_cfg = {};

        if (pwm == nullptr) {
            return nullptr;
        }
        /* analogWrite's GPIO % 4 channel map needs this routing; set it rather than rely on board_init. */
        bflb_pwm_v2_feature_control(pwm, PWM_CMD_IO_SEL, PWM_IO_SEL_SINGLE_END);
        pwm_set_counter(pwm);
        ch_cfg.positive_polarity = PWM_POLARITY_ACTIVE_HIGH;
        ch_cfg.negative_polarity = PWM_POLARITY_ACTIVE_HIGH;
        for (uint8_t ch = 0; ch < PWM_CHANNELS; ch++) {
            bflb_pwm_v2_channel_init(pwm, ch, &ch_cfg);
        }
        bflb_pwm_v2_start(pwm);
        dev = pwm;
    }
    return dev;
}

bool pin_pwm_release(pin_size_t pin)
{
    for (uint8_t &owner : pwm_owner) {
        if (owner == pin) {
            owner = PWM_NO_PIN;
            return true;
        }
    }
    return false;
}

/**
 * @brief Output PWM on a variant PWM pin with duty value / (2^bits - 1); value <= 0 holds the pin
 * low and values at or above full scale hold it high. Requests for non-PWM, reserved or
 * out-of-range pins, and for a pin whose channel (GPIO % 4) another pin drives, are ignored and
 * leave that pin unchanged. Not for interrupt context.
 */
void analogWrite(pin_size_t pin, int value)
{
    int gpio = pin_to_gpio(pin);
    if (gpio < 0 || !digitalPinHasPWM(pin)) {
        return;
    }
    uint8_t ch = gpio % PWM_CHANNELS;
    if (pwm_owner[ch] != PWM_NO_PIN && pwm_owner[ch] != pin) {
        return; /* Another pin drives this channel. */
    }
    struct bflb_device_s *pwm = pwm_dev();
    if (pwm == nullptr) {
        return;
    }
    uint32_t full = (1u << write_bits) - 1;
    pwm_level[ch] = value <= 0 ? 0 : ((uint32_t)value < full ? (uint32_t)value : full);
    pwm_full[ch] = full;
    pwm_apply(pwm, ch);
    bflb_pwm_v2_channel_positive_start(pwm, ch);
    pwm_owner[ch] = pin;
    pin_release_keep(gpio);
    bflb_gpio_init(bflb_device_get_by_name("gpio"), gpio,
                   GPIO_FUNC_PWM0 | GPIO_ALTERNATE | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_1);
}

/**
 * @brief Set the analogWrite value width to 1–31 bits; other values leave it unchanged. 32 is
 * rejected because full scale would not fit an int.
 */
void analogWriteResolution(int bits)
{
    if (bits >= 1 && bits <= 31) {
        write_bits = bits;
    }
}

/**
 * @brief Set the PWM frequency of every PWM pin to 1 Hz–1 MHz; other values leave it unchanged.
 * The change waits for the current period to end, then the counter restarts and each running pin
 * keeps its duty, recomputed for the new period.
 */
void analogWriteFrequency(uint32_t hz)
{
    struct bflb_device_s *pwm = hz >= 1 && hz <= PWM_MAX_HZ ? pwm_dev() : nullptr;

    if (pwm == nullptr) {
        return;
    }
    write_hz = hz;
    pwm_set_counter(pwm);
    for (uint8_t ch = 0; ch < PWM_CHANNELS; ch++) {
        if (pwm_owner[ch] != PWM_NO_PIN) {
            pwm_apply(pwm, ch);
        }
    }
    bflb_pwm_v2_start(pwm);
}

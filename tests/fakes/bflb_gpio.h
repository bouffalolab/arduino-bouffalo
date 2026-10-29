#ifndef BL616CL_RA4_BOOTSTRAP_TEST_BFLB_GPIO_H
#define BL616CL_RA4_BOOTSTRAP_TEST_BFLB_GPIO_H

#include <stdbool.h>
#include <stdint.h>

struct bflb_device_s {
};

#define GPIO_INPUT 0x20U
#define GPIO_OUTPUT 0x40U
#define GPIO_FLOAT 0x000U
#define GPIO_SMT_EN 0x800U
#define GPIO_DRV_1 0x1000U

enum bflb_gpio_fake_operation {
    BFLB_GPIO_FAKE_DEINIT,
    BFLB_GPIO_FAKE_SET,
    BFLB_GPIO_FAKE_RESET,
    BFLB_GPIO_FAKE_INIT,
};

struct bflb_gpio_fake_event {
    enum bflb_gpio_fake_operation operation;
    uint8_t pin;
    uint32_t config;
};

static struct bflb_gpio_fake_event bflb_gpio_fake_events[64];
static unsigned int bflb_gpio_fake_event_count;

static inline void bflb_gpio_fake_reset_events(void)
{
    bflb_gpio_fake_event_count = 0U;
}

static inline unsigned int bflb_gpio_fake_count(void)
{
    return bflb_gpio_fake_event_count;
}

static inline const struct bflb_gpio_fake_event *bflb_gpio_fake_event(unsigned int index)
{
    return index < bflb_gpio_fake_event_count ? &bflb_gpio_fake_events[index] : 0;
}

static inline void bflb_gpio_fake_record(
    enum bflb_gpio_fake_operation operation,
    uint8_t pin,
    uint32_t config)
{
    if (bflb_gpio_fake_event_count >=
        sizeof(bflb_gpio_fake_events) / sizeof(bflb_gpio_fake_events[0])) {
        return;
    }

    struct bflb_gpio_fake_event *event =
        &bflb_gpio_fake_events[bflb_gpio_fake_event_count++];
    event->operation = operation;
    event->pin = pin;
    event->config = config;
}

static inline struct bflb_device_s *bflb_device_get_by_name(const char *)
{
    static struct bflb_device_s device;
    return &device;
}

static inline void bflb_gpio_deinit(struct bflb_device_s *, uint8_t pin)
{
    bflb_gpio_fake_record(BFLB_GPIO_FAKE_DEINIT, pin, 0U);
}

static inline void bflb_gpio_set(struct bflb_device_s *, uint8_t pin)
{
    bflb_gpio_fake_record(BFLB_GPIO_FAKE_SET, pin, 0U);
}

static inline void bflb_gpio_reset(struct bflb_device_s *, uint8_t pin)
{
    bflb_gpio_fake_record(BFLB_GPIO_FAKE_RESET, pin, 0U);
}

static inline void bflb_gpio_init(struct bflb_device_s *, uint8_t pin, uint32_t config)
{
    bflb_gpio_fake_record(BFLB_GPIO_FAKE_INIT, pin, config);
}

static inline bool bflb_gpio_read(struct bflb_device_s *, uint8_t)
{
    return true;
}

#endif

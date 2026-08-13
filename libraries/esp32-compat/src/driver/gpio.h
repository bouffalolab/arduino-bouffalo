#ifndef BL616CL_ESP32_COMPAT_GPIO_H_
#define BL616CL_ESP32_COMPAT_GPIO_H_

#include <stdint.h>

typedef enum {
    GPIO_MODE_DISABLE = 0,
    GPIO_MODE_INPUT,
    GPIO_MODE_OUTPUT,
    GPIO_MODE_OUTPUT_OD,
    GPIO_MODE_INPUT_OUTPUT,
    GPIO_MODE_INPUT_OUTPUT_OD
} gpio_mode_t;

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    uint32_t pull_up_en;
    uint32_t pull_down_en;
    int intr_type;
} gpio_config_t;

#ifdef __cplusplus
extern "C" {
#endif

void gpio_config(gpio_config_t *cfg);
void gpio_set_level(int gpio, int level);
int gpio_get_level(int gpio);
void gpio_set_direction(int gpio, gpio_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif

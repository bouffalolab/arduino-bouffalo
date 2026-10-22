#include "bl616cl_pm.h"

#include "pin_keep.h"

void pin_release_keep(uint8_t pin)
{
    pm_disable_gpio_keep(pin);
}

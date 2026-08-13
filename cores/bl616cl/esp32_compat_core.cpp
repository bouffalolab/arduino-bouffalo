#include "Arduino.h"

EspClass ESP;

void EspClass::restart()
{
    // TODO(bl616cl): switch to a linked BL616CL system reset helper once the
    // platform runtime exposes one.
    for (;;) {
    }
}

extern "C" BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function,
                                              const char *name,
                                              uint32_t stackDepth,
                                              void *parameter,
                                              UBaseType_t priority,
                                              TaskHandle_t *taskHandle,
                                              BaseType_t coreID)
{
    (void)coreID;
    return xTaskCreate(function, name, stackDepth, parameter, priority, taskHandle);
}

extern "C" void ets_install_putc1(ets_putc_fn callback)
{
    (void)callback;
}

extern "C" void configTime(long timezone, long daylightOffset, const char *server)
{
    (void)timezone;
    (void)daylightOffset;
    (void)server;
}

extern "C" void esp_efuse_mac_get_default(uint8_t *mac)
{
    if (mac != nullptr) {
        for (size_t i = 0; i < 6; ++i) {
            mac[i] = 0;
        }
    }
}

extern "C" void esp_fill_random(void *buffer, size_t size)
{
    (void)buffer;
    (void)size;
}

#include "Arduino.h"
#include "bflb_efuse.h"
#include "bflb_sec_trng.h"
extern "C" {
#include "bl616cl_sys.h"
}

EspClass ESP;

void EspClass::restart()
{
    bl_sys_reset_system();
    for (;;) {}
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
    if (mac != nullptr && bflb_efuse_read_mac_address(mac, 1) != 0) {
        abort();
    }
}

extern "C" void esp_fill_random(void *buffer, size_t size)
{
    if (size != 0 && (buffer == nullptr || bflb_trng_readlen(static_cast<uint8_t *>(buffer), size) != 0)) {
        abort();
    }
}


#include "bouffalo_sdk.h"

void init(void)
{
    board_init();
}

unsigned long millis()
{
  return bflb_mtimer_get_time_ms();
}

unsigned long micros() {
  return bflb_mtimer_get_time_us();
}

void delay(unsigned long ms)
{
#ifndef CONFIG_FREERTOS
    bflb_mtimer_delay_us(ms * 1000);
#else
    vTaskDelay(ms);
#endif
}

void delayMicroseconds(unsigned int us)
{
    bflb_mtimer_delay_us(us);
}


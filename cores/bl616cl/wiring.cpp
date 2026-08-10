#include "Arduino.h"

#include "bflb_mtimer.h"

extern "C" void init(void)
{
}

extern "C" unsigned long millis(void)
{
    return (unsigned long)bflb_mtimer_get_time_ms();
}

extern "C" unsigned long micros(void)
{
    return (unsigned long)bflb_mtimer_get_time_us();
}

extern "C" void delay(unsigned long milliseconds)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        if (milliseconds == 0) {
            taskYIELD();
        } else {
            vTaskDelay(pdMS_TO_TICKS(milliseconds));
        }
        return;
    }

    bflb_mtimer_delay_ms(milliseconds);
}

extern "C" void delayMicroseconds(unsigned int microseconds)
{
    bflb_mtimer_delay_us(microseconds);
}

extern "C" void yield(void)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        taskYIELD();
    }
}

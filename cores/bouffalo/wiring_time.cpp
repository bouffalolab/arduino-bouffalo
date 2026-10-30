/*
 * millis/micros read the 64-bit microsecond mtimer, subtract the program start time and truncate
 * to unsigned long, preserving Arduino wraparound. Capture the origin in init() because uploader
 * warm reset does not clear mtimer. After scheduler startup delay yields for more than ms and at
 * most ms plus one 1 ms tick; before startup, including global constructors, it busy-waits.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "bflb_mtimer.h"

#include "Arduino.h"
#include "wiring_time.h"

static uint64_t start_us;

/**
 * @brief Capture the hardware microsecond counter as the Arduino program-time origin.
 */
void wiring_time_init(void)
{
    start_us = bflb_mtimer_get_time_us();
}

/**
 * @brief Report whether FreeRTOS has left its not-started state.
 */
static bool scheduler_running(void)
{
    return xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
}

/**
 * @brief Return elapsed program milliseconds with unsigned Arduino wraparound.
 */
unsigned long millis(void)
{
    return (unsigned long)((bflb_mtimer_get_time_us() - start_us) / 1000);
}

/**
 * @brief Return elapsed program microseconds with unsigned Arduino wraparound.
 */
unsigned long micros(void)
{
    return (unsigned long)(bflb_mtimer_get_time_us() - start_us);
}

/**
 * @brief Wait at least the requested milliseconds, yielding to FreeRTOS after scheduler
 * startup.
 */
void delay(unsigned long ms)
{
    if (!scheduler_running()) {
        bflb_mtimer_delay_ms(ms);
    } else if (ms == 0) {
        taskYIELD();
    } else {
        /*
 * vTaskDelay(n) guarantees only more than n-1 ticks. Wait one extra tick; compute in 64 bits and
 * split long delays to avoid overflow.
 */
        uint64_t ticks = ((uint64_t)ms * configTICK_RATE_HZ + 999) / 1000 + 1;
        while (ticks > 0) {
            TickType_t n = ticks > portMAX_DELAY - 1 ? portMAX_DELAY - 1 : (TickType_t)ticks;
            vTaskDelay(n);
            ticks -= n;
        }
    }
}

/**
 * @brief Busy-wait for the requested microsecond interval.
 */
void delayMicroseconds(unsigned int us)
{
    bflb_mtimer_delay_us(us);
}

/**
 * @brief Yield the current task when the scheduler is running.
 */
void yield(void)
{
    if (scheduler_running()) {
        taskYIELD();
    }
}

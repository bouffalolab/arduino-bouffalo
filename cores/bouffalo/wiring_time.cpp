/*
 * 时间函数。millis/micros 读 64 位 mtimer（微秒），截断为 unsigned long，与 Arduino 一样会回绕。
 * 调度器运行后 delay 让出 CPU，实际时长大于 ms、不超过 ms 加 1 个 tick（1 ms）；
 * 调度器启动前（如全局构造中）忙等。
 */
#include "FreeRTOS.h"
#include "task.h"
#include "bflb_mtimer.h"

#include "Arduino.h"

static bool scheduler_running(void)
{
    return xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
}

unsigned long millis(void)
{
    return (unsigned long)bflb_mtimer_get_time_ms();
}

unsigned long micros(void)
{
    return (unsigned long)bflb_mtimer_get_time_us();
}

void delay(unsigned long ms)
{
    if (!scheduler_running()) {
        bflb_mtimer_delay_ms(ms);
    } else if (ms == 0) {
        taskYIELD();
    } else {
        /* vTaskDelay(n) 只保证等待超过 n - 1 个 tick，多等 1 个 tick；用 64 位计算并分段，避免溢出 */
        uint64_t ticks = ((uint64_t)ms * configTICK_RATE_HZ + 999) / 1000 + 1;
        while (ticks > 0) {
            TickType_t n = ticks > portMAX_DELAY - 1 ? portMAX_DELAY - 1 : (TickType_t)ticks;
            vTaskDelay(n);
            ticks -= n;
        }
    }
}

void delayMicroseconds(unsigned int us)
{
    bflb_mtimer_delay_us(us);
}

void yield(void)
{
    if (scheduler_running()) {
        taskYIELD();
    }
}

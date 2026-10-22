/*
 * 固件唯一入口。初始化归属：
 * - 底层包：__start、SystemInit、时钟、flash、中断、堆（board_init()）；不初始化控制台。
 * - Core：C++ 全局构造、setup/loop 任务、FreeRTOS 调度器。
 * - 网络栈由 WiFi 库在首次使用时初始化，不在这里。
 */
#include <errno.h>
#include <reent.h>

#include "FreeRTOS.h"
#include "task.h"
#include "board.h"

#include "Arduino.h"
#include "cxx_runtime.h"

#ifndef ARDUINO_LOOP_STACK_SIZE
#define ARDUINO_LOOP_STACK_SIZE 8192 /* 字节 */
#endif
#ifndef ARDUINO_LOOP_PRIORITY
#define ARDUINO_LOOP_PRIORITY 1
#endif

extern "C" void __libc_init_array(void);

void initVariant(void) __attribute__((weak));
void initVariant(void)
{
}

static void loop_task(void *arg)
{
    (void)arg;
    setup();
    for (;;) {
        loop();
        if (arduino::serialEventRun) {
            arduino::serialEventRun();
        }
    }
}

/*
 * Core 不开 SDK 控制台：printf 的输出被 SDK 丢弃；SDK 的 _read_r（弱定义）会用空的控制台句柄读 UART，
 * 所以 stdin 在这里明确失败，getchar/scanf 返回 EOF。放在 main.cpp 以保证随入口一起链入。
 */
extern "C" _ssize_t _read_r(struct _reent *r, int fd, void *buf, size_t size)
{
    (void)r;
    (void)fd;
    (void)buf;
    (void)size;
    errno = EBADF;
    return -1;
}

int main(void)
{
    board_init();
    cxx_runtime_init(); /* 构造函数里的局部静态变量要用 guard 锁 */
    __libc_init_array();
    initVariant();

    if (xTaskCreate(loop_task, "loop", ARDUINO_LOOP_STACK_SIZE / sizeof(StackType_t), NULL,
                    ARDUINO_LOOP_PRIORITY, NULL) != pdPASS) {
        abort();
    }
    vTaskStartScheduler();
    abort(); /* 调度器不会返回 */
}

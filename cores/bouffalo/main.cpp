/*
 * 固件唯一入口。初始化归属：
 * - 底层包：__start、SystemInit、时钟、flash、中断、堆（board_init()，由 init() 调用）；不初始化控制台。
 * - Core：C++ 全局构造、setup/loop 任务、FreeRTOS 调度器；板卡启用 USB 时在 setup() 之前启动 USB 串口。
 * - 网络栈由 WiFi 库在首次使用时初始化，不在这里。
 * 顺序与官方 Core 相同：init()、initVariant()，再 setup()/loop()。
 */
#include <errno.h>
#include <reent.h>

#include "FreeRTOS.h"
#include "task.h"
#include "board.h"

#include "Arduino.h"
#include "cxx_runtime.h"
#include "isp_boot.h"
#include "usb_cdc_port.h"
#include "wiring_time.h"

#ifndef ARDUINO_LOOP_STACK_SIZE
#define ARDUINO_LOOP_STACK_SIZE 8192 /* 字节 */
#endif
#ifndef ARDUINO_LOOP_PRIORITY
#define ARDUINO_LOOP_PRIORITY 1
#endif

extern "C" void __libc_init_array(void);

/*
 * ArduinoCore-API 声明的芯片初始化；全局构造要用堆，所以在构造之前调用。
 * 烧录工具经 1200 bps touch 上传后用 warm reset 启动新固件，“进入下载模式”的请求仍在，这里清除。
 */
void init(void)
{
    board_init();
    isp_boot_clear();
    wiring_time_init();
}

void initVariant(void) __attribute__((weak));
void initVariant(void)
{
}

/* serialEvent()、serialEvent1()、serialEvent2()：sketch 定义时，每次 loop() 之后若对应串口有数据就调用 */
void serialEvent(void) __attribute__((weak));
void serialEvent1(void) __attribute__((weak));
void serialEvent2(void) __attribute__((weak));

void arduino::serialEventRun(void)
{
#if defined(USB_VID) || defined(PIN_SERIAL_TX)
    if (serialEvent && Serial.available()) {
        serialEvent();
    }
#endif
#if defined(PIN_SERIAL1_TX)
    if (serialEvent1 && Serial1.available()) {
        serialEvent1();
    }
#endif
#if defined(PIN_SERIAL2_TX)
    if (serialEvent2 && Serial2.available()) {
        serialEvent2();
    }
#endif
}

static void loop_task(void *arg)
{
    (void)arg;
#if defined(USB_VID)
    usb_cdc_port_init(); /* 中断回调要用定时器任务，所以在调度器运行后启动 */
#endif
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
    init();
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

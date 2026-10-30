/*
 * Unique firmware entry point. The dependency package owns __start, SystemInit, clocks, Flash,
 * interrupts and heap through board_init(), called by init(); it does not start a console. The
 * Core optionally selects uart2 for debug output, runs global constructors, creates the setup/loop
 * task and starts FreeRTOS. USB starts before setup() when enabled. WiFi initializes networking
 * lazily. Follow the Arduino sequence init(), initVariant(), then setup()/loop().
 */
#include <errno.h>
#include <reent.h>

#include "FreeRTOS.h"
#include "task.h"
#include "board.h"

#include "Arduino.h"
#include "cxx_runtime.h"
#include "isp_boot.h"
#include "pin_map.h"
#include "usb_cdc_port.h"
#include "wiring_time.h"

#ifndef ARDUINO_LOOP_STACK_SIZE
#define ARDUINO_LOOP_STACK_SIZE 8192 /* Bytes. */
#endif
#ifndef ARDUINO_LOOP_PRIORITY
#define ARDUINO_LOOP_PRIORITY 1
#endif

/**
 * @brief Run the linked C/C++ initialization arrays before starting the sketch task.
 */
extern "C" void __libc_init_array(void);

/**
 * @brief Initialize the chip before global constructors, which require the heap. Clear the
 * retained download request left by the uploader warm reset after 1200 bps touch.
 */
void init(void)
{
    board_init();
    isp_boot_clear();
    wiring_time_init();
#if defined(ARDUINO_BL_DEBUG_LOG_BAUD) && defined(PIN_DEBUG_LOG_TX)
    board_console_init("uart2", pin_to_gpio(PIN_DEBUG_LOG_TX), pin_to_gpio(PIN_DEBUG_LOG_RX), ARDUINO_BL_DEBUG_LOG_BAUD);
#endif
}

/**
 * @brief Provide the weak board-specific initialization hook after global constructors.
 */
void initVariant(void) __attribute__((weak));
/**
 * @brief Provide the weak board-specific initialization hook after global constructors.
 */
void initVariant(void)
{
}

/**
 * @brief After each loop(), call sketch-defined serialEvent/serialEvent1/serialEvent2 when the
 * corresponding port has data.
 */
void serialEvent(void) __attribute__((weak));
/**
 * @brief Provide the optional weak callback for pending Serial1 input.
 */
void serialEvent1(void) __attribute__((weak));
/**
 * @brief Provide the optional weak callback for pending Serial2 input.
 */
void serialEvent2(void) __attribute__((weak));

/**
 * @brief Dispatch sketch serial-event hooks for ports with pending input.
 */
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

/**
 * @brief Start USB when selected, then execute setup once and loop with serial-event dispatch.
 */
static void loop_task(void *arg)
{
    (void)arg;
#if defined(USB_VID)
    usb_cdc_port_init(); /* USB interrupt callbacks require the timer task, so start USB after the scheduler. */
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
 * With debug logging disabled, no SDK console is opened and printf output is discarded. The weak
 * SDK _read_r would read a console UART; explicitly fail stdin here so getchar/scanf return EOF.
 * Keep this definition with main to ensure it is linked.
 */
/**
 * @brief Reject unsupported stdin reads with EBADF instead of using an uninitialized SDK
 * console.
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

/**
 * @brief Initialize the Core and C++ runtime, create the sketch task and start the FreeRTOS
 * scheduler.
 */
int main(void)
{
    init();
    cxx_runtime_init(); /* Local-static initialization inside constructors requires the guard lock. */
    __libc_init_array();
    initVariant();

    if (xTaskCreate(loop_task, "loop", ARDUINO_LOOP_STACK_SIZE / sizeof(StackType_t), NULL,
                    ARDUINO_LOOP_PRIORITY, NULL) != pdPASS) {
        abort();
    }
    vTaskStartScheduler();
    abort(); /* The scheduler must not return. */
}

/*
 * Unique firmware entry point. The dependency package owns __start, SystemInit, clocks, Flash,
 * interrupts and heap; it does not start a console. The Core optionally selects uart2 for debug
 * output and follows the Arduino sequence init(), global constructors, initVariant(), then
 * setup()/loop() in its own task. USB starts before setup() when enabled.
 *
 * SYSINIT packages (ARDUINO_BL_SYSINIT_STARTUP, SDK v2.3.39 and later) run board initialization as
 * SYSINIT records, call arduino_bl_core_preinit() before running the constructors, start FreeRTOS,
 * bring up Wi-Fi when it is linked, and call main() from the SYSINIT task. Older packages
 * initialize the board in init() through board_init(); main() then runs the constructors and
 * starts FreeRTOS, and WiFi initializes networking on first use. Each firmware links one path.
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

#if !defined(ARDUINO_BL_SYSINIT_STARTUP)
/**
 * @brief Run the linked C/C++ initialization arrays before starting the sketch task.
 */
extern "C" void __libc_init_array(void);
#endif

/**
 * @brief Initialize the chip before global constructors, which require the heap. Clear the
 * retained download request left by the uploader warm reset after 1200 bps touch.
 */
void init(void)
{
#if !defined(ARDUINO_BL_SYSINIT_STARTUP)
    board_init();
#endif
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
 * @brief Create the setup/loop task; abort when it cannot be created.
 */
static void start_loop_task(void)
{
    if (xTaskCreate(loop_task, "loop", ARDUINO_LOOP_STACK_SIZE / sizeof(StackType_t), NULL,
                    ARDUINO_LOOP_PRIORITY, NULL) != pdPASS) {
        abort();
    }
}

#if defined(ARDUINO_BL_SYSINIT_STARTUP)
/**
 * @brief Package hook run once by SYSINIT after the heap and before global constructors.
 */
extern "C" void arduino_bl_core_preinit(void)
{
    init();
    cxx_runtime_init(); /* Local-static initialization inside constructors requires the guard lock. */
}

/**
 * @brief Called by SYSINIT from its task after constructors, with the scheduler running; return so
 * SYSINIT can deliver its remaining Wi-Fi events and delete its task.
 */
int main(void)
{
    initVariant();
    start_loop_task();
    return 0;
}
#else
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
    start_loop_task();
    vTaskStartScheduler();
    abort(); /* The scheduler must not return. */
}
#endif

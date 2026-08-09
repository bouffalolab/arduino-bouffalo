#include "Arduino.h"

extern "C" {
#include "board.h"
}

extern "C" void __libc_init_array(void);
extern "C" void *__dso_handle = nullptr;

static TaskHandle_t loop_task_handle;

extern "C" void initVariant(void) __attribute__((weak));
extern "C" void initVariant(void)
{
}

void serialEventRun(void) __attribute__((weak));

static void loopTask(void *)
{
    setup();

    for (;;) {
        loop();
        if (serialEventRun) {
            serialEventRun();
        }
        yield();
    }
}

int main(void)
{
    board_init();
    __libc_init_array();
    init();
    initVariant();

    BaseType_t created = xTaskCreate(
        loopTask,
        "loopTask",
        ARDUINO_LOOP_STACK_SIZE,
        nullptr,
        ARDUINO_LOOP_PRIORITY,
        &loop_task_handle);

    if (created != pdPASS) {
        printf("Arduino loop task creation failed\r\n");
        for (;;) {
        }
    }

    vTaskStartScheduler();

    for (;;) {
    }
}

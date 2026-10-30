#include "Arduino.h"

extern "C" {
#include "board.h"
#if defined(BL616CL_CONSTRUCTORS_WRAM_FIRST) && BL616CL_CONSTRUCTORS_WRAM_FIRST
#include "sdk/mm/mm.h"
#endif
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
#if defined(BL616CL_CONSTRUCTORS_WRAM_FIRST) && BL616CL_CONSTRUCTORS_WRAM_FIRST
    // Experimental CPU-only C++ constructor allocation placement. Restore
    // the exact configured order before creating tasks or wireless services.
    uint32_t saved_order[CONFIG_MM_HEAP_COUNT] = {};
    const uint32_t constructor_order[] = {MM_HEAP_WRAM_0, MM_HEAP_OCRAM_0};
    if (mm_heap_get_auto_list(saved_order, CONFIG_MM_HEAP_COUNT) != 0 ||
        mm_heap_set_auto_list(constructor_order, 2U) != 0) {
        printf("Constructor heap order setup failed\r\n");
        for (;;) {}
    }
#endif
    __libc_init_array();
#if defined(BL616CL_CONSTRUCTORS_WRAM_FIRST) && BL616CL_CONSTRUCTORS_WRAM_FIRST
    if (mm_heap_set_auto_list(saved_order, CONFIG_MM_HEAP_COUNT) != 0) {
        printf("Constructor heap order restore failed\r\n");
        for (;;) {}
    }
#endif
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

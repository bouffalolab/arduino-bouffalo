#include "Arduino.h"

extern "C" {
#include "board.h"
}

extern "C" void __libc_init_array(void);
extern "C" void *__dso_handle = nullptr;

/*
 * Bridge-profile FHOST diagnostics are optional for ordinary Arduino
 * applications. Keep the runtime linkable without telemetry; the UNO R4
 * bridge application supplies strong implementations where it consumes them.
 */
extern "C" __attribute__((weak)) void fhost_bridge_ap_start_progress(
    uint8_t stage, int result)
{
    (void)stage;
    (void)result;
}

extern "C" __attribute__((weak)) void fhost_bridge_ap_start_pointer(
    uint8_t location, uint32_t address)
{
    (void)location;
    (void)address;
}

extern "C" __attribute__((weak)) void fhost_bridge_ap_start_channel_counts(
    uint32_t chan2g4_count, uint32_t chan5g_count)
{
    (void)chan2g4_count;
    (void)chan5g_count;
}

extern "C" __attribute__((weak)) void
fhost_bridge_ap_start_heap_before_allocate(uint32_t size)
{
    (void)size;
}

extern "C" __attribute__((weak)) void
fhost_bridge_ap_start_heap_after_allocate(uint32_t address)
{
    (void)address;
}

extern "C" __attribute__((weak)) void fhost_bridge_ap_start_command(
    uint8_t command, uint8_t phase, int fhost_vif_idx, int wpa_state,
    int network_id)
{
    (void)command;
    (void)phase;
    (void)fhost_vif_idx;
    (void)wpa_state;
    (void)network_id;
}

extern "C" __attribute__((weak)) void fhost_bridge_ap_start_sync(
    uint8_t phase, uint32_t semaphore, int result, uint32_t count)
{
    (void)phase;
    (void)semaphore;
    (void)result;
    (void)count;
}

extern "C" __attribute__((weak)) void fhost_bridge_ap_start_trace(
    uint8_t event, uint32_t semaphore, int result, uint32_t count)
{
    (void)event;
    (void)semaphore;
    (void)result;
    (void)count;
}

extern "C" __attribute__((weak)) void fhost_bridge_ap_start_allocator_event(
    uint8_t stage, uint32_t value0, uint32_t value1, uint32_t value2)
{
    (void)stage;
    (void)value0;
    (void)value1;
    (void)value2;
}

extern "C" __attribute__((weak)) void fhost_bridge_ap_start_notification_ab(
    uint8_t phase, uint32_t message, uint32_t waiting_task,
    uint8_t request_matches, uint8_t use_task_notification, int result)
{
    (void)phase;
    (void)message;
    (void)waiting_task;
    (void)request_matches;
    (void)use_task_notification;
    (void)result;
}

extern "C" __attribute__((weak)) void fhost_bridge_ap_start_eloop_post(
    uint8_t phase, uint32_t body, uint32_t body_length, uint8_t event_type,
    uint8_t request_matches, uint32_t request, uint32_t request_word)
{
    (void)phase;
    (void)body;
    (void)body_length;
    (void)event_type;
    (void)request_matches;
    (void)request;
    (void)request_word;
}

extern "C" __attribute__((weak)) void fhost_bridge_ap_start_wpa_wait(
    uint8_t phase, int fhost_vif_idx, uint8_t expected_event,
    uint8_t observed_event, uint32_t target, uint32_t waiting_task,
    uint32_t callback_task, int result, uint8_t wpa_state)
{
    (void)phase;
    (void)fhost_vif_idx;
    (void)expected_event;
    (void)observed_event;
    (void)target;
    (void)waiting_task;
    (void)callback_task;
    (void)result;
    (void)wpa_state;
}

extern "C" __attribute__((weak)) void fhost_bridge_ap_start_wpa_event_path(
    uint8_t phase, int fhost_vif_idx, uint8_t event, uint8_t fhost_wpa_state,
    int operstate, int old_wpa_state, int new_wpa_state, int result,
    uint8_t callback_slot)
{
    (void)phase;
    (void)fhost_vif_idx;
    (void)event;
    (void)fhost_wpa_state;
    (void)operstate;
    (void)old_wpa_state;
    (void)new_wpa_state;
    (void)result;
    (void)callback_slot;
}

extern "C" __attribute__((weak)) void fhost_bridge_ap_start_create_ap(
    uint8_t phase, int result, uint8_t wpa_state, uint32_t detail)
{
    (void)phase;
    (void)result;
    (void)wpa_state;
    (void)detail;
}

extern "C" __attribute__((weak)) void fhost_bridge_ap_start_hostapd_lifecycle(
    uint8_t phase, int result, uint8_t iface_state,
    uint8_t wait_channel_update, uint32_t detail0, uint32_t detail1)
{
    (void)phase;
    (void)result;
    (void)iface_state;
    (void)wait_channel_update;
    (void)detail0;
    (void)detail1;
}

extern "C" __attribute__((weak)) void fhost_bridge_ap_start_status_code(
    uint8_t phase, int fhost_vif_idx, uint8_t message_kind,
    uint16_t message_length, uint8_t raw_state, uint8_t vif_state,
    int result)
{
    (void)phase;
    (void)fhost_vif_idx;
    (void)message_kind;
    (void)message_length;
    (void)raw_state;
    (void)vif_state;
    (void)result;
}

extern "C" __attribute__((weak)) void
fhost_bridge_ap_start_hostapd_state_update(
    uint8_t hostapd_state, uint8_t enabled, uint8_t raw_state_before,
    uint8_t vif_state_before, uint8_t raw_state_after,
    uint8_t vif_state_after)
{
    (void)hostapd_state;
    (void)enabled;
    (void)raw_state_before;
    (void)vif_state_before;
    (void)raw_state_after;
    (void)vif_state_after;
}

extern "C" __attribute__((weak)) void fhost_ap_state_update_from_hostapd(
    uint8_t hostapd_state, bool enabled)
{
    (void)hostapd_state;
    (void)enabled;
}

extern "C" __attribute__((weak)) void
fhost_bridge_ap_start_allocator_heap_walk(void)
{
}

extern "C" __attribute__((weak)) void fhost_bridge_ap_start_liveness(
    uint8_t phase)
{
    (void)phase;
}

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

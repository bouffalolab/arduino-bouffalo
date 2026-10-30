#ifndef UNOR4_BL616CL_WIFI_AP_START_DIAGNOSTIC_H
#define UNOR4_BL616CL_WIFI_AP_START_DIAGNOSTIC_H

#include <stdint.h>

class HardwareSerial;

namespace bridge {

enum class WifiApStartDiagnosticStage : uint8_t {
    Idle = 0U,
    Preparing = 1U,
    NativeCall = 2U,
    Completed = 3U,
};

struct WifiApStartDiagnostic {
    WifiApStartDiagnosticStage stage = WifiApStartDiagnosticStage::Idle;
    int8_t native_result = 0;
    uint8_t sta_vif_state = 0U;
    uint8_t ap_vif_state = 0U;
    uint8_t sta_connected = 0U;
    uint8_t ap_started = 0U;
    uint8_t requested_channel = 0U;
    int8_t sta_channel_result = -1;
    uint8_t sta_channel = 0U;
    uint8_t fhost_stage = 0U;
    int8_t fhost_result = 0;
    uint32_t attempts = 0U;
    uint32_t elapsed_ms = 0U;
    uint32_t started_at_ms = 0U;
    uint32_t completed_at_ms = 0U;
    uint32_t fhost_chan_address = 0U;
    uint32_t fhost_handler_chan_address = 0U;
    uint32_t fhost_response_source_chan_address = 0U;
    uint32_t fhost_response_copy_chan_address = 0U;
    uint32_t fhost_response_received_chan_address = 0U;
    uint32_t wpa_feature_chan_address = 0U;
    uint32_t fhost_chan2g4_count = 0U;
    uint32_t fhost_chan5g_count = 0U;
    uint8_t heap_probe_stage = 0U;
    uint32_t heap_request_size = 0U;
    uint32_t heap_total_before = 0U;
    uint32_t heap_ocram_before = 0U;
    uint32_t heap_wram_before = 0U;
    uint32_t heap_allocation_address = 0U;
    uint32_t heap_total_after = 0U;
    uint32_t heap_ocram_after = 0U;
    uint32_t heap_wram_after = 0U;
    uint8_t heap_malloc_failed = 0U;
    uint8_t fhost_command = 0U;
    uint8_t fhost_command_phase = 0U;
    int8_t fhost_command_vif = -1;
    uint8_t fhost_command_wpa_state = 0xffU;
    int8_t fhost_command_network_id = -1;
    uint8_t fhost_command_sequence = 0U;
    uint8_t fhost_sync_phase = 0U;
    int8_t fhost_sync_wait_result = 0;
    int8_t fhost_sync_signal_result = 0;
    uint8_t fhost_sync_wait_count_before = 0U;
    uint8_t fhost_sync_callback_count_before = 0U;
    uint8_t fhost_sync_callback_count_after = 0U;
    uint8_t fhost_sync_wait_count_after = 0U;
    uint32_t fhost_sync_wait_handle = 0U;
    uint32_t fhost_sync_callback_handle = 0U;
    uint32_t fhost_sync_update_count = 0U;
    uint32_t fhost_task_handle = 0U;
    uint8_t fhost_task_state = 0U;
    uint32_t fhost_task_stack_high_watermark = 0U;
    uint8_t liveness_phase = 0U;
    uint8_t liveness_event_count = 0U;
    bool persisted = false;
};

enum class WifiApStartTraceEvent : uint8_t {
    WaitBeforeTake = 1U,
    CallbackBeforeGive = 2U,
    CallbackGiveReturn = 3U,
    WaitTakeReturn = 4U,
    NotificationBefore = 5U,
    NotificationReturn = 6U,
    NotificationWaitBefore = 7U,
    NotificationWaitReturn = 8U,
};

struct WifiApStartTraceRecord {
    uint32_t sequence = 0U;
    WifiApStartTraceEvent event = WifiApStartTraceEvent::WaitBeforeTake;
    int8_t result = 0;
    uint32_t semaphore = 0U;
    uint32_t count = 0U;
};

struct WifiApStartTrace {
    uint32_t next_sequence = 0U;
    uint8_t record_count = 0U;
    WifiApStartTraceRecord records[3] = {};
    bool persisted = false;
};

struct WifiApStartNotificationDiagnostic {
    uint32_t attempt_count = 0U;
    uint32_t last_sequence = 0U;
    uint32_t waiting_task = 0U;
    uint8_t event_mask = 0U;
    WifiApStartTraceEvent last_event =
        WifiApStartTraceEvent::NotificationBefore;
    int8_t notification_result = 0;
    int8_t wait_result = 0;
    bool persisted = false;
};

struct WifiApStartNotificationAbDiagnostic {
    uint8_t phase_mask = 0U;
    uint8_t last_phase = 0U;
    uint8_t request_matches = 0U;
    uint8_t setup_use_task_notification = 0U;
    uint8_t queued_use_task_notification = 0U;
    uint8_t callback_use_task_notification = 0U;
    uint8_t wait_use_task_notification = 0U;
    int8_t last_result = 0;
    uint32_t update_count = 0U;
    uint32_t setup_message = 0U;
    uint32_t queued_message = 0U;
    uint32_t callback_message = 0U;
    uint32_t wait_message = 0U;
    uint32_t setup_waiting_task = 0U;
    uint32_t queued_waiting_task = 0U;
    uint32_t callback_waiting_task = 0U;
    uint32_t wait_waiting_task = 0U;
    bool persisted = false;
};

struct WifiApStartWpaWaitDiagnostic {
    uint16_t phase_mask = 0U;
    uint8_t last_phase = 0U;
    int8_t last_result = 0;
    int8_t vif = -1;
    uint8_t expected_event = 0xffU;
    uint8_t observed_event = 0xffU;
    uint8_t wpa_state = 0xffU;
    uint32_t update_count = 0U;
    uint32_t target = 0U;
    uint32_t waiting_task = 0U;
    uint32_t callback_task = 0U;
    bool persisted = false;
};

struct WifiApStartWpaEventPathDiagnostic {
    uint32_t phase_mask = 0U;
    uint8_t last_phase = 0U;
    int8_t vif = -1;
    uint8_t event = 0xffU;
    uint8_t fhost_wpa_state = 0xffU;
    int8_t operstate = -1;
    int8_t old_wpa_state = -1;
    int8_t new_wpa_state = -1;
    int8_t last_result = 0;
    uint8_t callback_slot = 0xffU;
    uint32_t update_count = 0U;
    uint32_t callback_slot_mask = 0U;
    bool persisted = false;
};

struct WifiApStartCreateApDiagnostic {
    uint16_t phase_mask = 0U;
    uint8_t last_phase = 0U;
    int8_t last_result = 0;
    uint8_t wpa_state = 0xffU;
    uint32_t detail = 0U;
    uint32_t update_count = 0U;
    bool persisted = false;
};

struct WifiApStartHostapdLifecycleDiagnostic {
    uint32_t phase_mask = 0U;
    uint8_t last_phase = 0U;
    int8_t last_result = 0;
    uint8_t iface_state = 0U;
    uint8_t wait_channel_update = 0U;
    uint32_t detail0 = 0U;
    uint32_t detail1 = 0U;
    uint32_t disable_network_caller = 0U;
    uint32_t ap_stop_caller = 0U;
    uint32_t bridge_stop_softap_caller = 0U;
    int16_t disable_network_id = -1;
    uint16_t deauthenticate_reason = 0U;
    uint32_t update_count = 0U;
    uint8_t record_count = 0U;
    struct Record {
        uint8_t phase = 0U;
        int8_t result = 0;
        uint8_t iface_state = 0U;
        uint8_t wait_channel_update = 0U;
        uint32_t detail0 = 0U;
    } records[3] = {};
    bool persisted = false;
};

struct WifiApStartHostapdStateUpdateRecord {
    uint32_t sequence = 0U;
    uint8_t hostapd_state = 0U;
    uint8_t enabled = 0U;
    uint8_t raw_state_before = 0U;
    uint8_t vif_state_before = 0U;
    uint8_t raw_state_after = 0U;
    uint8_t vif_state_after = 0U;
};

struct WifiApStartHostapdStateUpdateDiagnostic {
    uint32_t next_sequence = 0U;
    uint8_t record_count = 0U;
    WifiApStartHostapdStateUpdateRecord records[3] = {};
    bool persisted = false;
};

struct WifiApStartStatusCodeDiagnostic {
    uint16_t phase_mask = 0U;
    uint8_t last_phase = 0U;
    int8_t vif = -1;
    uint8_t message_kind = 0U;
    uint16_t message_length = 0U;
    uint8_t raw_state = 0xffU;
    uint8_t vif_state = 0xffU;
    int8_t last_result = 0;
    uint32_t update_count = 0U;
    bool persisted = false;
};

struct WifiApStartEloopPostDiagnostic {
    uint8_t last_phase = 0U;
    uint8_t caller_event_type = 0U;
    uint8_t callee_event_type = 0U;
    uint8_t caller_request_matches = 0U;
    uint8_t callee_ifname_prefix_matches = 0U;
    uint8_t callee_enable_network_matches = 0U;
    uint32_t caller_body = 0U;
    uint32_t caller_body_length = 0U;
    uint32_t callee_body = 0U;
    uint32_t callee_body_length = 0U;
    uint32_t callee_request = 0U;
    uint32_t callee_request_word = 0U;
    uint32_t update_count = 0U;
    bool persisted = false;
};

struct WifiApStartAllocatorTraceRecord {
    uint32_t sequence = 0U;
    uint8_t stage = 0U;
    uint32_t value0 = 0U;
    uint32_t value1 = 0U;
    uint32_t value2 = 0U;
};

struct WifiApStartAllocatorTrace {
    uint8_t fhost_stage = 0U;
    uint32_t next_sequence = 0U;
    uint8_t record_count = 0U;
    WifiApStartAllocatorTraceRecord records[3] = {};
    bool persisted = false;
};

struct WifiApStartAllocatorHeapWalkRecord {
    uint8_t heap_id = 0U;
    uint16_t allocated_block_count = 0U;
    uint32_t allocated_bytes = 0U;
    uint32_t largest_blocks[3] = {};
};

struct WifiApStartAllocatorHeapWalk {
    int8_t result = -1;
    uint8_t record_count = 0U;
    WifiApStartAllocatorHeapWalkRecord records[2] = {};
    bool persisted = false;
};

enum class WifiApStartWatchdogState : uint8_t {
    Disabled = 0U,
    Armed = 1U,
    Disarmed = 2U,
    ResetObserved = 3U,
    RecoveryComplete = 4U,
};

struct WifiApStartWatchdogDiagnostic {
    WifiApStartWatchdogState state = WifiApStartWatchdogState::Disabled;
    uint8_t reset_reason = 0U;
    uint16_t timeout_ms = 0U;
    uint32_t arm_count = 0U;
    bool enabled = false;
    bool persisted = false;
};

struct WifiApStartUartSnapshot {
    bool initialized = false;
    uint8_t rx_fifo_count = 0U;
    uint16_t interrupt_status = 0U;
    uint16_t interrupt_mask = 0U;
    uint32_t rx_config = 0U;
    uint32_t bit_period = 0U;
    uint32_t rx_received_count = 0U;
    uint32_t rx_overflow_count = 0U;
};

struct WifiApStartUartRecoveryDiagnostic {
    WifiApStartUartSnapshot user_uart_before = {};
    WifiApStartUartSnapshot at_uart_before = {};
    WifiApStartUartSnapshot user_uart_after = {};
    WifiApStartUartSnapshot at_uart_after = {};
    uint32_t cdc_baud_before = 0U;
    uint32_t cdc_baud_after = 0U;
    uint8_t reset_reason = 0U;
    bool before_valid = false;
    bool after_valid = false;
    bool persisted = false;
};

/*
 * This is a diagnostic-only recovery mechanism. It is a no-op unless the
 * bridge is compiled with BL616CL_AP_START_DIAGNOSTIC_WATCHDOG=1.
 */
void wifi_ap_start_watchdog_bootstrap();
void wifi_ap_start_watchdog_arm();
void wifi_ap_start_watchdog_disarm();
bool wifi_ap_start_watchdog_recovery_required();
void wifi_ap_start_watchdog_recovery_complete();
void wifi_ap_start_capture_uart_before_watchdog(
    const HardwareSerial &user_uart,
    const HardwareSerial &at_uart,
    uint32_t cdc_baud);
void wifi_ap_start_capture_uart_after_watchdog_boot(
    const HardwareSerial &user_uart,
    const HardwareSerial &at_uart,
    uint32_t cdc_baud);

void wifi_ap_start_diagnostic_begin(uint8_t requested_channel);
void wifi_ap_start_diagnostic_mark_native_call();
void wifi_ap_start_diagnostic_complete(int native_result);
void wifi_ap_start_diagnostic_record_bridge_stop_softap_caller(
    uint32_t caller);
WifiApStartDiagnostic wifi_ap_start_diagnostic_snapshot();
WifiApStartTrace wifi_ap_start_trace_snapshot();
WifiApStartNotificationDiagnostic wifi_ap_start_notification_snapshot();
WifiApStartNotificationAbDiagnostic wifi_ap_start_notification_ab_snapshot();
WifiApStartWpaWaitDiagnostic wifi_ap_start_wpa_wait_snapshot();
WifiApStartWpaEventPathDiagnostic wifi_ap_start_wpa_event_path_snapshot();
WifiApStartCreateApDiagnostic wifi_ap_start_create_ap_snapshot();
WifiApStartHostapdLifecycleDiagnostic
wifi_ap_start_hostapd_lifecycle_snapshot();
WifiApStartHostapdStateUpdateDiagnostic
wifi_ap_start_hostapd_state_update_snapshot();
WifiApStartStatusCodeDiagnostic wifi_ap_start_status_code_snapshot();
WifiApStartEloopPostDiagnostic wifi_ap_start_eloop_post_snapshot();
WifiApStartAllocatorTrace wifi_ap_start_allocator_trace_snapshot();
WifiApStartAllocatorHeapWalk wifi_ap_start_allocator_heap_walk_snapshot();
WifiApStartWatchdogDiagnostic wifi_ap_start_watchdog_snapshot();
WifiApStartUartRecoveryDiagnostic wifi_ap_start_uart_recovery_snapshot();
WifiApStartUartSnapshot wifi_ap_start_uart_live_snapshot(
    const HardwareSerial &serial);

} // namespace bridge

extern "C" void fhost_bridge_ap_start_progress(uint8_t stage, int result);
extern "C" void fhost_bridge_ap_start_liveness(uint8_t phase);
extern "C" void fhost_bridge_ap_start_pointer(uint8_t location,
                                                uint32_t address);
extern "C" void fhost_bridge_ap_start_channel_counts(uint32_t chan2g4_count,
                                                       uint32_t chan5g_count);
extern "C" void fhost_bridge_ap_start_heap_before_allocate(uint32_t size);
extern "C" void fhost_bridge_ap_start_heap_after_allocate(uint32_t address);
extern "C" void fhost_bridge_ap_start_command(
    uint8_t command,
    uint8_t phase,
    int fhost_vif_idx,
    int wpa_state,
    int network_id);
extern "C" void fhost_bridge_ap_start_sync(
    uint8_t phase,
    uint32_t semaphore,
    int result,
    uint32_t count);
extern "C" void fhost_bridge_ap_start_trace(
    uint8_t event,
    uint32_t semaphore,
    int result,
    uint32_t count);
extern "C" void fhost_bridge_ap_start_notification_ab(
    uint8_t phase,
    uint32_t message,
    uint32_t waiting_task,
    uint8_t request_matches,
    uint8_t use_task_notification,
    int result);
extern "C" void fhost_bridge_ap_start_wpa_wait(
    uint8_t phase,
    int fhost_vif_idx,
    uint8_t expected_event,
    uint8_t observed_event,
    uint32_t target,
    uint32_t waiting_task,
    uint32_t callback_task,
    int result,
    uint8_t wpa_state);
extern "C" void fhost_bridge_ap_start_wpa_event_path(
    uint8_t phase,
    int fhost_vif_idx,
    uint8_t event,
    uint8_t fhost_wpa_state,
    int operstate,
    int old_wpa_state,
    int new_wpa_state,
    int result,
    uint8_t callback_slot);
extern "C" void fhost_bridge_ap_start_create_ap(
    uint8_t phase,
    int result,
    uint8_t wpa_state,
    uint32_t detail);
extern "C" void fhost_bridge_ap_start_hostapd_lifecycle(
    uint8_t phase,
    int result,
    uint8_t iface_state,
    uint8_t wait_channel_update,
    uint32_t detail0,
    uint32_t detail1);
extern "C" void fhost_bridge_ap_start_hostapd_state_update(
    uint8_t hostapd_state,
    uint8_t enabled,
    uint8_t raw_state_before,
    uint8_t vif_state_before,
    uint8_t raw_state_after,
    uint8_t vif_state_after);
extern "C" void fhost_bridge_ap_start_status_code(
    uint8_t phase,
    int fhost_vif_idx,
    uint8_t message_kind,
    uint16_t message_length,
    uint8_t raw_state,
    uint8_t vif_state,
    int result);
extern "C" void fhost_bridge_ap_start_eloop_post(
    uint8_t phase,
    uint32_t body,
    uint32_t body_length,
    uint8_t event_type,
    uint8_t request_matches,
    uint32_t request,
    uint32_t request_word);
extern "C" void fhost_bridge_ap_start_allocator_event(
    uint8_t stage,
    uint32_t value0,
    uint32_t value1,
    uint32_t value2);
extern "C" int fhost_bridge_ap_start_allocator_trace_active(void);
extern "C" void fhost_bridge_ap_start_allocator_heap_walk(void);

#endif

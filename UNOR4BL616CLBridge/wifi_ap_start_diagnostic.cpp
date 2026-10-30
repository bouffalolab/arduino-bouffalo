#include "wifi_ap_start_diagnostic.h"
#include "runtime_allocation_diagnostic.h"

#include "Arduino.h"

#include <FreeRTOS.h>
#include <task.h>

#include "bflb_core.h"
#include "bflb_uart.h"
#include "hardware/uart_reg.h"
#include "sdk/mm/mm.h"

extern "C" {
#include "wifi_mgmr_ext.h"
}

#if defined(BL616CL_AP_START_DIAGNOSTIC_WATCHDOG) && \
    BL616CL_AP_START_DIAGNOSTIC_WATCHDOG
extern "C" {
#include "bflb_name.h"
#include "bflb_wdg.h"
#include "bl616cl_sys.h"
}
#endif

#include <string.h>

namespace bridge {
namespace {

#if defined(BL616CL_RUNTIME_ALLOCATION_DIAGNOSTIC) && BL616CL_RUNTIME_ALLOCATION_DIAGNOSTIC
static_assert(MM_HEAP_OCRAM_0 == 2 && MM_HEAP_WRAM_0 == 3, "heap report ABI");
RuntimeAllocationDiagnostic runtime_allocations;
#endif

constexpr uint32_t kDiagnosticMagic = 0x41505354U; // "APST"
constexpr uint16_t kDiagnosticVersion = 13U;
constexpr uint32_t kTraceMagic = 0x54524345U; // "TRCE"
constexpr uint16_t kTraceVersion = 1U;
constexpr uint8_t kTraceCapacity = 8U;
constexpr uint32_t kNotificationMagic = 0x4E544659U; // "NTFY"
constexpr uint16_t kNotificationVersion = 1U;
constexpr uint32_t kNotificationAbMagic = 0x4E544142U; // "NTAB"
constexpr uint16_t kNotificationAbVersion = 1U;
constexpr uint32_t kWpaWaitMagic = 0x57504157U; // "WPAW"
constexpr uint16_t kWpaWaitVersion = 1U;
constexpr uint32_t kWpaEventPathMagic = 0x57504550U; // "WPEP"
constexpr uint16_t kWpaEventPathVersion = 1U;
constexpr uint32_t kEloopPostMagic = 0x454C5054U; // "ELPT"
constexpr uint16_t kEloopPostVersion = 1U;
constexpr uint32_t kCreateApMagic = 0x41504341U; // "APCA"
constexpr uint16_t kCreateApVersion = 2U;
constexpr uint32_t kHostapdLifecycleMagic = 0x4841504CU; // "HAPL"
constexpr uint16_t kHostapdLifecycleVersion = 6U;
constexpr uint8_t kHostapdLifecycleCapacity = 3U;
constexpr uint32_t kHostapdStateUpdateMagic = 0x48535550U; // "HSUP"
constexpr uint16_t kHostapdStateUpdateVersion = 1U;
constexpr uint8_t kHostapdStateUpdateCapacity = 3U;
constexpr uint32_t kStatusCodeMagic = 0x53544344U; // "STCD"
constexpr uint16_t kStatusCodeVersion = 1U;
constexpr uint32_t kAllocatorTraceMagic = 0x414C4C43U; // "ALLC"
constexpr uint16_t kAllocatorTraceVersion = 1U;
constexpr uint8_t kAllocatorTraceCapacity = 16U;
constexpr uint32_t kAllocatorHeapWalkMagic = 0x414C4857U; // "ALHW"
constexpr uint16_t kAllocatorHeapWalkVersion = 1U;
constexpr uint8_t kAllocatorHeapWalkRecordCount = 2U;
constexpr uint8_t kAllocatorHeapWalkLargestCount = 3U;
constexpr uint16_t kTraceStackGuardBytes = 192U;
constexpr uint32_t kWatchdogMagic = 0x41505744U; // "APWD"
constexpr uint16_t kWatchdogVersion = 1U;
constexpr uint32_t kUartRecoveryMagic = 0x55524154U; // "URAT"
constexpr uint16_t kUartRecoveryVersion = 1U;

#if defined(BL616CL_AP_START_DIAGNOSTIC_WATCHDOG) && \
    BL616CL_AP_START_DIAGNOSTIC_WATCHDOG
#if defined(BL616CL_AP_START_DIAGNOSTIC_WATCHDOG_TIMEOUT_MS)
constexpr uint16_t kWatchdogTimeoutMs =
    BL616CL_AP_START_DIAGNOSTIC_WATCHDOG_TIMEOUT_MS;
#else
constexpr uint16_t kWatchdogTimeoutMs = 4000U;
#endif
static_assert(kWatchdogTimeoutMs != 0U,
              "AP-start diagnostic watchdog timeout must be positive");
#else
constexpr uint16_t kWatchdogTimeoutMs = 0U;
#endif

struct WifiApStartPersistent {
    uint32_t magic;
    uint16_t version;
    uint8_t liveness_phase;
    uint8_t liveness_event_count;
    uint8_t stage;
    int8_t native_result;
    uint8_t sta_vif_state;
    uint8_t ap_vif_state;
    uint8_t sta_connected;
    uint8_t ap_started;
    uint8_t requested_channel;
    int8_t sta_channel_result;
    uint8_t sta_channel;
    uint8_t fhost_stage;
    int8_t fhost_result;
    uint32_t attempts;
    uint32_t started_at_ms;
    uint32_t completed_at_ms;
    uint32_t fhost_chan_address;
    uint32_t fhost_handler_chan_address;
    uint32_t fhost_response_source_chan_address;
    uint32_t fhost_response_copy_chan_address;
    uint32_t fhost_response_received_chan_address;
    uint32_t wpa_feature_chan_address;
    uint32_t fhost_chan2g4_count;
    uint32_t fhost_chan5g_count;
    uint8_t heap_probe_stage;
    uint8_t reserved2[3];
    uint32_t heap_request_size;
    uint32_t heap_total_before;
    uint32_t heap_ocram_before;
    uint32_t heap_wram_before;
    uint32_t heap_allocation_address;
    uint32_t heap_total_after;
    uint32_t heap_ocram_after;
    uint32_t heap_wram_after;
    uint8_t heap_malloc_failed;
    uint8_t fhost_command;
    uint8_t fhost_command_phase;
    int8_t fhost_command_vif;
    uint8_t fhost_command_wpa_state;
    int8_t fhost_command_network_id;
    uint8_t fhost_command_sequence;
    uint8_t fhost_sync_phase;
    int8_t fhost_sync_wait_result;
    int8_t fhost_sync_signal_result;
    uint8_t fhost_sync_wait_count_before;
    uint8_t fhost_sync_callback_count_before;
    uint8_t fhost_sync_callback_count_after;
    uint8_t fhost_sync_wait_count_after;
    uint32_t fhost_sync_wait_handle;
    uint32_t fhost_sync_callback_handle;
    uint32_t fhost_sync_update_count;
    uint32_t fhost_task_handle;
    uint8_t fhost_task_state;
    uint8_t reserved3[3];
    uint32_t fhost_task_stack_high_watermark;
};

struct WifiApStartTracePersistentRecord {
    uint32_t sequence;
    uint8_t event;
    int8_t result;
    uint16_t reserved;
    uint32_t semaphore;
    uint32_t count;
};

struct WifiApStartTracePersistent {
    uint32_t magic;
    uint16_t version;
    uint8_t write_index;
    uint8_t record_count;
    uint32_t next_sequence;
    WifiApStartTracePersistentRecord records[kTraceCapacity];
};

struct WifiApStartNotificationPersistent {
    uint32_t magic;
    uint16_t version;
    uint8_t event_mask;
    uint8_t last_event;
    uint32_t attempt_count;
    uint32_t last_sequence;
    uint32_t waiting_task;
    int8_t notification_result;
    int8_t wait_result;
    uint16_t reserved;
};

struct WifiApStartNotificationAbPersistent {
    uint32_t magic;
    uint16_t version;
    uint8_t phase_mask;
    uint8_t last_phase;
    uint8_t request_matches;
    uint8_t setup_use_task_notification;
    uint8_t queued_use_task_notification;
    uint8_t callback_use_task_notification;
    uint8_t wait_use_task_notification;
    int8_t last_result;
    uint16_t reserved;
    uint32_t update_count;
    uint32_t setup_message;
    uint32_t queued_message;
    uint32_t callback_message;
    uint32_t wait_message;
    uint32_t setup_waiting_task;
    uint32_t queued_waiting_task;
    uint32_t callback_waiting_task;
    uint32_t wait_waiting_task;
};

struct WifiApStartWpaWaitPersistent {
    uint32_t magic;
    uint16_t version;
    uint16_t phase_mask;
    uint8_t last_phase;
    int8_t last_result;
    int8_t vif;
    uint8_t expected_event;
    uint8_t observed_event;
    uint8_t wpa_state;
    uint8_t reserved[3];
    uint32_t update_count;
    uint32_t target;
    uint32_t waiting_task;
    uint32_t callback_task;
};

struct WifiApStartWpaEventPathPersistent {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t phase_mask;
    uint8_t last_phase;
    int8_t vif;
    uint8_t event;
    uint8_t fhost_wpa_state;
    int8_t operstate;
    int8_t old_wpa_state;
    int8_t new_wpa_state;
    int8_t last_result;
    uint8_t callback_slot;
    uint8_t reserved2[3];
    uint32_t update_count;
    uint32_t callback_slot_mask;
};

struct WifiApStartCreateApPersistent {
    uint32_t magic;
    uint16_t version;
    uint16_t phase_mask;
    uint8_t last_phase;
    int8_t last_result;
    uint8_t wpa_state;
    uint8_t reserved;
    uint32_t detail;
    uint32_t update_count;
};

struct __attribute__((packed)) WifiApStartHostapdLifecyclePersistentRecord {
    uint8_t phase;
    int8_t result;
    uint8_t iface_state;
    uint8_t wait_channel_update;
    uint32_t detail0;
};

struct WifiApStartHostapdLifecyclePersistent {
    uint32_t magic;
    uint16_t version;
    uint8_t write_index;
    uint8_t record_count;
    uint32_t phase_mask;
    uint8_t last_phase;
    int8_t last_result;
    uint8_t iface_state;
    uint8_t wait_channel_update;
    uint32_t detail0;
    uint32_t detail1;
    uint32_t disable_network_caller;
    uint32_t ap_stop_caller;
    uint32_t bridge_stop_softap_caller;
    int16_t disable_network_id;
    uint16_t deauthenticate_reason;
    uint32_t update_count;
    WifiApStartHostapdLifecyclePersistentRecord
        records[kHostapdLifecycleCapacity];
};

struct __attribute__((packed)) WifiApStartHostapdStateUpdatePersistentRecord {
    uint32_t sequence;
    uint8_t hostapd_state;
    uint8_t enabled;
    uint8_t raw_state_before;
    uint8_t vif_state_before;
    uint8_t raw_state_after;
    uint8_t vif_state_after;
};

struct WifiApStartHostapdStateUpdatePersistent {
    uint32_t magic;
    uint16_t version;
    uint8_t write_index;
    uint8_t record_count;
    uint32_t next_sequence;
    WifiApStartHostapdStateUpdatePersistentRecord
        records[kHostapdStateUpdateCapacity];
};

struct WifiApStartStatusCodePersistent {
    uint32_t magic;
    uint16_t version;
    uint16_t phase_mask;
    uint8_t last_phase;
    int8_t vif;
    uint8_t message_kind;
    uint16_t message_length;
    uint8_t raw_state;
    uint8_t vif_state;
    int8_t last_result;
    uint8_t reserved;
    uint32_t update_count;
};

struct WifiApStartEloopPostPersistent {
    uint32_t magic;
    uint16_t version;
    uint8_t last_phase;
    uint8_t caller_event_type;
    uint8_t callee_event_type;
    uint8_t caller_request_matches;
    uint8_t callee_ifname_prefix_matches;
    uint8_t callee_enable_network_matches;
    uint8_t reserved[3];
    uint32_t caller_body;
    uint32_t caller_body_length;
    uint32_t callee_body;
    uint32_t callee_body_length;
    uint32_t callee_request;
    uint32_t callee_request_word;
    uint32_t update_count;
};

struct WifiApStartAllocatorTracePersistentRecord {
    uint32_t sequence;
    uint8_t stage;
    uint8_t reserved[3];
    uint32_t value0;
    uint32_t value1;
    uint32_t value2;
};

struct WifiApStartAllocatorTracePersistent {
    uint32_t magic;
    uint16_t version;
    uint8_t write_index;
    uint8_t record_count;
    uint8_t fhost_stage;
    uint8_t reserved[3];
    uint32_t next_sequence;
    WifiApStartAllocatorTracePersistentRecord records[kAllocatorTraceCapacity];
};

struct WifiApStartAllocatorHeapWalkPersistentRecord {
    uint8_t heap_id;
    uint8_t reserved;
    uint16_t allocated_block_count;
    uint32_t allocated_bytes;
    uint32_t largest_blocks[kAllocatorHeapWalkLargestCount];
};

struct WifiApStartAllocatorHeapWalkPersistent {
    uint32_t magic;
    uint16_t version;
    int8_t result;
    uint8_t record_count;
    WifiApStartAllocatorHeapWalkPersistentRecord
        records[kAllocatorHeapWalkRecordCount];
};

struct WifiApStartWatchdogPersistent {
    uint32_t magic;
    uint16_t version;
    uint8_t state;
    uint8_t reset_reason;
    uint16_t timeout_ms;
    uint16_t reserved;
    uint32_t arm_count;
};

struct WifiApStartUartSnapshotPersistent {
    uint8_t initialized;
    uint8_t rx_fifo_count;
    uint16_t interrupt_status;
    uint16_t interrupt_mask;
    uint16_t reserved;
    uint32_t rx_config;
    uint32_t bit_period;
    uint32_t rx_received_count;
    uint32_t rx_overflow_count;
};

struct WifiApStartUartRecoveryPersistent {
    uint32_t magic;
    uint16_t version;
    uint8_t before_valid;
    uint8_t after_valid;
    uint8_t reset_reason;
    uint8_t reserved[3];
    uint32_t cdc_baud_before;
    uint32_t cdc_baud_after;
    WifiApStartUartSnapshotPersistent user_uart_before;
    WifiApStartUartSnapshotPersistent at_uart_before;
    WifiApStartUartSnapshotPersistent user_uart_after;
    WifiApStartUartSnapshotPersistent at_uart_after;
};

/*
 * This trace is intentionally independent of persistent_diagnostic. AP start
 * reinitializes that aggregate before calling FHOST, while this ring preserves
 * the actual order of wait/give events across a reset.
 */
__attribute__((section(".noinit_data.trace"), used))
WifiApStartTracePersistent persistent_trace;

__attribute__((section(".noinit_data.trace"), used))
WifiApStartAllocatorTracePersistent persistent_allocator_trace;

__attribute__((section(".noinit_data.trace"), used))
WifiApStartAllocatorHeapWalkPersistent persistent_allocator_heap_walk;

/*
 * The current runtime linker starts .noinit_data at __StackTop. Interrupt
 * entry has been observed to disturb that boundary after a CKLink reset, so
 * retain a 192-byte guard before the stack-adjacent trace data.
 */
__attribute__((section(".noinit_data.diagnostic"), used))
uint8_t persistent_trace_stack_guard[kTraceStackGuardBytes];

/*
 * These records must survive the diagnostic watchdog reset. Ordinary OCRAM
 * .noinit is not cleared by start_load(), but it did not retain all records
 * reliably on this target. The bridge linker reserves the HBN RAM window below
 * the SDK-owned IOT2LP area specifically for this concise state.
 */
__attribute__((section(".hbn_noinit.diagnostic"), used))
WifiApStartPersistent persistent_diagnostic;

/*
 * The generic trace ring includes unrelated synchronous ELOOP requests after
 * ENABLE_NETWORK. Keep this concise outcome separately so the notification
 * sequence remains readable after the AP-start watchdog reset.
 */
__attribute__((section(".hbn_noinit.diagnostic"), used))
WifiApStartNotificationPersistent persistent_notification;

__attribute__((section(".hbn_noinit.diagnostic"), used))
WifiApStartNotificationAbPersistent persistent_notification_ab;

__attribute__((section(".hbn_noinit.diagnostic"), used))
WifiApStartWpaWaitPersistent persistent_wpa_wait;

__attribute__((section(".hbn_noinit.diagnostic"), used))
WifiApStartWpaEventPathPersistent persistent_wpa_event_path;

/*
 * These summaries are consumed during the live AP+STA attempt only. Keep
 * scarce HBN retention RAM for the state-update ring that must also explain a
 * watchdog reset.
 */
__attribute__((section(".noinit_data.diagnostic"), used))
WifiApStartCreateApPersistent persistent_create_ap;

__attribute__((section(".noinit_data.diagnostic"), used))
WifiApStartHostapdLifecyclePersistent persistent_hostapd_lifecycle;

__attribute__((section(".hbn_noinit.diagnostic"), used))
WifiApStartHostapdStateUpdatePersistent persistent_hostapd_state_update;

/*
 * Status-code telemetry is consumed while the current AP-start attempt is
 * live. Keep it out of the fixed-size HBN retention area, which is reserved
 * for watchdog-reset diagnostics.
 */
__attribute__((section(".noinit_data.diagnostic"), used))
WifiApStartStatusCodePersistent persistent_status_code;

__attribute__((section(".hbn_noinit.diagnostic"), used))
WifiApStartEloopPostPersistent persistent_eloop_post;

__attribute__((section(".hbn_noinit.diagnostic"), used))
WifiApStartWatchdogPersistent persistent_watchdog;

__attribute__((section(".hbn_noinit.diagnostic"), used))
WifiApStartUartRecoveryPersistent persistent_uart_recovery;

volatile WifiApStartDiagnostic diagnostic = {};

bool persistent_is_valid()
{
    return persistent_diagnostic.magic == kDiagnosticMagic &&
        persistent_diagnostic.version == kDiagnosticVersion;
}

bool trace_is_valid()
{
    return persistent_trace.magic == kTraceMagic &&
        persistent_trace.version == kTraceVersion &&
        persistent_trace.write_index < kTraceCapacity &&
        persistent_trace.record_count <= kTraceCapacity;
}

bool notification_is_valid()
{
    return persistent_notification.magic == kNotificationMagic &&
        persistent_notification.version == kNotificationVersion;
}

bool notification_ab_is_valid()
{
    return persistent_notification_ab.magic == kNotificationAbMagic &&
        persistent_notification_ab.version == kNotificationAbVersion;
}

bool wpa_wait_is_valid()
{
    return persistent_wpa_wait.magic == kWpaWaitMagic &&
        persistent_wpa_wait.version == kWpaWaitVersion;
}

bool wpa_event_path_is_valid()
{
    return persistent_wpa_event_path.magic == kWpaEventPathMagic &&
        persistent_wpa_event_path.version == kWpaEventPathVersion;
}

bool create_ap_is_valid()
{
    return persistent_create_ap.magic == kCreateApMagic &&
        persistent_create_ap.version == kCreateApVersion;
}

bool hostapd_lifecycle_is_valid()
{
    return persistent_hostapd_lifecycle.magic == kHostapdLifecycleMagic &&
        persistent_hostapd_lifecycle.version == kHostapdLifecycleVersion &&
        persistent_hostapd_lifecycle.write_index <
            kHostapdLifecycleCapacity &&
        persistent_hostapd_lifecycle.record_count <=
            kHostapdLifecycleCapacity;
}

bool hostapd_state_update_is_valid()
{
    return persistent_hostapd_state_update.magic ==
            kHostapdStateUpdateMagic &&
        persistent_hostapd_state_update.version ==
            kHostapdStateUpdateVersion &&
        persistent_hostapd_state_update.write_index <
            kHostapdStateUpdateCapacity &&
        persistent_hostapd_state_update.record_count <=
            kHostapdStateUpdateCapacity;
}

bool status_code_is_valid()
{
    return persistent_status_code.magic == kStatusCodeMagic &&
        persistent_status_code.version == kStatusCodeVersion;
}

bool eloop_post_is_valid()
{
    return persistent_eloop_post.magic == kEloopPostMagic &&
        persistent_eloop_post.version == kEloopPostVersion;
}

bool allocator_trace_is_valid()
{
    return persistent_allocator_trace.magic == kAllocatorTraceMagic &&
        persistent_allocator_trace.version == kAllocatorTraceVersion &&
        persistent_allocator_trace.write_index < kAllocatorTraceCapacity &&
        persistent_allocator_trace.record_count <= kAllocatorTraceCapacity;
}

bool allocator_heap_walk_is_valid()
{
    return persistent_allocator_heap_walk.magic == kAllocatorHeapWalkMagic &&
        persistent_allocator_heap_walk.version == kAllocatorHeapWalkVersion &&
        persistent_allocator_heap_walk.record_count <=
            kAllocatorHeapWalkRecordCount;
}

bool watchdog_is_valid()
{
    return persistent_watchdog.magic == kWatchdogMagic &&
        persistent_watchdog.version == kWatchdogVersion;
}

bool uart_recovery_is_valid()
{
    return persistent_uart_recovery.magic == kUartRecoveryMagic &&
        persistent_uart_recovery.version == kUartRecoveryVersion;
}

void uart_recovery_initialize_unlocked()
{
    memset(&persistent_uart_recovery, 0, sizeof(persistent_uart_recovery));
    persistent_uart_recovery.version = kUartRecoveryVersion;
    persistent_uart_recovery.magic = kUartRecoveryMagic;
}

uint32_t read_uart_register(
    const struct bflb_device_s *device,
    uint32_t offset)
{
    if (device == nullptr) {
        return 0U;
    }
    const uintptr_t address = static_cast<uintptr_t>(device->reg_base) + offset;
    return *reinterpret_cast<volatile const uint32_t *>(address);
}

WifiApStartUartSnapshotPersistent capture_uart_snapshot(
    const HardwareSerial &serial)
{
    WifiApStartUartSnapshotPersistent snapshot = {};
    struct bflb_device_s *device = serial.nativeHandle();
    if (device == nullptr) {
        return snapshot;
    }

    snapshot.initialized = 1U;
    const int fifo_count = bflb_uart_feature_control(
        device,
        UART_CMD_GET_RX_FIFO_CNT,
        0U);
    snapshot.rx_fifo_count = fifo_count > 0
        ? static_cast<uint8_t>(
            fifo_count > UART_FIFO_MAX ? UART_FIFO_MAX : fifo_count)
        : 0U;
    snapshot.interrupt_status = static_cast<uint16_t>(
        bflb_uart_get_intstatus(device));
    snapshot.interrupt_mask = static_cast<uint16_t>(
        read_uart_register(device, UART_INT_MASK_OFFSET));
    snapshot.rx_config = read_uart_register(device, UART_URX_CONFIG_OFFSET);
    snapshot.bit_period = read_uart_register(device, UART_BIT_PRD_OFFSET);
    snapshot.rx_received_count = serial.rxReceivedCount();
    snapshot.rx_overflow_count = serial.rxOverflowCount();
    return snapshot;
}

WifiApStartUartSnapshot to_uart_snapshot(
    const WifiApStartUartSnapshotPersistent &persistent)
{
    WifiApStartUartSnapshot snapshot = {};
    snapshot.initialized = persistent.initialized != 0U;
    snapshot.rx_fifo_count = persistent.rx_fifo_count;
    snapshot.interrupt_status = persistent.interrupt_status;
    snapshot.interrupt_mask = persistent.interrupt_mask;
    snapshot.rx_config = persistent.rx_config;
    snapshot.bit_period = persistent.bit_period;
    snapshot.rx_received_count = persistent.rx_received_count;
    snapshot.rx_overflow_count = persistent.rx_overflow_count;
    return snapshot;
}

void watchdog_initialize_unlocked()
{
    memset(&persistent_watchdog, 0, sizeof(persistent_watchdog));
    persistent_watchdog.version = kWatchdogVersion;
    persistent_watchdog.magic = kWatchdogMagic;
}

void watchdog_persist_unlocked(
    WifiApStartWatchdogState state,
    uint8_t reset_reason,
    uint16_t timeout_ms)
{
    if (!watchdog_is_valid()) {
        watchdog_initialize_unlocked();
    }
    persistent_watchdog.state = static_cast<uint8_t>(state);
    persistent_watchdog.reset_reason = reset_reason;
    persistent_watchdog.timeout_ms = timeout_ms;
    __asm__ volatile("" ::: "memory");
    persistent_watchdog.magic = kWatchdogMagic;
}

void trace_initialize_unlocked()
{
    memset(&persistent_trace, 0, sizeof(persistent_trace));
    persistent_trace.version = kTraceVersion;
    persistent_trace.magic = kTraceMagic;
}

void notification_initialize_unlocked()
{
    memset(&persistent_notification, 0, sizeof(persistent_notification));
    persistent_notification.version = kNotificationVersion;
    persistent_notification.magic = kNotificationMagic;
}

void notification_ab_initialize_unlocked()
{
    memset(&persistent_notification_ab, 0, sizeof(persistent_notification_ab));
    persistent_notification_ab.version = kNotificationAbVersion;
    persistent_notification_ab.magic = kNotificationAbMagic;
}

void wpa_wait_initialize_unlocked()
{
    memset(&persistent_wpa_wait, 0, sizeof(persistent_wpa_wait));
    persistent_wpa_wait.version = kWpaWaitVersion;
    persistent_wpa_wait.vif = -1;
    persistent_wpa_wait.expected_event = 0xffU;
    persistent_wpa_wait.observed_event = 0xffU;
    persistent_wpa_wait.wpa_state = 0xffU;
    persistent_wpa_wait.magic = kWpaWaitMagic;
}

void wpa_event_path_initialize_unlocked()
{
    memset(&persistent_wpa_event_path, 0, sizeof(persistent_wpa_event_path));
    persistent_wpa_event_path.version = kWpaEventPathVersion;
    persistent_wpa_event_path.vif = -1;
    persistent_wpa_event_path.event = 0xffU;
    persistent_wpa_event_path.fhost_wpa_state = 0xffU;
    persistent_wpa_event_path.operstate = -1;
    persistent_wpa_event_path.old_wpa_state = -1;
    persistent_wpa_event_path.new_wpa_state = -1;
    persistent_wpa_event_path.callback_slot = 0xffU;
    persistent_wpa_event_path.magic = kWpaEventPathMagic;
}

void create_ap_initialize_unlocked()
{
    memset(&persistent_create_ap, 0, sizeof(persistent_create_ap));
    persistent_create_ap.version = kCreateApVersion;
    persistent_create_ap.wpa_state = 0xffU;
    persistent_create_ap.magic = kCreateApMagic;
}

void hostapd_lifecycle_initialize_unlocked()
{
    memset(&persistent_hostapd_lifecycle, 0, sizeof(persistent_hostapd_lifecycle));
    persistent_hostapd_lifecycle.version = kHostapdLifecycleVersion;
    persistent_hostapd_lifecycle.magic = kHostapdLifecycleMagic;
}

void hostapd_state_update_initialize_unlocked()
{
    memset(
        &persistent_hostapd_state_update,
        0,
        sizeof(persistent_hostapd_state_update));
    persistent_hostapd_state_update.version = kHostapdStateUpdateVersion;
    persistent_hostapd_state_update.magic = kHostapdStateUpdateMagic;
}

void status_code_initialize_unlocked()
{
    memset(&persistent_status_code, 0, sizeof(persistent_status_code));
    persistent_status_code.version = kStatusCodeVersion;
    persistent_status_code.vif = -1;
    persistent_status_code.raw_state = 0xffU;
    persistent_status_code.vif_state = 0xffU;
    persistent_status_code.magic = kStatusCodeMagic;
}

void eloop_post_initialize_unlocked()
{
    memset(&persistent_eloop_post, 0, sizeof(persistent_eloop_post));
    persistent_eloop_post.version = kEloopPostVersion;
    persistent_eloop_post.magic = kEloopPostMagic;
}

void allocator_trace_initialize_unlocked()
{
    memset(&persistent_allocator_trace, 0, sizeof(persistent_allocator_trace));
    persistent_allocator_trace.version = kAllocatorTraceVersion;
    persistent_allocator_trace.magic = kAllocatorTraceMagic;
}

void allocator_heap_walk_initialize_unlocked()
{
    memset(&persistent_allocator_heap_walk, 0,
           sizeof(persistent_allocator_heap_walk));
    persistent_allocator_heap_walk.version = kAllocatorHeapWalkVersion;
    persistent_allocator_heap_walk.result = -1;
    persistent_allocator_heap_walk.magic = kAllocatorHeapWalkMagic;
}

void persist_unlocked()
{
    memset(&persistent_diagnostic, 0, sizeof(persistent_diagnostic));
    persistent_diagnostic.version = kDiagnosticVersion;
    persistent_diagnostic.stage = static_cast<uint8_t>(diagnostic.stage);
    persistent_diagnostic.native_result = diagnostic.native_result;
    persistent_diagnostic.sta_vif_state = diagnostic.sta_vif_state;
    persistent_diagnostic.ap_vif_state = diagnostic.ap_vif_state;
    persistent_diagnostic.sta_connected = diagnostic.sta_connected;
    persistent_diagnostic.ap_started = diagnostic.ap_started;
    persistent_diagnostic.requested_channel = diagnostic.requested_channel;
    persistent_diagnostic.sta_channel_result = diagnostic.sta_channel_result;
    persistent_diagnostic.sta_channel = diagnostic.sta_channel;
    persistent_diagnostic.fhost_stage = diagnostic.fhost_stage;
    persistent_diagnostic.fhost_result = diagnostic.fhost_result;
    persistent_diagnostic.attempts = diagnostic.attempts;
    persistent_diagnostic.started_at_ms = diagnostic.started_at_ms;
    persistent_diagnostic.completed_at_ms = diagnostic.completed_at_ms;
    persistent_diagnostic.fhost_chan_address = diagnostic.fhost_chan_address;
    persistent_diagnostic.fhost_handler_chan_address =
        diagnostic.fhost_handler_chan_address;
    persistent_diagnostic.fhost_response_source_chan_address =
        diagnostic.fhost_response_source_chan_address;
    persistent_diagnostic.fhost_response_copy_chan_address =
        diagnostic.fhost_response_copy_chan_address;
    persistent_diagnostic.fhost_response_received_chan_address =
        diagnostic.fhost_response_received_chan_address;
    persistent_diagnostic.wpa_feature_chan_address =
        diagnostic.wpa_feature_chan_address;
    persistent_diagnostic.fhost_chan2g4_count = diagnostic.fhost_chan2g4_count;
    persistent_diagnostic.fhost_chan5g_count = diagnostic.fhost_chan5g_count;
    persistent_diagnostic.heap_probe_stage = diagnostic.heap_probe_stage;
    persistent_diagnostic.heap_request_size = diagnostic.heap_request_size;
    persistent_diagnostic.heap_total_before = diagnostic.heap_total_before;
    persistent_diagnostic.heap_ocram_before = diagnostic.heap_ocram_before;
    persistent_diagnostic.heap_wram_before = diagnostic.heap_wram_before;
    persistent_diagnostic.heap_allocation_address =
        diagnostic.heap_allocation_address;
    persistent_diagnostic.heap_total_after = diagnostic.heap_total_after;
    persistent_diagnostic.heap_ocram_after = diagnostic.heap_ocram_after;
    persistent_diagnostic.heap_wram_after = diagnostic.heap_wram_after;
    persistent_diagnostic.heap_malloc_failed = diagnostic.heap_malloc_failed;
    persistent_diagnostic.fhost_command = diagnostic.fhost_command;
    persistent_diagnostic.fhost_command_phase = diagnostic.fhost_command_phase;
    persistent_diagnostic.fhost_command_vif = diagnostic.fhost_command_vif;
    persistent_diagnostic.fhost_command_wpa_state =
        diagnostic.fhost_command_wpa_state;
    persistent_diagnostic.fhost_command_network_id =
        diagnostic.fhost_command_network_id;
    persistent_diagnostic.fhost_command_sequence =
        diagnostic.fhost_command_sequence;
    persistent_diagnostic.fhost_sync_phase = diagnostic.fhost_sync_phase;
    persistent_diagnostic.fhost_sync_wait_result =
        diagnostic.fhost_sync_wait_result;
    persistent_diagnostic.fhost_sync_signal_result =
        diagnostic.fhost_sync_signal_result;
    persistent_diagnostic.fhost_sync_wait_count_before =
        diagnostic.fhost_sync_wait_count_before;
    persistent_diagnostic.fhost_sync_callback_count_before =
        diagnostic.fhost_sync_callback_count_before;
    persistent_diagnostic.fhost_sync_callback_count_after =
        diagnostic.fhost_sync_callback_count_after;
    persistent_diagnostic.fhost_sync_wait_count_after =
        diagnostic.fhost_sync_wait_count_after;
    persistent_diagnostic.fhost_sync_wait_handle =
        diagnostic.fhost_sync_wait_handle;
    persistent_diagnostic.fhost_sync_callback_handle =
        diagnostic.fhost_sync_callback_handle;
    persistent_diagnostic.fhost_sync_update_count =
        diagnostic.fhost_sync_update_count;
    persistent_diagnostic.fhost_task_handle = diagnostic.fhost_task_handle;
    persistent_diagnostic.fhost_task_state = diagnostic.fhost_task_state;
    persistent_diagnostic.fhost_task_stack_high_watermark =
        diagnostic.fhost_task_stack_high_watermark;
    persistent_diagnostic.liveness_phase = diagnostic.liveness_phase;
    persistent_diagnostic.liveness_event_count =
        diagnostic.liveness_event_count;
    __asm__ volatile("" ::: "memory");
    persistent_diagnostic.magic = kDiagnosticMagic;
}

void persist()
{
    taskENTER_CRITICAL();
    persist_unlocked();
    taskEXIT_CRITICAL();
}

struct AllocatorHeapWalkContext {
    WifiApStartAllocatorHeapWalkPersistentRecord
        records[kAllocatorHeapWalkRecordCount] = {};
};

int allocator_heap_walk_index(uint32_t heap_id)
{
    if (heap_id == MM_HEAP_OCRAM_0) {
        return 0;
    }
    if (heap_id == MM_HEAP_WRAM_0) {
        return 1;
    }
    return -1;
}

void allocator_heap_walk_record_block(
    struct mm_heap *heap,
    void *,
    size_t size,
    void *user)
{
    if (heap == nullptr || user == nullptr) {
        return;
    }

    const int index = allocator_heap_walk_index(heap->heap_id);
    if (index < 0) {
        return;
    }

    AllocatorHeapWalkContext *context =
        static_cast<AllocatorHeapWalkContext *>(user);
    WifiApStartAllocatorHeapWalkPersistentRecord &record =
        context->records[index];
    record.heap_id = static_cast<uint8_t>(heap->heap_id);
    if (record.allocated_block_count != UINT16_MAX) {
        ++record.allocated_block_count;
    }
    record.allocated_bytes += static_cast<uint32_t>(size);

    for (uint8_t position = 0U;
         position < kAllocatorHeapWalkLargestCount;
         ++position) {
        if (size <= record.largest_blocks[position]) {
            continue;
        }
        for (uint8_t shift = kAllocatorHeapWalkLargestCount - 1U;
             shift > position;
             --shift) {
            record.largest_blocks[shift] =
                record.largest_blocks[shift - 1U];
        }
        record.largest_blocks[position] = static_cast<uint32_t>(size);
        break;
    }
}

} // namespace

void wifi_ap_start_watchdog_bootstrap()
{
#if defined(BL616CL_AP_START_DIAGNOSTIC_WATCHDOG) && \
    BL616CL_AP_START_DIAGNOSTIC_WATCHDOG
    /*
     * The hardware WDT survives until this application gets CPU time after
     * reset. Stop it before starting USB/Wi-Fi, then retain the record that
     * says whether the previous AP call was still armed.
     */
    struct bflb_device_s *watchdog = bflb_device_get_by_name(BFLB_NAME_WDT);
    if (watchdog != nullptr) {
        bflb_wdg_stop(watchdog);
    }

    bl_sys_rstinfo_init();
    const uint8_t reset_reason = static_cast<uint8_t>(bl_sys_rstinfo_get());

    taskENTER_CRITICAL();
    if (!watchdog_is_valid()) {
        watchdog_initialize_unlocked();
    } else if (persistent_watchdog.state ==
        static_cast<uint8_t>(WifiApStartWatchdogState::Armed)) {
        watchdog_persist_unlocked(
            WifiApStartWatchdogState::ResetObserved,
            reset_reason,
            persistent_watchdog.timeout_ms);
    } else {
        persistent_watchdog.reset_reason = reset_reason;
    }
    taskEXIT_CRITICAL();
#endif
}

void wifi_ap_start_watchdog_arm()
{
#if defined(BL616CL_AP_START_DIAGNOSTIC_WATCHDOG) && \
    BL616CL_AP_START_DIAGNOSTIC_WATCHDOG
    struct bflb_device_s *watchdog = bflb_device_get_by_name(BFLB_NAME_WDT);
    if (watchdog == nullptr) {
        return;
    }

    taskENTER_CRITICAL();
    if (!watchdog_is_valid()) {
        watchdog_initialize_unlocked();
    }
    ++persistent_watchdog.arm_count;
    watchdog_persist_unlocked(
        WifiApStartWatchdogState::Armed,
        0U,
        kWatchdogTimeoutMs);
    taskEXIT_CRITICAL();

    const struct bflb_wdg_config_s config = {
        WDG_CLKSRC_32K,
        31U,
        kWatchdogTimeoutMs,
        WDG_MODE_RESET,
    };
    bflb_wdg_stop(watchdog);
    bflb_wdg_init(watchdog, &config);
    bflb_wdg_start(watchdog);
    bflb_wdg_reset_countervalue(watchdog);
#endif
}

void wifi_ap_start_watchdog_disarm()
{
#if defined(BL616CL_AP_START_DIAGNOSTIC_WATCHDOG) && \
    BL616CL_AP_START_DIAGNOSTIC_WATCHDOG
    struct bflb_device_s *watchdog = bflb_device_get_by_name(BFLB_NAME_WDT);
    if (watchdog != nullptr) {
        bflb_wdg_stop(watchdog);
    }

    taskENTER_CRITICAL();
    if (!watchdog_is_valid()) {
        watchdog_initialize_unlocked();
    }
    watchdog_persist_unlocked(
        WifiApStartWatchdogState::Disarmed,
        persistent_watchdog.reset_reason,
        kWatchdogTimeoutMs);
    taskEXIT_CRITICAL();
#endif
}

bool wifi_ap_start_watchdog_recovery_required()
{
#if defined(BL616CL_AP_START_DIAGNOSTIC_WATCHDOG) && \
    BL616CL_AP_START_DIAGNOSTIC_WATCHDOG
    taskENTER_CRITICAL();
    const bool required = watchdog_is_valid() &&
        persistent_watchdog.state ==
            static_cast<uint8_t>(WifiApStartWatchdogState::ResetObserved);
    taskEXIT_CRITICAL();
    return required;
#else
    return false;
#endif
}

void wifi_ap_start_watchdog_recovery_complete()
{
#if defined(BL616CL_AP_START_DIAGNOSTIC_WATCHDOG) && \
    BL616CL_AP_START_DIAGNOSTIC_WATCHDOG
    taskENTER_CRITICAL();
    if (!watchdog_is_valid()) {
        watchdog_initialize_unlocked();
    }
    watchdog_persist_unlocked(
        WifiApStartWatchdogState::RecoveryComplete,
        persistent_watchdog.reset_reason,
        kWatchdogTimeoutMs);
    taskEXIT_CRITICAL();
#endif
}

void wifi_ap_start_capture_uart_before_watchdog(
    const HardwareSerial &user_uart,
    const HardwareSerial &at_uart,
    uint32_t cdc_baud)
{
#if defined(BL616CL_AP_START_DIAGNOSTIC_WATCHDOG) && \
    BL616CL_AP_START_DIAGNOSTIC_WATCHDOG
    const WifiApStartUartSnapshotPersistent user_snapshot =
        capture_uart_snapshot(user_uart);
    const WifiApStartUartSnapshotPersistent at_snapshot =
        capture_uart_snapshot(at_uart);

    taskENTER_CRITICAL();
    uart_recovery_initialize_unlocked();
    persistent_uart_recovery.user_uart_before = user_snapshot;
    persistent_uart_recovery.at_uart_before = at_snapshot;
    persistent_uart_recovery.cdc_baud_before = cdc_baud;
    persistent_uart_recovery.before_valid = 1U;
    __asm__ volatile("" ::: "memory");
    persistent_uart_recovery.magic = kUartRecoveryMagic;
    taskEXIT_CRITICAL();
#else
    (void)user_uart;
    (void)at_uart;
    (void)cdc_baud;
#endif
}

void wifi_ap_start_capture_uart_after_watchdog_boot(
    const HardwareSerial &user_uart,
    const HardwareSerial &at_uart,
    uint32_t cdc_baud)
{
#if defined(BL616CL_AP_START_DIAGNOSTIC_WATCHDOG) && \
    BL616CL_AP_START_DIAGNOSTIC_WATCHDOG
    const WifiApStartUartSnapshotPersistent user_snapshot =
        capture_uart_snapshot(user_uart);
    const WifiApStartUartSnapshotPersistent at_snapshot =
        capture_uart_snapshot(at_uart);

    taskENTER_CRITICAL();
    if (uart_recovery_is_valid() &&
        watchdog_is_valid() &&
        persistent_watchdog.state ==
            static_cast<uint8_t>(WifiApStartWatchdogState::ResetObserved)) {
        persistent_uart_recovery.user_uart_after = user_snapshot;
        persistent_uart_recovery.at_uart_after = at_snapshot;
        persistent_uart_recovery.cdc_baud_after = cdc_baud;
        persistent_uart_recovery.reset_reason = persistent_watchdog.reset_reason;
        persistent_uart_recovery.after_valid = 1U;
        __asm__ volatile("" ::: "memory");
        persistent_uart_recovery.magic = kUartRecoveryMagic;
    }
    taskEXIT_CRITICAL();
#else
    (void)user_uart;
    (void)at_uart;
    (void)cdc_baud;
#endif
}

void wifi_ap_start_diagnostic_begin(uint8_t requested_channel)
{
    taskENTER_CRITICAL();
    allocator_trace_initialize_unlocked();
    wpa_wait_initialize_unlocked();
    wpa_event_path_initialize_unlocked();
    create_ap_initialize_unlocked();
    hostapd_state_update_initialize_unlocked();
    diagnostic.stage = WifiApStartDiagnosticStage::Preparing;
    diagnostic.native_result = 0;
    diagnostic.requested_channel = requested_channel;
    diagnostic.sta_channel = 0U;
    diagnostic.sta_channel_result = 0;
    ++diagnostic.attempts;
    diagnostic.started_at_ms = millis();
    diagnostic.completed_at_ms = 0U;
    diagnostic.sta_vif_state = 0U;
    diagnostic.ap_vif_state = 0U;
    diagnostic.sta_connected = 0U;
    diagnostic.ap_started = 0U;
    diagnostic.fhost_chan_address = 0U;
    diagnostic.fhost_handler_chan_address = 0U;
    diagnostic.fhost_response_source_chan_address = 0U;
    diagnostic.fhost_response_copy_chan_address = 0U;
    diagnostic.fhost_response_received_chan_address = 0U;
    diagnostic.wpa_feature_chan_address = 0U;
    diagnostic.fhost_chan2g4_count = 0U;
    diagnostic.fhost_chan5g_count = 0U;
    diagnostic.heap_probe_stage = 0U;
    diagnostic.heap_request_size = 0U;
    diagnostic.heap_total_before = 0U;
    diagnostic.heap_ocram_before = 0U;
    diagnostic.heap_wram_before = 0U;
    diagnostic.heap_allocation_address = 0U;
    diagnostic.heap_total_after = 0U;
    diagnostic.heap_ocram_after = 0U;
    diagnostic.heap_wram_after = 0U;
    diagnostic.heap_malloc_failed = 0U;
    diagnostic.fhost_command = 0U;
    diagnostic.fhost_command_phase = 0U;
    diagnostic.fhost_command_vif = -1;
    diagnostic.fhost_command_wpa_state = 0xffU;
    diagnostic.fhost_command_network_id = -1;
    diagnostic.fhost_command_sequence = 0U;
    diagnostic.fhost_sync_phase = 0U;
    diagnostic.fhost_sync_wait_result = 0;
    diagnostic.fhost_sync_signal_result = 0;
    diagnostic.fhost_sync_wait_count_before = 0U;
    diagnostic.fhost_sync_callback_count_before = 0U;
    diagnostic.fhost_sync_callback_count_after = 0U;
    diagnostic.fhost_sync_wait_count_after = 0U;
    diagnostic.fhost_sync_wait_handle = 0U;
    diagnostic.fhost_sync_callback_handle = 0U;
    diagnostic.fhost_sync_update_count = 0U;
    diagnostic.fhost_task_handle = 0U;
    diagnostic.fhost_task_state = 0U;
    diagnostic.fhost_task_stack_high_watermark = 0U;
    diagnostic.liveness_phase = 0U;
    diagnostic.liveness_event_count = 0U;
    persist_unlocked();
    taskEXIT_CRITICAL();
}

void wifi_ap_start_diagnostic_mark_native_call()
{
    taskENTER_CRITICAL();
    diagnostic.stage = WifiApStartDiagnosticStage::NativeCall;
    const TaskHandle_t task = xTaskGetCurrentTaskHandle();
    diagnostic.fhost_task_handle = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(task));
    diagnostic.fhost_task_state = static_cast<uint8_t>(eTaskGetState(task));
    diagnostic.fhost_task_stack_high_watermark = static_cast<uint32_t>(
        uxTaskGetStackHighWaterMark(task));
    diagnostic.liveness_phase = 1U;
    ++diagnostic.liveness_event_count;
    persist_unlocked();
    taskEXIT_CRITICAL();
}

void wifi_ap_start_diagnostic_complete(int native_result)
{
    taskENTER_CRITICAL();
    diagnostic.native_result = static_cast<int8_t>(native_result);
    diagnostic.completed_at_ms = millis();
    diagnostic.stage = WifiApStartDiagnosticStage::Completed;
    persist_unlocked();
    taskEXIT_CRITICAL();
}

extern "C" void fhost_bridge_ap_start_progress(uint8_t stage, int result)
{
    taskENTER_CRITICAL();
    if (stage == 250U && diagnostic.fhost_stage != 250U) {
        allocator_trace_initialize_unlocked();
    }
    diagnostic.fhost_stage = stage;
    diagnostic.fhost_result = static_cast<int8_t>(result);
    const TaskHandle_t task = xTaskGetCurrentTaskHandle();
    diagnostic.fhost_task_handle = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(task));
    diagnostic.fhost_task_state = static_cast<uint8_t>(eTaskGetState(task));
    diagnostic.fhost_task_stack_high_watermark = static_cast<uint32_t>(
        uxTaskGetStackHighWaterMark(task));
    persist_unlocked();
    taskEXIT_CRITICAL();
}

extern "C" void fhost_bridge_ap_start_liveness(uint8_t phase)
{
    /*
     * This hook executes directly in the WPA/FHOST control path. Do not call
     * FreeRTOS task inspection or the full persistence copy here: the marker
     * must remain useful even if the surrounding task is already wedged.
     */
    const uintptr_t irq_flags = bflb_irq_save();
    diagnostic.liveness_phase = phase;
    ++diagnostic.liveness_event_count;
    if (persistent_is_valid()) {
        persistent_diagnostic.liveness_phase = phase;
        ++persistent_diagnostic.liveness_event_count;
        __asm__ volatile("" ::: "memory");
    }
    bflb_irq_restore(irq_flags);
}

extern "C" int fhost_bridge_ap_start_allocator_trace_active(void)
{
    return diagnostic.fhost_stage == 250U ? 1 : 0;
}

extern "C" void fhost_bridge_ap_start_allocator_heap_walk(void)
{
    if (diagnostic.fhost_stage != 250U) {
        return;
    }

    AllocatorHeapWalkContext context = {};
    const int result = mm_walk_allocated_blocks(
        allocator_heap_walk_record_block,
        &context);

    const uintptr_t irq_flags = bflb_irq_save();
    allocator_heap_walk_initialize_unlocked();
    persistent_allocator_heap_walk.result = static_cast<int8_t>(result);
    persistent_allocator_heap_walk.record_count =
        kAllocatorHeapWalkRecordCount;
    memcpy(
        persistent_allocator_heap_walk.records,
        context.records,
        sizeof(context.records));
    __asm__ volatile("" ::: "memory");
    persistent_allocator_heap_walk.magic = kAllocatorHeapWalkMagic;
    bflb_irq_restore(irq_flags);
}

extern "C" void fhost_bridge_ap_start_pointer(uint8_t location,
                                               uint32_t address)
{
    taskENTER_CRITICAL();
    switch (location) {
    case 1U:
        diagnostic.fhost_chan_address = address;
        break;
    case 2U:
        diagnostic.fhost_handler_chan_address = address;
        break;
    case 3U:
        diagnostic.fhost_response_source_chan_address = address;
        break;
    case 4U:
        diagnostic.fhost_response_copy_chan_address = address;
        break;
    case 5U:
        diagnostic.fhost_response_received_chan_address = address;
        break;
    case 6U:
        diagnostic.wpa_feature_chan_address = address;
        break;
    case 7U:
        diagnostic.fhost_sync_wait_handle = address;
        break;
    case 8U:
        diagnostic.fhost_sync_callback_handle = address;
        break;
    default:
        taskEXIT_CRITICAL();
        return;
    }
    persist_unlocked();
    taskEXIT_CRITICAL();
}

extern "C" void fhost_bridge_ap_start_channel_counts(uint32_t chan2g4_count,
                                                      uint32_t chan5g_count)
{
    taskENTER_CRITICAL();
    diagnostic.fhost_chan2g4_count = chan2g4_count;
    diagnostic.fhost_chan5g_count = chan5g_count;
    persist_unlocked();
    taskEXIT_CRITICAL();
}

extern "C" void fhost_bridge_ap_start_heap_before_allocate(uint32_t size)
{
    diagnostic.heap_request_size = size;
    diagnostic.heap_probe_stage = 1U;
    persist();
    diagnostic.heap_total_before = static_cast<uint32_t>(kfree_size(0U));

    diagnostic.heap_probe_stage = 2U;
    persist();
    diagnostic.heap_ocram_before =
        static_cast<uint32_t>(kfree_size(MM_HEAP_OCRAM_0));

    diagnostic.heap_probe_stage = 3U;
    persist();
    diagnostic.heap_wram_before =
        static_cast<uint32_t>(kfree_size(MM_HEAP_WRAM_0));

    diagnostic.heap_probe_stage = 4U;
    persist();
}

extern "C" void fhost_bridge_ap_start_heap_after_allocate(uint32_t address)
{
    diagnostic.heap_allocation_address = address;
    diagnostic.heap_probe_stage = 5U;
    persist();
    diagnostic.heap_total_after = static_cast<uint32_t>(kfree_size(0U));

    diagnostic.heap_probe_stage = 6U;
    persist();
    diagnostic.heap_ocram_after =
        static_cast<uint32_t>(kfree_size(MM_HEAP_OCRAM_0));

    diagnostic.heap_probe_stage = 7U;
    persist();
    diagnostic.heap_wram_after =
        static_cast<uint32_t>(kfree_size(MM_HEAP_WRAM_0));

    diagnostic.heap_probe_stage = 8U;
    persist();
}

/*
 * The SDK default malloc-failure hook aborts the whole MCU. The bridge must
 * let the caller handle an allocation failure and keep USB/AT recovery alive.
 */
extern "C" void vApplicationMallocFailedHook(void)
{
    diagnostic.heap_malloc_failed = 1U;
    if (diagnostic.stage != WifiApStartDiagnosticStage::Idle) {
        persist();
    }
}

extern "C" void fhost_bridge_ap_start_command(
    uint8_t command,
    uint8_t phase,
    int fhost_vif_idx,
    int wpa_state,
    int network_id)
{
    taskENTER_CRITICAL();
    diagnostic.fhost_command = command;
    diagnostic.fhost_command_phase = phase;
    diagnostic.fhost_command_vif = static_cast<int8_t>(fhost_vif_idx);
    diagnostic.fhost_command_wpa_state = static_cast<uint8_t>(wpa_state);
    diagnostic.fhost_command_network_id = static_cast<int8_t>(network_id);
    ++diagnostic.fhost_command_sequence;
    if (diagnostic.stage != WifiApStartDiagnosticStage::Idle) {
        persist_unlocked();
    }
    taskEXIT_CRITICAL();
}

extern "C" void fhost_bridge_ap_start_sync(
    uint8_t phase,
    uint32_t semaphore,
    int result,
    uint32_t count)
{
    taskENTER_CRITICAL();
    ++diagnostic.fhost_sync_update_count;
    diagnostic.fhost_sync_phase = phase;
    switch (phase) {
    case 1U:
        diagnostic.fhost_sync_wait_handle = semaphore;
        diagnostic.fhost_sync_wait_count_before = static_cast<uint8_t>(count);
        break;
    case 2U:
        diagnostic.fhost_sync_callback_handle = semaphore;
        diagnostic.fhost_sync_callback_count_before =
            static_cast<uint8_t>(count);
        break;
    case 3U:
        diagnostic.fhost_sync_callback_handle = semaphore;
        diagnostic.fhost_sync_signal_result = static_cast<int8_t>(result);
        break;
    case 4U:
        diagnostic.fhost_sync_callback_handle = semaphore;
        diagnostic.fhost_sync_callback_count_after =
            static_cast<uint8_t>(count);
        break;
    case 5U:
        diagnostic.fhost_sync_wait_handle = semaphore;
        diagnostic.fhost_sync_wait_result = static_cast<int8_t>(result);
        diagnostic.fhost_sync_wait_count_after = static_cast<uint8_t>(count);
        break;
    case 6U:
        diagnostic.fhost_sync_wait_handle = semaphore;
        diagnostic.fhost_sync_wait_count_before = static_cast<uint8_t>(count);
        break;
    case 7U:
        diagnostic.fhost_sync_callback_handle = semaphore;
        diagnostic.fhost_sync_signal_result = static_cast<int8_t>(result);
        diagnostic.fhost_sync_callback_count_after =
            static_cast<uint8_t>(count);
        break;
    case 8U:
        diagnostic.fhost_sync_wait_handle = semaphore;
        diagnostic.fhost_sync_wait_result = static_cast<int8_t>(result);
        diagnostic.fhost_sync_wait_count_after = static_cast<uint8_t>(count);
        break;
    default:
        taskEXIT_CRITICAL();
        return;
    }
    persist_unlocked();
    taskEXIT_CRITICAL();
}

extern "C" void fhost_bridge_ap_start_trace(
    uint8_t event,
    uint32_t semaphore,
    int result,
    uint32_t count)
{
    taskENTER_CRITICAL();
    if (!trace_is_valid()) {
        trace_initialize_unlocked();
    }

    WifiApStartTracePersistentRecord &record =
        persistent_trace.records[persistent_trace.write_index];
    record.sequence = persistent_trace.next_sequence++;
    record.event = event;
    record.result = static_cast<int8_t>(result);
    record.reserved = 0U;
    record.semaphore = semaphore;
    record.count = count;

    persistent_trace.write_index =
        static_cast<uint8_t>((persistent_trace.write_index + 1U) % kTraceCapacity);
    if (persistent_trace.record_count < kTraceCapacity) {
        ++persistent_trace.record_count;
    }
    __asm__ volatile("" ::: "memory");
    persistent_trace.magic = kTraceMagic;

    if (event >= static_cast<uint8_t>(WifiApStartTraceEvent::NotificationBefore) &&
        event <= static_cast<uint8_t>(WifiApStartTraceEvent::NotificationWaitReturn)) {
        if (!notification_is_valid()) {
            notification_initialize_unlocked();
        }

        if (event ==
            static_cast<uint8_t>(WifiApStartTraceEvent::NotificationBefore)) {
            const uint32_t next_attempt = persistent_notification.attempt_count + 1U;
            memset(&persistent_notification, 0, sizeof(persistent_notification));
            persistent_notification.version = kNotificationVersion;
            persistent_notification.attempt_count = next_attempt;
        }

        persistent_notification.event_mask |= static_cast<uint8_t>(
            1U << (event -
                   static_cast<uint8_t>(
                       WifiApStartTraceEvent::NotificationBefore)));
        persistent_notification.last_event = event;
        persistent_notification.last_sequence = record.sequence;
        persistent_notification.waiting_task = semaphore;
        if (event ==
            static_cast<uint8_t>(WifiApStartTraceEvent::NotificationReturn)) {
            persistent_notification.notification_result =
                static_cast<int8_t>(result);
        } else if (event ==
                   static_cast<uint8_t>(
                       WifiApStartTraceEvent::NotificationWaitReturn)) {
            persistent_notification.wait_result = static_cast<int8_t>(result);
        }
        __asm__ volatile("" ::: "memory");
        persistent_notification.magic = kNotificationMagic;
    }
    taskEXIT_CRITICAL();
}

extern "C" void fhost_bridge_ap_start_notification_ab(
    uint8_t phase,
    uint32_t message,
    uint32_t waiting_task,
    uint8_t request_matches,
    uint8_t use_task_notification,
    int result)
{
    if (phase == 0U || phase > 8U) {
        return;
    }

    taskENTER_CRITICAL();
    if (!notification_ab_is_valid() || phase == 1U) {
        notification_ab_initialize_unlocked();
    }

    persistent_notification_ab.phase_mask |= static_cast<uint8_t>(
        1U << (phase - 1U));
    persistent_notification_ab.last_phase = phase;
    persistent_notification_ab.request_matches = request_matches;
    persistent_notification_ab.last_result = static_cast<int8_t>(result);
    ++persistent_notification_ab.update_count;

    switch (phase) {
    case 2U:
        persistent_notification_ab.setup_message = message;
        persistent_notification_ab.setup_waiting_task = waiting_task;
        persistent_notification_ab.setup_use_task_notification =
            use_task_notification;
        break;
    case 3U:
    case 4U:
        persistent_notification_ab.queued_message = message;
        persistent_notification_ab.queued_waiting_task = waiting_task;
        persistent_notification_ab.queued_use_task_notification =
            use_task_notification;
        break;
    case 5U:
    case 6U:
    case 7U:
        persistent_notification_ab.callback_message = message;
        persistent_notification_ab.callback_waiting_task = waiting_task;
        persistent_notification_ab.callback_use_task_notification =
            use_task_notification;
        break;
    case 8U:
        persistent_notification_ab.wait_message = message;
        persistent_notification_ab.wait_waiting_task = waiting_task;
        persistent_notification_ab.wait_use_task_notification =
            use_task_notification;
        break;
    default:
        break;
    }
    __asm__ volatile("" ::: "memory");
    persistent_notification_ab.magic = kNotificationAbMagic;
    taskEXIT_CRITICAL();
}

extern "C" void fhost_bridge_ap_start_wpa_wait(
    uint8_t phase,
    int fhost_vif_idx,
    uint8_t expected_event,
    uint8_t observed_event,
    uint32_t target,
    uint32_t waiting_task,
    uint32_t callback_task,
    int result,
    uint8_t wpa_state)
{
    if (phase == 0U || phase > 16U) {
        return;
    }

    taskENTER_CRITICAL();
    if (!wpa_wait_is_valid() || phase == 1U) {
        wpa_wait_initialize_unlocked();
    }

    persistent_wpa_wait.phase_mask |= static_cast<uint16_t>(1U << (phase - 1U));
    persistent_wpa_wait.last_phase = phase;
    persistent_wpa_wait.last_result = static_cast<int8_t>(result);
    persistent_wpa_wait.vif = static_cast<int8_t>(fhost_vif_idx);
    persistent_wpa_wait.expected_event = expected_event;
    persistent_wpa_wait.observed_event = observed_event;
    persistent_wpa_wait.wpa_state = wpa_state;
    persistent_wpa_wait.target = target;
    persistent_wpa_wait.waiting_task = waiting_task;
    persistent_wpa_wait.callback_task = callback_task;
    ++persistent_wpa_wait.update_count;
    __asm__ volatile("" ::: "memory");
    persistent_wpa_wait.magic = kWpaWaitMagic;
    taskEXIT_CRITICAL();
}

extern "C" void fhost_bridge_ap_start_wpa_event_path(
    uint8_t phase,
    int fhost_vif_idx,
    uint8_t event,
    uint8_t fhost_wpa_state,
    int operstate,
    int old_wpa_state,
    int new_wpa_state,
    int result,
    uint8_t callback_slot)
{
    if (phase == 0U || phase > 30U || fhost_vif_idx != 1) {
        return;
    }

    taskENTER_CRITICAL();
    if (phase == 1U) {
        wpa_event_path_initialize_unlocked();
    } else if (!wpa_event_path_is_valid()) {
        taskEXIT_CRITICAL();
        return;
    }

    persistent_wpa_event_path.phase_mask |= 1U << (phase - 1U);
    persistent_wpa_event_path.last_phase = phase;
    persistent_wpa_event_path.vif = static_cast<int8_t>(fhost_vif_idx);
    persistent_wpa_event_path.event = event;
    persistent_wpa_event_path.fhost_wpa_state = fhost_wpa_state;
    persistent_wpa_event_path.operstate = static_cast<int8_t>(operstate);
    persistent_wpa_event_path.old_wpa_state =
        static_cast<int8_t>(old_wpa_state);
    persistent_wpa_event_path.new_wpa_state =
        static_cast<int8_t>(new_wpa_state);
    persistent_wpa_event_path.last_result = static_cast<int8_t>(result);
    persistent_wpa_event_path.callback_slot = callback_slot;
    if (callback_slot < 32U) {
        persistent_wpa_event_path.callback_slot_mask |=
            1U << callback_slot;
    }
    ++persistent_wpa_event_path.update_count;
    __asm__ volatile("" ::: "memory");
    persistent_wpa_event_path.magic = kWpaEventPathMagic;
    taskEXIT_CRITICAL();
}

extern "C" void fhost_bridge_ap_start_create_ap(
    uint8_t phase,
    int result,
    uint8_t wpa_state,
    uint32_t detail)
{
    if (phase == 0U || phase > 16U) {
        return;
    }

    taskENTER_CRITICAL();
    if (phase == 1U) {
        create_ap_initialize_unlocked();
    } else if (!create_ap_is_valid()) {
        taskEXIT_CRITICAL();
        return;
    }

    persistent_create_ap.phase_mask |= static_cast<uint16_t>(1U << (phase - 1U));
    persistent_create_ap.last_phase = phase;
    persistent_create_ap.last_result = static_cast<int8_t>(result);
    persistent_create_ap.wpa_state = wpa_state;
    persistent_create_ap.detail = detail;
    ++persistent_create_ap.update_count;
    __asm__ volatile("" ::: "memory");
    persistent_create_ap.magic = kCreateApMagic;
    taskEXIT_CRITICAL();
}

extern "C" void fhost_bridge_ap_start_hostapd_lifecycle(
    uint8_t phase,
    int result,
    uint8_t iface_state,
    uint8_t wait_channel_update,
    uint32_t detail0,
    uint32_t detail1)
{
    if (phase == 0U || phase > 65U) {
        return;
    }

    taskENTER_CRITICAL();
    if (phase == 1U) {
        hostapd_lifecycle_initialize_unlocked();
    } else if (!hostapd_lifecycle_is_valid()) {
        taskEXIT_CRITICAL();
        return;
    }

    if (phase <= 32U) {
        persistent_hostapd_lifecycle.phase_mask |= 1U << (phase - 1U);
    }
    persistent_hostapd_lifecycle.last_phase = phase;
    persistent_hostapd_lifecycle.last_result = static_cast<int8_t>(result);
    persistent_hostapd_lifecycle.iface_state = iface_state;
    persistent_hostapd_lifecycle.wait_channel_update = wait_channel_update;
    persistent_hostapd_lifecycle.detail0 = detail0;
    persistent_hostapd_lifecycle.detail1 = detail1;
    if (phase == 39U) {
        persistent_hostapd_lifecycle.deauthenticate_reason =
            static_cast<uint16_t>(detail1);
    }
    if (phase == 40U) {
        persistent_hostapd_lifecycle.disable_network_caller = detail0;
        persistent_hostapd_lifecycle.disable_network_id =
            static_cast<int16_t>(detail1);
    }
    if (phase == 41U) {
        persistent_hostapd_lifecycle.ap_stop_caller = detail0;
    }
    ++persistent_hostapd_lifecycle.update_count;
    WifiApStartHostapdLifecyclePersistentRecord &record =
        persistent_hostapd_lifecycle.records[
            persistent_hostapd_lifecycle.write_index];
    record.phase = phase;
    record.result = static_cast<int8_t>(result);
    record.iface_state = iface_state;
    record.wait_channel_update = wait_channel_update;
    record.detail0 = detail0;
    persistent_hostapd_lifecycle.write_index = static_cast<uint8_t>(
        (persistent_hostapd_lifecycle.write_index + 1U) %
        kHostapdLifecycleCapacity);
    if (persistent_hostapd_lifecycle.record_count <
        kHostapdLifecycleCapacity) {
        ++persistent_hostapd_lifecycle.record_count;
    }
    __asm__ volatile("" ::: "memory");
    persistent_hostapd_lifecycle.magic = kHostapdLifecycleMagic;
    taskEXIT_CRITICAL();
}

void wifi_ap_start_diagnostic_record_bridge_stop_softap_caller(
    uint32_t caller)
{
    taskENTER_CRITICAL();
    if (!hostapd_lifecycle_is_valid()) {
        hostapd_lifecycle_initialize_unlocked();
    }
    persistent_hostapd_lifecycle.bridge_stop_softap_caller = caller;
    ++persistent_hostapd_lifecycle.update_count;
    __asm__ volatile("" ::: "memory");
    persistent_hostapd_lifecycle.magic = kHostapdLifecycleMagic;
    taskEXIT_CRITICAL();
}

extern "C" void fhost_bridge_ap_start_hostapd_state_update(
    uint8_t hostapd_state,
    uint8_t enabled,
    uint8_t raw_state_before,
    uint8_t vif_state_before,
    uint8_t raw_state_after,
    uint8_t vif_state_after)
{
    taskENTER_CRITICAL();
    if (!hostapd_state_update_is_valid()) {
        hostapd_state_update_initialize_unlocked();
    }

    WifiApStartHostapdStateUpdatePersistentRecord &record =
        persistent_hostapd_state_update.records[
            persistent_hostapd_state_update.write_index];
    record.sequence = persistent_hostapd_state_update.next_sequence++;
    record.hostapd_state = hostapd_state;
    record.enabled = enabled;
    record.raw_state_before = raw_state_before;
    record.vif_state_before = vif_state_before;
    record.raw_state_after = raw_state_after;
    record.vif_state_after = vif_state_after;
    persistent_hostapd_state_update.write_index = static_cast<uint8_t>(
        (persistent_hostapd_state_update.write_index + 1U) %
        kHostapdStateUpdateCapacity);
    if (persistent_hostapd_state_update.record_count <
        kHostapdStateUpdateCapacity) {
        ++persistent_hostapd_state_update.record_count;
    }
    __asm__ volatile("" ::: "memory");
    persistent_hostapd_state_update.magic = kHostapdStateUpdateMagic;
    taskEXIT_CRITICAL();
}

extern "C" void fhost_bridge_ap_start_status_code(
    uint8_t phase,
    int fhost_vif_idx,
    uint8_t message_kind,
    uint16_t message_length,
    uint8_t raw_state,
    uint8_t vif_state,
    int result)
{
    if (phase == 0U || phase > 8U || fhost_vif_idx != MGMR_VIF_AP) {
        return;
    }

    taskENTER_CRITICAL();
    if (phase == 1U) {
        status_code_initialize_unlocked();
    } else if (!status_code_is_valid()) {
        taskEXIT_CRITICAL();
        return;
    }

    persistent_status_code.phase_mask |= static_cast<uint16_t>(
        1U << (phase - 1U));
    persistent_status_code.last_phase = phase;
    persistent_status_code.vif = static_cast<int8_t>(fhost_vif_idx);
    persistent_status_code.message_kind = message_kind;
    persistent_status_code.message_length = message_length;
    /*
     * Only phase 8 has a post-transition FHOST VIF snapshot. Preserve it
     * across later callback/enqueue phases instead of overwriting it with the
     * 0xff sentinel used before the control task handles a message.
     */
    if (phase == 8U) {
        persistent_status_code.raw_state = raw_state;
        persistent_status_code.vif_state = vif_state;
    }
    persistent_status_code.last_result = static_cast<int8_t>(result);
    ++persistent_status_code.update_count;
    __asm__ volatile("" ::: "memory");
    persistent_status_code.magic = kStatusCodeMagic;
    taskEXIT_CRITICAL();
}

extern "C" void fhost_bridge_ap_start_eloop_post(
    uint8_t phase,
    uint32_t body,
    uint32_t body_length,
    uint8_t event_type,
    uint8_t request_matches,
    uint32_t request,
    uint32_t request_word)
{
    if (phase == 0U || phase > 11U) {
        return;
    }

    taskENTER_CRITICAL();
    if (!eloop_post_is_valid() && phase != 1U) {
        taskEXIT_CRITICAL();
        return;
    }
    if (phase == 1U) {
        eloop_post_initialize_unlocked();
    } else if (persistent_eloop_post.caller_body != body) {
        taskEXIT_CRITICAL();
        return;
    }

    persistent_eloop_post.last_phase = phase;
    ++persistent_eloop_post.update_count;
    if (phase == 1U) {
        persistent_eloop_post.caller_body = body;
        persistent_eloop_post.caller_body_length = body_length;
        persistent_eloop_post.caller_event_type = event_type;
        persistent_eloop_post.caller_request_matches = request_matches;
    } else {
        persistent_eloop_post.callee_body = body;
        persistent_eloop_post.callee_body_length = body_length;
        persistent_eloop_post.callee_event_type = event_type;
        persistent_eloop_post.callee_ifname_prefix_matches =
            phase >= 3U ? 1U : 0U;
        persistent_eloop_post.callee_enable_network_matches =
            request_matches;
        persistent_eloop_post.callee_request = request;
        persistent_eloop_post.callee_request_word = request_word;
    }
    __asm__ volatile("" ::: "memory");
    persistent_eloop_post.magic = kEloopPostMagic;
    taskEXIT_CRITICAL();
}

bool runtime_allocation_snapshot(uint8_t page, RuntimeAllocationRow *rows)
{
#if defined(BL616CL_RUNTIME_ALLOCATION_DIAGNOSTIC) && BL616CL_RUNTIME_ALLOCATION_DIAGNOSTIC
    const uintptr_t flags = bflb_irq_save();
    memcpy(rows, &runtime_allocations.rows[page == 40U ? 0U : 3U],
           3U * sizeof(RuntimeAllocationRow));
    bflb_irq_restore(flags);
    return true;
#else
    (void)page;
    memset(rows, 0, 3U * sizeof(RuntimeAllocationRow));
    return false;
#endif
}

extern "C" void fhost_bridge_ap_start_allocator_event(
    uint8_t stage,
    uint32_t value0,
    uint32_t value1,
    uint32_t value2)
{
    /*
     * MM invokes this hook both before and after its own interrupt lock. The
     * raw IRQ save/restore pair preserves that state instead of interacting
     * with FreeRTOS critical-section nesting.
     */
    const uintptr_t irq_flags = bflb_irq_save();
#if defined(BL616CL_RUNTIME_ALLOCATION_DIAGNOSTIC) && BL616CL_RUNTIME_ALLOCATION_DIAGNOSTIC
    runtime_allocations.event(stage, value0, value1, value2);
#endif
    if (diagnostic.fhost_stage == 250U &&
        (stage == 19U || stage == 20U)) {
        if (!allocator_trace_is_valid()) {
            allocator_trace_initialize_unlocked();
        }

        WifiApStartAllocatorTracePersistentRecord &record =
            persistent_allocator_trace.records[
                persistent_allocator_trace.write_index];
        record.sequence = persistent_allocator_trace.next_sequence++;
        record.stage = stage;
        record.reserved[0] = 0U;
        record.reserved[1] = 0U;
        record.reserved[2] = 0U;
        record.value0 = value0;
        record.value1 = value1;
        record.value2 = value2;
        persistent_allocator_trace.fhost_stage = diagnostic.fhost_stage;
        persistent_allocator_trace.write_index = static_cast<uint8_t>(
            (persistent_allocator_trace.write_index + 1U) %
            kAllocatorTraceCapacity);
        if (persistent_allocator_trace.record_count <
            kAllocatorTraceCapacity) {
            ++persistent_allocator_trace.record_count;
        }
        __asm__ volatile("" ::: "memory");
        persistent_allocator_trace.magic = kAllocatorTraceMagic;
    }
    bflb_irq_restore(irq_flags);
}

WifiApStartDiagnostic wifi_ap_start_diagnostic_snapshot()
{
    WifiApStartDiagnostic snapshot = {};
    taskENTER_CRITICAL();
    if (persistent_is_valid()) {
        snapshot.stage = static_cast<WifiApStartDiagnosticStage>(
            persistent_diagnostic.stage);
        snapshot.native_result = persistent_diagnostic.native_result;
        snapshot.sta_vif_state = persistent_diagnostic.sta_vif_state;
        snapshot.ap_vif_state = persistent_diagnostic.ap_vif_state;
        snapshot.sta_connected = persistent_diagnostic.sta_connected;
        snapshot.ap_started = persistent_diagnostic.ap_started;
        snapshot.requested_channel = persistent_diagnostic.requested_channel;
        snapshot.sta_channel_result = persistent_diagnostic.sta_channel_result;
        snapshot.sta_channel = persistent_diagnostic.sta_channel;
        snapshot.fhost_stage = persistent_diagnostic.fhost_stage;
        snapshot.fhost_result = persistent_diagnostic.fhost_result;
        snapshot.attempts = persistent_diagnostic.attempts;
        snapshot.started_at_ms = persistent_diagnostic.started_at_ms;
        snapshot.completed_at_ms = persistent_diagnostic.completed_at_ms;
        snapshot.fhost_chan_address = persistent_diagnostic.fhost_chan_address;
        snapshot.fhost_handler_chan_address =
            persistent_diagnostic.fhost_handler_chan_address;
        snapshot.fhost_response_source_chan_address =
            persistent_diagnostic.fhost_response_source_chan_address;
        snapshot.fhost_response_copy_chan_address =
            persistent_diagnostic.fhost_response_copy_chan_address;
        snapshot.fhost_response_received_chan_address =
            persistent_diagnostic.fhost_response_received_chan_address;
        snapshot.wpa_feature_chan_address =
            persistent_diagnostic.wpa_feature_chan_address;
        snapshot.fhost_chan2g4_count = persistent_diagnostic.fhost_chan2g4_count;
        snapshot.fhost_chan5g_count = persistent_diagnostic.fhost_chan5g_count;
        snapshot.heap_probe_stage = persistent_diagnostic.heap_probe_stage;
        snapshot.heap_request_size = persistent_diagnostic.heap_request_size;
        snapshot.heap_total_before = persistent_diagnostic.heap_total_before;
        snapshot.heap_ocram_before = persistent_diagnostic.heap_ocram_before;
        snapshot.heap_wram_before = persistent_diagnostic.heap_wram_before;
        snapshot.heap_allocation_address =
            persistent_diagnostic.heap_allocation_address;
        snapshot.heap_total_after = persistent_diagnostic.heap_total_after;
        snapshot.heap_ocram_after = persistent_diagnostic.heap_ocram_after;
        snapshot.heap_wram_after = persistent_diagnostic.heap_wram_after;
        snapshot.heap_malloc_failed = persistent_diagnostic.heap_malloc_failed;
        snapshot.fhost_command = persistent_diagnostic.fhost_command;
        snapshot.fhost_command_phase =
            persistent_diagnostic.fhost_command_phase;
        snapshot.fhost_command_vif = persistent_diagnostic.fhost_command_vif;
        snapshot.fhost_command_wpa_state =
            persistent_diagnostic.fhost_command_wpa_state;
        snapshot.fhost_command_network_id =
            persistent_diagnostic.fhost_command_network_id;
        snapshot.fhost_command_sequence =
            persistent_diagnostic.fhost_command_sequence;
        snapshot.fhost_sync_phase = persistent_diagnostic.fhost_sync_phase;
        snapshot.fhost_sync_wait_result =
            persistent_diagnostic.fhost_sync_wait_result;
        snapshot.fhost_sync_signal_result =
            persistent_diagnostic.fhost_sync_signal_result;
        snapshot.fhost_sync_wait_count_before =
            persistent_diagnostic.fhost_sync_wait_count_before;
        snapshot.fhost_sync_callback_count_before =
            persistent_diagnostic.fhost_sync_callback_count_before;
        snapshot.fhost_sync_callback_count_after =
            persistent_diagnostic.fhost_sync_callback_count_after;
        snapshot.fhost_sync_wait_count_after =
            persistent_diagnostic.fhost_sync_wait_count_after;
        snapshot.fhost_sync_wait_handle =
            persistent_diagnostic.fhost_sync_wait_handle;
        snapshot.fhost_sync_callback_handle =
            persistent_diagnostic.fhost_sync_callback_handle;
        snapshot.fhost_sync_update_count =
            persistent_diagnostic.fhost_sync_update_count;
        snapshot.fhost_task_handle = persistent_diagnostic.fhost_task_handle;
        snapshot.fhost_task_state = persistent_diagnostic.fhost_task_state;
        snapshot.fhost_task_stack_high_watermark =
            persistent_diagnostic.fhost_task_stack_high_watermark;
        snapshot.liveness_phase = persistent_diagnostic.liveness_phase;
        snapshot.liveness_event_count =
            persistent_diagnostic.liveness_event_count;
        snapshot.persisted = true;
    } else {
        snapshot.stage = diagnostic.stage;
        snapshot.native_result = diagnostic.native_result;
        snapshot.sta_vif_state = diagnostic.sta_vif_state;
        snapshot.ap_vif_state = diagnostic.ap_vif_state;
        snapshot.sta_connected = diagnostic.sta_connected;
        snapshot.ap_started = diagnostic.ap_started;
        snapshot.requested_channel = diagnostic.requested_channel;
        snapshot.sta_channel_result = diagnostic.sta_channel_result;
        snapshot.sta_channel = diagnostic.sta_channel;
        snapshot.fhost_stage = diagnostic.fhost_stage;
        snapshot.fhost_result = diagnostic.fhost_result;
        snapshot.attempts = diagnostic.attempts;
        snapshot.started_at_ms = diagnostic.started_at_ms;
        snapshot.completed_at_ms = diagnostic.completed_at_ms;
        snapshot.fhost_chan_address = diagnostic.fhost_chan_address;
        snapshot.fhost_handler_chan_address =
            diagnostic.fhost_handler_chan_address;
        snapshot.fhost_response_source_chan_address =
            diagnostic.fhost_response_source_chan_address;
        snapshot.fhost_response_copy_chan_address =
            diagnostic.fhost_response_copy_chan_address;
        snapshot.fhost_response_received_chan_address =
            diagnostic.fhost_response_received_chan_address;
        snapshot.wpa_feature_chan_address = diagnostic.wpa_feature_chan_address;
        snapshot.fhost_chan2g4_count = diagnostic.fhost_chan2g4_count;
        snapshot.fhost_chan5g_count = diagnostic.fhost_chan5g_count;
        snapshot.heap_probe_stage = diagnostic.heap_probe_stage;
        snapshot.heap_request_size = diagnostic.heap_request_size;
        snapshot.heap_total_before = diagnostic.heap_total_before;
        snapshot.heap_ocram_before = diagnostic.heap_ocram_before;
        snapshot.heap_wram_before = diagnostic.heap_wram_before;
        snapshot.heap_allocation_address = diagnostic.heap_allocation_address;
        snapshot.heap_total_after = diagnostic.heap_total_after;
        snapshot.heap_ocram_after = diagnostic.heap_ocram_after;
        snapshot.heap_wram_after = diagnostic.heap_wram_after;
        snapshot.heap_malloc_failed = diagnostic.heap_malloc_failed;
        snapshot.fhost_command = diagnostic.fhost_command;
        snapshot.fhost_command_phase = diagnostic.fhost_command_phase;
        snapshot.fhost_command_vif = diagnostic.fhost_command_vif;
        snapshot.fhost_command_wpa_state = diagnostic.fhost_command_wpa_state;
        snapshot.fhost_command_network_id = diagnostic.fhost_command_network_id;
        snapshot.fhost_command_sequence = diagnostic.fhost_command_sequence;
        snapshot.fhost_sync_phase = diagnostic.fhost_sync_phase;
        snapshot.fhost_sync_wait_result = diagnostic.fhost_sync_wait_result;
        snapshot.fhost_sync_signal_result = diagnostic.fhost_sync_signal_result;
        snapshot.fhost_sync_wait_count_before =
            diagnostic.fhost_sync_wait_count_before;
        snapshot.fhost_sync_callback_count_before =
            diagnostic.fhost_sync_callback_count_before;
        snapshot.fhost_sync_callback_count_after =
            diagnostic.fhost_sync_callback_count_after;
        snapshot.fhost_sync_wait_count_after =
            diagnostic.fhost_sync_wait_count_after;
        snapshot.fhost_sync_wait_handle = diagnostic.fhost_sync_wait_handle;
        snapshot.fhost_sync_callback_handle =
            diagnostic.fhost_sync_callback_handle;
        snapshot.fhost_sync_update_count = diagnostic.fhost_sync_update_count;
        snapshot.fhost_task_handle = diagnostic.fhost_task_handle;
        snapshot.fhost_task_state = diagnostic.fhost_task_state;
        snapshot.fhost_task_stack_high_watermark =
            diagnostic.fhost_task_stack_high_watermark;
        snapshot.liveness_phase = diagnostic.liveness_phase;
        snapshot.liveness_event_count = diagnostic.liveness_event_count;
        snapshot.persisted = false;
    }
    taskEXIT_CRITICAL();
    if (snapshot.started_at_ms != 0U) {
        const uint32_t finished_at = snapshot.completed_at_ms != 0U
            ? snapshot.completed_at_ms
            : millis();
        snapshot.elapsed_ms = finished_at - snapshot.started_at_ms;
    }

    /*
     * The persistent record intentionally survives a watchdog reset, but its
     * VIF fields describe the instant AP start was requested. Refresh the
     * returned snapshot from FHOST so host-side telemetry reports whether
     * AP+STA is currently active while the radio is running.
     */
    const int sta_vif_state = wifi_mgmr_sta_state_get_ext();
    const int ap_vif_state = wifi_mgmr_ap_state_get_ext();
    snapshot.sta_vif_state = sta_vif_state >= 0 && sta_vif_state <= 255
        ? static_cast<uint8_t>(sta_vif_state)
        : 0xffU;
    snapshot.ap_vif_state = ap_vif_state >= 0 && ap_vif_state <= 255
        ? static_cast<uint8_t>(ap_vif_state)
        : 0xffU;
    snapshot.sta_connected = wifi_mgmr_sta_state_get() != 0 ? 1U : 0U;
    snapshot.ap_started = wifi_mgmr_ap_state_get() != 0 ? 1U : 0U;

    uint8_t sta_channel = 0U;
    const int sta_channel_result = wifi_mgmr_sta_channel_get(&sta_channel);
    snapshot.sta_channel_result = static_cast<int8_t>(sta_channel_result);
    snapshot.sta_channel = sta_channel_result == 0 ? sta_channel : 0U;
    return snapshot;
}

WifiApStartTrace wifi_ap_start_trace_snapshot()
{
    WifiApStartTrace snapshot = {};
    taskENTER_CRITICAL();
    if (trace_is_valid()) {
        snapshot.next_sequence = persistent_trace.next_sequence;
        snapshot.record_count = persistent_trace.record_count > 3U
            ? 3U
            : persistent_trace.record_count;
        const uint8_t first =
            static_cast<uint8_t>((persistent_trace.write_index +
                                  kTraceCapacity -
                                  snapshot.record_count) %
                                 kTraceCapacity);
        for (uint8_t index = 0U; index < snapshot.record_count; ++index) {
            const WifiApStartTracePersistentRecord &record =
                persistent_trace.records[(first + index) % kTraceCapacity];
            snapshot.records[index].sequence = record.sequence;
            snapshot.records[index].event =
                static_cast<WifiApStartTraceEvent>(record.event);
            snapshot.records[index].result = record.result;
            snapshot.records[index].semaphore = record.semaphore;
            snapshot.records[index].count = record.count;
        }
        snapshot.persisted = true;
    }
    taskEXIT_CRITICAL();
    return snapshot;
}

WifiApStartNotificationDiagnostic wifi_ap_start_notification_snapshot()
{
    WifiApStartNotificationDiagnostic snapshot = {};
    taskENTER_CRITICAL();
    if (notification_is_valid()) {
        snapshot.attempt_count = persistent_notification.attempt_count;
        snapshot.last_sequence = persistent_notification.last_sequence;
        snapshot.waiting_task = persistent_notification.waiting_task;
        snapshot.event_mask = persistent_notification.event_mask;
        snapshot.last_event =
            static_cast<WifiApStartTraceEvent>(persistent_notification.last_event);
        snapshot.notification_result =
            persistent_notification.notification_result;
        snapshot.wait_result = persistent_notification.wait_result;
        snapshot.persisted = true;
    }
    taskEXIT_CRITICAL();
    return snapshot;
}

WifiApStartNotificationAbDiagnostic wifi_ap_start_notification_ab_snapshot()
{
    WifiApStartNotificationAbDiagnostic snapshot = {};
    taskENTER_CRITICAL();
    if (notification_ab_is_valid()) {
        snapshot.phase_mask = persistent_notification_ab.phase_mask;
        snapshot.last_phase = persistent_notification_ab.last_phase;
        snapshot.request_matches = persistent_notification_ab.request_matches;
        snapshot.setup_use_task_notification =
            persistent_notification_ab.setup_use_task_notification;
        snapshot.queued_use_task_notification =
            persistent_notification_ab.queued_use_task_notification;
        snapshot.callback_use_task_notification =
            persistent_notification_ab.callback_use_task_notification;
        snapshot.wait_use_task_notification =
            persistent_notification_ab.wait_use_task_notification;
        snapshot.last_result = persistent_notification_ab.last_result;
        snapshot.update_count = persistent_notification_ab.update_count;
        snapshot.setup_message = persistent_notification_ab.setup_message;
        snapshot.queued_message = persistent_notification_ab.queued_message;
        snapshot.callback_message = persistent_notification_ab.callback_message;
        snapshot.wait_message = persistent_notification_ab.wait_message;
        snapshot.setup_waiting_task =
            persistent_notification_ab.setup_waiting_task;
        snapshot.queued_waiting_task =
            persistent_notification_ab.queued_waiting_task;
        snapshot.callback_waiting_task =
            persistent_notification_ab.callback_waiting_task;
        snapshot.wait_waiting_task =
            persistent_notification_ab.wait_waiting_task;
        snapshot.persisted = true;
    }
    taskEXIT_CRITICAL();
    return snapshot;
}

WifiApStartWpaWaitDiagnostic wifi_ap_start_wpa_wait_snapshot()
{
    WifiApStartWpaWaitDiagnostic snapshot = {};
    taskENTER_CRITICAL();
    if (wpa_wait_is_valid()) {
        snapshot.phase_mask = persistent_wpa_wait.phase_mask;
        snapshot.last_phase = persistent_wpa_wait.last_phase;
        snapshot.last_result = persistent_wpa_wait.last_result;
        snapshot.vif = persistent_wpa_wait.vif;
        snapshot.expected_event = persistent_wpa_wait.expected_event;
        snapshot.observed_event = persistent_wpa_wait.observed_event;
        snapshot.wpa_state = persistent_wpa_wait.wpa_state;
        snapshot.update_count = persistent_wpa_wait.update_count;
        snapshot.target = persistent_wpa_wait.target;
        snapshot.waiting_task = persistent_wpa_wait.waiting_task;
        snapshot.callback_task = persistent_wpa_wait.callback_task;
        snapshot.persisted = true;
    }
    taskEXIT_CRITICAL();
    return snapshot;
}

WifiApStartWpaEventPathDiagnostic wifi_ap_start_wpa_event_path_snapshot()
{
    WifiApStartWpaEventPathDiagnostic snapshot = {};
    taskENTER_CRITICAL();
    if (wpa_event_path_is_valid()) {
        snapshot.phase_mask = persistent_wpa_event_path.phase_mask;
        snapshot.last_phase = persistent_wpa_event_path.last_phase;
        snapshot.vif = persistent_wpa_event_path.vif;
        snapshot.event = persistent_wpa_event_path.event;
        snapshot.fhost_wpa_state = persistent_wpa_event_path.fhost_wpa_state;
        snapshot.operstate = persistent_wpa_event_path.operstate;
        snapshot.old_wpa_state = persistent_wpa_event_path.old_wpa_state;
        snapshot.new_wpa_state = persistent_wpa_event_path.new_wpa_state;
        snapshot.last_result = persistent_wpa_event_path.last_result;
        snapshot.callback_slot = persistent_wpa_event_path.callback_slot;
        snapshot.update_count = persistent_wpa_event_path.update_count;
        snapshot.callback_slot_mask =
            persistent_wpa_event_path.callback_slot_mask;
        snapshot.persisted = true;
    }
    taskEXIT_CRITICAL();
    return snapshot;
}

WifiApStartCreateApDiagnostic wifi_ap_start_create_ap_snapshot()
{
    WifiApStartCreateApDiagnostic snapshot = {};
    taskENTER_CRITICAL();
    if (create_ap_is_valid()) {
        snapshot.phase_mask = persistent_create_ap.phase_mask;
        snapshot.last_phase = persistent_create_ap.last_phase;
        snapshot.last_result = persistent_create_ap.last_result;
        snapshot.wpa_state = persistent_create_ap.wpa_state;
        snapshot.detail = persistent_create_ap.detail;
        snapshot.update_count = persistent_create_ap.update_count;
        snapshot.persisted = true;
    }
    taskEXIT_CRITICAL();
    return snapshot;
}

WifiApStartHostapdLifecycleDiagnostic
wifi_ap_start_hostapd_lifecycle_snapshot()
{
    WifiApStartHostapdLifecycleDiagnostic snapshot = {};

    taskENTER_CRITICAL();
    if (hostapd_lifecycle_is_valid()) {
        snapshot.phase_mask = persistent_hostapd_lifecycle.phase_mask;
        snapshot.last_phase = persistent_hostapd_lifecycle.last_phase;
        snapshot.last_result = persistent_hostapd_lifecycle.last_result;
        snapshot.iface_state = persistent_hostapd_lifecycle.iface_state;
        snapshot.wait_channel_update =
            persistent_hostapd_lifecycle.wait_channel_update;
        snapshot.detail0 = persistent_hostapd_lifecycle.detail0;
        snapshot.detail1 = persistent_hostapd_lifecycle.detail1;
        snapshot.disable_network_caller =
            persistent_hostapd_lifecycle.disable_network_caller;
        snapshot.ap_stop_caller = persistent_hostapd_lifecycle.ap_stop_caller;
        snapshot.bridge_stop_softap_caller =
            persistent_hostapd_lifecycle.bridge_stop_softap_caller;
        snapshot.disable_network_id =
            persistent_hostapd_lifecycle.disable_network_id;
        snapshot.deauthenticate_reason =
            persistent_hostapd_lifecycle.deauthenticate_reason;
        snapshot.update_count = persistent_hostapd_lifecycle.update_count;
        snapshot.record_count = persistent_hostapd_lifecycle.record_count;
        const uint8_t first = static_cast<uint8_t>(
            (persistent_hostapd_lifecycle.write_index +
             kHostapdLifecycleCapacity -
             snapshot.record_count) %
            kHostapdLifecycleCapacity);
        for (uint8_t index = 0U; index < snapshot.record_count; ++index) {
            const WifiApStartHostapdLifecyclePersistentRecord &record =
                persistent_hostapd_lifecycle.records[
                    (first + index) % kHostapdLifecycleCapacity];
            WifiApStartHostapdLifecycleDiagnostic::Record &target =
                snapshot.records[index];
            target.phase = record.phase;
            target.result = record.result;
            target.iface_state = record.iface_state;
            target.wait_channel_update = record.wait_channel_update;
            target.detail0 = record.detail0;
        }
        snapshot.persisted = true;
    }
    taskEXIT_CRITICAL();

    return snapshot;
}

WifiApStartHostapdStateUpdateDiagnostic
wifi_ap_start_hostapd_state_update_snapshot()
{
    WifiApStartHostapdStateUpdateDiagnostic snapshot = {};

    taskENTER_CRITICAL();
    if (hostapd_state_update_is_valid()) {
        snapshot.next_sequence = persistent_hostapd_state_update.next_sequence;
        snapshot.record_count =
            persistent_hostapd_state_update.record_count > 3U
            ? 3U
            : persistent_hostapd_state_update.record_count;
        const uint8_t first = static_cast<uint8_t>(
            (persistent_hostapd_state_update.write_index +
             kHostapdStateUpdateCapacity -
             snapshot.record_count) %
            kHostapdStateUpdateCapacity);
        for (uint8_t index = 0U; index < snapshot.record_count; ++index) {
            const WifiApStartHostapdStateUpdatePersistentRecord &record =
                persistent_hostapd_state_update.records[
                    (first + index) % kHostapdStateUpdateCapacity];
            WifiApStartHostapdStateUpdateRecord &target =
                snapshot.records[index];
            target.sequence = record.sequence;
            target.hostapd_state = record.hostapd_state;
            target.enabled = record.enabled;
            target.raw_state_before = record.raw_state_before;
            target.vif_state_before = record.vif_state_before;
            target.raw_state_after = record.raw_state_after;
            target.vif_state_after = record.vif_state_after;
        }
        snapshot.persisted = true;
    }
    taskEXIT_CRITICAL();

    return snapshot;
}

WifiApStartStatusCodeDiagnostic wifi_ap_start_status_code_snapshot()
{
    WifiApStartStatusCodeDiagnostic snapshot = {};

    taskENTER_CRITICAL();
    if (status_code_is_valid()) {
        snapshot.phase_mask = persistent_status_code.phase_mask;
        snapshot.last_phase = persistent_status_code.last_phase;
        snapshot.vif = persistent_status_code.vif;
        snapshot.message_kind = persistent_status_code.message_kind;
        snapshot.message_length = persistent_status_code.message_length;
        snapshot.raw_state = persistent_status_code.raw_state;
        snapshot.vif_state = persistent_status_code.vif_state;
        snapshot.last_result = persistent_status_code.last_result;
        snapshot.update_count = persistent_status_code.update_count;
        snapshot.persisted = true;
    }
    taskEXIT_CRITICAL();

    return snapshot;
}

WifiApStartEloopPostDiagnostic wifi_ap_start_eloop_post_snapshot()
{
    WifiApStartEloopPostDiagnostic snapshot = {};
    taskENTER_CRITICAL();
    if (eloop_post_is_valid()) {
        snapshot.last_phase = persistent_eloop_post.last_phase;
        snapshot.caller_event_type = persistent_eloop_post.caller_event_type;
        snapshot.callee_event_type = persistent_eloop_post.callee_event_type;
        snapshot.caller_request_matches =
            persistent_eloop_post.caller_request_matches;
        snapshot.callee_ifname_prefix_matches =
            persistent_eloop_post.callee_ifname_prefix_matches;
        snapshot.callee_enable_network_matches =
            persistent_eloop_post.callee_enable_network_matches;
        snapshot.caller_body = persistent_eloop_post.caller_body;
        snapshot.caller_body_length = persistent_eloop_post.caller_body_length;
        snapshot.callee_body = persistent_eloop_post.callee_body;
        snapshot.callee_body_length = persistent_eloop_post.callee_body_length;
        snapshot.callee_request = persistent_eloop_post.callee_request;
        snapshot.callee_request_word = persistent_eloop_post.callee_request_word;
        snapshot.update_count = persistent_eloop_post.update_count;
        snapshot.persisted = true;
    }
    taskEXIT_CRITICAL();
    return snapshot;
}

WifiApStartAllocatorTrace wifi_ap_start_allocator_trace_snapshot()
{
    WifiApStartAllocatorTrace snapshot = {};
    taskENTER_CRITICAL();
    if (allocator_trace_is_valid()) {
        snapshot.fhost_stage = persistent_allocator_trace.fhost_stage;
        snapshot.next_sequence = persistent_allocator_trace.next_sequence;
        snapshot.record_count = persistent_allocator_trace.record_count > 3U
            ? 3U
            : persistent_allocator_trace.record_count;
        const uint8_t first = static_cast<uint8_t>(
            (persistent_allocator_trace.write_index +
             kAllocatorTraceCapacity -
             snapshot.record_count) %
            kAllocatorTraceCapacity);
        for (uint8_t index = 0U; index < snapshot.record_count; ++index) {
            const WifiApStartAllocatorTracePersistentRecord &record =
                persistent_allocator_trace.records[
                    (first + index) % kAllocatorTraceCapacity];
            snapshot.records[index].sequence = record.sequence;
            snapshot.records[index].stage = record.stage;
            snapshot.records[index].value0 = record.value0;
            snapshot.records[index].value1 = record.value1;
            snapshot.records[index].value2 = record.value2;
        }
        snapshot.persisted = true;
    }
    taskEXIT_CRITICAL();
    return snapshot;
}

WifiApStartAllocatorHeapWalk wifi_ap_start_allocator_heap_walk_snapshot()
{
    WifiApStartAllocatorHeapWalk snapshot = {};
    taskENTER_CRITICAL();
    if (allocator_heap_walk_is_valid()) {
        snapshot.result = persistent_allocator_heap_walk.result;
        snapshot.record_count = persistent_allocator_heap_walk.record_count;
        for (uint8_t index = 0U; index < snapshot.record_count; ++index) {
            const WifiApStartAllocatorHeapWalkPersistentRecord &record =
                persistent_allocator_heap_walk.records[index];
            WifiApStartAllocatorHeapWalkRecord &target =
                snapshot.records[index];
            target.heap_id = record.heap_id;
            target.allocated_block_count = record.allocated_block_count;
            target.allocated_bytes = record.allocated_bytes;
            memcpy(target.largest_blocks, record.largest_blocks,
                   sizeof(target.largest_blocks));
        }
        snapshot.persisted = true;
    }
    taskEXIT_CRITICAL();
    return snapshot;
}

WifiApStartWatchdogDiagnostic wifi_ap_start_watchdog_snapshot()
{
    WifiApStartWatchdogDiagnostic snapshot = {};
#if defined(BL616CL_AP_START_DIAGNOSTIC_WATCHDOG) && \
    BL616CL_AP_START_DIAGNOSTIC_WATCHDOG
    snapshot.enabled = true;
#endif
    taskENTER_CRITICAL();
    if (watchdog_is_valid()) {
        snapshot.state =
            static_cast<WifiApStartWatchdogState>(persistent_watchdog.state);
        snapshot.reset_reason = persistent_watchdog.reset_reason;
        snapshot.timeout_ms = persistent_watchdog.timeout_ms;
        snapshot.arm_count = persistent_watchdog.arm_count;
        snapshot.persisted = true;
    }
    taskEXIT_CRITICAL();
    return snapshot;
}

WifiApStartUartRecoveryDiagnostic wifi_ap_start_uart_recovery_snapshot()
{
    WifiApStartUartRecoveryDiagnostic snapshot = {};
    taskENTER_CRITICAL();
    if (uart_recovery_is_valid()) {
        snapshot.user_uart_before =
            to_uart_snapshot(persistent_uart_recovery.user_uart_before);
        snapshot.at_uart_before =
            to_uart_snapshot(persistent_uart_recovery.at_uart_before);
        snapshot.user_uart_after =
            to_uart_snapshot(persistent_uart_recovery.user_uart_after);
        snapshot.at_uart_after =
            to_uart_snapshot(persistent_uart_recovery.at_uart_after);
        snapshot.cdc_baud_before = persistent_uart_recovery.cdc_baud_before;
        snapshot.cdc_baud_after = persistent_uart_recovery.cdc_baud_after;
        snapshot.reset_reason = persistent_uart_recovery.reset_reason;
        snapshot.before_valid = persistent_uart_recovery.before_valid != 0U;
        snapshot.after_valid = persistent_uart_recovery.after_valid != 0U;
        snapshot.persisted = true;
    }
    taskEXIT_CRITICAL();
    return snapshot;
}

WifiApStartUartSnapshot wifi_ap_start_uart_live_snapshot(
    const HardwareSerial &serial)
{
    taskENTER_CRITICAL();
    const WifiApStartUartSnapshot snapshot =
        to_uart_snapshot(capture_uart_snapshot(serial));
    taskEXIT_CRITICAL();
    return snapshot;
}

} // namespace bridge

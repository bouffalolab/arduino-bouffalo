#include "ipv6_tcp_diagnostic.h"
#include "usb_cmsis_dap.h"

#include <string.h>

#include "Arduino.h"
#include "ap_packet_diagnostic.h"
#include "bl616cl_recovery_control.h"
#include "line_coding_request_gate.h"
#include "line_coding_request_queue.h"
#include "ra4_at_service.h"
#include "ra4_samba_programmer.h"
#include "ra4_samba_service.h"
#include "udp_socket_diagnostic.h"
#include "ra4_bootstrap_control.h"
#include "tcp_runtime_diagnostic.h"
#include "sta_tcp_diagnostic.h"
#include "mac_rx_diagnostic.h"
#include "lwip_allocation_diagnostic.h"
#include "controller_link_diagnostic.h"
#include "tcp_rx_native_policy.h"
#include "sta_rx_ownership.h"
#include "dhcp_tx_diagnostic.h"
#include "wifi_ap_start_diagnostic.h"
#include "runtime_allocation_diagnostic.h"
#include "runtime_heap_ownership.h"
#include "wifi_sta_disconnect_diagnostic.h"

extern "C" {
#include "freedap.h"
#include "sdk/mm/mm.h"
#include "usbd_cdc_acm.h"
#include "usbd_core.h"
#include "usbd_hid.h"
}

namespace bridge {
namespace {

constexpr uint8_t kUsbBusId = 0U;
constexpr uint16_t kVendorId = 0x2341U;
constexpr uint16_t kProductId = 0x1002U;
constexpr uint16_t kPacketSize = 64U;
constexpr uint16_t kCdcToUartCapacity = 512U;
constexpr uint8_t kGpio35FeatureCommand = 0xD3U;
constexpr uint8_t kCdcInTelemetryFeatureCommand = 0xD5U;
constexpr uint8_t kRa4SambaProbeFeatureCommand = 0xD6U;
constexpr uint8_t kGpio35FeatureVersion = 35U;
constexpr uint8_t kRa4ApplicationResetFeatureCommand = 0xD4U;
constexpr uint8_t kBl616clRecoveryFeatureCommand = 0xAAU;
constexpr uint8_t kUserUartRxPin = PIN_SERIAL_RX;

constexpr uint8_t kCdcInterruptEndpoint = 0x81U;
constexpr uint8_t kCdcOutEndpoint = 0x02U;
constexpr uint8_t kCdcInEndpoint = 0x83U;
constexpr uint8_t kHidOutEndpoint = 0x04U;
constexpr uint8_t kHidInEndpoint = 0x85U;

constexpr size_t kCdcDescriptorLength = CDC_ACM_DESCRIPTOR_LEN;
constexpr size_t kHidDescriptorLength = HID_CUSTOM_INOUT_DESCRIPTOR_LEN;
constexpr size_t kConfigurationDescriptorLength =
    9U + kCdcDescriptorLength + kHidDescriptorLength;

static const uint8_t kHidReportDescriptor[] = {
    0x06, 0x00, 0xFF, // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,       // Usage (0x01)
    0xA1, 0x01,       // Collection (Application)
    0x09, 0x02,       //   Usage (Input)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x40,       //   Report Count (64)
    0x81, 0x02,       //   Input (Data, Variable, Absolute)
    0x09, 0x03,       //   Usage (Output)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x40,       //   Report Count (64)
    0x91, 0x02,       //   Output (Data, Variable, Absolute)
    0x09, 0x04,       //   Usage (Feature)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x40,       //   Report Count (64)
    0xB1, 0x02,       //   Feature (Data, Variable, Absolute)
    0xC0,             // End Collection
};

static const uint8_t kDeviceDescriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(
        USB_2_0,
        0xEF,
        0x02,
        0x01,
        kVendorId,
        kProductId,
        0x0100,
        0x01),
};

static const uint8_t kConfigurationDescriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(
        kConfigurationDescriptorLength,
        0x03,
        0x01,
        USB_CONFIG_BUS_POWERED,
        100U),
    CDC_ACM_DESCRIPTOR_INIT(
        0x00,
        kCdcInterruptEndpoint,
        kCdcOutEndpoint,
        kCdcInEndpoint,
        kPacketSize,
        USB_STRING_PRODUCT_INDEX),
    HID_CUSTOM_INOUT_DESCRIPTOR_INIT(
        0x02,
        HID_SUBCLASS_NONE,
        sizeof(kHidReportDescriptor),
        kHidOutEndpoint,
        kHidInEndpoint,
        kPacketSize,
        0x01),
};

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t cdc_rx[kPacketSize];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t cdc_tx[kPacketSize];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t hid_rx[kPacketSize];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t hid_tx[kPacketSize];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t hid_feature[kPacketSize];

uint8_t cdc_to_uart[kCdcToUartCapacity];
volatile uint16_t cdc_to_uart_head = 0U;
volatile uint16_t cdc_to_uart_tail = 0U;
volatile bool hid_tx_busy = false;
volatile bool hid_rx_armed = false;
volatile bool usb_configured = false;
volatile bool cdc_rx_armed = false;
LineCodingRequestQueue line_coding_requests;
volatile bool ra4_application_reset_pending = false;
volatile bool ra4_samba_probe_pending = false;
volatile bool user_uart_bridge_enabled = true;
volatile uint32_t cdc_out_packet_count = 0U;
volatile uint32_t cdc_out_byte_count = 0U;
volatile uint32_t cdc_to_uart_drop_count = 0U;
volatile uint32_t uart_tx_byte_count = 0U;
volatile uint32_t uart_tx_failure_count = 0U;
volatile uint8_t line_coding_program_count = 0U;
volatile uint8_t line_coding_reset_count = 0U;
volatile uint8_t line_coding_normal_count = 0U;
volatile uint8_t line_coding_drop_count = 0U;
volatile uint32_t last_applied_line_coding_baud = 0U;
enum class CdcInState : uint8_t {
    kIdle = 0U,
    kDataInFlight = 1U,
    kZlpInFlight = 2U,
};

volatile CdcInState cdc_in_state = CdcInState::kIdle;
volatile uint16_t cdc_tx_pending_length = 0U;
volatile uint8_t diagnostic_feature_page = 0U;
volatile uint32_t cdc_in_submit_count = 0U;
volatile uint32_t cdc_in_submit_failure_count = 0U;
volatile uint32_t cdc_in_completion_count = 0U;
volatile uint32_t cdc_in_completion_byte_count = 0U;
volatile uint32_t cdc_in_recovery_count = 0U;
volatile uint32_t cdc_in_zlp_submit_count = 0U;
volatile uint32_t cdc_in_zlp_failure_count = 0U;
volatile uint32_t cdc_in_submit_byte_count = 0U;
volatile uint32_t usb_configured_generation = 0U;
volatile uint32_t usb_reset_event_count = 0U;
volatile uint32_t usb_disconnected_event_count = 0U;
volatile uint32_t usb_error_event_count = 0U;
volatile uint32_t usb_event_sequence = 0U;
volatile uint32_t hid_out_callback_count = 0U;
volatile uint32_t hid_out_invalid_packet_count = 0U;
volatile uint32_t hid_out_busy_drop_count = 0U;
volatile uint32_t hid_dap_request_count = 0U;
volatile uint32_t hid_in_submit_count = 0U;
volatile uint32_t hid_in_submit_failure_count = 0U;
volatile uint32_t hid_in_completion_count = 0U;
volatile uint32_t hid_read_rearm_count = 0U;
volatile uint32_t hid_read_rearm_failure_count = 0U;
volatile uint8_t hid_last_out_length = 0U;
volatile uint8_t hid_last_request_command = 0U;
volatile uint8_t hid_last_response_command = 0U;
volatile int8_t hid_last_in_submit_result = 0;
volatile int8_t hid_last_read_rearm_result = 0;
bool usb_initialized = false;
UserUartGpio35Control *user_uart_gpio35_control = nullptr;
Ra4BootstrapControl *ra4_bootstrap_control = nullptr;
Ra4AtService *ra4_at_service = nullptr;
Ra4SambaService *ra4_samba_service = nullptr;
Ra4SambaProgrammer *ra4_samba_programmer = nullptr;
struct cdc_line_coding cdc_current_line_coding = {
    .dwDTERate = 115200U,
    .bCharFormat = 0U,
    .bParityType = 0U,
    .bDataBits = 8U,
};

struct usbd_interface cdc_control_interface;
struct usbd_interface cdc_data_interface;
struct usbd_interface hid_interface;

const uint8_t *device_descriptor_callback(uint8_t)
{
    return kDeviceDescriptor;
}

const uint8_t *configuration_descriptor_callback(uint8_t)
{
    return kConfigurationDescriptor;
}

const char *string_descriptor_callback(uint8_t, uint8_t index)
{
    static const char kLanguageId[] = {0x09, 0x04};

    switch (index) {
    case USB_STRING_LANGID_INDEX:
        return kLanguageId;
    case USB_STRING_MFC_INDEX:
        return "Arduino";
    case USB_STRING_PRODUCT_INDEX:
        return "UNO WiFi R4 CMSIS-DAP";
    case USB_STRING_SERIAL_INDEX:
        return "BL616CL-PROVISIONAL";
    default:
        return nullptr;
    }
}

const struct usb_descriptor kDescriptors = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = configuration_descriptor_callback,
    .device_quality_descriptor_callback = nullptr,
    .other_speed_descriptor_callback = nullptr,
    .string_descriptor_callback = string_descriptor_callback,
    .msosv1_descriptor = nullptr,
    .msosv2_descriptor = nullptr,
    .webusb_url_descriptor = nullptr,
    .bos_descriptor = nullptr,
};

void rearm_cdc_read()
{
    // Reserve room for the whole next packet before accepting it. Leaving
    // OUT unarmed makes USB NAK the host instead of silently dropping bytes.
    const uintptr_t flags = mm_lock_save();
    const uint16_t used = (cdc_to_uart_head + kCdcToUartCapacity -
                           cdc_to_uart_tail) % kCdcToUartCapacity;
    if (usb_configured && !cdc_rx_armed &&
        kCdcToUartCapacity - 1U - used >= sizeof(cdc_rx)) {
        cdc_rx_armed = usbd_ep_start_read(
            kUsbBusId, kCdcOutEndpoint, cdc_rx, sizeof(cdc_rx)) == 0;
    }
    mm_unlock_restore(flags);
}

void rearm_hid_read()
{
    const int result =
        usbd_ep_start_read(kUsbBusId, kHidOutEndpoint, hid_rx, sizeof(hid_rx));
    ++hid_read_rearm_count;
    hid_last_read_rearm_result = static_cast<int8_t>(result);
    hid_rx_armed = result == 0;
    if (!hid_rx_armed) {
        ++hid_read_rearm_failure_count;
    }
}

bool enqueue_cdc_byte(uint8_t value)
{
    const uint16_t next = static_cast<uint16_t>(
        (cdc_to_uart_head + 1U) % kCdcToUartCapacity);
    if (next == cdc_to_uart_tail) {
        return false;
    }

    cdc_to_uart[cdc_to_uart_head] = value;
    cdc_to_uart_head = next;
    return true;
}

bool dequeue_cdc_byte(uint8_t &value)
{
    if (cdc_to_uart_tail == cdc_to_uart_head) {
        return false;
    }

    value = cdc_to_uart[cdc_to_uart_tail];
    cdc_to_uart_tail = static_cast<uint16_t>(
        (cdc_to_uart_tail + 1U) % kCdcToUartCapacity);
    return true;
}

void write_u32_le(uint8_t *destination, uint32_t value)
{
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8U);
    destination[2] = static_cast<uint8_t>(value >> 16U);
    destination[3] = static_cast<uint8_t>(value >> 24U);
}

void write_u16_le(uint8_t *destination, uint16_t value)
{
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8U);
}

void write_i32_le(uint8_t *destination, int value)
{
    write_u32_le(destination, static_cast<uint32_t>(value));
}

void refresh_user_uart_feature()
{
    const UserUartGpio35Status &status = user_uart_gpio35_control->status();
    hid_feature[5] = static_cast<uint8_t>(status.mode);
    hid_feature[6] =
        (status.input_enabled ? 0x01U : 0U) |
        (status.output_enabled ? 0x02U : 0U) |
        (status.expected ? 0x04U : 0U) |
        (status.pullup_enabled ? 0x08U : 0U);
    hid_feature[7] = status.pad_function;
    hid_feature[8] = status.uart_signal;
    hid_feature[9] = kUserUartRxPin;
    write_u32_le(&hid_feature[12], status.pad_config);
    write_u32_le(&hid_feature[16], status.uart_signal_config);
    write_u32_le(&hid_feature[20], status.sample_count);
    write_u32_le(&hid_feature[24], status.anomaly_count);
    write_u32_le(&hid_feature[28], cdc_out_packet_count);
    write_u32_le(&hid_feature[32], cdc_out_byte_count);
    write_u32_le(&hid_feature[36], cdc_to_uart_drop_count);
    write_u32_le(&hid_feature[40], uart_tx_byte_count);
    write_u32_le(&hid_feature[44], uart_tx_failure_count);
    write_u32_le(&hid_feature[48], Serial.rxReceivedCount());
    write_u32_le(&hid_feature[52], Serial.rxOverflowCount());
    write_u16_le(&hid_feature[56], Serial.bufferedRxCount());
    hid_feature[58] = line_coding_program_count;
    hid_feature[59] = line_coding_reset_count;
    hid_feature[60] = line_coding_normal_count;
    hid_feature[61] = line_coding_drop_count;
}

void refresh_cdc_in_feature()
{
    hid_feature[5] = 1U;
    hid_feature[6] = usb_configured ? 1U : 0U;
    hid_feature[7] = static_cast<uint8_t>(cdc_in_state);
    hid_feature[8] = cdc_tx_pending_length != 0U ? 1U : 0U;
    write_u16_le(&hid_feature[10], cdc_tx_pending_length);
    write_u32_le(&hid_feature[12], cdc_in_submit_count);
    write_u32_le(&hid_feature[16], cdc_in_submit_failure_count);
    write_u32_le(&hid_feature[20], cdc_in_completion_count);
    write_u32_le(&hid_feature[24], cdc_in_completion_byte_count);
    write_u32_le(&hid_feature[28], cdc_in_recovery_count);
    write_u32_le(&hid_feature[32], cdc_in_zlp_submit_count);
    write_u32_le(&hid_feature[36], cdc_in_zlp_failure_count);
    write_u32_le(&hid_feature[40], usb_configured_generation);
    write_u32_le(&hid_feature[44], usb_reset_event_count);
    write_u32_le(&hid_feature[48], usb_disconnected_event_count);
    write_u32_le(&hid_feature[52], usb_error_event_count);
    write_u32_le(&hid_feature[56], cdc_in_submit_byte_count);
    write_u32_le(&hid_feature[60], usb_event_sequence);
}

void refresh_ap_start_feature()
{
    const WifiApStartDiagnostic diagnostic = wifi_ap_start_diagnostic_snapshot();
    hid_feature[5] = 2U;
    hid_feature[6] = static_cast<uint8_t>(diagnostic.stage);
    hid_feature[7] = static_cast<uint8_t>(diagnostic.native_result);
    hid_feature[8] = diagnostic.sta_vif_state;
    hid_feature[9] = diagnostic.ap_vif_state;
    hid_feature[10] = diagnostic.sta_connected;
    hid_feature[11] = diagnostic.ap_started;
    hid_feature[12] = diagnostic.requested_channel;
    hid_feature[13] = static_cast<uint8_t>(diagnostic.sta_channel_result);
    hid_feature[14] = diagnostic.sta_channel;
    hid_feature[15] = diagnostic.fhost_stage;
    write_u32_le(&hid_feature[16], diagnostic.attempts);
    write_u32_le(&hid_feature[20], diagnostic.elapsed_ms);
    write_u32_le(&hid_feature[24], diagnostic.started_at_ms);
    write_u32_le(&hid_feature[28], diagnostic.completed_at_ms);
    write_u32_le(&hid_feature[32], diagnostic.fhost_chan_address);
    write_u32_le(&hid_feature[36], diagnostic.fhost_handler_chan_address);
    write_u32_le(&hid_feature[40], diagnostic.fhost_response_source_chan_address);
    write_u32_le(&hid_feature[44], diagnostic.fhost_response_copy_chan_address);
    write_u32_le(&hid_feature[48], diagnostic.fhost_response_received_chan_address);
    write_u32_le(&hid_feature[52], diagnostic.wpa_feature_chan_address);
    write_u32_le(&hid_feature[56], diagnostic.fhost_chan2g4_count);
    write_u32_le(&hid_feature[60], diagnostic.fhost_chan5g_count);
}

void refresh_ap_start_heap_feature()
{
    const WifiApStartDiagnostic diagnostic = wifi_ap_start_diagnostic_snapshot();
    hid_feature[5] = 3U;
    hid_feature[6] = diagnostic.fhost_stage;
    hid_feature[7] = static_cast<uint8_t>(diagnostic.fhost_result);
    hid_feature[8] = diagnostic.heap_probe_stage;
    hid_feature[9] = diagnostic.persisted ? 1U : 0U;
    hid_feature[10] = diagnostic.heap_malloc_failed;
    write_u32_le(&hid_feature[12], diagnostic.heap_request_size);
    write_u32_le(&hid_feature[16], diagnostic.heap_total_before);
    write_u32_le(&hid_feature[20], diagnostic.heap_ocram_before);
    write_u32_le(&hid_feature[24], diagnostic.heap_wram_before);
    write_u32_le(&hid_feature[28], diagnostic.heap_allocation_address);
    write_u32_le(&hid_feature[32], diagnostic.heap_total_after);
    write_u32_le(&hid_feature[36], diagnostic.heap_ocram_after);
    write_u32_le(&hid_feature[40], diagnostic.heap_wram_after);
    write_u32_le(&hid_feature[44], diagnostic.attempts);
    write_u32_le(&hid_feature[48], diagnostic.elapsed_ms);
}

void refresh_ap_start_command_feature()
{
    const WifiApStartDiagnostic diagnostic = wifi_ap_start_diagnostic_snapshot();
    hid_feature[5] = 4U;
    hid_feature[6] = diagnostic.fhost_command;
    hid_feature[7] = diagnostic.fhost_command_phase;
    hid_feature[8] = static_cast<uint8_t>(diagnostic.fhost_command_vif);
    hid_feature[9] = diagnostic.fhost_command_wpa_state;
    hid_feature[10] = static_cast<uint8_t>(diagnostic.fhost_command_network_id);
    hid_feature[11] = diagnostic.fhost_command_sequence;
    hid_feature[12] = diagnostic.fhost_stage;
    hid_feature[13] = static_cast<uint8_t>(diagnostic.fhost_result);
    hid_feature[14] = static_cast<uint8_t>(diagnostic.stage);
    hid_feature[15] = diagnostic.persisted ? 1U : 0U;
    write_u32_le(&hid_feature[16], diagnostic.attempts);
    write_u32_le(&hid_feature[20], diagnostic.elapsed_ms);
    write_u32_le(&hid_feature[24], diagnostic.started_at_ms);
    write_u32_le(&hid_feature[28], diagnostic.completed_at_ms);
    hid_feature[32] = diagnostic.fhost_sync_phase;
    hid_feature[33] = static_cast<uint8_t>(diagnostic.fhost_sync_wait_result);
    hid_feature[34] = static_cast<uint8_t>(diagnostic.fhost_sync_signal_result);
    hid_feature[35] = diagnostic.fhost_sync_wait_count_before;
    hid_feature[36] = diagnostic.fhost_sync_callback_count_before;
    hid_feature[37] = diagnostic.fhost_sync_callback_count_after;
    hid_feature[38] = diagnostic.fhost_sync_wait_count_after;
    write_u32_le(&hid_feature[40], diagnostic.fhost_sync_wait_handle);
    write_u32_le(&hid_feature[44], diagnostic.fhost_sync_callback_handle);
    write_u32_le(&hid_feature[48], diagnostic.fhost_sync_update_count);
    write_u32_le(&hid_feature[52], diagnostic.fhost_task_handle);
    hid_feature[56] = diagnostic.fhost_task_state;
    write_u32_le(&hid_feature[57], diagnostic.fhost_task_stack_high_watermark);
}

void refresh_ap_start_trace_feature()
{
    const WifiApStartTrace trace = wifi_ap_start_trace_snapshot();
    hid_feature[5] = 5U;
    hid_feature[6] = trace.persisted ? 1U : 0U;
    hid_feature[7] = trace.record_count;
    write_u32_le(&hid_feature[8], trace.next_sequence);

    for (uint8_t index = 0U; index < trace.record_count; ++index) {
        const WifiApStartTraceRecord &record = trace.records[index];
        const uint8_t offset = static_cast<uint8_t>(12U + index * 14U);
        write_u32_le(&hid_feature[offset], record.sequence);
        hid_feature[offset + 4U] = static_cast<uint8_t>(record.event);
        hid_feature[offset + 5U] = static_cast<uint8_t>(record.result);
        write_u32_le(&hid_feature[offset + 6U], record.semaphore);
        write_u32_le(&hid_feature[offset + 10U], record.count);
    }
}

void refresh_ap_start_notification_feature()
{
    const WifiApStartNotificationDiagnostic diagnostic =
        wifi_ap_start_notification_snapshot();
    hid_feature[5] = 15U;
    hid_feature[6] = diagnostic.persisted ? 1U : 0U;
    hid_feature[7] = diagnostic.event_mask;
    hid_feature[8] = static_cast<uint8_t>(diagnostic.last_event);
    hid_feature[9] = static_cast<uint8_t>(diagnostic.notification_result);
    hid_feature[10] = static_cast<uint8_t>(diagnostic.wait_result);
    write_u32_le(&hid_feature[12], diagnostic.attempt_count);
    write_u32_le(&hid_feature[16], diagnostic.last_sequence);
    write_u32_le(&hid_feature[20], diagnostic.waiting_task);
}

void refresh_ap_start_notification_ab_feature()
{
    const WifiApStartNotificationAbDiagnostic diagnostic =
        wifi_ap_start_notification_ab_snapshot();
    hid_feature[5] = 16U;
    hid_feature[6] = diagnostic.persisted ? 1U : 0U;
    hid_feature[7] = diagnostic.phase_mask;
    hid_feature[8] = diagnostic.last_phase;
    hid_feature[9] = diagnostic.setup_use_task_notification;
    hid_feature[10] = diagnostic.queued_use_task_notification;
    hid_feature[11] = diagnostic.callback_use_task_notification;
    hid_feature[12] = diagnostic.wait_use_task_notification;
    hid_feature[13] = static_cast<uint8_t>(diagnostic.last_result);
    hid_feature[14] = diagnostic.request_matches;
    write_u32_le(&hid_feature[16], diagnostic.update_count);
    write_u32_le(&hid_feature[20], diagnostic.setup_message);
    write_u32_le(&hid_feature[24], diagnostic.queued_message);
    write_u32_le(&hid_feature[28], diagnostic.callback_message);
    write_u32_le(&hid_feature[32], diagnostic.wait_message);
    write_u32_le(&hid_feature[36], diagnostic.setup_waiting_task);
    write_u32_le(&hid_feature[40], diagnostic.queued_waiting_task);
    write_u32_le(&hid_feature[44], diagnostic.callback_waiting_task);
    write_u32_le(&hid_feature[48], diagnostic.wait_waiting_task);
}

void refresh_ap_start_wpa_wait_feature()
{
    const WifiApStartWpaWaitDiagnostic diagnostic =
        wifi_ap_start_wpa_wait_snapshot();
    hid_feature[5] = 18U;
    hid_feature[6] = diagnostic.persisted ? 1U : 0U;
    write_u16_le(&hid_feature[7], diagnostic.phase_mask);
    hid_feature[9] = diagnostic.last_phase;
    hid_feature[10] = static_cast<uint8_t>(diagnostic.last_result);
    hid_feature[11] = static_cast<uint8_t>(diagnostic.vif);
    hid_feature[12] = diagnostic.expected_event;
    hid_feature[13] = diagnostic.observed_event;
    hid_feature[14] = diagnostic.wpa_state;
    write_u32_le(&hid_feature[16], diagnostic.update_count);
    write_u32_le(&hid_feature[20], diagnostic.target);
    write_u32_le(&hid_feature[24], diagnostic.waiting_task);
    write_u32_le(&hid_feature[28], diagnostic.callback_task);
}

void refresh_ap_start_wpa_event_path_feature()
{
    const WifiApStartWpaEventPathDiagnostic diagnostic =
        wifi_ap_start_wpa_event_path_snapshot();
    hid_feature[5] = 19U;
    hid_feature[6] = diagnostic.persisted ? 1U : 0U;
    write_u32_le(&hid_feature[7], diagnostic.phase_mask);
    hid_feature[11] = diagnostic.last_phase;
    hid_feature[12] = static_cast<uint8_t>(diagnostic.vif);
    hid_feature[13] = diagnostic.event;
    hid_feature[14] = diagnostic.fhost_wpa_state;
    hid_feature[15] = static_cast<uint8_t>(diagnostic.operstate);
    hid_feature[16] = static_cast<uint8_t>(diagnostic.old_wpa_state);
    hid_feature[17] = static_cast<uint8_t>(diagnostic.new_wpa_state);
    hid_feature[18] = static_cast<uint8_t>(diagnostic.last_result);
    hid_feature[19] = diagnostic.callback_slot;
    write_u32_le(&hid_feature[20], diagnostic.update_count);
    write_u32_le(&hid_feature[24], diagnostic.callback_slot_mask);
}

void refresh_ap_start_create_ap_feature()
{
    const WifiApStartCreateApDiagnostic diagnostic =
        wifi_ap_start_create_ap_snapshot();
    hid_feature[5] = 20U;
    hid_feature[6] = diagnostic.persisted ? 1U : 0U;
    write_u16_le(&hid_feature[7], diagnostic.phase_mask);
    hid_feature[9] = diagnostic.last_phase;
    hid_feature[10] = static_cast<uint8_t>(diagnostic.last_result);
    hid_feature[11] = diagnostic.wpa_state;
    write_u32_le(&hid_feature[16], diagnostic.detail);
    write_u32_le(&hid_feature[20], diagnostic.update_count);
}

void refresh_ap_start_hostapd_lifecycle_feature()
{
    const WifiApStartHostapdLifecycleDiagnostic diagnostic =
        wifi_ap_start_hostapd_lifecycle_snapshot();
    hid_feature[5] = 24U;
    hid_feature[6] = diagnostic.persisted ? 1U : 0U;
    write_u32_le(&hid_feature[7], diagnostic.phase_mask);
    hid_feature[11] = diagnostic.last_phase;
    hid_feature[12] = static_cast<uint8_t>(diagnostic.last_result);
    hid_feature[13] = diagnostic.iface_state;
    hid_feature[14] = diagnostic.wait_channel_update;
    write_u32_le(&hid_feature[16], diagnostic.detail0);
    write_u32_le(&hid_feature[20], diagnostic.detail1);
    write_u32_le(&hid_feature[24], diagnostic.disable_network_caller);
    write_u16_le(
        &hid_feature[28],
        static_cast<uint16_t>(diagnostic.disable_network_id));
    write_u16_le(&hid_feature[30], diagnostic.deauthenticate_reason);
    hid_feature[32] = diagnostic.record_count;
    for (uint8_t index = 0U; index < diagnostic.record_count; ++index) {
        const WifiApStartHostapdLifecycleDiagnostic::Record &record =
            diagnostic.records[index];
        const uint8_t offset = static_cast<uint8_t>(33U + index * 8U);
        hid_feature[offset] = record.phase;
        hid_feature[offset + 1U] = static_cast<uint8_t>(record.result);
        hid_feature[offset + 2U] = record.iface_state;
        hid_feature[offset + 3U] = record.wait_channel_update;
        write_u32_le(&hid_feature[offset + 4U], record.detail0);
    }
    write_u32_le(&hid_feature[57], diagnostic.bridge_stop_softap_caller);
}

void refresh_ap_start_hostapd_state_update_feature()
{
    const WifiApStartHostapdStateUpdateDiagnostic diagnostic =
        wifi_ap_start_hostapd_state_update_snapshot();
    hid_feature[5] = 26U;
    hid_feature[6] = diagnostic.persisted ? 1U : 0U;
    hid_feature[7] = diagnostic.record_count;
    write_u32_le(&hid_feature[8], diagnostic.next_sequence);
    for (uint8_t index = 0U; index < diagnostic.record_count; ++index) {
        const WifiApStartHostapdStateUpdateRecord &record =
            diagnostic.records[index];
        const size_t offset = 12U + static_cast<size_t>(index) * 12U;
        write_u32_le(&hid_feature[offset], record.sequence);
        hid_feature[offset + 4U] = record.hostapd_state;
        hid_feature[offset + 5U] = record.enabled;
        hid_feature[offset + 6U] = record.raw_state_before;
        hid_feature[offset + 7U] = record.vif_state_before;
        hid_feature[offset + 8U] = record.raw_state_after;
        hid_feature[offset + 9U] = record.vif_state_after;
    }
}

void refresh_ap_start_status_code_feature()
{
    const WifiApStartStatusCodeDiagnostic diagnostic =
        wifi_ap_start_status_code_snapshot();
    hid_feature[5] = 25U;
    hid_feature[6] = diagnostic.persisted ? 1U : 0U;
    write_u16_le(&hid_feature[7], diagnostic.phase_mask);
    hid_feature[9] = diagnostic.last_phase;
    hid_feature[10] = static_cast<uint8_t>(diagnostic.vif);
    hid_feature[11] = diagnostic.message_kind;
    write_u16_le(&hid_feature[12], diagnostic.message_length);
    hid_feature[14] = diagnostic.raw_state;
    hid_feature[15] = diagnostic.vif_state;
    hid_feature[16] = static_cast<uint8_t>(diagnostic.last_result);
    write_u32_le(&hid_feature[20], diagnostic.update_count);
}

void refresh_ap_start_eloop_post_feature()
{
    const WifiApStartEloopPostDiagnostic diagnostic =
        wifi_ap_start_eloop_post_snapshot();
    hid_feature[5] = 17U;
    hid_feature[6] = diagnostic.persisted ? 1U : 0U;
    hid_feature[7] = diagnostic.last_phase;
    hid_feature[8] = diagnostic.caller_event_type;
    hid_feature[9] = diagnostic.callee_event_type;
    hid_feature[10] = diagnostic.caller_request_matches;
    hid_feature[11] = diagnostic.callee_ifname_prefix_matches;
    hid_feature[12] = diagnostic.callee_enable_network_matches;
    write_u32_le(&hid_feature[16], diagnostic.caller_body);
    write_u32_le(&hid_feature[20], diagnostic.caller_body_length);
    write_u32_le(&hid_feature[24], diagnostic.callee_body);
    write_u32_le(&hid_feature[28], diagnostic.callee_body_length);
    write_u32_le(&hid_feature[32], diagnostic.callee_request);
    write_u32_le(&hid_feature[36], diagnostic.callee_request_word);
    write_u32_le(&hid_feature[40], diagnostic.update_count);
}

void refresh_ap_start_allocator_trace_feature()
{
    const WifiApStartAllocatorTrace trace =
        wifi_ap_start_allocator_trace_snapshot();
    hid_feature[5] = 6U;
    hid_feature[6] = trace.fhost_stage;
    hid_feature[7] = trace.record_count;
    write_u32_le(&hid_feature[8], trace.next_sequence);

    for (uint8_t index = 0U; index < trace.record_count; ++index) {
        const WifiApStartAllocatorTraceRecord &record = trace.records[index];
        const uint8_t offset = static_cast<uint8_t>(12U + index * 17U);
        write_u32_le(&hid_feature[offset], record.sequence);
        hid_feature[offset + 4U] = record.stage;
        write_u32_le(&hid_feature[offset + 5U], record.value0);
        write_u32_le(&hid_feature[offset + 9U], record.value1);
        write_u32_le(&hid_feature[offset + 13U], record.value2);
    }
}

void refresh_ap_start_allocator_heap_walk_feature()
{
    const WifiApStartAllocatorHeapWalk walk =
        wifi_ap_start_allocator_heap_walk_snapshot();
    hid_feature[5] = 21U;
    hid_feature[6] = walk.persisted ? 1U : 0U;
    hid_feature[7] = static_cast<uint8_t>(walk.result);
    hid_feature[8] = walk.record_count;

    for (uint8_t index = 0U; index < walk.record_count; ++index) {
        const WifiApStartAllocatorHeapWalkRecord &record = walk.records[index];
        const uint8_t offset = static_cast<uint8_t>(12U + index * 20U);
        hid_feature[offset] = record.heap_id;
        write_u16_le(&hid_feature[offset + 2U], record.allocated_block_count);
        write_u32_le(&hid_feature[offset + 4U], record.allocated_bytes);
        write_u32_le(&hid_feature[offset + 8U], record.largest_blocks[0]);
        write_u32_le(&hid_feature[offset + 12U], record.largest_blocks[1]);
        write_u32_le(&hid_feature[offset + 16U], record.largest_blocks[2]);
    }
}

void refresh_hid_data_path_feature()
{
    hid_feature[5] = 7U;
    hid_feature[6] = usb_configured ? 1U : 0U;
    hid_feature[7] = hid_tx_busy ? 1U : 0U;
    hid_feature[8] = hid_rx_armed ? 1U : 0U;
    hid_feature[9] = hid_last_out_length;
    hid_feature[10] = hid_last_request_command;
    hid_feature[11] = hid_last_response_command;
    hid_feature[12] = static_cast<uint8_t>(hid_last_in_submit_result);
    hid_feature[13] = static_cast<uint8_t>(hid_last_read_rearm_result);
    write_u32_le(&hid_feature[16], hid_out_callback_count);
    write_u32_le(&hid_feature[20], hid_out_invalid_packet_count);
    write_u32_le(&hid_feature[24], hid_out_busy_drop_count);
    write_u32_le(&hid_feature[28], hid_dap_request_count);
    write_u32_le(&hid_feature[32], hid_in_submit_count);
    write_u32_le(&hid_feature[36], hid_in_submit_failure_count);
    write_u32_le(&hid_feature[40], hid_in_completion_count);
    write_u32_le(&hid_feature[44], hid_read_rearm_count);
    write_u32_le(&hid_feature[48], hid_read_rearm_failure_count);
    write_u32_le(&hid_feature[52], usb_event_sequence);
}

void refresh_ap_start_watchdog_feature()
{
    const WifiApStartWatchdogDiagnostic diagnostic =
        wifi_ap_start_watchdog_snapshot();
    const WifiApStartDiagnostic ap_start =
        wifi_ap_start_diagnostic_snapshot();
    hid_feature[5] = 8U;
    hid_feature[6] = diagnostic.enabled ? 1U : 0U;
    hid_feature[7] = static_cast<uint8_t>(diagnostic.state);
    hid_feature[8] = diagnostic.reset_reason;
    hid_feature[9] = diagnostic.persisted ? 1U : 0U;
    write_u16_le(&hid_feature[10], diagnostic.timeout_ms);
    write_u32_le(&hid_feature[12], diagnostic.arm_count);
    hid_feature[16] = ap_start.liveness_phase;
    hid_feature[17] = ap_start.liveness_event_count;
    hid_feature[18] = ap_start.fhost_stage;
    hid_feature[19] = static_cast<uint8_t>(ap_start.stage);
    hid_feature[20] = static_cast<uint8_t>(ap_start.fhost_result);
    hid_feature[21] = static_cast<uint8_t>(ap_start.native_result);
    hid_feature[22] = ap_start.persisted ? 1U : 0U;
}

void refresh_ra4_bootstrap_feature()
{
    hid_feature[5] = 9U;
    if (ra4_bootstrap_control == nullptr) {
        return;
    }

    const Ra4BootstrapDiagnostic &diagnostic =
        ra4_bootstrap_control->diagnostic();
    hid_feature[6] = static_cast<uint8_t>(diagnostic.operation);
    hid_feature[7] = diagnostic.active ? 1U : 0U;
    hid_feature[8] = diagnostic.succeeded ? 1U : 0U;
    hid_feature[9] = diagnostic.record_count;
    write_u32_le(&hid_feature[10], diagnostic.sequence);
    for (uint8_t index = 0U;
         index < diagnostic.record_count &&
         index < Ra4BootstrapDiagnostic::kRecordCapacity;
         ++index) {
        const Ra4BootstrapDiagnosticRecord &record = diagnostic.records[index];
        const uint8_t offset = static_cast<uint8_t>(14U + index * 6U);
        hid_feature[offset] = record.step;
        hid_feature[offset + 1U] = record.boot_state;
        hid_feature[offset + 2U] = record.reset_state;
        hid_feature[offset + 3U] = static_cast<uint8_t>(record.boot_level);
        hid_feature[offset + 4U] = static_cast<uint8_t>(record.reset_level);
        hid_feature[offset + 5U] = record.succeeded;
    }
}

void write_uart_recovery_snapshot(
    uint8_t *destination,
    const WifiApStartUartSnapshot &snapshot)
{
    destination[0] = snapshot.initialized ? 1U : 0U;
    destination[1] = snapshot.rx_fifo_count;
    write_u16_le(&destination[2], snapshot.interrupt_status);
    write_u16_le(&destination[4], snapshot.interrupt_mask);
    write_u32_le(&destination[6], snapshot.rx_config);
    write_u32_le(&destination[10], snapshot.bit_period);
    write_u32_le(&destination[14], snapshot.rx_received_count);
    write_u32_le(&destination[18], snapshot.rx_overflow_count);
}

void refresh_ra4_uart_recovery_feature(bool at_uart)
{
    const WifiApStartUartRecoveryDiagnostic diagnostic =
        wifi_ap_start_uart_recovery_snapshot();
    hid_feature[5] = at_uart ? 11U : 10U;
    hid_feature[6] = diagnostic.persisted ? 1U : 0U;
    hid_feature[7] =
        (diagnostic.before_valid ? 0x01U : 0U) |
        (diagnostic.after_valid ? 0x02U : 0U);
    hid_feature[8] = diagnostic.reset_reason;
    write_u32_le(&hid_feature[9], diagnostic.cdc_baud_before);
    write_u32_le(&hid_feature[13], diagnostic.cdc_baud_after);
    write_uart_recovery_snapshot(
        &hid_feature[17],
        at_uart ? diagnostic.at_uart_before : diagnostic.user_uart_before);
    write_uart_recovery_snapshot(
        &hid_feature[39],
        at_uart ? diagnostic.at_uart_after : diagnostic.user_uart_after);
}

void refresh_ra4_uart_live_feature(bool at_uart)
{
    hid_feature[5] = at_uart ? 13U : 12U;
    const WifiApStartUartSnapshot snapshot = wifi_ap_start_uart_live_snapshot(
        at_uart ? Serial1 : Serial);
    write_uart_recovery_snapshot(&hid_feature[6], snapshot);
}

void refresh_ra4_at_transport_feature()
{
    hid_feature[5] = 14U;
    if (ra4_at_service == nullptr) {
        return;
    }

    const UartTransportDiagnostics diagnostic =
        ra4_at_service->uart_diagnostics();
    hid_feature[6] = diagnostic.initialized ? 1U : 0U;
    write_u32_le(&hid_feature[8], diagnostic.received_bytes);
    write_u32_le(&hid_feature[12], diagnostic.buffered_rx_bytes);
    write_u32_le(&hid_feature[16], diagnostic.rx_overflow_count);
    write_u32_le(&hid_feature[20], diagnostic.buffered_tx_bytes);
    write_u32_le(&hid_feature[24], diagnostic.tx_overflow_count);
    write_u32_le(&hid_feature[28], diagnostic.rx_error_status_seen);
    write_u32_le(&hid_feature[32], diagnostic.rx_fifo_errors_seen);
}

void refresh_ra4_ota_download_feature()
{
    hid_feature[5] = 32U;
    if (ra4_at_service == nullptr) {
        return;
    }

    const Ra4OtaDownloadDiagnostic diagnostic =
        ra4_at_service->ota_download_diagnostic();
    hid_feature[6] = static_cast<uint8_t>(diagnostic.last_read_result);
    hid_feature[7] = static_cast<uint8_t>(diagnostic.timeout_reason);
    hid_feature[8] = diagnostic.secure ? 1U : 0U;
    hid_feature[9] = diagnostic.last_read_valid ? 1U : 0U;
    write_u32_le(&hid_feature[12], static_cast<uint32_t>(diagnostic.last_read_size));
    write_u32_le(&hid_feature[16], static_cast<uint32_t>(diagnostic.content_length));
    write_u32_le(&hid_feature[20], static_cast<uint32_t>(diagnostic.downloaded_size));
    write_u32_le(&hid_feature[24], diagnostic.poll_count);
    write_u32_le(&hid_feature[28], diagnostic.data_count);
    write_u32_le(&hid_feature[32], diagnostic.pending_count);
    write_u32_le(&hid_feature[36], diagnostic.closed_count);
    write_u32_le(&hid_feature[40], diagnostic.error_count);
    write_u32_le(&hid_feature[44], diagnostic.last_progress_ms);
    write_u32_le(&hid_feature[48], diagnostic.last_poll_ms);
}

void refresh_ra4_ota_download_backend_feature()
{
    const Ra4OtaDownloadBackendDiagnostic diagnostic =
        bouffalo_ra4_ota_download_backend_diagnostic_snapshot();
    hid_feature[5] = 33U;
    hid_feature[6] = diagnostic.open ? 1U : 0U;
    hid_feature[7] = diagnostic.secure ? 1U : 0U;
    write_i32_le(&hid_feature[8], diagnostic.handle);
    write_u32_le(&hid_feature[12], diagnostic.available_call_count);
    write_u32_le(&hid_feature[16], diagnostic.available_ready_count);
    write_u32_le(&hid_feature[20], diagnostic.available_pending_count);
    write_u32_le(&hid_feature[24], diagnostic.available_failure_count);
    write_u32_le(&hid_feature[28], static_cast<uint32_t>(diagnostic.last_available));
    write_u32_le(&hid_feature[32], diagnostic.receive_call_count);
    write_u32_le(&hid_feature[36], diagnostic.receive_failure_count);
    write_u32_le(&hid_feature[40], static_cast<uint32_t>(diagnostic.last_receive_size));
    hid_feature[44] = diagnostic.last_receive_succeeded ? 1U : 0U;
}

void refresh_ra4_samba_feature()
{
    hid_feature[5] = 34U;
    if (ra4_samba_service == nullptr) {
        return;
    }

    const Ra4SambaDiagnostic &diagnostic = ra4_samba_service->diagnostic();
    hid_feature[6] = static_cast<uint8_t>(diagnostic.state);
    hid_feature[7] = static_cast<uint8_t>(diagnostic.error);
    hid_feature[8] = diagnostic.active ? 1U : 0U;
    hid_feature[9] = diagnostic.succeeded ? 1U : 0U;
    write_u32_le(&hid_feature[10], diagnostic.sequence);
    write_u32_le(&hid_feature[14], diagnostic.started_at_ms);
    write_u32_le(&hid_feature[18], diagnostic.completed_at_ms);
    write_u32_le(&hid_feature[22], diagnostic.transmitted_bytes);
    write_u32_le(&hid_feature[26], diagnostic.received_bytes);
    constexpr size_t kVersionOffset = 30U;
    constexpr size_t kVersionBytes = 32U;
    const size_t version_size = strnlen(diagnostic.version, sizeof(diagnostic.version));
    const size_t copied = version_size < kVersionBytes ? version_size : kVersionBytes;
    memcpy(&hid_feature[kVersionOffset], diagnostic.version, copied);
    hid_feature[62] = static_cast<uint8_t>(version_size);
    hid_feature[63] = version_size >= 2U
        ? static_cast<uint8_t>(diagnostic.version[version_size - 2U])
        : 0U;
    if (diagnostic.error == Ra4SambaError::kBinaryReplyTimeout ||
        diagnostic.error == Ra4SambaError::kBinaryReplyInvalid) {
        hid_feature[60] = diagnostic.binary_reply_bytes[0];
        hid_feature[61] = diagnostic.binary_reply_bytes[1];
    }
}

void refresh_ra4_samba_programmer_feature()
{
    hid_feature[5] = 35U;
    if (ra4_samba_programmer == nullptr) {
        return;
    }

    const Ra4SambaProgrammerDiagnostic &diagnostic =
        ra4_samba_programmer->diagnostic();
    hid_feature[6] = static_cast<uint8_t>(diagnostic.state);
    hid_feature[7] = static_cast<uint8_t>(diagnostic.error);
    hid_feature[8] = diagnostic.active ? 1U : 0U;
    hid_feature[9] = diagnostic.succeeded ? 1U : 0U;
    write_u32_le(&hid_feature[10], diagnostic.sequence);
    write_u32_le(&hid_feature[14], diagnostic.started_at_ms);
    write_u32_le(&hid_feature[18], diagnostic.completed_at_ms);
    write_u32_le(&hid_feature[22], diagnostic.transmitted_bytes);
    write_u32_le(&hid_feature[26], diagnostic.received_bytes);
    write_u32_le(&hid_feature[30], diagnostic.image_bytes);
    write_u32_le(&hid_feature[34], diagnostic.verified_bytes);
    write_u16_le(&hid_feature[38], diagnostic.chunk_index);
    const size_t version_size = strnlen(
        diagnostic.version,
        sizeof(diagnostic.version));
    hid_feature[40] = static_cast<uint8_t>(version_size);
    hid_feature[41] = version_size >= 2U
        ? static_cast<uint8_t>(diagnostic.version[version_size - 2U])
        : 0U;
    constexpr size_t kVersionOffset = 42U;
    constexpr size_t kVersionBytes = sizeof(hid_feature) - kVersionOffset;
    const size_t copied = version_size < kVersionBytes ? version_size : kVersionBytes;
    memcpy(&hid_feature[kVersionOffset], diagnostic.version, copied);
}

void refresh_time_service_feature()
{
    hid_feature[5] = 36U;
    if (ra4_at_service == nullptr) {
        return;
    }

    const TimeServiceDiagnostic diagnostic = ra4_at_service->time_diagnostic();
    hid_feature[6] = diagnostic.station_ready ? 1U : 0U;
    hid_feature[7] = diagnostic.sync_running ? 1U : 0U;
    hid_feature[8] = diagnostic.synced ? 1U : 0U;
    hid_feature[9] = static_cast<uint8_t>(diagnostic.last_failure);
    write_u32_le(&hid_feature[10], diagnostic.got_ip_events);
    write_u32_le(&hid_feature[14], diagnostic.task_create_count);
    write_u32_le(&hid_feature[18], diagnostic.task_create_failure_count);
    write_u32_le(&hid_feature[22], diagnostic.sync_attempt_count);
    write_u32_le(&hid_feature[26], diagnostic.dns_failure_count);
    write_u32_le(&hid_feature[30], diagnostic.socket_failure_count);
    write_u32_le(&hid_feature[34], diagnostic.send_failure_count);
    write_u32_le(&hid_feature[38], diagnostic.receive_failure_count);
    write_u32_le(&hid_feature[42], diagnostic.packet_failure_count);
    write_u32_le(&hid_feature[46], diagnostic.rtc_failure_count);
    write_u32_le(&hid_feature[50], diagnostic.success_count);
    write_u16_le(
        &hid_feature[54],
        static_cast<uint16_t>(diagnostic.last_dns_result));
    write_u16_le(
        &hid_feature[56],
        static_cast<uint16_t>(diagnostic.last_receive_result));
}

void refresh_time_service_readback_feature()
{
    hid_feature[5] = 37U;
    if (ra4_at_service == nullptr) {
        return;
    }

    const TimeServiceDiagnostic diagnostic = ra4_at_service->time_diagnostic();
    hid_feature[6] = diagnostic.station_ready ? 1U : 0U;
    hid_feature[7] = diagnostic.synced ? 1U : 0U;
    write_u16_le(&hid_feature[8], diagnostic.last_rtc_year);
    write_u32_le(&hid_feature[10], diagnostic.last_ntp_epoch);
    write_u32_le(&hid_feature[14], diagnostic.last_rtc_epoch);
    write_u32_le(&hid_feature[18], diagnostic.last_published_epoch);
    write_u32_le(&hid_feature[22], diagnostic.epoch_read_count);
    write_u32_le(&hid_feature[26], diagnostic.rtc_failure_count);
    write_u32_le(&hid_feature[30], diagnostic.success_count);
}

void refresh_tcp_runtime_feature()
{
    const TcpRuntimeDiagnostic diagnostic = tcp_runtime_diagnostic_snapshot();
    hid_feature[5] = 22U;
    write_u32_le(&hid_feature[6], diagnostic.listener_flags);
    write_u32_le(&hid_feature[10], diagnostic.generation);
    write_i32_le(&hid_feature[14], diagnostic.listener_handle);
    write_u16_le(&hid_feature[18], diagnostic.requested_port);
    write_i32_le(&hid_feature[20], diagnostic.last_listener_errno);
    write_u32_le(&hid_feature[24], diagnostic.listener_create_count);
    write_u32_le(&hid_feature[28], diagnostic.accept_attempt_count);
    write_u32_le(&hid_feature[32], diagnostic.accept_success_count);
    write_u32_le(&hid_feature[36], diagnostic.accept_would_block_count);
    write_u32_le(&hid_feature[40], diagnostic.accept_failure_count);
    write_i32_le(&hid_feature[44], diagnostic.last_accept_errno);
}

void refresh_tcp_data_plane_feature()
{
    const TcpRuntimeDiagnostic diagnostic = tcp_runtime_diagnostic_snapshot();
    hid_feature[5] = 23U;
    write_u32_le(&hid_feature[6], diagnostic.accepted_flags);
    write_i32_le(&hid_feature[10], diagnostic.accepted_handle);
    write_i32_le(&hid_feature[14], diagnostic.accepted_peer_errno);
    write_i32_le(&hid_feature[18], diagnostic.accepted_socket_errno);
    write_i32_le(&hid_feature[22], diagnostic.accepted_socket_error);
    write_u32_le(&hid_feature[26], diagnostic.available_probe_count);
    write_u32_le(&hid_feature[30], diagnostic.available_ready_count);
    write_u32_le(&hid_feature[34], diagnostic.available_eof_count);
    write_u32_le(&hid_feature[38], diagnostic.available_would_block_count);
    write_u32_le(&hid_feature[42], diagnostic.available_failure_count);
    write_i32_le(&hid_feature[46], diagnostic.last_available_errno);
}

void refresh_tcp_listener_lwip_feature()
{
    const TcpRuntimeDiagnostic diagnostic = tcp_runtime_diagnostic_lwip_snapshot();
    const ApPacketDiagnostic packets = ap_packet_diagnostic_snapshot();
    hid_feature[5] = 28U;
    hid_feature[6] = diagnostic.listen_pcb_count;
    hid_feature[7] = diagnostic.matching_listen_pcb_count;
    write_u32_le(&hid_feature[8], diagnostic.configured_local_address);
    write_u32_le(&hid_feature[12], diagnostic.observed_local_address);
    write_u16_le(&hid_feature[16], diagnostic.observed_local_port);
    write_i32_le(&hid_feature[18], diagnostic.observed_local_errno);
    write_u32_le(&hid_feature[22], packets.last_listener_source_address);
    write_u32_le(&hid_feature[26], packets.last_listener_destination_address);
    for (uint8_t index = 0U; index < 3U; ++index) {
        const TcpRuntimeDiagnostic::ListenPcb &entry =
            diagnostic.listen_pcbs[index];
        const uint8_t offset = static_cast<uint8_t>(30U + index * 8U);
        write_u16_le(&hid_feature[offset], entry.local_port);
        write_u32_le(&hid_feature[offset + 2U], entry.local_address);
        hid_feature[offset + 6U] = entry.netif_index;
        hid_feature[offset + 7U] = entry.state;
    }
    hid_feature[54] = diagnostic.listener_socket_flags;
    hid_feature[55] = diagnostic.listener_netconn_state;
    hid_feature[56] = diagnostic.listener_netconn_type;
    hid_feature[57] = diagnostic.listener_pcb_state;
    write_u16_le(&hid_feature[58], diagnostic.listener_pcb_local_port);
    write_u32_le(&hid_feature[60], diagnostic.listener_pcb_local_address);
}

void refresh_tcp_listener_close_feature()
{
    const TcpRuntimeDiagnostic diagnostic = tcp_runtime_diagnostic_snapshot();
    hid_feature[5] = 31U;
    write_u32_le(&hid_feature[6], diagnostic.close_request_count);
    write_u32_le(&hid_feature[10], diagnostic.listener_close_request_count);
    write_i32_le(&hid_feature[14], diagnostic.last_close_handle);
    hid_feature[18] = static_cast<uint8_t>(diagnostic.last_close_origin);
    write_i32_le(&hid_feature[20], diagnostic.last_close_result);
    write_i32_le(&hid_feature[24], diagnostic.last_close_errno);
    write_u32_le(&hid_feature[28], diagnostic.last_close_generation);
    write_u32_le(&hid_feature[32], diagnostic.generation);
    write_i32_le(&hid_feature[36], diagnostic.listener_handle);
}

void refresh_ap_packet_input_feature()
{
    const ApPacketDiagnostic diagnostic = ap_packet_diagnostic_snapshot();
    hid_feature[5] = 27U;
    write_u32_le(&hid_feature[6], diagnostic.input_packet_count);
    write_u32_le(&hid_feature[10], diagnostic.sta_packet_count);
    write_u32_le(&hid_feature[14], diagnostic.ap_packet_count);
    write_u32_le(&hid_feature[18], diagnostic.ap_ipv4_packet_count);
    write_u32_le(&hid_feature[22], diagnostic.ap_tcp_packet_count);
    write_u32_le(&hid_feature[26], diagnostic.ap_tcp_listener_port_count);
    write_u32_le(&hid_feature[30], diagnostic.malformed_packet_count);
    write_u32_le(&hid_feature[34], diagnostic.last_packet_sequence);
    write_u16_le(&hid_feature[38], diagnostic.last_tcp_source_port);
    write_u16_le(&hid_feature[40], diagnostic.last_tcp_destination_port);
    hid_feature[42] = diagnostic.last_tcp_flags;
    hid_feature[43] = diagnostic.last_packet_is_sta;
    hid_feature[44] = diagnostic.last_packet_ipv4;
    hid_feature[45] = diagnostic.last_packet_tcp;
    write_u32_le(&hid_feature[46], diagnostic.ap_tcp_listener_output_count);
    write_u32_le(
        &hid_feature[50],
        diagnostic.ap_tcp_listener_syn_ack_output_count);
    write_u32_le(&hid_feature[54], diagnostic.ap_tcp_listener_reset_output_count);
    write_u16_le(&hid_feature[58], diagnostic.last_output_tcp_source_port);
    write_u16_le(&hid_feature[60], diagnostic.last_output_tcp_destination_port);
    hid_feature[62] = diagnostic.last_output_tcp_flags;
    hid_feature[63] = diagnostic.last_output_packet_is_sta;
}

void refresh_ap_udp_packet_input_feature()
{
    const ApPacketDiagnostic diagnostic = ap_packet_diagnostic_snapshot();
    hid_feature[5] = 29U;
    write_u32_le(&hid_feature[6], diagnostic.ap_udp_packet_count);
    write_u32_le(&hid_feature[10], diagnostic.ap_udp_listener_port_count);
    write_u32_le(&hid_feature[14], diagnostic.ap_udp_listener_output_count);
    write_u16_le(&hid_feature[18], diagnostic.last_udp_source_port);
    write_u16_le(&hid_feature[20], diagnostic.last_udp_destination_port);
    write_u32_le(&hid_feature[22], diagnostic.last_udp_source_address);
    write_u32_le(&hid_feature[26], diagnostic.last_udp_destination_address);
    hid_feature[30] = diagnostic.last_udp_packet_is_sta;
    write_u16_le(&hid_feature[31], diagnostic.last_output_udp_source_port);
    write_u16_le(&hid_feature[33], diagnostic.last_output_udp_destination_port);
    hid_feature[35] = diagnostic.last_output_udp_packet_is_sta;
    // Append-only UDP diagnostic extension; existing offsets remain stable.
    hid_feature[36] = 2U;
    write_u32_le(&hid_feature[40], diagnostic.ap_dhcp_client_input_count);
    write_u32_le(&hid_feature[44], diagnostic.sta_dhcp_server_input_count);
    write_u32_le(&hid_feature[48], diagnostic.dhcp_server_output_count);
    write_u32_le(&hid_feature[52], diagnostic.dhcp_offer_output_count);
    write_u32_le(&hid_feature[56], diagnostic.dhcp_ack_output_count);
    write_u32_le(&hid_feature[60], diagnostic.dhcp_nak_output_count);
}

void encode_udp_socket_feature(const UdpSocketDiagnostic &diagnostic, uint8_t *report)
{
    report[5] = 30U;
    report[6] = diagnostic.pcb_count;
    report[7] = diagnostic.matching_port_pcb_count;
    report[8] = diagnostic.socket_pcb_found;
    report[9] = diagnostic.socket_pcb_netif_index;
    report[10] = diagnostic.socket_pcb_flags;
    report[11] = diagnostic.socket_netconn_state;
    report[12] = diagnostic.socket_netconn_type;
    report[13] = static_cast<uint8_t>(diagnostic.socket_error);
    write_u16_le(&report[14], diagnostic.socket_local_port);
    write_u16_le(&report[16], diagnostic.socket_remote_port);
    write_u32_le(&report[18], diagnostic.socket_local_address);
    write_u16_le(&report[22], diagnostic.udp_recv_delta);
    write_u16_le(&report[24], diagnostic.udp_drop_delta);
    write_u16_le(&report[26], diagnostic.udp_checksum_error_delta);
    write_u16_le(&report[28], diagnostic.udp_length_error_delta);
    write_u16_le(&report[30], diagnostic.udp_protocol_error_delta);
    write_u16_le(&report[32], diagnostic.udp_route_error_delta);
    write_u16_le(&report[34], diagnostic.udp_xmit_delta);
    write_u16_le(&report[36], diagnostic.receive_call_count);
    write_u16_le(&report[38], diagnostic.receive_would_block_count);
    write_u16_le(&report[40], diagnostic.receive_success_count);
    write_u16_le(&report[42], diagnostic.receive_failure_count);
    write_u16_le(&report[44], diagnostic.receive_restore_failure_count);
    write_u16_le(&report[46], diagnostic.receive_byte_count);
    write_u16_le(
        &report[48],
        static_cast<uint16_t>(diagnostic.last_receive_result));
    report[50] = static_cast<uint8_t>(diagnostic.last_receive_errno);
    write_u16_le(&report[51], diagnostic.parse_attempt_count);
    report[53] = diagnostic.parse_reject_mask;
    report[54] = diagnostic.last_parse_mode;
    report[55] = diagnostic.last_parse_argument_count;
    report[56] = diagnostic.last_parse_slot;
    report[57] = diagnostic.last_parse_slot_open;
    report[58] = diagnostic.last_parse_backend_present;
}

// Explicit HID request only. Keep allocator traversal out of USB callbacks.
volatile bool runtime_heap_pending = false;
uint32_t runtime_heap_sequence = 0U;
void sample_runtime_heap()
{
    if (!runtime_heap_pending) return;
    runtime_heap_pending = false;
    uint8_t report[64] = {};
    memcpy(report, "U35D", 4U);
    report[4] = kGpio35FeatureVersion;
    report[5] = 39U;
    write_u32_le(&report[6], ++runtime_heap_sequence);
    write_u32_le(&report[10], millis());
    const uint32_t ids[] = {MM_HEAP_OCRAM_0, MM_HEAP_WRAM_0};
    const uintptr_t flags = mm_lock_save();
    for (size_t i = 0U; i < 2U; ++i) {
        uint8_t *row = &report[14U + i * 24U];
        if (!g_mem_manager.initialized) continue;
        row[0] |= 1U;
        mm_heap_t *heap = g_mem_manager.heaps[ids[i]];
        if (heap == nullptr) continue;
        row[0] |= 2U;
        if (!heap->is_active) continue;
        row[0] |= 4U;
        row[1] = static_cast<uint8_t>(heap->allocator_id);
        if (heap->allocator_id >= CONFIG_MM_ALLOCATOR_COUNT) continue;
        const mm_allocator_t *allocator = g_mem_manager.allocators[heap->allocator_id];
        if (allocator == nullptr || heap->allocator_priv == nullptr) continue;
        row[0] |= 8U;
        if (allocator->get_free_size != nullptr) {
            row[0] |= 16U;
            write_u32_le(&row[4], allocator->get_free_size(heap));
        }
        if (allocator->get_usage_info != nullptr) {
            row[0] |= 32U;
            mm_usage_info info = {};
            allocator->get_usage_info(heap, &info);
            const uint32_t nodes = info.free_node > 65535U ? 65535U : info.free_node;
            row[2] = static_cast<uint8_t>(nodes);
            row[3] = static_cast<uint8_t>(nodes >> 8U);
            write_u32_le(&row[8], info.free_size);
            write_u32_le(&row[12], info.max_free_size);
            write_u32_le(&row[16], info.used_size);
            write_u32_le(&row[20], info.total_size);
        }
    }
    // Publication and both allocator views are atomic against interrupts.
    memcpy(hid_feature, report, sizeof(report));
    mm_unlock_restore(flags);
}

volatile bool udp_socket_pending = false;
void sample_udp_socket()
{
    if (!udp_socket_pending) return;
    udp_socket_pending = false;
    // BSD getsockopt and the TCP/IP core mutex may block. Never execute
    // them in the USB control request callback.
    const UdpSocketDiagnostic diagnostic = udp_socket_diagnostic_snapshot();
    uint8_t report[64] = {};
    memcpy(report, "U35D", 4U);
    report[4] = kGpio35FeatureVersion;
    encode_udp_socket_feature(diagnostic, report);
    const uintptr_t flags = mm_lock_save();
    if (diagnostic_feature_page == 30U) memcpy(hid_feature, report, sizeof(report));
    mm_unlock_restore(flags);
}

void refresh_hid_feature()
{
    // Page39 publishes directly into the existing report buffer in poll().
    if (diagnostic_feature_page == 39U || diagnostic_feature_page == 30U) return;
    memset(hid_feature, 0, sizeof(hid_feature));
    hid_feature[0] = 'U';
    hid_feature[1] = '3';
    hid_feature[2] = '5';
    hid_feature[3] = 'D';
    hid_feature[4] = kGpio35FeatureVersion;

    if (diagnostic_feature_page == 82U) {
        mac_rx_diagnostic_report(82U, hid_feature);
        return;
    }
    if (diagnostic_feature_page == 81U) {
        udp_rx_fragment_report(hid_feature);
        return;
    }
    if (diagnostic_feature_page >= 63U && diagnostic_feature_page <= 80U) {
        controller_link_diagnostic_report(diagnostic_feature_page, hid_feature);
        return;
    }

    if (diagnostic_feature_page == 60U) {
        tcp_rx_physical_report(hid_feature);
        return;
    }
    if (diagnostic_feature_page == 59U) {
        tcp_rx_memory_report(hid_feature);
        return;
    }

    if (diagnostic_feature_page >= 53U && diagnostic_feature_page <= 57U) {
        lwip_allocation_diagnostic_report(diagnostic_feature_page, hid_feature);
        return;
    }
    if (diagnostic_feature_page == 52U) {
        dhcp_tx_diagnostic_report(hid_feature);
        return;
    }
    if (diagnostic_feature_page == 61U || diagnostic_feature_page == 62U) {
        hid_feature[5] = diagnostic_feature_page;
        ipv6_tcp_diagnostic_report(hid_feature);
        return;
    }
    if (diagnostic_feature_page == 51U) {
        sta_rx_ownership_report(hid_feature);
        return;
    }
    if (diagnostic_feature_page >= 48U && diagnostic_feature_page <= 50U) {
        mac_rx_diagnostic_report(diagnostic_feature_page, hid_feature);
        return;
    }
    if (diagnostic_feature_page == 46U || diagnostic_feature_page == 47U || diagnostic_feature_page == 58U) {
        sta_tcp_diagnostic_report(diagnostic_feature_page, hid_feature);
        return;
    }
    if (diagnostic_feature_page >= 42U && diagnostic_feature_page <= 45U) {
        runtime_heap_ownership_report(diagnostic_feature_page, hid_feature);
        return;
    }
    if (diagnostic_feature_page == 40U || diagnostic_feature_page == 41U) {
        RuntimeAllocationRow rows[3] = {};
        hid_feature[5] = diagnostic_feature_page;
        hid_feature[6] = runtime_allocation_snapshot(diagnostic_feature_page, rows);
        hid_feature[7] = 3U;
        for (size_t i = 0; i < 3U; ++i) {
            const size_t offset = 8U + i * 16U;
            write_u32_le(&hid_feature[offset], rows[i].calls);
            write_u32_le(&hid_feature[offset + 4U], rows[i].failures);
            write_u32_le(&hid_feature[offset + 8U], rows[i].max_request);
            write_u32_le(&hid_feature[offset + 12U], rows[i].max_failed_request);
        }
        return;
    }

    if (diagnostic_feature_page == 38U) {
        const auto diagnostic = wifi_sta_disconnect_diagnostic_snapshot();
        hid_feature[5] = 38U;
        write_u32_le(&hid_feature[6], diagnostic.attempts);
        write_u32_le(&hid_feature[10], diagnostic.events);
        hid_feature[14] = diagnostic.stage;
        hid_feature[15] = static_cast<uint8_t>(diagnostic.result);
        hid_feature[16] = diagnostic.next;
        hid_feature[17] = diagnostic.count;
        memcpy(&hid_feature[18], diagnostic.stages, 16U);
        memcpy(&hid_feature[34], diagnostic.results, 16U);
        return;
    }

    if (diagnostic_feature_page == 1U) {
        refresh_cdc_in_feature();
        return;
    }
    if (diagnostic_feature_page == 2U) {
        refresh_ap_start_feature();
        return;
    }
    if (diagnostic_feature_page == 3U) {
        refresh_ap_start_heap_feature();
        return;
    }
    if (diagnostic_feature_page == 4U) {
        refresh_ap_start_command_feature();
        return;
    }
    if (diagnostic_feature_page == 5U) {
        refresh_ap_start_trace_feature();
        return;
    }
    if (diagnostic_feature_page == 6U) {
        refresh_ap_start_allocator_trace_feature();
        return;
    }
    if (diagnostic_feature_page == 7U) {
        refresh_hid_data_path_feature();
        return;
    }
    if (diagnostic_feature_page == 8U) {
        refresh_ap_start_watchdog_feature();
        return;
    }
    if (diagnostic_feature_page == 9U) {
        refresh_ra4_bootstrap_feature();
        return;
    }
    if (diagnostic_feature_page == 10U) {
        refresh_ra4_uart_recovery_feature(false);
        return;
    }
    if (diagnostic_feature_page == 11U) {
        refresh_ra4_uart_recovery_feature(true);
        return;
    }
    if (diagnostic_feature_page == 12U) {
        refresh_ra4_uart_live_feature(false);
        return;
    }
    if (diagnostic_feature_page == 13U) {
        refresh_ra4_uart_live_feature(true);
        return;
    }
    if (diagnostic_feature_page == 14U) {
        refresh_ra4_at_transport_feature();
        return;
    }
    if (diagnostic_feature_page == 15U) {
        refresh_ap_start_notification_feature();
        return;
    }
    if (diagnostic_feature_page == 16U) {
        refresh_ap_start_notification_ab_feature();
        return;
    }
    if (diagnostic_feature_page == 17U) {
        refresh_ap_start_eloop_post_feature();
        return;
    }
    if (diagnostic_feature_page == 18U) {
        refresh_ap_start_wpa_wait_feature();
        return;
    }
    if (diagnostic_feature_page == 19U) {
        refresh_ap_start_wpa_event_path_feature();
        return;
    }
    if (diagnostic_feature_page == 20U) {
        refresh_ap_start_create_ap_feature();
        return;
    }
    if (diagnostic_feature_page == 21U) {
        refresh_ap_start_allocator_heap_walk_feature();
        return;
    }
    if (diagnostic_feature_page == 22U) {
        refresh_tcp_runtime_feature();
        return;
    }
    if (diagnostic_feature_page == 23U) {
        refresh_tcp_data_plane_feature();
        return;
    }
    if (diagnostic_feature_page == 24U) {
        refresh_ap_start_hostapd_lifecycle_feature();
        return;
    }
    if (diagnostic_feature_page == 25U) {
        refresh_ap_start_status_code_feature();
        return;
    }
    if (diagnostic_feature_page == 26U) {
        refresh_ap_start_hostapd_state_update_feature();
        return;
    }
    if (diagnostic_feature_page == 27U) {
        refresh_ap_packet_input_feature();
        return;
    }
    if (diagnostic_feature_page == 28U) {
        refresh_tcp_listener_lwip_feature();
        return;
    }
    if (diagnostic_feature_page == 29U) {
        refresh_ap_udp_packet_input_feature();
        return;
    }
    if (diagnostic_feature_page == 30U) {
        return;
    }
    if (diagnostic_feature_page == 31U) {
        refresh_tcp_listener_close_feature();
        return;
    }
    if (diagnostic_feature_page == 32U) {
        refresh_ra4_ota_download_feature();
        return;
    }
    if (diagnostic_feature_page == 33U) {
        refresh_ra4_ota_download_backend_feature();
        return;
    }
    if (diagnostic_feature_page == 34U) {
        refresh_ra4_samba_feature();
        return;
    }
    if (diagnostic_feature_page == 35U) {
        refresh_ra4_samba_programmer_feature();
        return;
    }
    if (diagnostic_feature_page == 36U) {
        refresh_time_service_feature();
        return;
    }
    if (diagnostic_feature_page == 37U) {
        refresh_time_service_readback_feature();
        return;
    }
    if (user_uart_gpio35_control != nullptr) {
        refresh_user_uart_feature();
    }
}

void cdc_out_callback(uint8_t, uint8_t, uint32_t nbytes)
{
    cdc_rx_armed = false;
    if (nbytes > sizeof(cdc_rx)) {
        nbytes = sizeof(cdc_rx);
    }

    ++cdc_out_packet_count;
    cdc_out_byte_count += nbytes;
    if (!user_uart_bridge_enabled) {
        cdc_to_uart_drop_count += nbytes;
        rearm_cdc_read();
        return;
    }
    for (uint32_t index = 0U; index < nbytes; ++index) {
        if (!enqueue_cdc_byte(cdc_rx[index])) {
            cdc_to_uart_drop_count += nbytes - index;
            break;
        }
    }
    rearm_cdc_read();
}

void cdc_in_callback(uint8_t, uint8_t, uint32_t nbytes)
{
    ++cdc_in_completion_count;
    cdc_in_completion_byte_count += nbytes;

    if (cdc_in_state == CdcInState::kDataInFlight) {
        cdc_tx_pending_length = 0U;

        if (nbytes != 0U && (nbytes % kPacketSize) == 0U) {
            cdc_in_state = CdcInState::kZlpInFlight;
            ++cdc_in_zlp_submit_count;
            if (usbd_ep_start_write(kUsbBusId, kCdcInEndpoint, nullptr, 0U) == 0) {
                return;
            }
            ++cdc_in_zlp_failure_count;
        }

        cdc_in_state = CdcInState::kIdle;
        return;
    }

    if (cdc_in_state == CdcInState::kZlpInFlight) {
        cdc_in_state = CdcInState::kIdle;
    }
}

void hid_out_callback(uint8_t, uint8_t, uint32_t nbytes)
{
    hid_rx_armed = false;
    ++hid_out_callback_count;
    hid_last_out_length = static_cast<uint8_t>(
        nbytes > UINT8_MAX ? UINT8_MAX : nbytes);

    if (nbytes == 0U || nbytes > sizeof(hid_rx)) {
        ++hid_out_invalid_packet_count;
    } else if (hid_tx_busy) {
        ++hid_out_busy_drop_count;
    } else {
        memset(hid_tx, 0, sizeof(hid_tx));
        hid_last_request_command = hid_rx[0];
        (void)dap_process_request(
            hid_rx,
            static_cast<int>(nbytes),
            hid_tx,
            static_cast<int>(sizeof(hid_tx)));
        ++hid_dap_request_count;
        hid_last_response_command = hid_tx[0];
        ++hid_in_submit_count;
        const int result =
            usbd_ep_start_write(kUsbBusId, kHidInEndpoint, hid_tx, sizeof(hid_tx));
        hid_last_in_submit_result = static_cast<int8_t>(result);
        hid_tx_busy = result == 0;
        if (hid_tx_busy) {
            return;
        }
        ++hid_in_submit_failure_count;
    }

    rearm_hid_read();
}

void hid_in_callback(uint8_t, uint8_t, uint32_t)
{
    ++hid_in_completion_count;
    hid_tx_busy = false;
    if (!hid_rx_armed) {
        rearm_hid_read();
    }
}

struct usbd_endpoint cdc_out_endpoint = {
    .ep_addr = kCdcOutEndpoint,
    .ep_cb = cdc_out_callback,
};

struct usbd_endpoint cdc_in_endpoint = {
    .ep_addr = kCdcInEndpoint,
    .ep_cb = cdc_in_callback,
};

struct usbd_endpoint hid_out_endpoint = {
    .ep_addr = kHidOutEndpoint,
    .ep_cb = hid_out_callback,
};

struct usbd_endpoint hid_in_endpoint = {
    .ep_addr = kHidInEndpoint,
    .ep_cb = hid_in_callback,
};

void usb_event_callback(uint8_t, uint8_t event)
{
    ++usb_event_sequence;

    switch (event) {
    case USBD_EVENT_RESET:
        cdc_rx_armed = false;
        ++usb_reset_event_count;
        usb_configured = false;
        if (cdc_in_state != CdcInState::kIdle) {
            ++cdc_in_recovery_count;
        }
        cdc_in_state = CdcInState::kIdle;
        hid_tx_busy = false;
        hid_rx_armed = false;
        break;
    case USBD_EVENT_DISCONNECTED:
        cdc_rx_armed = false;
        ++usb_disconnected_event_count;
        usb_configured = false;
        if (cdc_in_state != CdcInState::kIdle) {
            ++cdc_in_recovery_count;
        }
        cdc_in_state = CdcInState::kIdle;
        hid_tx_busy = false;
        hid_rx_armed = false;
        break;
    case USBD_EVENT_ERROR:
    case USBD_EVENT_DEINIT:
        cdc_rx_armed = false;
        ++usb_error_event_count;
        usb_configured = false;
        if (cdc_in_state != CdcInState::kIdle) {
            ++cdc_in_recovery_count;
        }
        cdc_in_state = CdcInState::kIdle;
        hid_tx_busy = false;
        hid_rx_armed = false;
        break;
    case USBD_EVENT_CONFIGURED:
        cdc_rx_armed = false;
        ++usb_configured_generation;
        usb_configured = true;
        if (cdc_in_state != CdcInState::kIdle) {
            ++cdc_in_recovery_count;
        }
        cdc_in_state = CdcInState::kIdle;
        rearm_cdc_read();
        rearm_hid_read();
        break;
    default:
        break;
    }
}

void forward_cdc_to_uart()
{
    if (!user_uart_bridge_enabled) {
        return;
    }
    uint8_t value = 0U;
    while (dequeue_cdc_byte(value)) {
        if (Serial.write(value) != 1U) {
            ++uart_tx_failure_count;
            const uint16_t previous = cdc_to_uart_tail == 0U
                ? kCdcToUartCapacity - 1U
                : cdc_to_uart_tail - 1U;
            cdc_to_uart_tail = previous;
            cdc_to_uart[previous] = value;
            break;
        }
        ++uart_tx_byte_count;
    }
}

void forward_uart_to_cdc()
{
    if (!user_uart_bridge_enabled ||
        !usb_configured ||
        cdc_in_state != CdcInState::kIdle) {
        return;
    }

    if (cdc_tx_pending_length == 0U) {
        size_t size = 0U;
        while (size < sizeof(cdc_tx) && Serial.available() > 0) {
            const int value = Serial.read();
            if (value < 0) {
                break;
            }
            cdc_tx[size++] = static_cast<uint8_t>(value);
        }
        cdc_tx_pending_length = static_cast<uint16_t>(size);
    }

    if (cdc_tx_pending_length == 0U) {
        return;
    }

    const uint16_t pending_length = cdc_tx_pending_length;
    cdc_in_state = CdcInState::kDataInFlight;
    ++cdc_in_submit_count;
    cdc_in_submit_byte_count += pending_length;
    if (usbd_ep_start_write(
            kUsbBusId,
            kCdcInEndpoint,
            cdc_tx,
            pending_length) != 0) {
        ++cdc_in_submit_failure_count;
        cdc_in_state = CdcInState::kIdle;
    }
}

} // namespace

void usb_cmsis_dap_get_feature(uint8_t **data, uint32_t *length)
{
    refresh_hid_feature();
    if (data != nullptr) {
        *data = hid_feature;
    }
    if (length != nullptr) {
        *length = sizeof(hid_feature);
    }
}

void usb_cmsis_dap_set_feature(
    uint8_t report_type,
    const uint8_t *report,
    uint32_t report_length)
{
    if (report_type != HID_REPORT_FEATURE ||
        report == nullptr ||
        report_length < 2U) {
        return;
    }

    if (report[0] == kRa4ApplicationResetFeatureCommand) {
        ra4_application_reset_pending = true;
        return;
    }

    if (report[0] == kBl616clRecoveryFeatureCommand) {
        (void)bl616cl_recovery_request();
        return;
    }

    if (report[0] == kRa4SambaProbeFeatureCommand &&
        report_length >= 2U && report[1] == 1U) {
        ra4_samba_probe_pending = true;
        return;
    }

    if (report[0] == kCdcInTelemetryFeatureCommand && report_length >= 2U) {
            diagnostic_feature_page = report[1] <= 82U ? report[1] : 0U;
        if (diagnostic_feature_page == 39U) runtime_heap_pending = true;
        if (diagnostic_feature_page == 30U) {
            udp_socket_pending = true;
            hid_feature[5] = 0U; // Host waits for the poll-published page.
        }
        if (diagnostic_feature_page == 42U) runtime_heap_ownership_request();
        return;
    }

    if (report[0] != kGpio35FeatureCommand ||
        user_uart_gpio35_control == nullptr) {
        return;
    }

    if (report[1] == static_cast<uint8_t>(UserUartGpio35Mode::kUart0Rx)) {
        (void)user_uart_gpio35_control->set_mode(UserUartGpio35Mode::kUart0Rx);
    } else if (report[1] == static_cast<uint8_t>(UserUartGpio35Mode::kHighImpedanceInput)) {
        (void)user_uart_gpio35_control->set_mode(
            UserUartGpio35Mode::kHighImpedanceInput);
    } else if (report[1] == static_cast<uint8_t>(UserUartGpio35Mode::kUart0RxNoPullup)) {
        (void)user_uart_gpio35_control->set_mode(
            UserUartGpio35Mode::kUart0RxNoPullup);
    } else if (report[1] == static_cast<uint8_t>(UserUartGpio35Mode::kPadDisabled)) {
        (void)user_uart_gpio35_control->set_mode(
            UserUartGpio35Mode::kPadDisabled);
    }
}

bool usb_cmsis_dap_begin()
{
    if (usb_initialized) {
        return true;
    }

    dap_init();
    usbd_desc_register(kUsbBusId, &kDescriptors);
    usbd_add_interface(
        kUsbBusId,
        usbd_cdc_acm_init_intf(kUsbBusId, &cdc_control_interface));
    usbd_add_interface(
        kUsbBusId,
        usbd_cdc_acm_init_intf(kUsbBusId, &cdc_data_interface));
    usbd_add_interface(
        kUsbBusId,
        usbd_hid_init_intf(
            kUsbBusId,
            &hid_interface,
            kHidReportDescriptor,
            sizeof(kHidReportDescriptor)));
    usbd_add_endpoint(kUsbBusId, &cdc_out_endpoint);
    usbd_add_endpoint(kUsbBusId, &cdc_in_endpoint);
    usbd_add_endpoint(kUsbBusId, &hid_out_endpoint);
    usbd_add_endpoint(kUsbBusId, &hid_in_endpoint);

    if (usbd_initialize(kUsbBusId, 0U, usb_event_callback) != 0) {
        return false;
    }

    usb_initialized = true;
    return true;
}

void usb_cmsis_dap_poll()
{
    sample_runtime_heap();
    sample_udp_socket();
    runtime_heap_ownership_poll();
    if (user_uart_gpio35_control != nullptr) {
        user_uart_gpio35_control->poll();
    }
    forward_cdc_to_uart();
    rearm_cdc_read();
    forward_uart_to_cdc();
}

bool usb_cmsis_dap_take_line_coding(uint32_t *baud)
{
    return line_coding_requests.pop(baud);
}

void usb_cmsis_dap_note_line_coding_applied(uint32_t baud)
{
    last_applied_line_coding_baud = baud;
    if (baud == 1200U) {
        ++line_coding_program_count;
    } else if (baud == 2400U) {
        ++line_coding_reset_count;
    } else {
        ++line_coding_normal_count;
    }
}

uint32_t usb_cmsis_dap_current_line_coding_baud()
{
    return cdc_current_line_coding.dwDTERate;
}

bool usb_cmsis_dap_set_user_uart_bridge_enabled(bool enabled)
{
    user_uart_bridge_enabled = enabled;
    cdc_to_uart_head = 0U;
    cdc_to_uart_tail = 0U;
    cdc_tx_pending_length = 0U;
    if (!enabled) {
        Serial.clearRx();
    }
    return true;
}

bool usb_cmsis_dap_user_uart_bridge_enabled()
{
    return user_uart_bridge_enabled;
}

bool usb_cmsis_dap_take_ra4_application_reset_request()
{
    if (!ra4_application_reset_pending) {
        return false;
    }

    ra4_application_reset_pending = false;
    return true;
}

bool usb_cmsis_dap_take_ra4_samba_probe_request()
{
    if (!ra4_samba_probe_pending) {
        return false;
    }
    ra4_samba_probe_pending = false;
    return true;
}

void usb_cmsis_dap_set_line_coding(const struct cdc_line_coding &line_coding)
{
    const uint32_t previous_baud = cdc_current_line_coding.dwDTERate;
    cdc_current_line_coding = line_coding;
    if (should_queue_line_coding_action(previous_baud, line_coding.dwDTERate)) {
        if (!line_coding_requests.push(line_coding.dwDTERate)) {
            ++line_coding_drop_count;
        }
    }
}

void usb_cmsis_dap_get_line_coding(struct cdc_line_coding *line_coding)
{
    if (line_coding != nullptr) {
        *line_coding = cdc_current_line_coding;
    }
}

void usb_cmsis_dap_set_user_uart_gpio35_control(UserUartGpio35Control *control)
{
    user_uart_gpio35_control = control;
}

void usb_cmsis_dap_set_ra4_at_service(Ra4AtService *service)
{
    ra4_at_service = service;
}

void usb_cmsis_dap_set_ra4_samba_service(Ra4SambaService *service)
{
    ra4_samba_service = service;
}

void usb_cmsis_dap_set_ra4_samba_programmer(Ra4SambaProgrammer *programmer)
{
    ra4_samba_programmer = programmer;
}

void usb_cmsis_dap_set_ra4_bootstrap_control(Ra4BootstrapControl *control)
{
    ra4_bootstrap_control = control;
}

} // namespace bridge

extern "C" void usbd_hid_get_report(
    uint8_t,
    uint8_t,
    uint8_t,
    uint8_t,
    uint8_t **data,
    uint32_t *length)
{
    bridge::usb_cmsis_dap_get_feature(data, length);
}

extern "C" void usbd_hid_set_report(
    uint8_t,
    uint8_t,
    uint8_t,
    uint8_t report_type,
    uint8_t *report,
    uint32_t report_length)
{
    bridge::usb_cmsis_dap_set_feature(report_type, report, report_length);
}

extern "C" void usbd_cdc_acm_set_line_coding(
    uint8_t,
    uint8_t,
    struct cdc_line_coding *line_coding)
{
    if (line_coding == nullptr) {
        return;
    }

    bridge::usb_cmsis_dap_set_line_coding(*line_coding);
}

extern "C" void usbd_cdc_acm_get_line_coding(
    uint8_t,
    uint8_t,
    struct cdc_line_coding *line_coding)
{
    bridge::usb_cmsis_dap_get_line_coding(line_coding);
}

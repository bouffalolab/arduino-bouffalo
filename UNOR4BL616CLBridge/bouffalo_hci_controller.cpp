#include "bouffalo_hci_controller.h"
#include "coex_diagnostic.h"

#include <string.h>

extern "C" {
#include <FreeRTOS.h>
#include <task.h>
#include "btble_lib_api.h"
#include "hci_onchip.h"
}

namespace bridge {

namespace {

constexpr uint8_t kH4Command = 0x01U;
constexpr uint8_t kH4AclData = 0x02U;
constexpr uint16_t kResetOpcode = 0x0C03U;
constexpr uint16_t kLeSetScanParametersOpcode = 0x200BU;
constexpr uint16_t kLeSetScanEnableOpcode = 0x200CU;
constexpr uint8_t kLeMetaAdvertisingReport = 0x02U;

uint16_t read_le16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t current_time_ms()
{
    return static_cast<uint32_t>(xTaskGetTickCount()) * portTICK_PERIOD_MS;
}

} // namespace

BouffaloHciController *BouffaloHciController::active_instance_ = nullptr;

BleHciController BouffaloHciController::active_controller()
{
    auto *controller = active_instance_;
    return controller != nullptr && controller->initialized()
        ? controller->controller() : BleHciController{};
}

void BouffaloHciController::apply_ap_scan_priority_if_active()
{
#if defined(BL616CL_AP_SCAN_PTI)
    auto *controller = active_instance_;
    if (controller != nullptr && controller->initialized() &&
        controller->scan_active_.load()) {
        coex_apply_ap_scan_priority(true);
    }
#endif
}

void BouffaloHciController::set_initialization_priority(
    uint8_t controller_task_priority)
{
    initialization_priority_ = controller_task_priority;
}

bool BouffaloHciController::apply_ap_join_scan_priority_if_active()
{
#if defined(BL616CL_AP_JOIN_SCAN_PTI)
    auto *controller = active_instance_;
    if (controller != nullptr && controller->initialized() &&
        controller->scan_active_.load()) {
        coex_apply_ap_join_scan_priority();
        return true;
    }
#endif
    return false;
}

bool BouffaloHciController::initialize_controller(uint8_t controller_task_priority)
{
    if (controller_initialized_ && hci_interface_initialized_) {
        return true;
    }
    if (controller_initialized_ || hci_interface_initialized_) {
        return false;
    }
    if (active_instance_ != nullptr && active_instance_ != this) {
        return false;
    }

    active_instance_ = this;
    btble_controller_init(controller_task_priority);
    controller_initialized_ = true;
#if defined(BL616CL_DIAGNOSTIC_DISABLE_CS2)
    // Opt-in interoperability control only. The pinned controller exports
    // this SDK API; llm_cs2_set_flag preserves the selection across HCI Reset.
    // Keep the normal CSA#2-capable controller configuration as the default.
    btble_controller_set_cs2(0U);
#endif

    if (bt_onchiphci_interface_init(&BouffaloHciController::receive_from_sdk) != 0U) {
        btble_controller_deinit();
        controller_initialized_ = false;
        active_instance_ = nullptr;
        return false;
    }

    hci_interface_initialized_ = true;
    return true;
}

void BouffaloHciController::deinitialize_controller()
{
    stop_impl();
    if (controller_initialized_) {
        btble_controller_deinit();
    }
    controller_initialized_ = false;
    hci_interface_initialized_ = false;
    if (active_instance_ == this) {
        active_instance_ = nullptr;
    }
}

bool BouffaloHciController::initialized() const
{
    return controller_initialized_ && hci_interface_initialized_;
}

BleHciController BouffaloHciController::controller()
{
    return {
        this,
        &BouffaloHciController::start,
        &BouffaloHciController::stop,
        &BouffaloHciController::send,
        &BouffaloHciController::pause_scan,
        &BouffaloHciController::resume_scan,
    };
}

bool BouffaloHciController::start(
    void *context,
    BleHciController::ReceivePacket receiver,
    void *receiver_context)
{
    return static_cast<BouffaloHciController *>(context)->start_impl(receiver, receiver_context);
}

void BouffaloHciController::stop(void *context)
{
    static_cast<BouffaloHciController *>(context)->stop_impl();
}

bool BouffaloHciController::send(void *context, const uint8_t *packet, size_t packet_size)
{
    return static_cast<BouffaloHciController *>(context)->send_impl(packet, packet_size);
}

void BouffaloHciController::receive_from_sdk(
    uint8_t packet_type,
    uint16_t source_id,
    uint8_t *parameters,
    uint8_t parameter_size)
{
    if (active_instance_ != nullptr) {
        active_instance_->receive_from_sdk_impl(
            static_cast<BouffaloHciPacketType>(packet_type),
            source_id,
            parameters,
            parameter_size);
    }
}

bool BouffaloHciController::start_impl(
    BleHciController::ReceivePacket receiver,
    void *receiver_context)
{
    if (transport_active_ || receiver == nullptr) {
        return false;
    }
    if (!initialized() &&
        !initialize_controller(initialization_priority_)) {
        return false;
    }

    packet_adapter_.set_receiver(receiver, receiver_context);
    transport_active_ = true;
    return true;
}

void BouffaloHciController::stop_impl()
{
    transport_active_ = false;
    packet_adapter_.clear_receiver();
    idle_scan_disable_compat_.reset();
    scan_advertising_quiet_gate_.reset();
    scan_advertising_rate_gate_.reset();
    reset_scan_parameter_prime();
    scan_enable_pending_ = false;
    scan_enable_pending_active_ = false;
    scan_active_ = false;
    scan_paused_ = false;
    tls_scan_completion_.store(0U);
    /*
     * ArduinoBLE may end its transport without sending LE Set Scan Enable(0).
     * Reset the on-chip controller at the session boundary so a later
     * HCIBEGIN cannot inherit scan activity or queued advertising reports.
     */
    if (controller_initialized_) {
        btble_controller_reset();
    }
}

bool BouffaloHciController::send_impl(const uint8_t *packet, size_t packet_size)
{
    if (!transport_active_ || packet == nullptr || packet_size == 0U) {
        return false;
    }

    hci_pkt_struct sdk_packet = {};
    uint16_t destination_id = 0U;
    uint8_t packet_type = 0U;
    bool scan_enable_command = false;
    bool scan_enable_active = false;

    switch (packet[0]) {
    case kH4Command:
        if (packet_size < 4U || packet_size != static_cast<size_t>(packet[3]) + 4U) {
            return false;
        }
        if (read_le16(packet + 1U) == kLeSetScanEnableOpcode &&
            (tls_scan_completion_.load() == 1U ||
             tls_scan_completion_.load() == 4U ||
             tls_scan_completion_.load() == 5U)) return false;
        if (idle_scan_disable_compat_.should_complete(packet, packet_size)) {
            static constexpr uint8_t kStatusSuccess = 0U;
            idle_scan_disable_compat_.note_synthetic_idle_disable();
            return packet_adapter_.receive_callback(
                BouffaloHciPacketType::CommandComplete,
                kLeSetScanEnableOpcode,
                &kStatusSuccess,
                sizeof(kStatusSuccess),
                rx_packet_,
                sizeof(rx_packet_));
        }
        if (read_le16(packet + 1U) == kResetOpcode) {
            reset_scan_parameter_prime();
            scan_advertising_quiet_gate_.reset();
            scan_advertising_rate_gate_.reset();
            scan_enable_pending_ = false;
            scan_enable_pending_active_ = false;
            scan_active_ = false;
            scan_paused_ = false;
            tls_scan_completion_.store(0U);
        }
        if (idle_scan_disable_compat_.take_scan_parameters_prime_request(packet, packet_size)) {
            scan_parameter_prime_pending_ = true;
        }
        idle_scan_disable_compat_.observe_host_packet(packet, packet_size);
        packet_type = BT_HCI_CMD;
        /*
         * bt_onchiphci_send() routes an HCI command by its opcode stored in
         * hci_cmd.opcode. The destination task ID remains zero; the SDK
         * resolves the controller task from the opcode internally.
         */
        sdk_packet.p.hci_cmd.opcode = read_le16(packet + 1U);
        sdk_packet.p.hci_cmd.params = const_cast<uint8_t *>(packet + 4U);
        sdk_packet.p.hci_cmd.param_len = packet[3];
        scan_enable_command =
            sdk_packet.p.hci_cmd.opcode == kLeSetScanEnableOpcode &&
            packet[3] == 2U;
        scan_enable_active = scan_enable_command && packet[4] != 0U;
#if defined(BL616CL_AP_SCAN_PTI)
        if (scan_enable_active) coex_apply_ap_scan_priority();
#endif
        if (scan_enable_command) {
            scan_enable_pending_ = true;
            scan_enable_pending_active_ = scan_enable_active;
            if (scan_enable_active) scan_duplicate_filter_ = packet[5];
        }
        break;

    case kH4AclData: {
        if (packet_size < 5U || packet_size != static_cast<size_t>(read_le16(packet + 3U)) + 5U) {
            return false;
        }
        const uint16_t handle_with_flags = read_le16(packet + 1U);
        packet_type = BT_HCI_ACL_DATA;
        destination_id = static_cast<uint16_t>(handle_with_flags & 0x0FFFU);
        sdk_packet.p.acl_data.conhdl = destination_id;
        sdk_packet.p.acl_data.pb_bc_flag = static_cast<uint8_t>(handle_with_flags >> 12U);
        sdk_packet.p.acl_data.len = read_le16(packet + 3U);
        sdk_packet.p.acl_data.buffer = const_cast<uint8_t *>(packet + 5U);
        break;
    }

    default:
        return false;
    }

    const bool sent = bt_onchiphci_send(packet_type, destination_id, &sdk_packet) == 0;
    if (!sent && scan_enable_command) {
        scan_enable_pending_ = false;
        scan_enable_pending_active_ = false;
    }
    return sent;
}

bool BouffaloHciController::pause_scan(void *context)
{
    return static_cast<BouffaloHciController *>(context)->pause_scan_impl();
}

void BouffaloHciController::resume_scan(void *context)
{
    static_cast<BouffaloHciController *>(context)->resume_scan_impl();
}

bool BouffaloHciController::send_tls_scan_enable(uint8_t enable)
{
    // This runs in the AT task; completion arrives in the controller task.
    // Never expose these private completions to the RA4M1 HCI command queue.
    const uint8_t prior = tls_scan_completion_.load();
    if (prior == 1U || prior == 4U || prior == 5U) return false;
    uint8_t parameters[] = {enable, scan_duplicate_filter_};
    hci_pkt_struct sdk_packet = {};
    sdk_packet.p.hci_cmd.opcode = kLeSetScanEnableOpcode;
    sdk_packet.p.hci_cmd.params = parameters;
    sdk_packet.p.hci_cmd.param_len = sizeof(parameters);
    const uint8_t waiting = enable == 0U ? 1U : 5U;
    tls_scan_completion_.store(waiting);
    if (bt_onchiphci_send(BT_HCI_CMD, 0U, &sdk_packet) != 0) {
        ++scan_window_counters().rejected;
        tls_scan_completion_.store(0U);
        return false;
    }
    const uint32_t started = current_time_ms();
    while (tls_scan_completion_.load() == waiting &&
           current_time_ms() - started < 1000U) {
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
    // Keep ownership of a late completion; the callback restores scanning if
    // the AT task could not complete its resume before this event arrived.
    if (tls_scan_completion_.load() == waiting) ++scan_window_counters().wait_timeouts;
    return tls_scan_completion_.load() == 2U;
}

bool BouffaloHciController::pause_scan_impl()
{
    if (!transport_active_ || !scan_active_ || scan_paused_ ||
        scan_enable_pending_) return false;
    scan_paused_ = true;
    ++scan_window_counters().pause_attempts;
    if (send_tls_scan_enable(0U)) {
        // Give queued WLAN receive work a bounded RF window before an AT
        // receive/availability probe. Idle BLE and no-scan TLS pay no delay.
        vTaskDelay(pdMS_TO_TICKS(20U));
        return true;
    }
    resume_scan_impl();
    return false;
}

void BouffaloHciController::resume_scan_impl()
{
    if (!transport_active_ || !scan_paused_) return;
    if (tls_scan_completion_.load() == 2U) vTaskDelay(pdMS_TO_TICKS(20U));
    // Disabling legacy scanning preserves its parameters in the controller.
    // Restore the host's duplicate filter as well as scan enable.
    uint32_t waiting_disable = 1U;
    if (tls_scan_completion_.compare_exchange_strong(waiting_disable, 4U)) return;
    ++scan_window_counters().restore_attempts;
    if (send_tls_scan_enable(1U)) scan_paused_ = false;
}

bool BouffaloHciController::send_internal_scan_disable()
{
    static uint8_t parameters[] = { 0U, 0U };
    hci_pkt_struct sdk_packet = {};
    sdk_packet.p.hci_cmd.opcode = kLeSetScanEnableOpcode;
    sdk_packet.p.hci_cmd.params = parameters;
    sdk_packet.p.hci_cmd.param_len = sizeof(parameters);
    return bt_onchiphci_send(BT_HCI_CMD, 0U, &sdk_packet) == 0;
}

void BouffaloHciController::reset_scan_parameter_prime()
{
    scan_parameter_prime_pending_ = false;
    scan_parameter_prime_waiting_ = false;
    scan_parameter_prime_status_ = 0U;
}

void BouffaloHciController::receive_from_sdk_impl(
    BouffaloHciPacketType packet_type,
    uint16_t source_id,
    uint8_t *parameters,
    uint8_t parameter_size)
{
    if (!transport_active_) {
        return;
    }

    const uint8_t tls_scan_state = tls_scan_completion_.load();
    if (packet_type == BouffaloHciPacketType::CommandComplete &&
        source_id == kLeSetScanEnableOpcode &&
        (tls_scan_state == 1U || tls_scan_state == 4U || tls_scan_state == 5U)) {
        const bool success = parameter_size == 1U && parameters != nullptr &&
            parameters[0] == 0U;
        if (!success) ++scan_window_counters().rejected;
        else if (tls_scan_state == 5U) ++scan_window_counters().restore_confirmed;
        else ++scan_window_counters().disable_confirmed;
        // A disable may finish after its bounded wait. In that case restore
        // the original scan asynchronously and retain private event ownership.
        uint32_t expected = 1U;
        if (tls_scan_state == 1U &&
            tls_scan_completion_.compare_exchange_strong(expected, success ? 2U : 3U)) {
            return;
        }
        if (tls_scan_state == 4U || expected == 4U) {
            uint8_t restore[] = {1U, scan_duplicate_filter_};
            hci_pkt_struct packet = {};
            packet.p.hci_cmd.opcode = kLeSetScanEnableOpcode;
            packet.p.hci_cmd.params = restore;
            packet.p.hci_cmd.param_len = sizeof(restore);
            tls_scan_completion_.store(5U);
            ++scan_window_counters().restore_attempts;
            if (bt_onchiphci_send(BT_HCI_CMD, 0U, &packet) != 0) {
                ++scan_window_counters().rejected;
                tls_scan_completion_.store(3U);
            }
            return;
        }
        if (success) scan_paused_ = false;
        tls_scan_completion_.store(success ? 2U : 3U);
        return;
    }

    if (packet_type == BouffaloHciPacketType::CommandComplete &&
        source_id == kLeSetScanParametersOpcode &&
        scan_parameter_prime_pending_) {
        scan_parameter_prime_pending_ = false;
        if (parameter_size != 1U || parameters == nullptr || parameters[0] != 0U) {
            (void)packet_adapter_.receive_callback(
                BouffaloHciPacketType::CommandComplete,
                kLeSetScanParametersOpcode,
                parameters,
                parameter_size,
                rx_packet_,
                sizeof(rx_packet_));
            return;
        }

        scan_parameter_prime_status_ = parameters[0];
        scan_parameter_prime_waiting_ = true;
        if (!send_internal_scan_disable()) {
            scan_parameter_prime_waiting_ = false;
            static constexpr uint8_t kStatusCommandDisallowed = 0x0CU;
            (void)packet_adapter_.receive_callback(
                BouffaloHciPacketType::CommandComplete,
                kLeSetScanParametersOpcode,
                &kStatusCommandDisallowed,
                sizeof(kStatusCommandDisallowed),
                rx_packet_,
                sizeof(rx_packet_));
        }
        return;
    }

    if (packet_type == BouffaloHciPacketType::CommandComplete &&
        source_id == kLeSetScanEnableOpcode &&
        scan_parameter_prime_waiting_) {
        scan_parameter_prime_waiting_ = false;
        const uint8_t status =
            parameter_size == 1U && parameters != nullptr ? parameters[0] : 0x1FU;
        const uint8_t parameter_status = status == 0U ? scan_parameter_prime_status_ : status;
        (void)packet_adapter_.receive_callback(
            BouffaloHciPacketType::CommandComplete,
            kLeSetScanParametersOpcode,
            &parameter_status,
            sizeof(parameter_status),
            rx_packet_,
            sizeof(rx_packet_));
        return;
    }

    if (packet_type == BouffaloHciPacketType::CommandComplete &&
        source_id == kLeSetScanEnableOpcode &&
        scan_enable_pending_) {
        scan_enable_pending_ = false;
        if (parameter_size == 1U && parameters != nullptr && parameters[0] == 0U) {
            if (scan_enable_pending_active_) {
                scan_active_ = true;
                scan_advertising_rate_gate_.reset();
                scan_advertising_quiet_gate_.arm(current_time_ms());
            } else {
                scan_active_ = false;
                scan_advertising_quiet_gate_.reset();
                scan_advertising_rate_gate_.reset();
            }
        }
        scan_enable_pending_active_ = false;
    }

    if (packet_type == BouffaloHciPacketType::LeEvent &&
        parameter_size != 0U &&
        parameters != nullptr &&
        parameters[0] == kLeMetaAdvertisingReport &&
        scan_active_) {
        const uint32_t now_ms = current_time_ms();
        if (scan_advertising_quiet_gate_.should_drop(now_ms) ||
            scan_advertising_rate_gate_.should_drop(now_ms)) {
            return;
        }
    }

    if (packet_type == BouffaloHciPacketType::AclData) {
        /*
         * The SDK helper copies the controller-owned ACL buffer and releases
         * it before this callback returns. The current bridge m2s1 profile
         * uses BLE ACL RX buffers below this fixed H:4 maximum. A profile
         * change must recheck that contract because the SDK helper has no
         * output-capacity argument.
         */
        const uint16_t acl_packet_size = bt_onchiphci_handle_rx_acl(parameters, rx_packet_ + 1U);
        if (acl_packet_size == 0U || acl_packet_size + 1U > sizeof(rx_packet_)) {
            return;
        }
        (void)packet_adapter_.receive_acl(
            rx_packet_ + 1U,
            acl_packet_size,
            rx_packet_,
            sizeof(rx_packet_));
        return;
    }

    (void)packet_adapter_.receive_callback(
        packet_type,
        source_id,
        parameters,
        parameter_size,
        rx_packet_,
        sizeof(rx_packet_));
}

} // namespace bridge

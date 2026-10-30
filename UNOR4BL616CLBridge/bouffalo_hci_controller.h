#ifndef UNOR4_BL616CL_BOUFFALO_HCI_CONTROLLER_H
#define UNOR4_BL616CL_BOUFFALO_HCI_CONTROLLER_H

#include <stdint.h>
#include <atomic>

#include "ble_hci_service.h"
#include "hci_idle_scan_disable_compat.h"
#include "hci_scan_advertising_quiet_gate.h"
#include "bouffalo_hci_packet_adapter.h"

namespace bridge {

/*
 * This is the BL616CL-specific half of the raw HCI transport. Runtime startup
 * must call initialize_controller() only after its RF prerequisites are ready.
 */
class BouffaloHciController {
public:
    void set_initialization_priority(uint8_t controller_task_priority);
    bool initialize_controller(uint8_t controller_task_priority);
    void deinitialize_controller();
    bool initialized() const;

    BleHciController controller();
    static void apply_ap_scan_priority_if_active();
    static bool apply_ap_join_scan_priority_if_active();
    static BleHciController active_controller();

private:
    static bool start(void *context, BleHciController::ReceivePacket receiver, void *receiver_context);
    static void stop(void *context);
    static bool send(void *context, const uint8_t *packet, size_t packet_size);
    static void receive_from_sdk(uint8_t packet_type, uint16_t source_id, uint8_t *parameters, uint8_t parameter_size);

    bool start_impl(BleHciController::ReceivePacket receiver, void *receiver_context);
    void stop_impl();
    bool send_impl(const uint8_t *packet, size_t packet_size);
    bool pause_scan_impl();
    void resume_scan_impl();
    static bool pause_scan(void *context);
    static void resume_scan(void *context);
    bool send_internal_scan_disable();
    bool send_tls_scan_enable(uint8_t enable);
    void reset_scan_parameter_prime();
    void receive_from_sdk_impl(
        BouffaloHciPacketType packet_type,
        uint16_t source_id,
        uint8_t *parameters,
        uint8_t parameter_size);

    static BouffaloHciController *active_instance_;

    BouffaloHciPacketAdapter packet_adapter_;
    HciIdleScanDisableCompat idle_scan_disable_compat_;
    HciScanAdvertisingQuietGate scan_advertising_quiet_gate_;
    HciScanAdvertisingRateGate scan_advertising_rate_gate_;
    bool scan_parameter_prime_pending_ = false;
    bool scan_parameter_prime_waiting_ = false;
    bool scan_enable_pending_ = false;
    bool scan_enable_pending_active_ = false;
    std::atomic<bool> scan_active_{false};
    std::atomic<bool> scan_paused_{false};
    uint8_t scan_duplicate_filter_ = 0U;
    std::atomic<uint32_t> tls_scan_completion_{0U};
    uint8_t scan_parameter_prime_status_ = 0U;
    uint8_t rx_packet_[BleHciService::kMaxPacketBytes] = {};
    uint8_t initialization_priority_ = 0U;
    bool controller_initialized_ = false;
    bool hci_interface_initialized_ = false;
    bool transport_active_ = false;
};

} // namespace bridge

#endif

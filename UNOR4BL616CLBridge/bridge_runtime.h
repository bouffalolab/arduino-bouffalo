#ifndef UNOR4_BL616CL_BRIDGE_RUNTIME_H
#define UNOR4_BL616CL_BRIDGE_RUNTIME_H

#include <stdint.h>

#include "ble_hci_service.h"
#include "bouffalo_hci_controller.h"

extern "C" {
#include "async_event.h"
}

namespace bridge {

enum class BridgeRuntimeState : uint8_t {
    Created,
    WaitingForWifiInit,
    WaitingForWifiManager,
    Ready,
    Failed,
};

enum class BridgeRuntimeFailure : uint8_t {
    None,
    AlreadyActive,
    EasyFlashInit,
    RfInit,
    WifiEventFilter,
    FhostInit,
    WifiInitTimeout,
    WifiManagerStart,
    WifiManagerTimeout,
    HeapOrder,
};

/*
 * Owns the one-time SDK initialization sequence shared by bridge services.
 * BLE controller initialization is deferred until the RA4M1 requests
 * AT+HCIBEGIN. Keeping the controller out of the default Wi-Fi-only startup
 * path preserves WRAM for SoftAP and other WiFiS3 users that never use BLE.
 */
class BridgeRuntime {
public:
    typedef void (*WifiEventListener)(void *context, uint16_t code);

    static constexpr uint32_t kWifiTimeoutMs = 20000U;

    explicit BridgeRuntime(BouffaloHciController &hci_controller);

    bool begin(uint8_t controller_task_priority, uint8_t controller_priority);
    void poll();
    void set_wifi_event_listener(WifiEventListener listener, void *context);

    bool ready() const;
    bool failed() const;
    BridgeRuntimeState state() const;
    BridgeRuntimeFailure failure() const;
    int failure_status() const;

    static const char *failure_text(BridgeRuntimeFailure failure);
    static BleHciSynchronization hci_synchronization();

private:
    static void wifi_event_handler(async_input_event_t event, void *private_data);
    static void enter_critical(void *context);
    static void exit_critical(void *context);

    void handle_wifi_event(async_input_event_t event);
    void fail(BridgeRuntimeFailure failure, int status);
    int restore_initialization_heap_order();

    static BridgeRuntime *active_instance_;

    BouffaloHciController &hci_controller_;
    volatile bool wifi_init_done_ = false;
    volatile bool wifi_manager_done_ = false;
    volatile int wifi_manager_status_ = 0;
    uint32_t wifi_started_at_ = 0U;
    WifiEventListener wifi_event_listener_ = nullptr;
    void *wifi_event_listener_context_ = nullptr;
    BridgeRuntimeState state_ = BridgeRuntimeState::Created;
    BridgeRuntimeFailure failure_ = BridgeRuntimeFailure::None;
    int failure_status_ = 0;
#if defined(BL616CL_RUNTIME_INIT_WRAM_FIRST) && BL616CL_RUNTIME_INIT_WRAM_FIRST
    uint32_t saved_heap_order_[8] = {};
    bool initialization_heap_order_active_ = false;
#endif
};

} // namespace bridge

#endif

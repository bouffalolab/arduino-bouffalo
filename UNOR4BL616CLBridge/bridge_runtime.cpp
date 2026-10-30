#include <Arduino.h>

#include "bridge_runtime.h"
#include "ap_packet_input_hook.h"
#include "bridge_config.h"
#include "runtime_heap_policy.h"

extern "C" {
#include <FreeRTOS.h>
#include <task.h>
#include "async_event.h"
#include "bflb_mtd.h"
#include "easyflash.h"
#include "fhost_api.h"
#include "lwip/tcpip.h"
#include "rfparam_adapter.h"
#include "sdk/mm/mm.h"
#include "wifi_mgmr.h"
#include "wifi_mgmr_ext.h"
}

namespace bridge {

#if defined(BL616CL_RUNTIME_INIT_WRAM_FIRST) && BL616CL_RUNTIME_INIT_WRAM_FIRST && \
    defined(BL616CL_RUNTIME_WRAM_FIRST) && BL616CL_RUNTIME_WRAM_FIRST
#error "Select initialization-only or persistent WRAM priority, not both"
#endif

BridgeRuntime *BridgeRuntime::active_instance_ = nullptr;

BridgeRuntime::BridgeRuntime(BouffaloHciController &hci_controller)
    : hci_controller_(hci_controller)
{
}

bool BridgeRuntime::begin(uint8_t controller_task_priority, uint8_t controller_priority)
{
    if (state_ != BridgeRuntimeState::Created) {
        return !failed();
    }
    if (active_instance_ != nullptr && active_instance_ != this) {
        fail(BridgeRuntimeFailure::AlreadyActive, -1);
        return false;
    }

    active_instance_ = this;
    (void)controller_task_priority;
    hci_controller_.set_initialization_priority(controller_priority);

#if defined(BL616CL_RUNTIME_INIT_WRAM_FIRST) && BL616CL_RUNTIME_INIT_WRAM_FIRST
    static_assert(CONFIG_MM_HEAP_COUNT == 8, "saved heap order capacity");
    int order_status = mm_heap_get_auto_list(saved_heap_order_, CONFIG_MM_HEAP_COUNT);
    if (order_status != 0) {
        fail(BridgeRuntimeFailure::HeapOrder, order_status);
        return false;
    }
    const uint32_t initialization_order[] = {MM_HEAP_WRAM_0, MM_HEAP_OCRAM_0};
    order_status = mm_heap_set_auto_list(initialization_order, 2U);
    if (order_status != 0) {
        fail(BridgeRuntimeFailure::HeapOrder, order_status);
        return false;
    }
    initialization_heap_order_active_ = true;
#endif

#if defined(BL616CL_RUNTIME_WRAM_FIRST) && BL616CL_RUNTIME_WRAM_FIRST
    // Experimental only: preserve CPU heap for later allocations. Explicit
    // RF/packet WRAM requests retain their SDK selection and do not fall back.
    const uint32_t heap_order[] = {MM_HEAP_WRAM_0, MM_HEAP_OCRAM_0};
    const int heap_status = mm_heap_set_auto_list(heap_order, 2U);
    if (heap_status != 0) {
        fail(BridgeRuntimeFailure::HeapOrder, heap_status);
        return false;
    }
#endif

    bflb_mtd_init();

    const EfErrCode easyflash_status = easyflash_init();
    if (easyflash_status != EF_NO_ERR) {
        fail(BridgeRuntimeFailure::EasyFlashInit, static_cast<int>(easyflash_status));
        return false;
    }

    const int rf_status = rfparam_init(0, nullptr, 0);
    if (rf_status != 0) {
        fail(BridgeRuntimeFailure::RfInit, rf_status);
        return false;
    }

    const int event_status =
        async_register_event_filter(EV_WIFI, &BridgeRuntime::wifi_event_handler, nullptr);
    if (event_status != 0) {
        fail(BridgeRuntimeFailure::WifiEventFilter, event_status);
        return false;
    }

    tcpip_init(nullptr, nullptr);
    wifi_task_create();
    wifi_mgmr_coex_enable(kEnableWifiBleCoexistence);
    const int fhost_status = fhost_init();
    if (fhost_status != 0) {
        fail(BridgeRuntimeFailure::FhostInit, fhost_status);
        return false;
    }
    ap_packet_input_hook_begin();

    wifi_started_at_ = millis();
    state_ = BridgeRuntimeState::WaitingForWifiInit;
    return true;
}

void BridgeRuntime::poll()
{
    if (failed() || ready()) {
        return;
    }

    const uint32_t now = millis();
    switch (state_) {
    case BridgeRuntimeState::WaitingForWifiInit:
        if (!wifi_init_done_) {
            if (static_cast<uint32_t>(now - wifi_started_at_) > kWifiTimeoutMs) {
                fail(BridgeRuntimeFailure::WifiInitTimeout, -1);
            }
            return;
        }
        if (wifi_manager_status_ != 0) {
            fail(BridgeRuntimeFailure::WifiManagerStart, wifi_manager_status_);
            return;
        }
        state_ = BridgeRuntimeState::WaitingForWifiManager;
        return;

    case BridgeRuntimeState::WaitingForWifiManager:
        if (!wifi_manager_done_) {
            if (static_cast<uint32_t>(now - wifi_started_at_) > kWifiTimeoutMs) {
                fail(BridgeRuntimeFailure::WifiManagerTimeout, -1);
            }
            return;
        }
        {
            const int status = restore_initialization_heap_order();
            if (status != 0) {
                fail(BridgeRuntimeFailure::HeapOrder, status);
                return;
            }
        }
        runtime_heap_policy_enable(true);
        state_ = BridgeRuntimeState::Ready;
        return;

    case BridgeRuntimeState::Created:
    case BridgeRuntimeState::Ready:
    case BridgeRuntimeState::Failed:
        return;
    }
}

void BridgeRuntime::set_wifi_event_listener(
    WifiEventListener listener,
    void *context)
{
    wifi_event_listener_ = listener;
    wifi_event_listener_context_ = context;
}

bool BridgeRuntime::ready() const
{
    return state_ == BridgeRuntimeState::Ready;
}

bool BridgeRuntime::failed() const
{
    return state_ == BridgeRuntimeState::Failed;
}

BridgeRuntimeState BridgeRuntime::state() const
{
    return state_;
}

BridgeRuntimeFailure BridgeRuntime::failure() const
{
    return failure_;
}

int BridgeRuntime::failure_status() const
{
    return failure_status_;
}

const char *BridgeRuntime::failure_text(BridgeRuntimeFailure failure)
{
    switch (failure) {
    case BridgeRuntimeFailure::None:
        return "NONE";
    case BridgeRuntimeFailure::AlreadyActive:
        return "RUNTIME_ACTIVE";
    case BridgeRuntimeFailure::EasyFlashInit:
        return "EASYFLASH_INIT";
    case BridgeRuntimeFailure::RfInit:
        return "RF_INIT";
    case BridgeRuntimeFailure::WifiEventFilter:
        return "WIFI_EVENT_FILTER";
    case BridgeRuntimeFailure::FhostInit:
        return "FHOST_INIT";
    case BridgeRuntimeFailure::WifiInitTimeout:
        return "WIFI_INIT_TIMEOUT";
    case BridgeRuntimeFailure::WifiManagerStart:
        return "WIFI_MGMR_START";
    case BridgeRuntimeFailure::HeapOrder:
        return "HEAP_ORDER";
    case BridgeRuntimeFailure::WifiManagerTimeout:
        return "WIFI_MGMR_READY_TIMEOUT";
    }
    return "UNKNOWN";
}

BleHciSynchronization BridgeRuntime::hci_synchronization()
{
    return {
        nullptr,
        &BridgeRuntime::enter_critical,
        &BridgeRuntime::exit_critical,
    };
}

void BridgeRuntime::wifi_event_handler(async_input_event_t event, void *)
{
    if (active_instance_ != nullptr) {
        active_instance_->handle_wifi_event(event);
    }
}

void BridgeRuntime::enter_critical(void *)
{
    taskENTER_CRITICAL();
}

void BridgeRuntime::exit_critical(void *)
{
    taskEXIT_CRITICAL();
}

void BridgeRuntime::handle_wifi_event(async_input_event_t input_event)
{
    if (input_event == nullptr) {
        return;
    }

    switch (input_event->code) {
    case CODE_WIFI_ON_INIT_DONE:
        wifi_manager_status_ = wifi_mgmr_task_start();
        wifi_init_done_ = true;
        break;
    case CODE_WIFI_ON_MGMR_DONE:
        wifi_manager_done_ = true;
        break;
    default:
        break;
    }

    if (wifi_event_listener_ != nullptr) {
        wifi_event_listener_(wifi_event_listener_context_, input_event->code);
    }
}

void BridgeRuntime::fail(BridgeRuntimeFailure failure, int status)
{
    const int restore_status = restore_initialization_heap_order();
    if (restore_status != 0) {
        failure = BridgeRuntimeFailure::HeapOrder;
        status = restore_status;
    }
    failure_ = failure;
    failure_status_ = status;
    state_ = BridgeRuntimeState::Failed;
}

int BridgeRuntime::restore_initialization_heap_order()
{
#if defined(BL616CL_RUNTIME_INIT_WRAM_FIRST) && BL616CL_RUNTIME_INIT_WRAM_FIRST
    if (initialization_heap_order_active_) {
        initialization_heap_order_active_ = false;
        return mm_heap_set_auto_list(saved_heap_order_, CONFIG_MM_HEAP_COUNT);
    }
#endif
    return 0;
}

} // namespace bridge

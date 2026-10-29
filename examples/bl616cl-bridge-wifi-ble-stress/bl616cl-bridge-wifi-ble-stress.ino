#include <Arduino.h>

extern "C" {
#include <FreeRTOS.h>
#include <task.h>
#include "async_event.h"
#include "bflb_mtd.h"
#include "btble_lib_api.h"
#include "easyflash.h"
#include "fhost_api.h"
#include "hci_onchip.h"
#include "lwip/tcpip.h"
#include "mm.h"
#include "rfparam_adapter.h"
#include "wifi_mgmr.h"
#include "wifi_mgmr_ext.h"
}

namespace {

constexpr uint16_t kHciResetOpcode = 0x0C03U;
constexpr uint8_t kRequiredCycles = 10U;
constexpr unsigned long kWifiTimeoutMs = 20000UL;
constexpr unsigned long kControllerTimeoutMs = 10000UL;
constexpr unsigned long kCycleTimeoutMs = 25000UL;
constexpr unsigned long kCycleGapMs = 500UL;
constexpr size_t kMaxCallbackBytes = 64U;

volatile bool wifi_init_done = false;
volatile bool wifi_manager_done = false;
volatile int wifi_manager_status = 0;
volatile bool wifi_scan_done = false;
volatile unsigned long wifi_scan_completed_at = 0UL;

volatile bool controller_task_started = false;
volatile bool controller_started = false;
volatile bool hci_interface_ready = false;
volatile int hci_interface_status = 0;
volatile bool hci_callback_received = false;
volatile bool hci_callback_overflow = false;
volatile uint8_t hci_packet_type = 0U;
volatile uint16_t hci_source_id = 0U;
volatile uint8_t hci_param_length = 0U;
volatile uint8_t hci_params[kMaxCallbackBytes];
volatile unsigned long hci_reset_completed_at = 0UL;

bool smoke_failed = false;
bool controller_task_requested = false;
bool cycle_active = false;
bool hci_reset_sent = false;
bool hci_reset_complete = false;
bool result_reported = false;
uint8_t completed_cycles = 0U;
unsigned long wifi_started_at = 0UL;
unsigned long controller_started_at = 0UL;
unsigned long cycle_started_at = 0UL;
unsigned long hci_reset_sent_at = 0UL;
unsigned long next_cycle_at = 0UL;
size_t initial_free_bytes = 0U;
size_t first_cycle_free_bytes = 0U;
size_t last_cycle_free_bytes = 0U;
size_t minimum_free_bytes = 0U;
unsigned long maximum_scan_ms = 0UL;
unsigned long maximum_hci_ms = 0UL;
unsigned long maximum_cycle_ms = 0UL;

void report_failure(const char *stage, int status)
{
    Serial.print("BL616CL_SMOKE:FAIL stage=");
    Serial.print(stage);
    Serial.print(" code=");
    Serial.println(status);
    smoke_failed = true;
}

void wifi_event_handler(async_input_event_t event, void *)
{
    if (event == nullptr) {
        return;
    }

    switch (event->code) {
    case CODE_WIFI_ON_INIT_DONE:
        wifi_init_done = true;
        wifi_manager_status = wifi_mgmr_task_start();
        break;
    case CODE_WIFI_ON_MGMR_DONE:
        wifi_manager_done = true;
        break;
    case CODE_WIFI_ON_SCAN_DONE:
        wifi_scan_completed_at = millis();
        wifi_scan_done = true;
        break;
    default:
        break;
    }
}

void hci_receive_callback(
    uint8_t packet_type,
    uint16_t source_id,
    uint8_t *params,
    uint8_t param_length)
{
    if (param_length > kMaxCallbackBytes) {
        hci_callback_overflow = true;
        hci_callback_received = true;
        return;
    }

    for (uint8_t index = 0U; index < param_length; ++index) {
        hci_params[index] = params[index];
    }

    hci_packet_type = packet_type;
    hci_source_id = source_id;
    hci_param_length = param_length;
    hci_reset_completed_at = millis();
    hci_callback_received = true;
}

void ble_controller_task(void *)
{
    controller_task_started = true;
    Serial.println("BL616CL_SMOKE:BLE_CONTROLLER_INIT_STARTED");
    btble_controller_init(configMAX_PRIORITIES - 1U);
    controller_started = true;
    Serial.println("BL616CL_SMOKE:BLE_CONTROLLER_STARTED");

    hci_interface_status = bt_onchiphci_interface_init(hci_receive_callback);
    hci_interface_ready = true;
    vTaskDelete(nullptr);
}

void start_controller_task()
{
    controller_task_requested = true;
    controller_started_at = millis();
    if (xTaskCreate(
            ble_controller_task,
            "ble_controller",
            1024U,
            nullptr,
            configMAX_PRIORITIES - 2U,
            nullptr) != pdPASS) {
        report_failure("BLE_TASK_CREATE", -1);
    }
}

void start_stress_cycle()
{
    wifi_scan_done = false;
    hci_callback_received = false;
    hci_callback_overflow = false;
    hci_reset_sent = false;
    hci_reset_complete = false;
    wifi_scan_completed_at = 0UL;
    hci_reset_completed_at = 0UL;

    wifi_mgmr_scan_params_t scan = {};
    cycle_started_at = millis();
    const int scan_status = wifi_mgmr_sta_scan(&scan);
    if (scan_status != 0) {
        report_failure("SCAN_START", scan_status);
        return;
    }

    cycle_active = true;
    Serial.print("BL616CL_SMOKE:COEX_STRESS_SCAN_STARTED cycle=");
    Serial.println(completed_cycles + 1U);

    hci_pkt_struct packet = {};
    packet.p.hci_cmd.opcode = kHciResetOpcode;
    packet.p.hci_cmd.params = nullptr;
    packet.p.hci_cmd.param_len = 0U;

    hci_reset_sent_at = millis();
    const int hci_status = bt_onchiphci_send(BT_HCI_CMD, kHciResetOpcode, &packet);
    if (hci_status != 0) {
        report_failure("HCI_RESET_SEND", hci_status);
        return;
    }

    hci_reset_sent = true;
}

void process_hci_response()
{
    if (!hci_callback_received) {
        return;
    }

    hci_callback_received = false;
    const bool is_reset_complete =
        !hci_callback_overflow &&
        hci_packet_type == BT_HCI_CMD_CMP_EVT &&
        hci_source_id == kHciResetOpcode &&
        hci_param_length == 1U &&
        hci_params[0] == 0U;

    if (!is_reset_complete) {
        report_failure("HCI_RESET_RESPONSE", -1);
        return;
    }

    hci_reset_complete = true;
}

void complete_stress_cycle()
{
    const unsigned long now = millis();
    const uint32_t scan_count = wifi_mgmr_sta_scanlist_nums_get();
    const unsigned long scan_ms = wifi_scan_completed_at - cycle_started_at;
    const unsigned long hci_ms = hci_reset_completed_at - hci_reset_sent_at;
    const unsigned long cycle_ms = now - cycle_started_at;
    const size_t free_bytes = kfree_size(0U);

    if (free_bytes == 0U) {
        report_failure("HEAP_FREE_QUERY", -1);
        return;
    }

    ++completed_cycles;
    cycle_active = false;
    next_cycle_at = now + kCycleGapMs;

    if (completed_cycles == 1U) {
        first_cycle_free_bytes = free_bytes;
        minimum_free_bytes = free_bytes;
    } else if (free_bytes < minimum_free_bytes) {
        minimum_free_bytes = free_bytes;
    }
    last_cycle_free_bytes = free_bytes;

    if (scan_ms > maximum_scan_ms) {
        maximum_scan_ms = scan_ms;
    }
    if (hci_ms > maximum_hci_ms) {
        maximum_hci_ms = hci_ms;
    }
    if (cycle_ms > maximum_cycle_ms) {
        maximum_cycle_ms = cycle_ms;
    }

    Serial.print("BL616CL_SMOKE:COEX_STRESS_CYCLE_DONE cycle=");
    Serial.print(completed_cycles);
    Serial.print(" count=");
    Serial.print(scan_count);
    Serial.print(" scan_ms=");
    Serial.print(scan_ms);
    Serial.print(" hci_ms=");
    Serial.print(hci_ms);
    Serial.print(" cycle_ms=");
    Serial.print(cycle_ms);
    Serial.print(" heap_free=");
    Serial.print(free_bytes);
    Serial.print(" heap_min=");
    Serial.println(minimum_free_bytes);

    if (completed_cycles == kRequiredCycles) {
        const long heap_delta =
            static_cast<long>(last_cycle_free_bytes) -
            static_cast<long>(first_cycle_free_bytes);
        Serial.print("BL616CL_SMOKE:COEX_STRESS_DONE cycles=");
        Serial.print(kRequiredCycles);
        Serial.print(" heap_initial=");
        Serial.print(initial_free_bytes);
        Serial.print(" heap_first=");
        Serial.print(first_cycle_free_bytes);
        Serial.print(" heap_last=");
        Serial.print(last_cycle_free_bytes);
        Serial.print(" heap_min=");
        Serial.print(minimum_free_bytes);
        Serial.print(" heap_delta=");
        Serial.print(heap_delta);
        Serial.print(" max_scan_ms=");
        Serial.print(maximum_scan_ms);
        Serial.print(" max_hci_ms=");
        Serial.print(maximum_hci_ms);
        Serial.print(" max_cycle_ms=");
        Serial.println(maximum_cycle_ms);
        result_reported = true;
    }
}

} // namespace

void setup()
{
    Serial.begin(2000000);
    delay(25);
    Serial.println("BL616CL_SMOKE:BOOT");

    bflb_mtd_init();
    const EfErrCode easyflash_status = easyflash_init();
    if (easyflash_status != EF_NO_ERR) {
        report_failure("EASYFLASH_INIT", static_cast<int>(easyflash_status));
        return;
    }

    const int rf_status = rfparam_init(0, nullptr, 0);
    if (rf_status != 0) {
        report_failure("RF_INIT", rf_status);
        return;
    }

    const int event_status =
        async_register_event_filter(EV_WIFI, wifi_event_handler, nullptr);
    if (event_status != 0) {
        report_failure("WIFI_EVENT_FILTER", event_status);
        return;
    }

    tcpip_init(nullptr, nullptr);
    wifi_task_create();
    const int fhost_status = fhost_init();
    if (fhost_status != 0) {
        report_failure("FHOST_INIT", fhost_status);
        return;
    }

    wifi_started_at = millis();
    Serial.println("BL616CL_SMOKE:WIFI_COEX_INIT_STARTED");
}

void loop()
{
    if (smoke_failed || result_reported) {
        delay(100);
        return;
    }

    const unsigned long now = millis();

    if (!wifi_init_done) {
        if (now - wifi_started_at > kWifiTimeoutMs) {
            report_failure("WIFI_INIT_TIMEOUT", -1);
        }
        delay(10);
        return;
    }

    if (wifi_manager_status != 0) {
        report_failure("WIFI_MGMR_START", wifi_manager_status);
        return;
    }

    if (!wifi_manager_done) {
        if (now - wifi_started_at > kWifiTimeoutMs) {
            report_failure("WIFI_MGMR_READY_TIMEOUT", -1);
        }
        delay(10);
        return;
    }

    if (!controller_task_requested) {
        start_controller_task();
        return;
    }

    if (!controller_task_started || !controller_started || !hci_interface_ready) {
        if (now - controller_started_at > kControllerTimeoutMs) {
            report_failure("BLE_CONTROLLER_TIMEOUT", -1);
        }
        delay(10);
        return;
    }

    if (hci_interface_status != 0) {
        report_failure("HCI_INTERFACE_INIT", hci_interface_status);
        return;
    }

    if (initial_free_bytes == 0U) {
        initial_free_bytes = kfree_size(0U);
        if (initial_free_bytes == 0U) {
            report_failure("HEAP_INITIAL_QUERY", -1);
            return;
        }
        Serial.print("BL616CL_SMOKE:COEX_STRESS_READY heap_initial=");
        Serial.println(initial_free_bytes);
    }

    if (!cycle_active) {
        if (now >= next_cycle_at) {
            start_stress_cycle();
        }
        delay(10);
        return;
    }

    if (hci_reset_sent) {
        process_hci_response();
    }

    if (wifi_scan_done && hci_reset_complete) {
        complete_stress_cycle();
        return;
    }

    if (now - cycle_started_at > kCycleTimeoutMs) {
        report_failure("COEX_CYCLE_TIMEOUT", -1);
    }

    delay(10);
}

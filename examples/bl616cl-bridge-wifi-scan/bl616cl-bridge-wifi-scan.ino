#include <Arduino.h>

extern "C" {
#include <FreeRTOS.h>
}

#include <BL616CLHciAdapter.h>
#include <BL616CLWifiService.h>

namespace {

bool startup_failed = false;
bool scan_started = false;
bool scan_result_reported = false;

bridge::BouffaloHciController hci_controller;
bridge::BridgeRuntime bridge_runtime(hci_controller);
bridge::WifiStatusTracker wifi_status;
bridge::WifiService wifi_service(
    wifi_status,
    bridge::bouffalo_wifi_backend());

void report_failure(const char *stage, int status)
{
    Serial.print("BL616CL_SMOKE:FAIL stage=");
    Serial.print(stage);
    Serial.print(" code=");
    Serial.println(status);
    startup_failed = true;
}

void on_wifi_event(void *context, uint16_t code)
{
    static_cast<bridge::WifiService *>(context)->on_wifi_event(code);
}

} // namespace

void setup()
{
    Serial.begin(2000000);
    delay(25);
    Serial.println("BL616CL_SMOKE:BOOT");

    bridge_runtime.set_wifi_event_listener(on_wifi_event, &wifi_service);
    if (!bridge_runtime.begin(configMAX_PRIORITIES - 2U, configMAX_PRIORITIES - 1U)) {
        report_failure(
            bridge::BridgeRuntime::failure_text(bridge_runtime.failure()),
            bridge_runtime.failure_status());
        return;
    }
    Serial.println("BL616CL_SMOKE:RUNTIME_BOOTSTRAP_STARTED");
}

void loop()
{
    if (startup_failed || scan_result_reported) {
        delay(100);
        return;
    }

    bridge_runtime.poll();
    if (bridge_runtime.failed()) {
        report_failure(
            bridge::BridgeRuntime::failure_text(bridge_runtime.failure()),
            bridge_runtime.failure_status());
        return;
    }
    if (!bridge_runtime.ready()) {
        delay(10);
        return;
    }

    if (!scan_started) {
        if (!wifi_service.start_scan()) {
            report_failure("SCAN_START", -1);
            return;
        }
        scan_started = true;
        Serial.println("BL616CL_SMOKE:WIFI_SCAN_STARTED");
    }

    if (wifi_service.scan_complete()) {
        const size_t scan_count = wifi_service.scan_result_count();
        Serial.print("BL616CL_SMOKE:WIFI_SCAN_DONE count=");
        Serial.println(scan_count);
        scan_result_reported = true;
        return;
    }

    delay(10);
}

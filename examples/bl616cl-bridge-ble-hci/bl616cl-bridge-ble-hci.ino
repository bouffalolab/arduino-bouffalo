#include <Arduino.h>
#include <string.h>

extern "C" {
#include <FreeRTOS.h>
}

#include <BL616CLHciAdapter.h>

namespace {

constexpr unsigned long kCommandTimeoutMs = 10000UL;
constexpr uint8_t kResetPacket[] = { 0x01U, 0x03U, 0x0CU, 0x00U };
constexpr uint8_t kResetCompletePacket[] = {
    0x04U, 0x0EU, 0x04U, 0x01U, 0x03U, 0x0CU, 0x00U
};
constexpr uint8_t kResetCycles = 2U;

bool smoke_failed = false;
bool reset_sent = false;
bool result_reported = false;
uint8_t reset_cycle = 0U;
unsigned long reset_sent_at = 0UL;

bridge::BouffaloHciController hci_controller;
bridge::BridgeRuntime bridge_runtime(hci_controller);
bridge::BleHciService hci_service(
    hci_controller.controller(),
    bridge::BridgeRuntime::hci_synchronization());

void report_failure(const char *stage, int status)
{
    Serial.print("BL616CL_SMOKE:FAIL stage=");
    Serial.print(stage);
    Serial.print(" code=");
    Serial.println(status);
    smoke_failed = true;
}

void send_h4_reset()
{
    if (!hci_service.send_packet(kResetPacket, sizeof(kResetPacket))) {
        report_failure("HCI_RESET_SEND", -1);
        return;
    }

    reset_sent = true;
    reset_sent_at = millis();
    Serial.print("BL616CL_SMOKE:HCI_H4_RESET_SENT cycle=");
    Serial.println(reset_cycle + 1U);
}

void validate_reset_complete()
{
    uint8_t packet[bridge::BleHciService::kMaxPacketBytes] = {};
    const size_t packet_size = hci_service.read(packet, sizeof(packet));
    if (packet_size != sizeof(kResetCompletePacket) ||
        memcmp(packet, kResetCompletePacket, sizeof(kResetCompletePacket)) != 0) {
        report_failure("HCI_RESET_RESPONSE", static_cast<int>(packet_size));
        return;
    }

    Serial.print("BL616CL_SMOKE:HCI_EVENT_H4 cycle=");
    Serial.print(reset_cycle + 1U);
    Serial.println(" hex=04 0E 04 01 03 0C 00");

    ++reset_cycle;
    reset_sent = false;
    if (reset_cycle >= kResetCycles) {
        Serial.print("BL616CL_SMOKE:HCI_ADAPTER_LIFECYCLE_DONE cycles=");
        Serial.println(kResetCycles);
        result_reported = true;
        return;
    }

    hci_service.end();
    if (hci_service.active()) {
        report_failure("HCI_END_ACTIVE", -1);
        return;
    }
    Serial.println("BL616CL_SMOKE:HCI_TRANSPORT_END");

    if (!hci_service.begin()) {
        report_failure("HCI_REBEGIN", -1);
        return;
    }
    Serial.println("BL616CL_SMOKE:HCI_TRANSPORT_REBEGIN");
}

} // namespace

void setup()
{
    Serial.begin(2000000);
    delay(25);
    Serial.println("BL616CL_SMOKE:BOOT");

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
    if (smoke_failed || result_reported) {
        delay(100);
        return;
    }

    const unsigned long now = millis();

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

    if (!hci_controller.initialize_controller(configMAX_PRIORITIES - 1U)) {
        report_failure("HCI_CONTROLLER_REINIT", -1);
        return;
    }

    if (!hci_service.active() && !hci_service.begin()) {
        report_failure("HCI_BEGIN", -1);
        return;
    }

    if (!reset_sent) {
        send_h4_reset();
        return;
    }

    if (hci_service.available() != 0U) {
        validate_reset_complete();
        return;
    }

    if (now - reset_sent_at > kCommandTimeoutMs) {
        report_failure("HCI_RESET_TIMEOUT", -1);
    }

    delay(10);
}

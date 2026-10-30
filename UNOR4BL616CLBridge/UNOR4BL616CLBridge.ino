#include "ipv6_tcp_diagnostic.h"
#include "network_idle_coex.h"
#include "bridge_config.h"
#include "bridge_runtime.h"
#include "bl616cl_recovery_control.h"
#include "cdc_line_coding_control.h"
#include "ra4_at_service.h"
#include "ra4_bootstrap_control.h"
#include "ra4_samba_service.h"
#include "ra4_samba_programmer.h"
#include "ra4_watchdog_recovery.h"
#include "usb_cmsis_dap.h"
#include "wifi_ap_start_diagnostic.h"

bridge::BouffaloHciController hci_controller;
bridge::BridgeRuntime bridge_runtime(hci_controller);
bridge::Ra4AtService ra4_at_service(Serial1);
bridge::UserUartGpio35Control user_uart_gpio35_control;
bridge::Ra4BootstrapControl ra4_bootstrap(
    bridge::kRa4Boot,
    bridge::kRa4Reset,
    bridge::make_bouffalo_ra4_bootstrap_hooks());

namespace {

bool disconnect_wifi(void *)
{
    return ra4_at_service.disconnect_wifi_for_bootstrap();
}

bool enter_program_mode(void *)
{
    return ra4_bootstrap.enter_program_mode();
}

bool reset_with_boot_low(void *)
{
    return ra4_bootstrap.reset_with_boot_low();
}

bool update_user_uart_baud(void *, uint32_t baud)
{
    return Serial.updateBaudRate(baud);
}

bool clear_user_uart_rx(void *)
{
    Serial.clearRx();
    return true;
}

bool suspend_user_uart_bridge(void *)
{
    return bridge::usb_cmsis_dap_set_user_uart_bridge_enabled(false);
}

bool resume_user_uart_bridge(void *)
{
    return bridge::usb_cmsis_dap_set_user_uart_bridge_enabled(true);
}

uint32_t current_user_uart_baud(void *)
{
    return bridge::usb_cmsis_dap_current_line_coding_baud();
}

bool samba_update_user_uart_baud(void *, uint32_t baud)
{
    return Serial.updateBaudRate(baud);
}

void samba_clear_user_uart_rx(void *)
{
    Serial.clearRx();
}

size_t samba_write_user_uart(void *, const uint8_t *data, size_t size)
{
    const size_t written = Serial.write(data, size);
    if (written == size) {
        Serial.flush();
    }
    return written;
}

int samba_read_user_uart(void *)
{
    return Serial.read();
}

} // namespace

bridge::Ra4SambaService ra4_samba(
    ra4_bootstrap,
    {
        samba_update_user_uart_baud,
        samba_clear_user_uart_rx,
        samba_write_user_uart,
        samba_read_user_uart,
        nullptr,
    },
    {
        suspend_user_uart_bridge,
        resume_user_uart_bridge,
        current_user_uart_baud,
        nullptr,
    });

bridge::Ra4SambaProgrammer ra4_samba_programmer(
    ra4_bootstrap,
    {
        samba_update_user_uart_baud,
        samba_clear_user_uart_rx,
        samba_write_user_uart,
        samba_read_user_uart,
        nullptr,
    },
    {
        suspend_user_uart_bridge,
        resume_user_uart_bridge,
        current_user_uart_baud,
        nullptr,
    });

bridge::CdcLineCodingControl cdc_line_coding(
    {
        disconnect_wifi,
        enter_program_mode,
        reset_with_boot_low,
        update_user_uart_baud,
        clear_user_uart_rx,
        nullptr,
    },
    bridge::kDefaultUserUartBaud);
bool hci_backend_attached = false;
bool ra4_at_task_started = false;

void on_wifi_event(void *context, uint16_t code)
{
    static_cast<bridge::Ra4AtService *>(context)->on_wifi_event(code);
}

namespace {

/*
 * Network calls made by the Bouffalo fhost API can wait for asynchronous WPA
 * events. Keep that work off the Arduino loop task so USB CDC and CMSIS-DAP
 * stay responsive while an AT command is pending.
 */
void ra4_at_task(void *)
{
    for (;;) {
        if (cdc_line_coding.at_service_enabled()) {
            ra4_at_service.poll();
            if (bridge_runtime.ready()) bridge::network_idle_coex_poll();
        }
        delay(1);
    }
}

} // namespace

void setup()
{
    bridge::wifi_ap_start_watchdog_bootstrap();
#if defined(BL616CL_RA4_NORMAL_BOOT_ON_STARTUP) && BL616CL_RA4_NORMAL_BOOT_ON_STARTUP
    (void)ra4_bootstrap.begin_normal_boot();
#else
    (void)ra4_bootstrap.begin();
#endif
    Serial.begin(bridge::kDefaultUserUartBaud);
    Serial1.begin(bridge::kDefaultAtUartBaud);
    (void)user_uart_gpio35_control.begin();
    bridge::usb_cmsis_dap_set_ra4_bootstrap_control(&ra4_bootstrap);
    bridge::usb_cmsis_dap_set_user_uart_gpio35_control(&user_uart_gpio35_control);
    bridge::usb_cmsis_dap_set_ra4_at_service(&ra4_at_service);
    bridge::usb_cmsis_dap_set_ra4_samba_service(&ra4_samba);
    bridge::usb_cmsis_dap_set_ra4_samba_programmer(&ra4_samba_programmer);
    ra4_at_service.set_ra4_samba_programmer(&ra4_samba_programmer);
    (void)bridge::usb_cmsis_dap_begin();
    bridge::wifi_ap_start_capture_uart_after_watchdog_boot(
        Serial,
        Serial1,
        bridge::usb_cmsis_dap_current_line_coding_baud());
    (void)bridge::ra4_watchdog_recover_application(ra4_bootstrap, Serial);
    (void)ra4_at_service.begin();
    bridge_runtime.set_wifi_event_listener(on_wifi_event, &ra4_at_service);
    (void)bridge_runtime.begin(configMAX_PRIORITIES - 2U, configMAX_PRIORITIES - 1U);
    ra4_at_task_started = xTaskCreate(
        ra4_at_task,
        "ra4_at",
        2048U,
        nullptr,
        ARDUINO_LOOP_PRIORITY + 1U,
        nullptr) == pdPASS;
}

void loop()
{
    /*
     * HID callbacks only queue recovery. Perform the SDK reset from loop
     * context so USB callback code never changes boot configuration or resets
     * the CPU while CherryUSB is handling a control transfer.
     */
    if (bridge::bl616cl_recovery_poll()) {
        return;
    }

    uint32_t requested_baud = 0U;
    if (bridge::usb_cmsis_dap_take_ra4_samba_probe_request() &&
        !ra4_samba_programmer.busy() && !ra4_samba.busy()) {
        (void)ra4_samba.request_read_only_handshake();
    }
    if (!ra4_samba_programmer.busy()) {
        ra4_samba.poll(millis());
    }
    if (!ra4_samba.busy()) {
        ra4_samba_programmer.poll(millis());
    }
    if (!ra4_samba.busy() && !ra4_samba_programmer.busy() &&
        bridge::usb_cmsis_dap_take_line_coding(&requested_baud)) {
        (void)cdc_line_coding.apply(requested_baud);
        bridge::usb_cmsis_dap_note_line_coding_applied(requested_baud);
    }
    if (!ra4_samba.busy() && !ra4_samba_programmer.busy() &&
        bridge::usb_cmsis_dap_take_ra4_application_reset_request()) {
        (void)ra4_bootstrap.reset_to_application();
        (void)Serial.updateBaudRate(bridge::kDefaultUserUartBaud);
        Serial.clearRx();
    }
    bridge::usb_cmsis_dap_poll();
    bridge_runtime.poll();
    if (bridge_runtime.ready()) bridge::ipv6_tcp_diagnostic_poll();
    if (!hci_backend_attached && bridge_runtime.ready()) {
        hci_backend_attached = ra4_at_service.set_hci_controller(
            hci_controller.controller(),
            bridge::BridgeRuntime::hci_synchronization());
    }
    if (!ra4_at_task_started && cdc_line_coding.at_service_enabled()) {
        ra4_at_service.poll();
    }
    delay(1);
}

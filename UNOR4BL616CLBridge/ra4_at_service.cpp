#include "ra4_at_service.h"

#include "bridge_config.h"
#include "bl616cl_recovery_control.h"
#include "coex_diagnostic.h"

#include <stdio.h>

namespace bridge {

namespace {

constexpr char kFirmwareVersionCommand[] = "+FWVERSION";
constexpr char kFirmwareVersionU32Command[] = "+FWVERSION_U32";
constexpr char kGmrCommand[] = "+GMR";
constexpr char kCommandListCommand[] = "+CMD";
constexpr char kStatusCommand[] = "+GETSTATUS";
constexpr char kGetTimeCommand[] = "+GETTIME";
constexpr char kSoftResetWifiCommand[] = "+SOFTRESETWIFI";
constexpr char kResetCommand[] = "+RESET";
constexpr char kRestartBootloaderCommand[] = "+RESTARTBOOTLOADER";
constexpr char kExitCommand[] = "+EXIT";
constexpr char kOtaUpdateCommand[] = "+OTAUPDATE";

bool is_read_command(const at::Command &command, const char *name)
{
    return command.mode == at::CommandMode::Read && strcmp(command.name, name) == 0;
}

} // namespace

Ra4AtService::Ra4AtService(
    HardwareSerial &serial,
    BleHciController hci_controller,
    BleHciSynchronization hci_synchronization)
    : serial_(serial),
      transport_(
          {
              kAtMaxLineBytes,
              kAtMaxRawPayloadBytes,
              kAtMaxResponseBytes,
              kAtRawPayloadTimeoutMs,
          },
          &Ra4AtService::write_serial,
          this),
      wifi_(
          wifi_status_,
          bouffalo_wifi_backend(),
          {
              this,
              &Ra4AtService::hci_now_ms,
              &Ra4AtService::hci_idle,
          }),
      tcp_clients_(bouffalo_tcp_client_backend()),
      tcp_servers_(bouffalo_tcp_client_backend()),
      ping_(bouffalo_ping_backend()),
      udp_(bouffalo_udp_backend()),
      udp_runtime_(
          bouffalo_udp_backend(),
          {
              this,
              &Ra4AtService::hci_now_ms,
              &Ra4AtService::hci_idle,
          }),
      tls_clients_(
          bouffalo_tls_client_backend(),
          &Ra4AtService::copy_softse_private_key,
          this),
      ota_(ota_storage_.ra4_ota_cache_backend()),
      ota_at_(ota_, bouffalo_ra4_ota_download_backend()),
      hci_(hci_controller, hci_synchronization),
      hci_at_(
          hci_,
          {
              this,
              &Ra4AtService::hci_now_ms,
              &Ra4AtService::hci_idle,
          })
{
    transport_.set_command_handler(&Ra4AtService::dispatch_command, this);
    tcp_clients_.set_scan_control(
        hci_controller.pause_scan, hci_controller.resume_scan,
        hci_controller.context);
    tcp_servers_.set_scan_control(
        hci_controller.pause_scan, hci_controller.resume_scan,
        hci_controller.context);
    udp_.set_scan_control(
        hci_controller.pause_scan, hci_controller.resume_scan,
        hci_controller.context);
    tls_clients_.set_scan_control(
        hci_controller.pause_scan, hci_controller.resume_scan,
        hci_controller.context);
}

bool Ra4AtService::begin()
{
    return uart_.begin(serial_);
}

bool Ra4AtService::set_hci_controller(
    BleHciController hci_controller,
    BleHciSynchronization hci_synchronization)
{
    if (!hci_.set_controller(hci_controller, hci_synchronization)) return false;
    tcp_clients_.set_scan_control(
        hci_controller.pause_scan, hci_controller.resume_scan,
        hci_controller.context);
    tcp_servers_.set_scan_control(
        hci_controller.pause_scan, hci_controller.resume_scan,
        hci_controller.context);
    udp_.set_scan_control(
        hci_controller.pause_scan, hci_controller.resume_scan,
        hci_controller.context);
    tls_clients_.set_scan_control(
        hci_controller.pause_scan, hci_controller.resume_scan,
        hci_controller.context);
    return true;
}

bool Ra4AtService::disconnect_wifi_for_bootstrap()
{
    return wifi_.disconnect_for_bootstrap();
}

void Ra4AtService::set_ra4_samba_programmer(
    Ra4SambaProgrammer *programmer)
{
    ra4_samba_programmer_ = programmer;
}

void Ra4AtService::on_wifi_event(uint16_t code)
{
    wifi_.on_wifi_event(code);
    time_.on_wifi_event(code);
}

WifiStatusTracker &Ra4AtService::wifi_status_tracker()
{
    return wifi_status_;
}

UartTransportDiagnostics Ra4AtService::uart_diagnostics()
{
    return uart_.diagnostics();
}

Ra4OtaDownloadDiagnostic Ra4AtService::ota_download_diagnostic() const
{
    return ota_at_.download_diagnostic();
}

TimeServiceDiagnostic Ra4AtService::time_diagnostic() const
{
    return time_.diagnostic();
}

void Ra4AtService::poll()
{
    time_.process();
    ota_at_.poll(millis());
    complete_ota_update();
    uart_.drain_tx();
    transport_.flush_output();
    uart_.drain_tx();

    if (pending_reset_ != 0U) {
        if (!transport_.output_pending() &&
            uart_.diagnostics().buffered_tx_bytes == 0U) {
            serial_.flush();
            if (pending_reset_ == 1U) {
                (void)bl616cl_reset_request();
            } else {
                (void)bl616cl_recovery_request();
            }
            pending_reset_ = 0U;
        }
        return;
    }

    if (self_fota_.take_reset_request()) {
        pending_reset_ = 1U;
        return;
    }

    if (uart_.take_rx_overflow()) {
        uart_.discard_rx();
        transport_.reset();
        input_size_ = 0U;
        input_position_ = 0U;
        (void)transport_.write_error();
    }

    if (transport_.output_pending()) {
        transport_.tick(millis());
        return;
    }

    if (wifi_.poll()) {
        return;
    }

    if (input_position_ == input_size_) {
        input_size_ = uart_.read(input_, sizeof(input_));
        input_position_ = 0U;
    }

    if (input_position_ != input_size_) {
        input_position_ += transport_.feed(
            input_ + input_position_,
            input_size_ - input_position_,
            millis());
    } else {
        transport_.tick(millis());
    }

    uart_.drain_tx();
    transport_.flush_output();
    uart_.drain_tx();
}

at::CommandResult Ra4AtService::handle_command(const at::Command &command, at::AtTransport &transport)
{
    if (strcmp(command.name, kExitCommand) == 0) {
        return at::CommandResult::Ok;
    }

    if (strcmp(command.name, kResetCommand) == 0 ||
        strcmp(command.name, kRestartBootloaderCommand) == 0) {
        if (command.mode != at::CommandMode::Run) {
            return at::CommandResult::Error;
        }
        if (!transport.write_response(command, "")) {
            return at::CommandResult::Error;
        }
        pending_reset_ = strcmp(command.name, kResetCommand) == 0 ? 1U : 2U;
        return at::CommandResult::Ok;
    }

    if (strcmp(command.name, kSoftResetWifiCommand) == 0 &&
        command.mode == at::CommandMode::Run) {
        tcp_servers_.close_all(TcpCloseOrigin::SoftResetWifi);
        tcp_clients_.close_all();
        udp_.close_all();
        tls_clients_.close_all();
    }

    if (strcmp(command.name, kOtaUpdateCommand) == 0) {
        return handle_ota_update(command, transport);
    }

    if (self_fota_.handles_command(command)) {
        return self_fota_.handle_command(command, transport);
    }

    if (is_read_command(command, kFirmwareVersionCommand)) {
        transport.write_response(command, kFirmwareVersion);
        return at::CommandResult::Ok;
    }

    if (is_read_command(command, kFirmwareVersionU32Command)) {
        static constexpr uint8_t version[] = {
            kFirmwareMajor,
            kFirmwareMinor,
            kFirmwarePatch,
            0,
        };
        transport.write_cstr(command.name);
        transport.write_cstr(": ");
        transport.write_data(version, sizeof(version));
        transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }

    if (strcmp(command.name, kGmrCommand) == 0 && command.mode == at::CommandMode::Run) {
        transport.write_cstr("<len>\r\n");
        return at::CommandResult::Ok;
    }

    if (is_read_command(command, kCommandListCommand)) {
        transport.write_response(command, "<test>");
        return at::CommandResult::Ok;
    }

    if (is_read_command(command, kStatusCommand)) {
        transport.write_response(command, wifi_status_text(wifi_status_.status()));
        return at::CommandResult::Ok;
    }

    if (is_read_command(command, "+TCPLISTENDIAG")) {
        const auto &d = tcp_listen_ingress_counters();
        char values[96] = {};
        const int count = snprintf(values, sizeof(values), "%lu,%lu,%lu,%lu,%lu,%lu,%lu",
            static_cast<unsigned long>(d.events[0].load()),
            static_cast<unsigned long>(d.events[1].load()),
            static_cast<unsigned long>(d.events[2].load()),
            static_cast<unsigned long>(d.events[3].load()),
            static_cast<unsigned long>(d.events[4].load()),
            static_cast<unsigned long>(d.events[5].load()),
            static_cast<unsigned long>(d.last_port.load()));
        return count > 0 && static_cast<size_t>(count) < sizeof(values) &&
                transport.write_response(command, values)
            ? at::CommandResult::Ok : at::CommandResult::Error;
    }

    if (is_read_command(command, "+UARTDIAG")) {
        const auto d = uart_.diagnostics();
        char values[96] = {};
        const int count = snprintf(values, sizeof(values), "%lu,%lu,%lu,%lu,%lu,%lu,%lu",
            static_cast<unsigned long>(d.received_bytes),
            static_cast<unsigned long>(d.buffered_rx_bytes),
            static_cast<unsigned long>(d.rx_overflow_count),
            static_cast<unsigned long>(d.buffered_tx_bytes),
            static_cast<unsigned long>(d.tx_overflow_count),
            static_cast<unsigned long>(d.rx_error_status_seen),
            static_cast<unsigned long>(d.rx_fifo_errors_seen));
        return count > 0 && static_cast<size_t>(count) < sizeof(values) &&
                transport.write_response(command, values)
            ? at::CommandResult::Ok : at::CommandResult::Error;
    }

    if (is_read_command(command, "+COEXDIAG")) {
        const CoexDiagnostic snapshot = coex_diagnostic_snapshot();
        const ScanWindowCounters &scan = scan_window_counters();
        const WifiSynCounters &syn = wifi_syn_counters();
        const auto &removal = ap_station_removal_counters();
        const auto &join = ap_join_scan_counters();
        char values[448] = {};
        const int count = snprintf(values, sizeof(values), "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu",
            static_cast<unsigned long>(snapshot.pta_config),
            static_cast<unsigned long>(snapshot.wlan_control),
            static_cast<unsigned long>(snapshot.wlan_pti),
            static_cast<unsigned long>(snapshot.wlan_pti2),
            static_cast<unsigned long>(snapshot.wlan_control2),
            static_cast<unsigned long>(snapshot.wlan_rx),
            static_cast<unsigned long>(snapshot.wlan_rx_abort),
            static_cast<unsigned long>(scan.pause_attempts.load()),
            static_cast<unsigned long>(scan.disable_confirmed.load()),
            static_cast<unsigned long>(scan.restore_attempts.load()),
            static_cast<unsigned long>(scan.restore_confirmed.load()),
            static_cast<unsigned long>(scan.rejected.load()),
            static_cast<unsigned long>(scan.wait_timeouts.load()),
            static_cast<unsigned long>(syn.confirmed.load()),
            static_cast<unsigned long>(syn.acknowledged.load()),
            static_cast<unsigned long>(syn.discarded.load()),
            static_cast<unsigned long>(syn.last_discard_site.load()),
            static_cast<unsigned long>(snapshot.ap_state),
            static_cast<unsigned long>(snapshot.ap_stations),
            static_cast<unsigned long>(snapshot.discarded_destination_matches_station),
            static_cast<unsigned long>(snapshot.discarded_source_interface),
            static_cast<unsigned long>(snapshot.scan_priorities),
            static_cast<unsigned long>(removal.removes.load()),
            static_cast<unsigned long>(removal.remove_site.load()),
            static_cast<unsigned long>(removal.free_site.load()),
            static_cast<unsigned long>(removal.timeout_next.load()),
            static_cast<unsigned long>(removal.flags.load()),
            static_cast<unsigned long>(removal.rx_leave_kind.load()),
            static_cast<unsigned long>(removal.rx_leave_reason.load()),
            static_cast<unsigned long>(join.active.load()),
            static_cast<unsigned long>(join.attempts.load()),
            static_cast<unsigned long>(join.dhcp_ack_transmitted.load()),
            static_cast<unsigned long>(join.completed.load()),
            static_cast<unsigned long>(join.timeouts.load()),
            static_cast<unsigned long>(join.elapsed_ms.load()),
            static_cast<unsigned long>(join.client_ip_reply_acknowledged.load()));
        return count > 0 && static_cast<size_t>(count) < sizeof(values) &&
                transport.write_response(command, values)
            ? at::CommandResult::Ok : at::CommandResult::Error;
    }

    if (strcmp(command.name, kGetTimeCommand) == 0 &&
        command.mode == at::CommandMode::Write &&
        command.argument_count == 0U) {
        char epoch[sizeof("4294967295")] = {};
        const int written = snprintf(
            epoch,
            sizeof(epoch),
            "%lu",
            static_cast<unsigned long>(time_.epoch_seconds()));
        return written > 0 && static_cast<size_t>(written) < sizeof(epoch) &&
                transport.write_response(command, epoch)
            ? at::CommandResult::Ok
            : at::CommandResult::Error;
    }

    if (wifi_.handles_command(command)) {
        return wifi_.handle_command(command, transport);
    }

    if (tcp_servers_.handles_server_command(command)) {
        return tcp_servers_.handle_server_command(command, transport);
    }

    if (tcp_servers_.handles_client_command(command)) {
        return tcp_servers_.handle_client_command(command, transport);
    }

    if (tcp_clients_.handles_command(command)) {
        return tcp_clients_.handle_command(command, transport);
    }

    if (ping_.handles_command(command)) {
        return ping_.handle_command(command, transport);
    }

    if (udp_.handles_command(command)) {
        return udp_.handle_command(command, transport);
    }

    if (udp_runtime_.handles_command(command)) {
        return udp_runtime_.handle_command(command, transport);
    }

    if (tls_clients_.handles_command(command)) {
        return tls_clients_.handle_command(command, transport);
    }

    if (storage_.handles_command(command)) {
        return storage_.handle_command(command, transport);
    }

    if (ota_at_.handles_command(command)) {
        return ota_at_.handle_command(command, transport);
    }

    if (softse_.handles_command(command)) {
        return softse_.handle_command(command, transport);
    }

    return hci_at_.handle_command(command, transport);
}

at::CommandResult Ra4AtService::handle_ota_update(
    const at::Command &command,
    at::AtTransport &transport)
{
    if ((command.mode != at::CommandMode::Run &&
         command.mode != at::CommandMode::Write) ||
        (command.mode == at::CommandMode::Run && command.argument_count != 0U) ||
        (command.mode == at::CommandMode::Write &&
         (command.argument_count != 1U || command.arguments[0][0] == '\0')) ||
        ra4_samba_programmer_ == nullptr ||
        ota_update_transport_ != nullptr ||
        ra4_samba_programmer_->busy() ||
        ota_.state() != Ra4OtaService::State::Ready) {
        return at::CommandResult::Error;
    }

    const char *path = command.mode == at::CommandMode::Write
        ? command.arguments[0]
        : nullptr;
    if (!ota_storage_.ready_ra4_ota_cache(path) ||
        !ra4_samba_programmer_->request_program(
            ota_storage_.ra4_ota_image_source())) {
        return at::CommandResult::Error;
    }
    ota_update_command_ = command;
    ota_update_transport_ = &transport;
    return at::CommandResult::Deferred;
}

void Ra4AtService::complete_ota_update()
{
    if (ota_update_transport_ == nullptr || ra4_samba_programmer_ == nullptr) {
        return;
    }
    if (!ota_update_transport_->response_deferred()) {
        ota_update_transport_ = nullptr;
        return;
    }
    if (ra4_samba_programmer_->busy()) {
        return;
    }
    at::AtTransport *transport = ota_update_transport_;
    ota_update_transport_ = nullptr;
    const char *result = ra4_samba_programmer_->diagnostic().succeeded
        ? "1"
        : "0";
    const bool written = transport->write_response(ota_update_command_, result);
    (void)transport->complete_deferred_response(
        written ? at::CommandResult::Ok : at::CommandResult::Error);
}

size_t Ra4AtService::write_serial(void *context, const uint8_t *data, size_t size)
{
    return static_cast<Ra4AtService *>(context)->uart_.write(data, size);
}

at::CommandResult Ra4AtService::dispatch_command(void *context, const at::Command &command, at::AtTransport &transport)
{
    return static_cast<Ra4AtService *>(context)->handle_command(command, transport);
}

bool Ra4AtService::copy_softse_private_key(
    void *context,
    const char *slot,
    uint8_t *destination,
    size_t capacity,
    size_t *size)
{
    return static_cast<Ra4AtService *>(context)->softse_.copy_private_key(
        slot,
        destination,
        capacity,
        size);
}

uint32_t Ra4AtService::hci_now_ms(void *)
{
    return millis();
}

void Ra4AtService::hci_idle(void *)
{
    delay(1);
}

} // namespace bridge

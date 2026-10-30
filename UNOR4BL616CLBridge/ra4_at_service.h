#ifndef UNOR4_BL616CL_RA4_AT_SERVICE_H
#define UNOR4_BL616CL_RA4_AT_SERVICE_H

#include "Arduino.h"
#include "at_transport.h"
#include "ble_hci_at_service.h"
#include "bouffalo_wifi_backend.h"
#include "uart_transport.h"
#include "wifi_service.h"
#include "wifi_status.h"
#include "tcp_client_service.h"
#include "tcp_server_service.h"
#include "ping_service.h"
#include "ra4_ota_at_service.h"
#include "ra4_ota_service.h"
#include "ra4_samba_programmer.h"
#include "softse_service.h"
#include "self_fota_service.h"
#include "storage_service.h"
#include "udp_runtime_diagnostic.h"
#include "udp_service.h"
#include "tls_client_service.h"
#include "time_service.h"

namespace bridge {

class Ra4AtService {
public:
    explicit Ra4AtService(
        HardwareSerial &serial,
        BleHciController hci_controller = {},
        BleHciSynchronization hci_synchronization = {});

    bool begin();
    void poll();
    bool set_hci_controller(
        BleHciController hci_controller,
        BleHciSynchronization hci_synchronization = {});
    bool disconnect_wifi_for_bootstrap();
    void set_ra4_samba_programmer(Ra4SambaProgrammer *programmer);
    void on_wifi_event(uint16_t code);
    WifiStatusTracker &wifi_status_tracker();
    UartTransportDiagnostics uart_diagnostics();
    Ra4OtaDownloadDiagnostic ota_download_diagnostic() const;
    TimeServiceDiagnostic time_diagnostic() const;

private:
    static size_t write_serial(void *context, const uint8_t *data, size_t size);
    static at::CommandResult dispatch_command(void *context, const at::Command &command, at::AtTransport &transport);
    static bool copy_softse_private_key(
        void *context,
        const char *slot,
        uint8_t *destination,
        size_t capacity,
        size_t *size);
    static uint32_t hci_now_ms(void *context);
    static void hci_idle(void *context);
    at::CommandResult handle_command(const at::Command &command, at::AtTransport &transport);
    at::CommandResult handle_ota_update(
        const at::Command &command,
        at::AtTransport &transport);
    void complete_ota_update();

    HardwareSerial &serial_;
    UartTransport uart_;
    at::AtTransport transport_;
    WifiStatusTracker wifi_status_;
    WifiService wifi_;
    TcpClientService tcp_clients_;
    TcpServerService tcp_servers_;
    PingService ping_;
    UdpService udp_;
    UdpRuntimeDiagnostic udp_runtime_;
    TlsClientService tls_clients_;
    TimeService time_;
    StorageService storage_;
    StorageService ota_storage_{"RA4_OTA"};
    Ra4OtaService ota_;
    Ra4OtaAtService ota_at_;
    SoftSeService softse_;
    SelfFotaService self_fota_;
    BleHciService hci_;
    BleHciAtService hci_at_;
    Ra4SambaProgrammer *ra4_samba_programmer_ = nullptr;
    at::AtTransport *ota_update_transport_ = nullptr;
    at::Command ota_update_command_ = {};
    uint8_t input_[kAtPollBudgetBytes];
    size_t input_size_ = 0U;
    size_t input_position_ = 0U;
    uint8_t pending_reset_ = 0U;
};

} // namespace bridge

#endif

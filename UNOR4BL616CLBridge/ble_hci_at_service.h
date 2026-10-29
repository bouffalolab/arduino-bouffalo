#ifndef UNOR4_BL616CL_BLE_HCI_AT_SERVICE_H
#define UNOR4_BL616CL_BLE_HCI_AT_SERVICE_H

#include "at_transport.h"
#include "ble_hci_service.h"

namespace bridge {

class BleHciAtService {
public:
    BleHciAtService(BleHciService &hci, BleHciWaitHooks wait_hooks);

    at::CommandResult handle_command(const at::Command &command, at::AtTransport &transport);

private:
    static at::CommandResult handle_raw_write(void *context, const uint8_t *data, size_t size);
    static bool parse_unsigned(const char *text, uint32_t maximum, uint32_t &value);
    static bool write_unsigned(at::AtTransport &transport, size_t value);
    static bool write_prompt(const at::Command &command, at::AtTransport &transport);

    BleHciService &hci_;
    BleHciWaitHooks wait_hooks_;
};

} // namespace bridge

#endif

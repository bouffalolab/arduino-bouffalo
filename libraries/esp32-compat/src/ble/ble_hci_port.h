#ifndef BL616CL_ESP32_COMPAT_BLE_HCI_PORT_H_
#define BL616CL_ESP32_COMPAT_BLE_HCI_PORT_H_

// Bring-up entry for the BL616CL BLE controller in external-host (HCI
// passthrough) mode.  Called by HCIVirtualTransport::begin(), i.e. when the
// RA4M1 host issues AT+HCI_BEGIN.
bool ble_controller_start(void);

// Bring-up forensics: dump BLE2 core control/version/irq/event registers.
void ble2_core_dump(const char *tag);
void ble_wide_dump(void);
void ble_wcheck_dump(void);
void ble_emfull_dump(void);
void ble_rawmem_dump(void);

#endif

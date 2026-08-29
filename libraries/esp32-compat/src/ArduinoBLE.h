#ifndef BL616CL_ESP32_COMPAT_ARDUINOBLE_H_
#define BL616CL_ESP32_COMPAT_ARDUINOBLE_H_

// The ArduinoBLE host library does not run on the BL616CL: on the UNO R4 the
// BLE host lives on the RA4M1 and talks HCI to this firmware through the
// AT+HCI_* commands.  This header only exists so the bridge sketch can
// compile its BLE command table; the actual HCI transport is implemented by
// HCIVirtualTransport (utility/HCIVirtualTransport.cpp) backed by the
// btblecontroller port (ble/ble_hci_port.cpp).

#endif

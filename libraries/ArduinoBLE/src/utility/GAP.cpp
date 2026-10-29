/*
  This file is part of the ArduinoBLE library.
  Copyright (c) 2018 Arduino SA. All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "BLEUuid.h"
#include "HCI.h"

#include "GAP.h"

#define GAP_MAX_DISCOVERED_QUEUE_SIZE 32

#define GAP_ADV_IND (0x00)
#define GAP_ADV_SCAN_IND (0x02)
#define GAP_ADV_NONCONN_IND (0x03)

GAPClass::GAPClass() :
  _advertising(false),
  _scanning(false),
  _advertisingInterval(160),
  _scanInterval(0x0020),
  _scanWindow(0x0020),
  _connectable(true),
  _discoverEventHandler(NULL)
{
}

GAPClass::~GAPClass()
{
}

bool GAPClass::advertising()
{
  return _advertising;
}

int GAPClass::advertise(uint8_t* advData, uint8_t advDataLen, uint8_t* scanData, uint8_t scanDataLen)
{
  uint8_t directBdaddr[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

  uint8_t type = (_connectable) ? GAP_ADV_IND : (scanDataLen ? GAP_ADV_SCAN_IND : GAP_ADV_NONCONN_IND);

  stopAdvertise();

  if (HCI.leSetAdvertisingParameters(_advertisingInterval, _advertisingInterval, type, 0x00, 0x00, directBdaddr, 0x07, 0) != 0) {
    return 0;
  }

  if (HCI.leSetAdvertisingData(advDataLen, advData) != 0) {
    return 0;
  }

  if (HCI.leSetScanResponseData(scanDataLen, scanData) != 0) {
    return 0;
  }

  if (HCI.leSetAdvertiseEnable(0x01) != 0) {
    return 0;
  }

  _advertising = true;

  return 1;
}

void GAPClass::stopAdvertise()
{
  _advertising = false;

  HCI.leSetAdvertiseEnable(0x00);
}

int GAPClass::scan(bool withDuplicates)
{
#if defined(ARDUINO_BLE_COEX_SNAPSHOT)
  memset(_coexScan, 0, sizeof(_coexScan));
#endif
  HCI.leSetScanEnable(false, true);

  // Active scan, public own address type, no filter; defaults to a 20 ms continuous window.
  /*
    Warning (from BLUETOOTH SPECIFICATION 5.x):
    - scan interval: mandatory range from 0x0012 to 0x1000; only even values are valid
    - scan window: mandatory range from 0x0011 to 0x1000
    - The scan window can only be less than or equal to the scan interval
  */
  if (HCI.leSetScanParameters(0x01, _scanInterval, _scanWindow, 0x00, 0x00) != 0) {
    return false;
  }

  if (HCI.leSetScanEnable(true, !withDuplicates) != 0) {
    return 0;
  }

  _scanning = true;
  return 1;
}

int GAPClass::scanForName(String name, bool withDuplicates)
{
  _scanNameFilter    = name;
  _scanUuidFilter    = "";
  _scanAddressFilter = "";

  return scan(withDuplicates);
}

int GAPClass::scanForUuid(String uuid, bool withDuplicates)
{
  _scanNameFilter    = "";
  _scanUuidFilter    = uuid;
  _scanAddressFilter = "";

  return scan(withDuplicates);
}

int GAPClass::scanForAddress(String address, bool withDuplicates)
{
  _scanNameFilter    = "";
  _scanUuidFilter    = "";
  _scanAddressFilter = address;

  return scan(withDuplicates);
}

void GAPClass::stopScan()
{
  HCI.leSetScanEnable(false, false);

  _scanning = false;

  for (unsigned int i = 0; i < _discoveredDevices.size(); i++) {
    BLEDevice* device = _discoveredDevices.get(i);

    delete device;
  }

  _discoveredDevices.clear();
}

bool GAPClass::setScanParameters(uint16_t interval, uint16_t window)
{
  if (_scanning || interval < 0x0012 || interval > 0x1000 ||
      (interval & 1) != 0 || window < 0x0011 || window > interval) {
    return false;
  }

  _scanInterval = interval;
  _scanWindow = window;
  return true;
}

BLEDevice GAPClass::available()
{
  for (unsigned int i = 0; i < _discoveredDevices.size();) {
    BLEDevice* device = _discoveredDevices.get(i);

    if (scanResultReady(*device)) {
      BLEDevice result = *device;

      _discoveredDevices.remove(i);
      delete device;

      if (matchesScanFilter(result)) {
        return result;
      } else {
        continue;
      } 
    } else {
      ++i;
    }
  }

  return BLEDevice();
}

void GAPClass::setAdvertisingInterval(uint16_t advertisingInterval)
{
  _advertisingInterval = advertisingInterval;
}

void GAPClass::setConnectable(bool connectable)
{
  _connectable = connectable;
}

void GAPClass::setEventHandler(BLEDeviceEvent event, BLEDeviceEventHandler eventHandler)
{
  if (event == BLEDiscovered) {
    _discoverEventHandler = eventHandler;
  }
}

void GAPClass::handleLeAdvertisingReport(uint8_t type, uint8_t addressType, uint8_t address[6],
                                          uint8_t eirLength, uint8_t eirData[], int8_t rssi)
{
  if (!_scanning) {
    return;
  }
#if defined(ARDUINO_BLE_COEX_SNAPSHOT)
  ++_coexScan[type == 0x04 ? 1 : 0];
#endif

  if (_discoverEventHandler && type == 0x03) {
    // call event handler and skip adding to discover list
    BLEDevice device(addressType, address);

    device.setAdvertisementData(type, eirLength, eirData, rssi);

    if (matchesScanFilter(device)) {
      _discoverEventHandler(device);
    }
    return;
  }

  BLEDevice* discoveredDevice = NULL;
  int discoveredIndex = -1;

  for (unsigned int i = 0; i < _discoveredDevices.size(); i++) {
    BLEDevice* device = _discoveredDevices.get(i);

    if (device->hasAddress(addressType, address)) {
      discoveredDevice = device;
      discoveredIndex = i;

      break;
    }
  }

  if (discoveredDevice == NULL) {
    if (_discoveredDevices.size() >= GAP_MAX_DISCOVERED_QUEUE_SIZE) {
#if defined(ARDUINO_BLE_COEX_SNAPSHOT)
      ++_coexScan[2];
#endif
      // HCI.poll() can process a burst before available() runs. Keep UUID
      // matches until the application can consume them; evict unrelated
      // devices first. If every entry matches, replacing the oldest still
      // leaves a bounded queue of usable results.
      unsigned evictIndex = 0;
      if (_scanUuidFilter.length() > 0) {
        for (unsigned i = 0; i < _discoveredDevices.size(); ++i) {
          if (!matchesScanFilter(*_discoveredDevices.get(i))) {
            evictIndex = i;
            break;
          }
        }
      }
      BLEDevice* device_first = _discoveredDevices.remove(evictIndex);
      if (device_first != NULL) {
        delete device_first;
      }
    }

    discoveredDevice = new BLEDevice(addressType, address);

    _discoveredDevices.add(discoveredDevice);
    discoveredIndex = _discoveredDevices.size() - 1;
  }

  if (type != 0x04) {
    discoveredDevice->setAdvertisementData(type, eirLength, eirData, rssi);
  } else {
    discoveredDevice->setScanResponseData(eirLength, eirData, rssi);
  }
#if defined(ARDUINO_BLE_COEX_SNAPSHOT)
  if (matchesScanFilter(*discoveredDevice)) {
    ++_coexScan[3];
    if (scanResultReady(*discoveredDevice)) ++_coexScan[4];
  }
#endif

  if (scanResultReady(*discoveredDevice) && _discoverEventHandler) {
    // remove from list and report as discovered
    BLEDevice device = *discoveredDevice;

    _discoveredDevices.remove(discoveredIndex);
    delete discoveredDevice;

    if (matchesScanFilter(device)) {
      _discoverEventHandler(device);
    }
  }
}

bool GAPClass::scanResultReady(BLEDevice& device)
{
  // A UUID-filtered scan already has its result in the advertisement. Do not
  // require an optional scan response: it may be lost, or its queued partner
  // may be evicted while the controller suppresses duplicate reports.
  return device.discovered() ||
      (_scanUuidFilter.length() > 0 && matchesScanFilter(device));
}

bool GAPClass::matchesScanFilter(const BLEDevice& device)
{
  if (_scanAddressFilter.length() > 0 && !(_scanAddressFilter.equalsIgnoreCase(device.address()))) {
    return false; // drop doesn't match
  } else if (_scanNameFilter.length() > 0 && _scanNameFilter != device.localName()) {
    return false; // drop doesn't match
  } else if (_scanUuidFilter.length() > 0 && !(_scanUuidFilter.equalsIgnoreCase(device.advertisedServiceUuid()))) {
    return false; // drop doesn't match
  }

  return true;
}

#if !defined(FAKE_GAP)
GAPClass GAPObj;
GAPClass& GAP = GAPObj;
#endif

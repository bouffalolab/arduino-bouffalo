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

#include <catch.hpp>
#include "HCIFakeTransport.h"
#include <stdexcept>

#define private public
#define protected public
#include "BLEDevice.h"
#include "utility/GATT.h"
#include "utility/GAP.h"
#include "BLEProperty.h"
#include "utility/HCI.h"

#if defined(ARDUINO_BLE_COEX_SNAPSHOT)
TEST_CASE("diagnostic CSA2 query validates the LE features response", "[ArduinoBLE::HCI]")
{
  class FeaturesHci : public HCIClass {
  public:
    uint8_t features[8] = {};
    int status = 0;
    int length = 8;
    bool nullResponse = false;
    int sendCommand(uint16_t opcode, uint8_t plen, void* parameters) override {
      REQUIRE(opcode == 0x2003);
      REQUIRE(plen == 0);
      REQUIRE(parameters == nullptr);
      _cmdResponseLen = length;
      _cmdResponse = nullResponse ? nullptr : features;
      return status;
    }
  } hci;
  SECTION("only bit 14 determines CSA2 support") {
    hci.features[1] = 0xbf;
    REQUIRE(hci.readLeChannelSelection2() == 0);
    hci.features[1] = 0x40;
    REQUIRE(hci.readLeChannelSelection2() == 1);
  }
  SECTION("short or missing response is unavailable") {
    hci.length = 7;
    REQUIRE(hci.readLeChannelSelection2() == -1);
    hci.length = 8;
    hci.nullResponse = true;
    REQUIRE(hci.readLeChannelSelection2() == -1);
  }
  SECTION("controller failure is unavailable") {
    hci.status = 1;
    REQUIRE(hci.readLeChannelSelection2() == -1);
  }
}
#endif

TEST_CASE("HCI polling yields when controller input stays continuously available", "[ArduinoBLE::HCI]")
{
  // A valid Command Complete stream keeps arriving while the application polls.
  const uint8_t packet[] = {4, 0x0e, 4, 1, 0x0c, 0x20, 0};
  unsigned reads = 0;
  int offered = sizeof(packet);
  unsigned limit = 100;
  HCIFakeTransport.availableHook = [&]() { return reads < limit ? offered : 0; };
  HCIFakeTransport.readHook = [&]() {
    if (reads >= 100) throw std::runtime_error("poll never yielded to application");
    return packet[reads++ % sizeof(packet)];
  };
  struct Cleanup {
    ~Cleanup() {
      HCIFakeTransport.availableHook = nullptr;
      HCIFakeTransport.readHook = nullptr;
    }
  } cleanup;
  HCIClass hci;
  SECTION("newly arriving frames remain for a later poll") {
    REQUIRE_NOTHROW(hci.poll());
    REQUIRE(reads == sizeof(packet));
    REQUIRE(hci._cmdCompleteOpcode == 0x200c);
    REQUIRE_NOTHROW(hci.poll());
    REQUIRE(reads == 2 * sizeof(packet));
  }
  SECTION("a later completion cannot replace the command result before its caller sees it") {
    const uint8_t events[] = {4, 0x0f, 4, 0, 1, 0x0d, 0x20,
                              4, 0x0e, 4, 1, 0x0c, 0x20, 0};
    limit = sizeof(events);
    offered = sizeof(events);
    HCIFakeTransport.availableHook = [&]() {
      if (reads >= sizeof(events)) throw std::runtime_error("command result was skipped");
      return int(sizeof(events) - reads);
    };
    HCIFakeTransport.readHook = [&]() { return events[reads++]; };
    REQUIRE(hci.sendCommand(0x200d) == 0);
    REQUIRE(reads == 7);
    REQUIRE(hci._cmdCompleteOpcode == 0x200d);
    hci.poll();
    REQUIRE(reads == sizeof(events));
    REQUIRE(hci._cmdCompleteOpcode == 0x200c);
  }
  SECTION("continuous non-command events yield within the batch budget") {
    const uint8_t event[] = {4, 0xff, 1, 0};
    HCIFakeTransport.availableHook = []() { return 4; };
    HCIFakeTransport.readHook = [&]() {
      if (reads >= 1025) throw std::runtime_error("event stream never yielded");
      return event[reads++ % sizeof(event)];
    };
    REQUIRE_NOTHROW(hci.poll());
    REQUIRE(reads == 1024);
    REQUIRE(hci._recvIndex == 0);
  }
  SECTION("batch boundaries preserve a partially received event") {
    const uint8_t event[] = {4, 0xff, 4, 0, 0, 0, 0};
    HCIFakeTransport.availableHook = []() { return 7; };
    HCIFakeTransport.readHook = [&]() {
      if (reads >= 2049) throw std::runtime_error("event stream exceeded two batches");
      return event[reads++ % sizeof(event)];
    };
    REQUIRE_NOTHROW(hci.poll());
    REQUIRE(reads == 1024);
    REQUIRE(hci._recvIndex == 2);
    REQUIRE_NOTHROW(hci.poll());
    REQUIRE(reads == 2048);
    REQUIRE(hci._recvIndex == 4);
  }
  SECTION("continuous invalid framing is also bounded") {
    HCIFakeTransport.availableHook = []() { return 1; };
    HCIFakeTransport.readHook = [&]() {
      if (reads >= 1025) throw std::runtime_error("invalid input never yielded");
      ++reads;
      return 0xff;
    };
    REQUIRE_NOTHROW(hci.poll());
    REQUIRE(reads == 1024);
    REQUIRE(hci._recvIndex == 0);
  }
  SECTION("yielding across a partial frame preserves parser state") {
    limit = 2;
    REQUIRE_NOTHROW(hci.poll());
    REQUIRE(reads == 2);
    REQUIRE(hci._recvIndex == 2);
    limit = sizeof(packet);
    REQUIRE_NOTHROW(hci.poll());
    REQUIRE(reads == sizeof(packet));
    REQUIRE(hci._cmdCompleteOpcode == 0x200c);
    REQUIRE(hci._recvIndex == 0);
  }
}

TEST_CASE("UUID scan returns matching connectable advertisement without scan response", "[ArduinoBLE::GAP]")
{
  GAPClass gap;
  gap._scanning = true;
  gap._scanUuidFilter = "180f";
  uint8_t address[] = {1, 2, 3, 4, 5, 6};
  uint8_t advertisement[] = {3, 3, 0x0f, 0x18};
  gap.handleLeAdvertisingReport(0, 0, address, sizeof(advertisement), advertisement, -40);
  BLEDevice found = gap.available();
  REQUIRE(bool(found));
  REQUIRE(found.advertisedServiceUuid() == "180f");
  REQUIRE_FALSE(found.hasLocalName());
  REQUIRE_FALSE(bool(gap.available()));
}

TEST_CASE("UUID scan filters other advertisements and general scan still waits for response", "[ArduinoBLE::GAP]")
{
  GAPClass gap;
  gap._scanning = true;
  gap._scanUuidFilter = "180f";
  uint8_t address[] = {1, 2, 3, 4, 5, 6};
  uint8_t advertisement[] = {3, 3, 0x0a, 0x18};
  uint8_t response[] = {2, 9, 'x'};
  gap.handleLeAdvertisingReport(0, 0, address, sizeof(advertisement), advertisement, -40);
  REQUIRE_FALSE(bool(gap.available()));
  gap.handleLeAdvertisingReport(4, 0, address, sizeof(response), response, -40);
  REQUIRE_FALSE(bool(gap.available()));
  gap._scanUuidFilter = "";
  gap.handleLeAdvertisingReport(0, 0, address, sizeof(advertisement), advertisement, -40);
  REQUIRE_FALSE(bool(gap.available()));
  gap.handleLeAdvertisingReport(4, 0, address, sizeof(response), response, -40);
  BLEDevice found = gap.available();
  REQUIRE(bool(found));
  REQUIRE(found.localName() == "x");
  REQUIRE_FALSE(bool(gap.available()));
}

TEST_CASE("128 bit UUID callback scan does not require scan response", "[ArduinoBLE::GAP]")
{
  GAPClass gap;
  gap._scanning = true;
  gap._scanUuidFilter = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c041";
  static unsigned calls;
  calls = 0;
  gap._discoverEventHandler = [](BLEDevice device) {
    REQUIRE(device.advertisedServiceUuid() == "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c041");
    ++calls;
  };
  uint8_t address[] = {1, 2, 3, 4, 5, 6};
  uint8_t advertisement[] = {17, 7, 0x41, 0xc0, 0xc9, 0x2b, 0x2b, 0x5a, 0x7c, 0x9b,
                            0x79, 0x4f, 0xf8, 0x7d, 0x20, 0x1c, 0x1f, 0x9d};
  gap.handleLeAdvertisingReport(0, 0, address, sizeof(advertisement), advertisement, -40);
  REQUIRE(calls == 1);
  REQUIRE_FALSE(bool(gap.available()));
}

TEST_CASE("UUID result survives unrelated advertising burst before available", "[ArduinoBLE::GAP]")
{
  GAPClass gap;
  gap._scanning = true;
  gap._scanUuidFilter = "180f";
  uint8_t address[] = {1, 2, 3, 4, 5, 6};
  uint8_t target[] = {3, 3, 0x0f, 0x18};
  uint8_t noise[] = {3, 3, 0x0a, 0x18};
  gap.handleLeAdvertisingReport(0, 0, address, sizeof(target), target, -40);
  for (unsigned i = 7; i < 80; ++i) {
    address[0] = i;
    gap.handleLeAdvertisingReport(3, 0, address, sizeof(noise), noise, -50);
  }
  BLEDevice found = gap.available();
  while (gap._discoveredDevices.size()) delete gap._discoveredDevices.remove(0);
  REQUIRE(bool(found));
  REQUIRE(found.advertisedServiceUuid() == "180f");
}

TEST_CASE("Filtering a preceding device does not skip a queued UUID match", "[ArduinoBLE::GAP]")
{
  GAPClass gap;
  gap._scanning = true;
  gap._scanUuidFilter = "180f";
  uint8_t address[] = {1, 2, 3, 4, 5, 6};
  uint8_t noise[] = {3, 3, 0x0a, 0x18};
  uint8_t target[] = {3, 3, 0x0f, 0x18};
  gap.handleLeAdvertisingReport(3, 0, address, sizeof(noise), noise, -40);
  address[0] = 7;
  gap.handleLeAdvertisingReport(0, 0, address, sizeof(target), target, -40);
  BLEDevice found = gap.available();
  while (gap._discoveredDevices.size()) delete gap._discoveredDevices.remove(0);
  REQUIRE(bool(found));
  REQUIRE(found.advertisedServiceUuid() == "180f");
}

TEST_CASE("BLE discovered device test", "[ArduinoBLE::BLEDevice]")
{

  WHEN("Retrieve local name from advertisement packet")
  {
    // Mocking advertisement packet
    uint8_t advType = 0x03;
    uint8_t eirLength = 6;
    uint8_t eirData[] = {0x05, 0x09, 't', 'e', 's', 't'};
    uint8_t rssi = 0;

    // Expected results
    String goldenName = "test";

    // Simulate device discovery
    BLEDevice device = BLEDevice();
    device.setAdvertisementData(0x03, eirLength, eirData, rssi); 

    bool hasName = device.hasLocalName();
    REQUIRE(hasName);

    String name = device.localName();
    REQUIRE(goldenName == name);

  }

}

TEST_CASE("Repeated scan responses replace bounded data", "[ArduinoBLE::BLEDevice]")
{
  BLEDevice device;
  uint8_t advertisement[31] = {30, 0xff};
  uint8_t response[31] = {30, 0x09};
  device.setAdvertisementData(0, sizeof(advertisement), advertisement, -50);
  for (unsigned cycle = 0; cycle < 128; ++cycle) {
    response[2] = cycle;
    device.setScanResponseData(sizeof(response), response, -40);
    REQUIRE(device._eirDataLength == 62);
    REQUIRE(memcmp(device._eirData, advertisement, 31) == 0);
    REQUIRE(memcmp(device._eirData + 31, response, 31) == 0);
  }
  uint8_t shortResponse[] = {2, 0x09, 'x'};
  device.setScanResponseData(sizeof(shortResponse), shortResponse, -30);
  REQUIRE(device._eirDataLength == 34);
  REQUIRE(device.localName() == "x");
  device.setScanResponseData(0, NULL, -20);
  REQUIRE(device._eirDataLength == 31);
  device.setAdvertisementData(3, sizeof(shortResponse), shortResponse, -10);
  REQUIRE(device._advertisementDataLength == 3);
  REQUIRE(device._eirDataLength == 3);
}

TEST_CASE("Oversized advertising data is rejected", "[ArduinoBLE::BLEDevice]")
{
  BLEDevice device;
  uint8_t valid[] = {2, 0x09, 'x'};
  uint8_t oversized[32] = {};
  device.setAdvertisementData(3, sizeof(valid), valid, -10);
  device.setAdvertisementData(3, sizeof(oversized), oversized, -20);
  device.setScanResponseData(sizeof(oversized), oversized, -30);
  device.setAdvertisementData(0xff, sizeof(valid), valid, -40);
  device.setScanResponseData(1, NULL, -50);
  REQUIRE(device._eirDataLength == 3);
  REQUIRE(device._rssi == -10);
  REQUIRE(device.localName() == "x");
}

TEST_CASE("GATT cleanup is safe before begin and after end", "[ArduinoBLE::GATT]")
{
  GATT.end();
  GATT.begin();
  GATT.end();
  GATT.end();
  REQUIRE(GATT._genericAccessService == NULL);
  REQUIRE(GATT._deviceNameCharacteristic == NULL);
  REQUIRE(GATT._appearanceCharacteristic == NULL);
  REQUIRE(GATT._genericAttributeService == NULL);
  REQUIRE(GATT._servicesChangedCharacteristic == NULL);
}

TEST_CASE("GATT cycles preserve application characteristic ownership", "[ArduinoBLE::GATT]")
{
  BLEService service("180f");
  BLECharacteristic characteristic("2a19", BLERead | BLEWrite, 512);
  uint8_t value[512] = {};
  for (unsigned cycle = 0; cycle < 32; ++cycle) {
    GATT.begin();
    service.addCharacteristic(characteristic);
    GATT.addService(service.local());
    GATT.end();
    value[0] = cycle;
    REQUIRE(characteristic.writeValue(value, sizeof(value)) == 1);
    REQUIRE(characteristic.valueLength() == 512);
    REQUIRE(GATT.attributeCount() == 0);
  }
}

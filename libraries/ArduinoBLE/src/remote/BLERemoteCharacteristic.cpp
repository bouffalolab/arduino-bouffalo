/*
  This file is part of the ArduinoBLE library.
  Copyright (c) 2019 Arduino SA. All rights reserved.

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

#include "BLEProperty.h"

#include "utility/ATT.h"

#include "BLERemoteCharacteristic.h"

namespace {

constexpr int kMaxAttributeValueBytes = 512;
constexpr uint8_t kAttError = 0x01;
constexpr uint8_t kAttReadResponse = 0x0b;
constexpr uint8_t kAttReadBlobRequest = 0x0c;
constexpr uint8_t kAttReadBlobResponse = 0x0d;
constexpr uint8_t kAttPrepareWriteResponse = 0x17;
constexpr uint8_t kAttExecuteWriteResponse = 0x19;
constexpr uint8_t kAttInvalidOffset = 0x07;

bool is_invalid_read_blob_offset(const uint8_t response[], int responseLength)
{
  return responseLength == 5 &&
         response[0] == kAttError &&
         response[1] == kAttReadBlobRequest &&
         response[4] == kAttInvalidOffset;
}

bool append_value(uint8_t*& value, int& valueLength, const uint8_t data[], int length)
{
  if (length < 0 || valueLength > kMaxAttributeValueBytes - length) {
    return false;
  }
  if (length == 0) {
    return true;
  }

  uint8_t* resized = (uint8_t*)realloc(value, valueLength + length);
  if (resized == NULL) {
    return false;
  }

  memcpy(resized + valueLength, data, length);
  value = resized;
  valueLength += length;
  return true;
}

bool replace_value(uint8_t*& destination, int& destinationLength, const uint8_t value[], int length)
{
  if (length < 0 || length > kMaxAttributeValueBytes) {
    return false;
  }

  uint8_t* replacement = NULL;
  if (length != 0) {
    replacement = (uint8_t*)malloc(length);
    if (replacement == NULL) {
      return false;
    }
    memcpy(replacement, value, length);
  }

  free(destination);
  destination = replacement;
  destinationLength = length;
  return true;
}

} // namespace

BLERemoteCharacteristic::BLERemoteCharacteristic(const uint8_t uuid[], uint8_t uuidLen, uint16_t connectionHandle,
                                                  uint16_t startHandle, uint16_t permissions, uint16_t valueHandle) :
  BLERemoteAttribute(uuid, uuidLen),
  _connectionHandle(connectionHandle),
  _startHandle(startHandle),
  _properties((uint8_t)(permissions & 0x00FF)),
  _permissions((uint8_t)((permissions & 0xFF00)>>8)),
  _valueHandle(valueHandle),
  _value(NULL),
  _valueLength(0),
  _valueUpdated(false),
  _updatedValueRead(true),
  _valueUpdatedEventHandler(NULL)
{
}

BLERemoteCharacteristic::~BLERemoteCharacteristic()
{
  for (unsigned int i = 0; i < descriptorCount(); i++) {
    BLERemoteDescriptor* d = descriptor(i);

    if (d->release() == 0) {
      delete d;
    }
  }

  _descriptors.clear();

  if (_value) {
    free(_value);
    _value = NULL;
  }
}

uint16_t BLERemoteCharacteristic::startHandle() const
{
  return _startHandle;
}

uint8_t BLERemoteCharacteristic::properties() const
{
  return _properties;
}

const uint8_t* BLERemoteCharacteristic::value() const
{
  return _value;
}

int BLERemoteCharacteristic::valueLength() const
{
  return _valueLength;
}

uint8_t BLERemoteCharacteristic::operator[] (int offset) const
{
  if (_value) {
    return _value[offset];
  }

  return 0;
}

int BLERemoteCharacteristic::writeValue(const uint8_t value[], int length, bool withResponse)
{
  if (!ATT.connected(_connectionHandle) || value == NULL || length < 0 ||
      length > kMaxAttributeValueBytes) {
    return false;
  }

  uint16_t maxLength = ATT.mtu(_connectionHandle) - 3;

  if (length <= (int)maxLength && (_properties & BLEWrite) && withResponse) {
    uint8_t resp[4];
    int respLength = ATT.writeReq(_connectionHandle, _valueHandle, value, length, resp);

    if (!respLength) {
      return 0;
    }

    if (resp[0] == 0x01) {
      // error
      return 0;
    }

    return replace_value(_value, _valueLength, value, length) ? 1 : 0;
  } else if (length <= (int)maxLength && (_properties & BLEWriteWithoutResponse)) {
    ATT.writeCmd(_connectionHandle, _valueHandle, value, length);

    return replace_value(_value, _valueLength, value, length) ? 1 : 0;
  } else if (length > (int)maxLength && withResponse && (_properties & BLEWrite)) {
    const uint16_t mtu = ATT.mtu(_connectionHandle);
    if (mtu <= 5) {
      return 0;
    }

    const uint8_t chunkLength = (uint8_t)min((uint16_t)255, (uint16_t)(mtu - 5));
    uint8_t* response = (uint8_t*)malloc(mtu);
    if (chunkLength == 0 || response == NULL) {
      return 0;
    }

    bool prepared = false;
    int offset = 0;
    while (offset < length) {
      const int remaining = length - offset;
      const uint8_t chunk = (uint8_t)min((int)chunkLength, remaining);
      const int responseLength = ATT.prepareWriteReq(
          _connectionHandle,
          _valueHandle,
          offset,
          value + offset,
          chunk,
          response);
      if (responseLength != 5 + chunk ||
          response[0] != kAttPrepareWriteResponse ||
          response[1] != (uint8_t)(_valueHandle & 0xff) ||
          response[2] != (uint8_t)(_valueHandle >> 8) ||
          response[3] != (uint8_t)(offset & 0xff) ||
          response[4] != (uint8_t)(offset >> 8) ||
          memcmp(response + 5, value + offset, chunk) != 0) {
        if (prepared) {
          ATT.executeWriteReq(_connectionHandle, false, response);
        }
        free(response);
        return 0;
      }
      prepared = true;
      offset += chunk;
    }

    const int responseLength = ATT.executeWriteReq(_connectionHandle, true, response);
    const bool executeSucceeded =
        responseLength == 1 && response[0] == kAttExecuteWriteResponse;
    if (!executeSucceeded) {
      ATT.executeWriteReq(_connectionHandle, false, response);
    }
    free(response);
    if (!executeSucceeded) {
      return 0;
    }

    return replace_value(_value, _valueLength, value, length) ? 1 : 0;
  }

  return 0;
}

int BLERemoteCharacteristic::writeValue(const char* value, bool withResponse)
{
  return writeValue((uint8_t*)value, strlen(value), withResponse);
}

bool BLERemoteCharacteristic::valueUpdated()
{
  ATT.connected(_connectionHandle); // to force a poll

  bool result = _valueUpdated;

  _valueUpdated = false;

  return result;
}

bool BLERemoteCharacteristic::updatedValueRead()
{
  bool result = _updatedValueRead;

  _updatedValueRead = true;

  return result;
}

bool BLERemoteCharacteristic::read()
{
  if (!ATT.connected(_connectionHandle)) {
    return false;
  }

  const uint16_t mtu = ATT.mtu(_connectionHandle);
  if (mtu < 2) {
    _valueLength = 0;
    return false;
  }

  uint8_t* response = (uint8_t*)malloc(mtu);
  if (response == NULL) {
    _valueLength = 0;
    return false;
  }

  uint8_t* value = NULL;
  int valueLength = 0;
  int responseLength = ATT.readReq(_connectionHandle, _valueHandle, response);
  if (!responseLength || response[0] != kAttReadResponse ||
      !append_value(value, valueLength, response + 1, responseLength - 1)) {
    free(response);
    free(value);
    _valueLength = 0;
    return false;
  }

  const int fullResponseLength = mtu - 1;
  while (responseLength - 1 == fullResponseLength) {
    responseLength = ATT.readBlobReq(
        _connectionHandle,
        _valueHandle,
        valueLength,
        response);
    if (is_invalid_read_blob_offset(response, responseLength)) {
      break;
    }
    if (!responseLength || response[0] != kAttReadBlobResponse ||
        !append_value(value, valueLength, response + 1, responseLength - 1)) {
      free(response);
      free(value);
      _valueLength = 0;
      return false;
    }
  }

  free(response);
  free(_value);
  _value = value;
  _valueLength = valueLength;

  return true;
}

bool BLERemoteCharacteristic::writeCccd(uint16_t value)
{
  int numDescriptors = descriptorCount();

  for (int i = 0; i < numDescriptors; i++) {
    BLERemoteDescriptor* d = descriptor(i);

    if (strcmp(d->uuid(), "2902") == 0) {
      return d->writeValue((uint8_t*)&value, sizeof(value));
    }
  }

  if (_properties & (BLENotify | BLEIndicate)) {
    // no CCCD descriptor found, fallback to _valueHandle + 1
    BLERemoteDescriptor cccd(NULL, 0, _connectionHandle, _valueHandle + 1);

    return cccd.writeValue((uint8_t*)&value, sizeof(value));
  }

  return false;
}

uint16_t BLERemoteCharacteristic::valueHandle() const
{
  return _valueHandle;
}

unsigned int BLERemoteCharacteristic::descriptorCount() const
{
  return _descriptors.size();
}

BLERemoteDescriptor* BLERemoteCharacteristic::descriptor(unsigned int index) const
{
  return _descriptors.get(index);
}

void BLERemoteCharacteristic::setEventHandler(BLECharacteristicEvent event, BLECharacteristicEventHandler eventHandler)
{
  if (event == BLEUpdated) {
    _valueUpdatedEventHandler = eventHandler;
  }
}

void BLERemoteCharacteristic::addDescriptor(BLERemoteDescriptor* descriptor)
{
  descriptor->retain();

  _descriptors.add(descriptor);
}

void BLERemoteCharacteristic::writeValue(BLEDevice device, const uint8_t value[], int length)
{
  _valueLength = length;
  _value = (uint8_t*)realloc(_value, _valueLength);

  if (_value == NULL) {
    _valueLength = 0;
    return;
  }

  _valueUpdated = true;
  _updatedValueRead = false;
  memcpy(_value, value, _valueLength);

  if (_valueUpdatedEventHandler) {
    _valueUpdatedEventHandler(device, BLECharacteristic(this));
  }
}

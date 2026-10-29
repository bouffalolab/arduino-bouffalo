/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef HCI_ACL_FLOW_CONTROL_H
#define HCI_ACL_FLOW_CONTROL_H

#include <stdint.h>

// Completed-packet events and disconnections apply to one connection handle.
// Never release another connection's controller buffers on either event.
class HCIACLFlowControl {
public:
  HCIACLFlowControl() { clear(); }

  void clear() {
    for (unsigned i = 0; i < kSlots; ++i) {
      _entries[i].handle = 0xffff;
      _entries[i].pending = 0;
    }
  }

  unsigned pending() const {
    unsigned total = 0;
    for (unsigned i = 0; i < kSlots; ++i) total += _entries[i].pending;
    return total;
  }

  bool queued(uint16_t handle) {
    if (handle > 0x0fff) return false;
    Entry* entry = find(handle);
    if (!entry) {
      for (unsigned i = 0; i < kSlots; ++i) {
        if (_entries[i].pending == 0) {
          entry = &_entries[i];
          entry->handle = handle;
          break;
        }
      }
    }
    if (!entry || entry->pending == 255) return false;
    ++entry->pending;
    return true;
  }

  void completed(uint16_t handle, uint16_t count) {
    Entry* entry = find(handle);
    if (entry) {
      entry->pending = count >= entry->pending ? 0 : entry->pending - count;
    }
  }

  void disconnected(uint16_t handle) {
    Entry* entry = find(handle);
    if (entry) {
      entry->pending = 0;
      entry->handle = 0xffff;
    }
  }

private:
  static const unsigned kSlots = 8;
  struct Entry { uint16_t handle; uint8_t pending; } _entries[kSlots];

  Entry* find(uint16_t handle) {
    for (unsigned i = 0; i < kSlots; ++i) {
      if (_entries[i].handle == handle) return &_entries[i];
    }
    return 0;
  }
};

#endif

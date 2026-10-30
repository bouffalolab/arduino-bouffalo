#ifndef UNOR4_BL616CL_BLE_HCI_SERVICE_H
#define UNOR4_BL616CL_BLE_HCI_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "byte_ring.h"

namespace bridge {

struct BleHciController {
    typedef void (*ReceivePacket)(void *context, const uint8_t *packet, size_t size);
    typedef bool (*Start)(void *context, ReceivePacket receive, void *receive_context);
    typedef void (*Stop)(void *context);
    typedef bool (*Send)(void *context, const uint8_t *packet, size_t size);
    typedef bool (*PauseScan)(void *context);
    typedef void (*ResumeScan)(void *context);

    void *context = nullptr;
    Start start = nullptr;
    Stop stop = nullptr;
    Send send = nullptr;
    PauseScan pause_scan = nullptr;
    ResumeScan resume_scan = nullptr;
};

struct BleHciSynchronization {
    typedef void (*Callback)(void *context);

    void *context = nullptr;
    Callback enter = nullptr;
    Callback exit = nullptr;
};

struct BleHciWaitHooks {
    typedef uint32_t (*NowMs)(void *context);
    typedef void (*Idle)(void *context);

    void *context = nullptr;
    NowMs now_ms = nullptr;
    Idle idle = nullptr;
};

enum class BleHciWaitResult : uint8_t {
    DataAvailable,
    TimedOut,
    Inactive,
    Invalid,
};

class BleHciService {
public:
    static constexpr size_t kMaxPacketBytes = 1024U;
    /*
     * A complete H:4 ACL packet is admitted up to kMaxPacketBytes. Keep the
     * control queue at the same byte capacity so a valid large ACL packet is
     * never rejected solely because it cannot fit in one queue transaction.
     */
    static constexpr size_t kControlRxQueueBytes = kMaxPacketBytes;
    static constexpr size_t kControlRxQueueStorageBytes = kControlRxQueueBytes + 1U;
    /*
     * Rebalance the fixed RAM budget toward complete control/ACL packets.
     * ArduinoBLE drains advertising reports in bounded HCIREAD chunks, so a
     * 1536-byte report backlog remains ample while the control queue can hold
     * one maximum-size ACL packet.
     */
    static constexpr size_t kAdvertisingRxQueueBytes = 1536U;
    static constexpr size_t kAdvertisingRxQueueStorageBytes = kAdvertisingRxQueueBytes + 1U;
    static constexpr uint32_t kMaxWaitMs = 5000U;

    explicit BleHciService(
        BleHciController controller = {},
        BleHciSynchronization synchronization = {});

    bool begin();
    void end();
    bool active() const;
    bool set_controller(
        BleHciController controller,
        BleHciSynchronization synchronization = {});

    bool send_packet(const uint8_t *packet, size_t size);
    bool receive_packet(const uint8_t *packet, size_t size);
    size_t available() const;
    size_t read(uint8_t *destination, size_t limit);
    void clear();
    void discard_advertising_reports();

    BleHciWaitResult wait_for_packet(uint32_t timeout_ms, const BleHciWaitHooks &hooks) const;

    uint32_t dropped_packet_count() const;
    uint32_t dropped_byte_count() const;

private:
    static void receive_from_controller(void *context, const uint8_t *packet, size_t size);
    static bool valid_host_packet(const uint8_t *packet, size_t size);
    static bool valid_controller_packet(const uint8_t *packet, size_t size);
    static bool valid_h4_packet(const uint8_t *packet, size_t size, bool host_to_controller);
    static bool is_advertising_report(const uint8_t *packet, size_t size);
    static bool is_le_scan_disable_command(const uint8_t *packet, size_t size);

    void enter() const;
    void exit() const;

    BleHciController controller_;
    BleHciSynchronization synchronization_;
    ByteRing<kControlRxQueueStorageBytes> control_rx_;
    ByteRing<kAdvertisingRxQueueStorageBytes> advertising_rx_;
    size_t rx_packet_remaining_ = 0U;
    bool rx_packet_advertising_ = false;
    volatile bool active_ = false;
    volatile uint32_t dropped_packet_count_ = 0U;
    volatile uint32_t dropped_byte_count_ = 0U;
};

} // namespace bridge

#endif

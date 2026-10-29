#include "ble_hci_service.h"

namespace bridge {

namespace {

constexpr uint8_t kH4Command = 0x01U;
constexpr uint8_t kH4AclData = 0x02U;
constexpr uint8_t kH4Event = 0x04U;
constexpr uint8_t kEventLeMeta = 0x3EU;
constexpr uint8_t kLeMetaAdvertisingReport = 0x02U;
constexpr uint16_t kLeSetScanEnableOpcode = 0x200CU;

uint16_t read_le16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8U);
}

bool h4_packet_has_size(size_t size, size_t header_size, size_t payload_size)
{
    return size == header_size + payload_size;
}

template <size_t Capacity>
size_t queued_packet_size(const ByteRing<Capacity> &queue)
{
    uint8_t type = 0U, low = 0U, high = 0U;
    if (!queue.peek(0U, type)) {
        return 0U;
    }
    if (type == kH4Event && queue.peek(2U, low)) {
        return 3U + low;
    }
    if (type == kH4AclData && queue.peek(3U, low) && queue.peek(4U, high)) {
        return 5U + low + (static_cast<size_t>(high) << 8U);
    }
    return 0U;
}

} // namespace

BleHciService::BleHciService(
    BleHciController controller,
    BleHciSynchronization synchronization)
    : controller_(controller), synchronization_(synchronization)
{
}

bool BleHciService::begin()
{
    if (active_) {
        return true;
    }

    if (controller_.start == nullptr || controller_.send == nullptr) {
        return false;
    }

    clear();
    if (!controller_.start(controller_.context, &BleHciService::receive_from_controller, this)) {
        return false;
    }

    active_ = true;
    return true;
}

void BleHciService::end()
{
    if (active_ && controller_.stop != nullptr) {
        controller_.stop(controller_.context);
    }

    active_ = false;
    clear();
}

bool BleHciService::active() const
{
    return active_;
}

bool BleHciService::set_controller(
    BleHciController controller,
    BleHciSynchronization synchronization)
{
    enter();
    if (active_) {
        exit();
        return false;
    }

    controller_ = controller;
    synchronization_ = synchronization;
    control_rx_.clear();
    advertising_rx_.clear();
    rx_packet_remaining_ = 0U;
    rx_packet_advertising_ = false;
    exit();
    return true;
}

bool BleHciService::send_packet(const uint8_t *packet, size_t size)
{
    if (!active_ || controller_.send == nullptr || !valid_host_packet(packet, size)) {
        return false;
    }

    if (is_le_scan_disable_command(packet, size)) {
        /*
         * ArduinoBLE stops scanning synchronously. Retaining a large backlog
         * of reports here can prevent the controller's Disable completion
         * from being queued before the host polls again.
         */
        discard_advertising_reports();
    }

    return controller_.send(controller_.context, packet, size);
}

bool BleHciService::receive_packet(const uint8_t *packet, size_t size)
{
    if (!active_ || !valid_controller_packet(packet, size)) {
        return false;
    }

    enter();
    ByteRing<kControlRxQueueStorageBytes> *queue = &control_rx_;
    if (is_advertising_report(packet, size)) {
        queue = nullptr;
    }

    const size_t free_bytes =
        queue != nullptr ? queue->free() : advertising_rx_.free();
    if (free_bytes < size) {
        ++dropped_packet_count_;
        dropped_byte_count_ += static_cast<uint32_t>(size);
        exit();
        return false;
    }

    const bool complete =
        queue != nullptr ? queue->write(packet, size) == size :
                           advertising_rx_.write(packet, size) == size;
    if (!complete) {
        ++dropped_packet_count_;
        dropped_byte_count_ += static_cast<uint32_t>(size);
    }
    exit();
    return complete;
}

size_t BleHciService::available() const
{
    enter();
    const size_t size = control_rx_.size() + advertising_rx_.size();
    exit();
    return size;
}

size_t BleHciService::read(uint8_t *destination, size_t limit)
{
    if (destination == nullptr) {
        return 0U;
    }
    enter();
    size_t size = 0U;
    while (size < limit) {
        if (rx_packet_remaining_ == 0U) {
            // Priority may change only between H:4 packets. HCIREAD may split
            // a packet anywhere, including its header, across AT responses.
            rx_packet_advertising_ = control_rx_.size() == 0U;
            rx_packet_remaining_ = rx_packet_advertising_
                ? queued_packet_size(advertising_rx_)
                : queued_packet_size(control_rx_);
            if (rx_packet_remaining_ == 0U) {
                break;
            }
        }
        const size_t chunk = rx_packet_remaining_ < limit - size
            ? rx_packet_remaining_ : limit - size;
        const size_t read = rx_packet_advertising_
            ? advertising_rx_.read(destination + size, chunk)
            : control_rx_.read(destination + size, chunk);
        size += read;
        rx_packet_remaining_ -= read;
        if (read == 0U) {
            break;
        }
    }
    exit();
    return size;
}

void BleHciService::clear()
{
    enter();
    control_rx_.clear();
    advertising_rx_.clear();
    rx_packet_remaining_ = 0U;
    rx_packet_advertising_ = false;
    exit();
}

void BleHciService::discard_advertising_reports()
{
    enter();
    if (rx_packet_advertising_ && rx_packet_remaining_ != 0U) {
        // Finish the packet already visible to the host; drop only reports
        // whose first byte has not been delivered.
        advertising_rx_.truncate(rx_packet_remaining_);
    } else {
        advertising_rx_.clear();
    }
    exit();
}

BleHciWaitResult BleHciService::wait_for_packet(uint32_t timeout_ms, const BleHciWaitHooks &hooks) const
{
    if (!active_) {
        return BleHciWaitResult::Inactive;
    }
    if (hooks.now_ms == nullptr) {
        return BleHciWaitResult::Invalid;
    }

    const uint32_t bounded_timeout = timeout_ms <= kMaxWaitMs ? timeout_ms : kMaxWaitMs;
    const uint32_t started_at = hooks.now_ms(hooks.context);
    while (available() == 0U) {
        const uint32_t now_ms = hooks.now_ms(hooks.context);
        if (static_cast<uint32_t>(now_ms - started_at) >= bounded_timeout) {
            return BleHciWaitResult::TimedOut;
        }
        if (hooks.idle != nullptr) {
            hooks.idle(hooks.context);
        }
    }

    return BleHciWaitResult::DataAvailable;
}

uint32_t BleHciService::dropped_packet_count() const
{
    return dropped_packet_count_;
}

uint32_t BleHciService::dropped_byte_count() const
{
    return dropped_byte_count_;
}

void BleHciService::receive_from_controller(void *context, const uint8_t *packet, size_t size)
{
    static_cast<BleHciService *>(context)->receive_packet(packet, size);
}

bool BleHciService::valid_host_packet(const uint8_t *packet, size_t size)
{
    return valid_h4_packet(packet, size, true);
}

bool BleHciService::valid_controller_packet(const uint8_t *packet, size_t size)
{
    return valid_h4_packet(packet, size, false);
}

bool BleHciService::valid_h4_packet(const uint8_t *packet, size_t size, bool host_to_controller)
{
    if (packet == nullptr || size == 0U || size > kMaxPacketBytes) {
        return false;
    }

    switch (packet[0]) {
    case kH4Command:
        return host_to_controller && size >= 4U &&
               h4_packet_has_size(size, 4U, packet[3]);
    case kH4AclData:
        return size >= 5U &&
               h4_packet_has_size(size, 5U, read_le16(packet + 3U));
    case kH4Event:
        return !host_to_controller && size >= 3U &&
               h4_packet_has_size(size, 3U, packet[2]);
    default:
        return false;
    }
}

bool BleHciService::is_advertising_report(const uint8_t *packet, size_t size)
{
    return size >= 4U &&
           packet[0] == kH4Event &&
           packet[1] == kEventLeMeta &&
           packet[2] >= 1U &&
           packet[3] == kLeMetaAdvertisingReport;
}

bool BleHciService::is_le_scan_disable_command(const uint8_t *packet, size_t size)
{
    return size == 6U &&
           packet[0] == kH4Command &&
           read_le16(packet + 1U) == kLeSetScanEnableOpcode &&
           packet[3] == 2U &&
           packet[4] == 0U &&
           packet[5] <= 1U;
}

void BleHciService::enter() const
{
    if (synchronization_.enter != nullptr) {
        synchronization_.enter(synchronization_.context);
    }
}

void BleHciService::exit() const
{
    if (synchronization_.exit != nullptr) {
        synchronization_.exit(synchronization_.context);
    }
}

} // namespace bridge

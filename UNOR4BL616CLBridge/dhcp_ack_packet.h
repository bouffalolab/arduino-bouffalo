#pragma once
#include <stddef.h>
#include <stdint.h>

namespace bridge {
inline bool is_ap_client_ip_reply(const uint8_t *packet, size_t size, uint32_t ap_ipv4)
{
    if (packet == nullptr || size < 34U || ap_ipv4 == 0U ||
        packet[12] != 8U || packet[13] != 0U || packet[14] >> 4U != 4U)
        return false;
    const size_t ihl = (packet[14] & 15U) * 4U;
    if (ihl < 20U || size < 14U + ihl ||
        ((size_t(packet[16]) << 8U) | packet[17]) < ihl) return false;
    const uint8_t *source = reinterpret_cast<const uint8_t *>(&ap_ipv4);
    bool same_destination = true;
    for (size_t index = 0U; index < 4U; ++index) {
        if (packet[26U + index] != source[index]) return false;
        if (packet[30U + index] != source[index]) same_destination = false;
    }
    if (same_destination || packet[30] == 0U || packet[30] >= 224U) return false;
    if (packet[23] == 17U) {
        const size_t udp = 14U + ihl;
        if (size < udp + 8U ||
            (packet[udp] == 0U && packet[udp + 1U] == 67U) ||
            (packet[udp + 2U] == 0U && packet[udp + 3U] == 68U)) return false;
    }
    return true;
}

// A bounded passive classification of an outbound, unfragmented DHCP reply.
// The caller supplies an Ethernet prefix, including at most 64 option bytes.
inline uint8_t dhcp_server_message_type(const uint8_t *packet, size_t size)
{
    if (packet == nullptr || size < 14U + 20U + 8U + 240U + 3U ||
        packet[12] != 8U || packet[13] != 0U || packet[14] >> 4U != 4U ||
        packet[23] != 17U || ((packet[20] & 0x3FU) | packet[21]) != 0U)
        return false;
    const size_t ihl = (packet[14] & 15U) * 4U;
    if (ihl < 20U) return false;
    const size_t udp = 14U + ihl;
    const size_t bootp = udp + 8U;
    if (size < bootp + 243U || packet[udp] != 0U || packet[udp + 1U] != 67U ||
        packet[udp + 2U] != 0U || packet[udp + 3U] != 68U ||
        packet[bootp] != 2U || packet[bootp + 1U] != 1U || packet[bootp + 2U] != 6U ||
        packet[bootp + 236U] != 99U || packet[bootp + 237U] != 130U ||
        packet[bootp + 238U] != 83U || packet[bootp + 239U] != 99U)
        return false;
    const size_t udp_length = (size_t(packet[udp + 4U]) << 8U) | packet[udp + 5U];
    const size_t ip_length = (size_t(packet[16]) << 8U) | packet[17];
    if (udp_length < 251U || ip_length < ihl + udp_length) return false;
    const size_t end = size < udp + udp_length ? size : udp + udp_length;
    size_t cursor = bootp + 240U;
    while (cursor < end) {
        const uint8_t option = packet[cursor++];
        if (option == 255U) return false;
        if (option == 0U) continue;
        if (cursor >= end) return false;
        const size_t length = packet[cursor++];
        if (length > end - cursor) return false;
        if (option == 53U) return length == 1U && packet[cursor] <= 8U ? packet[cursor] : 0U;
        cursor += length;
    }
    return false;
}
inline bool is_dhcp_ack_packet(const uint8_t *packet, size_t size)
{
    return dhcp_server_message_type(packet, size) == 5U;
}
}

#ifndef BRIDGE_UDP_FRAGMENT_CONTENT_DIAGNOSTIC_H
#define BRIDGE_UDP_FRAGMENT_CONTENT_DIAGNOSTIC_H
#include <stddef.h>
#include <stdint.h>

namespace bridge {
// Bounded diagnostic for the current 1500-MTU, <=16 KiB UDP send. Keeps
// fingerprints, never payload, pointers to caller storage, or allocation.
struct UdpFragmentContentDiagnostic {
    uint32_t active = 0, checked = 0, header_errors = 0, payload_errors = 0;
    uint32_t mask = 0, identification = 0;
    uint32_t udp_checksum_errors = 0, udp_checksum_residual = 0, payload_sum = 0;
    uint32_t expected[12] = {};
    size_t payload_size = 0;
    uint32_t destination = 0;
    uint16_t destination_port = 0;
    bool identified = false;
    static uint16_t be16(const uint8_t *p) { return uint16_t(p[0]) * 256U + p[1]; }
    static uint32_t be32(const uint8_t *p) {
        return (uint32_t(p[0]) << 24U) | (uint32_t(p[1]) << 16U) |
               (uint32_t(p[2]) << 8U) | p[3];
    }
    static uint32_t hash(const uint8_t *p, size_t n, uint32_t h = 2166136261U) {
        while (n--) h = (h ^ *p++) * 16777619U;
        return h;
    }
    void prepare(const uint8_t *data, size_t n, const uint8_t *remote, uint16_t port) {
        *this = {};
        if (!data || !remote || n <= 1472U || n > 16384U) return;
        for (size_t i = 0; i < n; i += 2) {
            payload_sum += uint32_t(data[i]) << 8U;
            if (i + 1 < n) payload_sum += data[i + 1];
        }
        active = 1; payload_size = n; destination = be32(remote); destination_port = port;
        for (size_t index = 0, offset = 0; offset < n; ++index) {
            size_t length = index == 0 ? 1472U : 1480U;
            if (length > n - offset) length = n - offset;
            expected[index] = hash(data + offset, length);
            offset += length;
        }
    }
    // read copies bytes from the still-owned Ethernet pbuf. No full-frame
    // scratch buffer: at most 42-byte prefix and 64-byte payload chunks.
    template<class Reader> void observe(Reader read, size_t ethernet_offset) {
        if (!active) return;
        uint8_t header[42] = {};
        if (read(header, sizeof(header), ethernet_offset) < 34U ||
            be16(header + 12) != 0x0800 || header[14] != 0x45 || header[23] != 17 ||
            be32(header + 30) != destination) return;
        const uint16_t flags = be16(header + 20);
        const size_t offset = (flags & 0x1fffU) * 8U;
        if (offset == 0) {
            if (read(header, sizeof(header), ethernet_offset) != sizeof(header) ||
                be16(header + 36) != destination_port) return;
            identification = be16(header + 18); identified = true;
            if (be16(header + 40) != 0) {
                uint32_t udp_sum = payload_sum + 17U + be16(header + 38);
                for (size_t i = 26; i < 42; i += 2) udp_sum += be16(header + i);
                while (udp_sum >> 16) udp_sum = (udp_sum & 0xffffU) + (udp_sum >> 16);
                udp_checksum_residual = (~udp_sum) & 0xffffU;
                if (udp_checksum_residual) ++udp_checksum_errors;
            }
        }
        if (!identified || be16(header + 18) != identification) return;
        const size_t index = offset / 1480U;
        const size_t total = be16(header + 16);
        const size_t expected_length = payload_size + 8U > offset ?
            (payload_size + 8U - offset < 1480U ? payload_size + 8U - offset : 1480U) : 0U;
        uint32_t checksum = 0;
        for (size_t i = 14; i < 34; i += 2) checksum += be16(header + i);
        while (checksum >> 16) checksum = (checksum & 0xffffU) + (checksum >> 16);
        ++checked;
        if (index >= 12 || offset % 1480U || total != expected_length + 20U ||
            !expected_length || checksum != 0xffffU || (flags & 0xc000U) ||
            bool(flags & 0x2000U) != (offset + expected_length < payload_size + 8U)) {
            ++header_errors; return;
        }
        const size_t skip = offset == 0 ? 8U : 0U;
        uint32_t fingerprint = 2166136261U;
        uint8_t chunk[64];
        for (size_t cursor = skip; cursor < expected_length;) {
            size_t length = expected_length - cursor;
            if (length > sizeof(chunk)) length = sizeof(chunk);
            if (read(chunk, length, ethernet_offset + 34U + cursor) != length) {
                ++payload_errors; return;
            }
            fingerprint = hash(chunk, length, fingerprint); cursor += length;
        }
        if (fingerprint != expected[index]) ++payload_errors;
        mask |= 1U << index;
    }
};
}
#endif

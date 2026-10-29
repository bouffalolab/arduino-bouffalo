#pragma once
#include <stddef.h>
#include <stdint.h>

namespace bridge {
// Observe the still-owned, decrypted LLC prefix. Retain numeric metadata only.
struct UdpMacFragmentObserver {
    uint32_t total = 0, mask = 0, sequence = 0;
    uint16_t id = 0, count = 0, offsets[12] = {};
    void observe(const uint8_t *llc, size_t size, uint32_t sn) {
        if (!llc || size < 28 || llc[0] != 0xaa || llc[1] != 0xaa ||
            llc[2] != 3 || llc[6] != 8 || llc[7] != 0) return;
        const uint8_t *ip = llc + 8;
        if ((ip[0] >> 4) != 4 || (ip[0] & 15) < 5 || ip[9] != 17) return;
        const uint16_t flags = uint16_t(ip[6]) * 256U + ip[7];
        if (!(flags & 0x3fffU)) return;
        const uint16_t next_id = uint16_t(ip[4]) * 256U + ip[5];
        if (!total || next_id != id) {
            id = next_id; count = 0; mask = 0;
            for (auto &offset : offsets) offset = 0;
        }
        const uint16_t offset = flags & 0x1fffU;
        offsets[count % 12] = offset;
        if (offset % 185U == 0 && offset / 185U < 12) mask |= 1U << (offset / 185U);
        ++count; ++total; sequence = sn;
    }
};
}

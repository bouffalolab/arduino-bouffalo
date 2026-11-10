#include "BLUpdate.h"
#include <string.h>
extern "C" {
#include "bflb_flash.h"
#include "bflb_boot2.h"
#include "ota/ota.h"
#include "utils_crc.h"
}

BLUpdateClass BLUpdate;

/** @brief Decode an unaligned little-endian integer from an image header. */
static uint32_t read32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

/** @brief Find two safe firmware slots in the current partition table. */
static bool capacity(uint32_t &available) {
    if (bflb_boot2_init()) return false;
    const auto *table = bflb_boot2_get_pt_config();
    if (table->pt_table.entryCnt > PT_ENTRY_MAX) return false;
    const pt_table_entry_config *fw = nullptr;
    for (unsigned i = 0; i < table->pt_table.entryCnt; ++i) {
        const auto &p = table->pt_entries[i];
        if (!memcmp(p.name, "FW\0", 3)) {
            if (fw) return false;
            fw = &p;
        }
    }
    const uint32_t flash = bflb_flash_get_size();
    if (!fw || fw->device || fw->active_index > 1 || fw->start_address[0] != 0x10000 ||
        fw->max_len[0] < 0x10000 || fw->max_len[0] != fw->max_len[1] ||
        fw->max_len[0] % 4096 || fw->start_address[1] != fw->start_address[0] + fw->max_len[0] ||
        fw->start_address[1] >= flash || fw->max_len[1] > flash - fw->start_address[1]) return false;
    for (unsigned i = 0; i < table->pt_table.entryCnt; ++i) {
        const auto &p = table->pt_entries[i];
        if (&p == fw || p.device) continue;
        for (unsigned n = 0; n < 2; ++n) {
            if (!p.max_len[n]) continue;
            if (p.start_address[n] >= flash || p.max_len[n] > flash - p.start_address[n]) return false;
            bool mfg = !memcmp(p.name, "mfg\0", 4) && n == 0 &&
                       p.start_address[n] == fw->start_address[1] && p.max_len[n] == fw->max_len[1];
            for (unsigned slot = 0; slot < 2; ++slot) {
                if (mfg && slot == 1) continue;
                if (p.start_address[n] < fw->start_address[slot] + fw->max_len[slot] &&
                    fw->start_address[slot] < p.start_address[n] + p.max_len[n]) return false;
            }
        }
    }
    available = fw->max_len[!fw->active_index];
    return true;
}

/** @brief Abort the backend and retain a diagnostic error. */
bool BLUpdateClass::fail(Error error) {
    if (handle_) ota_abort(handle_);
    handle_ = nullptr;
    state_ = State::Failed;
    error_ = error;
    return false;
}

bool BLUpdateClass::begin(const uint8_t *header, size_t length) {
    if (state_ == State::Receiving || state_ == State::Verified || state_ == State::Committed) {
        error_ = Error::State;
        return false;
    }
    static const uint8_t chip[16] = "BL616CL";
    if (!header || length != sizeof(header_) || memcmp(header, "BL60X_OTA_Ver1.0", 16) ||
        memcmp(header + 16, "RAW ", 4) || memcmp(header + 32, chip, sizeof(chip))) return fail(Error::Header);
    uint32_t available = 0;
    if (!capacity(available)) return fail(Error::Layout);
    uint32_t imageSize = read32(header + 20);
    if (imageSize < 4096 || imageSize > available) return fail(Error::Size);
    memcpy(header_, header, sizeof(header_));
    size_ = imageSize;
    written_ = 0;
    error_ = Error::None;
    state_ = State::Receiving;
    return true;
}

bool BLUpdateClass::write(uint32_t offset, const uint8_t *data, size_t length) {
    if (state_ != State::Receiving) { error_ = Error::State; return false; }
    if (!data || !length || offset != written_ || length > size_ - written_) return fail(Error::Offset);
    if (!written_) {
        if (length < 256 || memcmp(data, "BFNP", 4) || memcmp(data + 8, "FCFG", 4) ||
            memcmp(data + 100, "PCFG", 4) || read32(data + 132) != size_ - 4096) return fail(Error::Image);
        struct crc32_stream_ctx crc;
        utils_crc32_stream_init(&crc);
        utils_crc32_stream_feed_block(&crc, data, 252);
        if (utils_crc32_stream_results(&crc) != read32(data + 252)) return fail(Error::Image);
        handle_ = ota_start();
        if (!handle_) return fail(Error::Backend);
        uint8_t first[512 + 256];
        memcpy(first, header_, 512);
        memcpy(first + 512, data, 256);
        if (ota_update(handle_, first, sizeof(first))) return fail(Error::Backend);
        data += 256; length -= 256; written_ += 256;
    }
    uint8_t buffer[256];
    while (length) {
        size_t count = length < sizeof(buffer) ? length : sizeof(buffer);
        memcpy(buffer, data, count);
        if (ota_update(handle_, buffer, count)) return fail(Error::Backend);
        data += count; length -= count; written_ += count;
    }
    return true;
}

bool BLUpdateClass::finish() {
    if (state_ != State::Receiving) { error_ = Error::State; return false; }
    if (written_ != size_ || !handle_) return fail(Error::Size);
    if (ota_verify_hash(handle_)) return fail(Error::Hash);
    state_ = State::Verified;
    return true;
}

bool BLUpdateClass::commit() {
    if (state_ != State::Verified) { error_ = Error::State; return false; }
    if (ota_finish(handle_, 0, 0)) return fail(Error::Backend);
    handle_ = nullptr;
    state_ = State::Committed;
    error_ = Error::None;
    return true;
}

void BLUpdateClass::abort() {
    if (state_ == State::Committed) { error_ = Error::State; return; }
    if (handle_) ota_abort(handle_);
    handle_ = nullptr;
    state_ = State::Idle;
    error_ = Error::None;
    size_ = written_ = 0;
}

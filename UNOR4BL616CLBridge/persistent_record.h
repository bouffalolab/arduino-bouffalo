#ifndef BRIDGE_PERSISTENT_RECORD_H
#define BRIDGE_PERSISTENT_RECORD_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
extern "C" {
#include "easyflash.h"
}

namespace bridge {

// Metadata and payload share one LittleFS commit. Existing metadata-only
// records remain readable; the next successful write migrates just that key.
inline bool persistent_record_size_matches(size_t total, size_t header, size_t value)
{
    return total == header || total == header + value;
}

inline bool persistent_record_write(const char *key, const void *header,
                                    size_t header_size, const void *value, size_t size)
{
    if (header_size == 0 || size > SIZE_MAX - header_size || (size && !value)) {
        return false;
    }
    uint8_t *record = static_cast<uint8_t *>(malloc(header_size + size));
    if (!record) { return false; }
    memcpy(record, header, header_size);
    if (size) { memcpy(record + header_size, value, size); }
    const bool ok = ef_set_env_blob(key, record, header_size + size) == EF_NO_ERR;
    free(record);
    return ok;
}

inline bool persistent_record_read(const char *metadata_key, const char *legacy_key,
                                   size_t header_size, size_t expected_size,
                                   void *destination, size_t offset, size_t size)
{
    if (offset > expected_size || size > expected_size - offset) { return false; }
    size_t total = 0;
    ef_get_env_blob(metadata_key, nullptr, 0, &total);
    const char *key = metadata_key;
    if (total == header_size + expected_size) {
        offset += header_size;
    } else if (total == header_size) {
        key = legacy_key;
        ef_get_env_blob(key, nullptr, 0, &total);
        if (total != expected_size) { return false; }
    } else {
        return false;
    }
    return size == 0 || ef_get_env_blob_offset(key, destination, size, nullptr,
                                               static_cast<int>(offset)) == size;
}

} // namespace bridge
#endif

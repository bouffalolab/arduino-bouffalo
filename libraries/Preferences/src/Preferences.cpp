// SPDX-License-Identifier: Apache-2.0
#include "Preferences.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char directory[] = "/.preferences";
/**
 * @brief Validate a bounded Preferences namespace or key component.
 */
static bool validName(const char *name) { return name && *name && strlen(name) <= 15; }
/**
 * @brief Decode an unsigned little-endian 16-bit value.
 */
static uint16_t le16(const uint8_t *p) { return p[0] | uint16_t(p[1]) << 8; }

/**
 * @brief Identify the private Preferences path subtree protected from file AT operations.
 */
bool Preferences::isStoragePath(const char *path) {
    if (!path || *path != '/') return false;
    for (;;) {
        while (*path == '/') ++path;
        if (path[0] == '.' && (path[1] == '/' || path[1] == 0)) { ++path; continue; }
        break;
    }
    return strcspn(path, "/") == sizeof(directory) - 2 &&
           !strncmp(path, directory + 1, sizeof(directory) - 2);
}

/**
 * @brief Read and validate one serialized Preferences record header.
 */
bool Preferences::record(File &file, Record &entry) {
    uint8_t header[20];
    if (file.read(header, sizeof(header)) != sizeof(header)) { _error = LFS_ERR_CORRUPT; return false; }
    memcpy(entry.key, header, 16);
    entry.type = static_cast<PreferenceType>(header[16]);
    entry.size = le16(header + 18);
    if (!memchr(entry.key, 0, 16) || !entry.key[0] || header[17] || entry.type >= PT_INVALID ||
        entry.size > MAX_VALUE_SIZE || (entry.type < PT_STR && entry.size != (1u << (entry.type / 2))) ||
        (entry.type == PT_BLOB && !entry.size)) { _error = LFS_ERR_CORRUPT; return false; }
    return true;
}

/**
 * @brief Open and validate the namespace store header and record count.
 */
bool Preferences::load(File &file, uint16_t &count) {
    file = LittleFS.open(_path);
    if (!file) { _error = LittleFS.lastError(); return false; }
    uint8_t header[8];
    if (file.read(header, sizeof(header)) != sizeof(header) || memcmp(header, "BLP1", 4) ||
        le16(header + 4) != 1 || (count = le16(header + 6)) > MAX_KEYS) {
        _error = LFS_ERR_CORRUPT; return false;
    }
    const size_t end = file.size();
    for (unsigned i = 0; i < count; ++i) {
        Record r;
        if (!record(file, r)) return false;
        size_t pos = file.position();
        if (pos > end || r.size > end - pos || !file.seek(pos + r.size)) {
            _error = LFS_ERR_CORRUPT; return false;
        }
    }
    if (file.position() != end || !file.seek(8)) { _error = LFS_ERR_CORRUPT; return false; }
    _error = 0;
    return true;
}

/**
 * @brief Open a bounded namespace on littlefs; create a missing writable namespace without
 * formatting storage.
 */
bool Preferences::begin(const char *name, bool readOnly, const char *partition) {
    if (_started || !validName(name) || (partition && *partition && strcmp(partition, "lfs"))) {
        _error = LFS_ERR_INVAL; return false;
    }
    if (!LittleFS.begin()) { _error = LittleFS.lastError(); return false; }
    size_t pos = snprintf(_path, sizeof(_path), "%s/", directory);
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(name); *p; ++p)
        pos += snprintf(_path + pos, sizeof(_path) - pos, "%02x", *p);
    strcpy(_path + pos, ".kv");
    File file; uint16_t count;
    if (!load(file, count)) {
        if (_error != LFS_ERR_NOENT || readOnly) return false;
        if (!LittleFS.exists(directory) && !LittleFS.mkdir(directory)) { _error = LittleFS.lastError(); return false; }
        char temporary[68]; snprintf(temporary, sizeof(temporary), "%s.tmp", _path);
        File out = LittleFS.open(temporary, "w");
        const uint8_t header[] = {'B','L','P','1',1,0,0,0};
        bool written = out && out.write(header, sizeof(header)) == sizeof(header);
        bool closed = out && out.close();
        if (!written || !closed || !LittleFS.rename(temporary, _path)) {
            _error = LittleFS.lastError() ? LittleFS.lastError() : LFS_ERR_IO; return false;
        }
    }
    _readOnly = readOnly; _started = true; _error = 0;
    return true;
}

/**
 * @brief Release the active namespace state without unmounting shared littlefs.
 */
void Preferences::end() { _started = false; _readOnly = false; _path[0] = 0; _error = 0; }

/**
 * @brief Locate a key record in an open namespace store.
 */
bool Preferences::find(const char *key, File &file, Record &entry) {
    if (!_started) { _error = LFS_ERR_BADF; return false; }
    if (!validName(key)) { _error = LFS_ERR_INVAL; return false; }
    uint16_t count;
    if (!load(file, count)) return false;
    for (unsigned i = 0; i < count; ++i) {
        if (!record(file, entry)) return false;
        if (!strcmp(key, entry.key)) return true;
        if (!file.seek(file.position() + entry.size)) { _error = LFS_ERR_IO; return false; }
    }
    _error = LFS_ERR_NOENT;
    return false;
}

/**
 * @brief Rewrite the namespace through a temporary file, committing the replacement only after
 * success.
 */
bool Preferences::change(const char *key, PreferenceType type, const uint8_t *value, size_t size) {
    if (!_started || _readOnly) { _error = LFS_ERR_BADF; return false; }
    if (key && !validName(key)) { _error = LFS_ERR_INVAL; return false; }
    File source; uint16_t count;
    if (!load(source, count)) return false;
    bool found = false;
    for (unsigned i = 0; key && i < count; ++i) {
        Record r;
        if (!record(source, r)) return false;
        found |= !strcmp(key, r.key);
        if (!source.seek(source.position() + r.size)) { _error = LFS_ERR_IO; return false; }
    }
    if (key && type == PT_INVALID && !found) { _error = LFS_ERR_NOENT; return false; }
    unsigned next = key ? count - found + (type != PT_INVALID) : 0;
    if (next > MAX_KEYS) { _error = LFS_ERR_NOSPC; return false; }
    char temporary[68]; snprintf(temporary, sizeof(temporary), "%s.tmp", _path);
    File out = LittleFS.open(temporary, "w");
    uint8_t header[8] = {'B','L','P','1',1,0,static_cast<uint8_t>(next),0};
    bool ok = out && out.write(header, sizeof(header)) == sizeof(header) && source.seek(8);
    uint8_t buffer[256];
    for (unsigned i = 0; key && ok && i < count; ++i) {
        uint8_t raw[20];
        ok = source.read(raw, sizeof(raw)) == sizeof(raw);
        if (!ok) break;
        size_t left = le16(raw + 18);
        if (!strcmp(key, reinterpret_cast<const char *>(raw))) {
            ok = source.seek(source.position() + left); continue;
        }
        ok = out.write(raw, sizeof(raw)) == sizeof(raw);
        while (ok && left) {
            size_t n = min(left, sizeof(buffer));
            ok = source.read(buffer, n) == (int)n && out.write(buffer, n) == n;
            left -= n;
        }
    }
    if (key && type != PT_INVALID && ok) {
        uint8_t raw[20] = {};
        memcpy(raw, key, strlen(key)); raw[16] = type; raw[18] = size; raw[19] = size >> 8;
        ok = out.write(raw, sizeof(raw)) == sizeof(raw) && out.write(value, size) == size;
    }
    int error = out.lastError();
    bool closed = out && out.close();
    if (!closed && !error) error = out.lastError();
    bool sourceClosed = source.close();
    if (!ok || !closed || !sourceClosed) { _error = error ? error : LFS_ERR_IO; return false; }
    if (!LittleFS.rename(temporary, _path)) { _error = LittleFS.lastError(); return false; }
    _error = 0;
    return true;
}

/**
 * @brief Validate and store a typed key value; return stored bytes or zero on failure.
 */
size_t Preferences::put(const char *key, PreferenceType type, const void *value, size_t size) {
    if (!validName(key) || type >= PT_INVALID || size > MAX_VALUE_SIZE || (size && !value)) { _error = LFS_ERR_INVAL; return 0; }
    return change(key, type, static_cast<const uint8_t *>(value), size) ? size : 0;
}
/**
 * @brief Store a NUL-terminated string, excluding its terminator from the serialized value.
 */
size_t Preferences::putString(const char *key, const char *value) {
    if (!value) { _error = LFS_ERR_INVAL; return 0; }
    return put(key, PT_STR, value, strlen(value));
}
/**
 * @brief Store an opaque byte value within the configured Preferences value limit.
 */
size_t Preferences::putBytes(const char *key, const void *value, size_t size) {
    if (!size) { _error = LFS_ERR_INVAL; return 0; }
    return put(key, PT_BLOB, value, size);
}
/**
 * @brief Read a matching typed key into the caller's bounded buffer, recording type/size
 * errors.
 */
size_t Preferences::get(const char *key, PreferenceType type, void *value, size_t maxLen) {
    File file; Record r;
    if (!find(key, file, r)) return 0;
    if (r.type != type || maxLen < r.size || (r.size && !value)) { _error = LFS_ERR_INVAL; return 0; }
    if (file.read(static_cast<uint8_t *>(value), r.size) != r.size) { _error = LFS_ERR_IO; return 0; }
    _error = 0;
    return r.size;
}
/**
 * @brief Return the stored key type, or PT_INVALID when absent or invalid.
 */
PreferenceType Preferences::getType(const char *key) { File file; Record r; return find(key, file, r) ? r.type : PT_INVALID; }
/**
 * @brief Report whether the current namespace contains a valid record for the key.
 */
bool Preferences::isKey(const char *key) { return getType(key) != PT_INVALID; }
/**
 * @brief Return a stored blob's byte length, or zero on failure.
 */
size_t Preferences::getBytesLength(const char *key) {
    File file; Record r;
    if (!find(key, file, r)) return 0;
    if (r.type != PT_BLOB) { _error = LFS_ERR_INVAL; return 0; }
    return r.size;
}
/**
 * @brief Copy a stored blob into a sufficiently large caller buffer.
 */
size_t Preferences::getBytes(const char *key, void *value, size_t maxLen) {
    if (!value || !maxLen) return getBytesLength(key);
    return get(key, PT_BLOB, value, maxLen);
}
/**
 * @brief Read a stored string with termination, or return the caller's fallback on failure.
 */
size_t Preferences::getString(const char *key, char *value, size_t maxLen) {
    if (!value || !maxLen) { _error = LFS_ERR_INVAL; return 0; }
    size_t n = get(key, PT_STR, value, maxLen - 1);
    if (_error) return 0;
    value[n] = 0;
    return n + 1;
}
/**
 * @brief Read a stored string with termination, or return the caller's fallback on failure.
 */
String Preferences::getString(const char *key, String fallback) {
    File file; Record r;
    if (!find(key, file, r)) return fallback;
    if (r.type != PT_STR) { _error = LFS_ERR_INVAL; return fallback; }
    char *buffer = static_cast<char *>(malloc(r.size + 1));
    if (!buffer) { _error = LFS_ERR_NOMEM; return fallback; }
    int n = file.read(reinterpret_cast<uint8_t *>(buffer), r.size);
    if (n != r.size) { free(buffer); _error = LFS_ERR_IO; return fallback; }
    buffer[r.size] = 0;
    String result(buffer, r.size); free(buffer);
    if (result.length() != r.size) { _error = LFS_ERR_NOMEM; return fallback; }
    _error = 0;
    return result;
}
/**
 * @brief Remove one validated key from the writable namespace through a replacement file.
 */
bool Preferences::remove(const char *key) {
    if (!validName(key)) { _error = LFS_ERR_INVAL; return false; }
    return change(key, PT_INVALID, nullptr, 0);
}
/**
 * @brief Replace the writable namespace with an empty store.
 */
bool Preferences::clear() { return change(nullptr, PT_INVALID, nullptr, 0); }
/**
 * @brief Return remaining logical key slots in the current namespace, not free Flash bytes.
 */
size_t Preferences::freeEntries() {
    if (!_started) { _error = LFS_ERR_BADF; return 0; }
    File file; uint16_t count;
    return load(file, count) ? MAX_KEYS - count : 0;
}

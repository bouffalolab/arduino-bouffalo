// Public API adapted from Espressif Arduino Preferences 2.0.9.
// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <Arduino.h>
#include <LittleFS.h>

enum PreferenceType {
    PT_I8, PT_U8, PT_I16, PT_U16, PT_I32, PT_U32, PT_I64, PT_U64, PT_STR, PT_BLOB, PT_INVALID
};

// Serialize all Preferences and LittleFS calls in task context. No internal locks.
class Preferences {
public:
    static constexpr size_t MAX_KEYS = 64;
    static constexpr size_t MAX_VALUE_SIZE = 1023;
    /**
     * @brief Open a bounded namespace on littlefs; create a missing writable namespace without
     * formatting storage.
     *
     * @param name Nonempty namespace, at most 15 bytes; encoded into a private storage path.
     * @param readOnly Refuse writes and creation when true.
     * @param partition nullptr, an empty string or lfs selects the sole supported partition.
     * @return true on success; false on invalid state, missing read-only storage or filesystem
     *   error.
     */
    bool begin(const char *name, bool readOnly = false, const char *partition = nullptr);
    /**
     * @brief Release the active namespace state without unmounting shared littlefs.
     */
    void end();
    /**
     * @brief Replace the writable namespace with an empty store.
     */
    bool clear();
    /**
     * @brief Remove one validated key from the writable namespace through a replacement file.
     */
    bool remove(const char *key);
    /**
     * @brief Report whether the current namespace contains a valid record for the key.
     */
    bool isKey(const char *key);
    /**
     * @brief Return the stored key type, or PT_INVALID when absent or invalid.
     */
    PreferenceType getType(const char *key);
    /**
     * @brief Return remaining logical key slots in the current namespace, not free Flash
     * bytes.
     */
    size_t freeEntries(); // Remaining logical key slots in this namespace, not NVS statistics.
    /**
     * @brief Return the most recent operation error for this object.
     */
    int lastError() const { return _error; }
    /**
     * @brief Identify the private Preferences path subtree protected from file AT operations.
     */
    static bool isStoragePath(const char *path);

/**
 * @brief Generate typed integer accessors using the shared little-endian record format.
 * @param Name Suffix of the generated put/get methods.
 * @param Type Integer type encoded by the accessor.
 * @param Tag Matching PreferenceType record tag.
 * @note put methods return stored bytes or zero; get methods return the supplied fallback
 * on an absent key, type mismatch or read failure. Inspect lastError() to distinguish errors.
 */
#define PREFERENCES_INTEGER(Name, Type, Tag) \
    size_t put##Name(const char *key, Type value) { return putNumber(key, Tag, value); } \
    Type get##Name(const char *key, Type fallback = 0) { return getNumber(key, Tag, fallback); }
    PREFERENCES_INTEGER(Char, int8_t, PT_I8)
    PREFERENCES_INTEGER(UChar, uint8_t, PT_U8)
    PREFERENCES_INTEGER(Short, int16_t, PT_I16)
    PREFERENCES_INTEGER(UShort, uint16_t, PT_U16)
    PREFERENCES_INTEGER(Int, int32_t, PT_I32)
    PREFERENCES_INTEGER(UInt, uint32_t, PT_U32)
    PREFERENCES_INTEGER(Long64, int64_t, PT_I64)
    PREFERENCES_INTEGER(ULong64, uint64_t, PT_U64)
#undef PREFERENCES_INTEGER
    /**
     * @brief Store a signed 32-bit value using the compatible Int representation.
     */
    size_t putLong(const char *key, int32_t v) { return putInt(key, v); }
    /**
     * @brief Store an unsigned 32-bit value using the compatible UInt representation.
     */
    size_t putULong(const char *key, uint32_t v) { return putUInt(key, v); }
    /**
     * @brief Read a signed 32-bit value or the supplied fallback.
     */
    int32_t getLong(const char *key, int32_t v = 0) { return getInt(key, v); }
    /**
     * @brief Read an unsigned 32-bit value or the supplied fallback.
     */
    uint32_t getULong(const char *key, uint32_t v = 0) { return getUInt(key, v); }
    /**
     * @brief Store a boolean as the compatible one-byte unsigned representation.
     */
    size_t putBool(const char *key, bool v) { return putUChar(key, v ? 1 : 0); }
    /**
     * @brief Read the boolean representation or return the supplied fallback.
     */
    bool getBool(const char *key, bool v = false) { return getUChar(key, v ? 1 : 0) == 1; }
    /**
     * @brief Store the floating-point object's bytes as a blob.
     */
    size_t putFloat(const char *key, float v) { return putBytes(key, &v, sizeof(v)); }
    /**
     * @brief Store the double-precision object's bytes as a blob.
     */
    size_t putDouble(const char *key, double v) { return putBytes(key, &v, sizeof(v)); }
    /**
     * @brief Read a correctly sized float blob or return the supplied fallback.
     */
    float getFloat(const char *key, float v = NAN) { return getFloating(key, v); }
    /**
     * @brief Read a correctly sized double blob or return the supplied fallback.
     */
    double getDouble(const char *key, double v = NAN) { return getFloating(key, v); }
    /**
     * @brief Store a NUL-terminated string, excluding its terminator from the serialized
     * value.
     */
    size_t putString(const char *key, const char *value);
    /**
     * @brief Store a NUL-terminated string, excluding its terminator from the serialized
     * value.
     */
    size_t putString(const char *key, String value) { return putString(key, value.c_str()); }
    /**
     * @brief Store an opaque byte value within the configured Preferences value limit.
     *
     * @param key Nonempty key of at most 15 bytes.
     * @param value Caller-owned bytes copied before the operation returns.
     * @param size Blob length from 1 through MAX_VALUE_SIZE.
     * @return Stored byte count, or zero on failure; inspect lastError().
     */
    size_t putBytes(const char *key, const void *value, size_t size);
    /**
     * @brief Read a stored string with termination, or return the caller's fallback on
     * failure.
     */
    String getString(const char *key, String fallback = String());
    /**
     * @brief Read a stored string with termination, or return the caller's fallback on
     * failure.
     */
    size_t getString(const char *key, char *value, size_t maxLen);
    /**
     * @brief Return a stored blob's byte length, or zero on failure.
     */
    size_t getBytesLength(const char *key);
    /**
     * @brief Copy a stored blob into a sufficiently large caller buffer.
     */
    size_t getBytes(const char *key, void *value, size_t maxLen);

private:
    struct Record { char key[16]; PreferenceType type; uint16_t size; };
    char _path[64] = {};
    bool _started = false, _readOnly = false;
    int _error = 0;
    /**
     * @brief Open and validate the namespace store header and record count.
     */
    bool load(File &file, uint16_t &count);
    /**
     * @brief Read and validate one serialized Preferences record header.
     */
    bool record(File &file, Record &entry);
    /**
     * @brief Locate a key record in an open namespace store.
     */
    bool find(const char *key, File &file, Record &entry);
    /**
     * @brief Rewrite the namespace through a temporary file, committing the replacement only
     * after success.
     */
    bool change(const char *key, PreferenceType type, const uint8_t *value, size_t size);
    /**
     * @brief Validate and store a typed key value; return stored bytes or zero on failure.
     */
    size_t put(const char *key, PreferenceType type, const void *value, size_t size);
    /**
     * @brief Read a matching typed key into the caller's bounded buffer, recording type/size
     * errors.
     */
    size_t get(const char *key, PreferenceType type, void *value, size_t maxLen);
    /**
     * @brief Decode a floating-point blob only when its size matches the requested type.
     */
    template<typename T> T getFloating(const char *key, T fallback) {
        T value = fallback;
        if (get(key, PT_BLOB, &value, sizeof(value)) != sizeof(value)) {
            if (!_error) _error = LFS_ERR_INVAL;
            return fallback;
        }
        return value;
    }
    /**
     * @brief Encode a typed integer in little-endian order before storing it.
     */
    template<typename T> size_t putNumber(const char *key, PreferenceType type, T value) {
        uint8_t bytes[sizeof(T)];
        for (size_t i = 0; i < sizeof(T); ++i) bytes[i] = uint64_t(value) >> (8 * i);
        return put(key, type, bytes, sizeof(bytes));
    }
    /**
     * @brief Decode a typed little-endian integer or return the supplied fallback.
     */
    template<typename T> T getNumber(const char *key, PreferenceType type, T fallback) {
        uint8_t bytes[sizeof(T)];
        if (get(key, type, bytes, sizeof(bytes)) != sizeof(bytes)) return fallback;
        uint64_t value = 0;
        for (size_t i = 0; i < sizeof(T); ++i) value |= uint64_t(bytes[i]) << (8 * i);
        return static_cast<T>(value);
    }
};

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
    bool begin(const char *name, bool readOnly = false, const char *partition = nullptr);
    void end();
    bool clear();
    bool remove(const char *key);
    bool isKey(const char *key);
    PreferenceType getType(const char *key);
    size_t freeEntries(); // Remaining logical key slots in this namespace, not NVS statistics.
    int lastError() const { return _error; }
    static bool isStoragePath(const char *path);

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
    size_t putLong(const char *key, int32_t v) { return putInt(key, v); }
    size_t putULong(const char *key, uint32_t v) { return putUInt(key, v); }
    int32_t getLong(const char *key, int32_t v = 0) { return getInt(key, v); }
    uint32_t getULong(const char *key, uint32_t v = 0) { return getUInt(key, v); }
    size_t putBool(const char *key, bool v) { return putUChar(key, v ? 1 : 0); }
    bool getBool(const char *key, bool v = false) { return getUChar(key, v ? 1 : 0) == 1; }
    size_t putFloat(const char *key, float v) { return putBytes(key, &v, sizeof(v)); }
    size_t putDouble(const char *key, double v) { return putBytes(key, &v, sizeof(v)); }
    float getFloat(const char *key, float v = NAN) { return getFloating(key, v); }
    double getDouble(const char *key, double v = NAN) { return getFloating(key, v); }
    size_t putString(const char *key, const char *value);
    size_t putString(const char *key, String value) { return putString(key, value.c_str()); }
    size_t putBytes(const char *key, const void *value, size_t size);
    String getString(const char *key, String fallback = String());
    size_t getString(const char *key, char *value, size_t maxLen);
    size_t getBytesLength(const char *key);
    size_t getBytes(const char *key, void *value, size_t maxLen);

private:
    struct Record { char key[16]; PreferenceType type; uint16_t size; };
    char _path[64] = {};
    bool _started = false, _readOnly = false;
    int _error = 0;
    bool load(File &file, uint16_t &count);
    bool record(File &file, Record &entry);
    bool find(const char *key, File &file, Record &entry);
    bool change(const char *key, PreferenceType type, const uint8_t *value, size_t size);
    size_t put(const char *key, PreferenceType type, const void *value, size_t size);
    size_t get(const char *key, PreferenceType type, void *value, size_t maxLen);
    template<typename T> T getFloating(const char *key, T fallback) {
        T value = fallback;
        if (get(key, PT_BLOB, &value, sizeof(value)) != sizeof(value)) {
            if (!_error) _error = LFS_ERR_INVAL;
            return fallback;
        }
        return value;
    }
    template<typename T> size_t putNumber(const char *key, PreferenceType type, T value) {
        uint8_t bytes[sizeof(T)];
        for (size_t i = 0; i < sizeof(T); ++i) bytes[i] = uint64_t(value) >> (8 * i);
        return put(key, type, bytes, sizeof(bytes));
    }
    template<typename T> T getNumber(const char *key, PreferenceType type, T fallback) {
        uint8_t bytes[sizeof(T)];
        if (get(key, type, bytes, sizeof(bytes)) != sizeof(bytes)) return fallback;
        uint64_t value = 0;
        for (size_t i = 0; i < sizeof(T); ++i) value |= uint64_t(bytes[i]) << (8 * i);
        return static_cast<T>(value);
    }
};

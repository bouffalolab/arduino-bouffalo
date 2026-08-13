#ifndef BL616CL_ESP32_COMPAT_PREFERENCES_H_
#define BL616CL_ESP32_COMPAT_PREFERENCES_H_

#include <Arduino.h>
#include <WString.h>

enum PreferenceType {
    PT_I8 = 0,
    PT_U8,
    PT_I16,
    PT_U16,
    PT_I32,
    PT_U32,
    PT_I64,
    PT_U64,
    PT_STR,
    PT_BLOB,
    PT_INVALID
};

class Preferences {
public:
    bool begin(const char *name, bool readOnly = false) { (void)name; (void)readOnly; return false; }
    bool begin(const char *name, bool readOnly, const char *partition) { (void)name; (void)readOnly; (void)partition; return false; }
    void end() {}
    bool clear() { return false; }
    bool remove(const char *key) { (void)key; return false; }

    size_t putChar(const char *key, int8_t value) { (void)key; (void)value; return 0; }
    size_t putUChar(const char *key, uint8_t value) { (void)key; (void)value; return 0; }
    size_t putShort(const char *key, int16_t value) { (void)key; (void)value; return 0; }
    size_t putUShort(const char *key, uint16_t value) { (void)key; (void)value; return 0; }
    size_t putInt(const char *key, int32_t value) { (void)key; (void)value; return 0; }
    size_t putUInt(const char *key, uint32_t value) { (void)key; (void)value; return 0; }
    size_t putLong64(const char *key, int64_t value) { (void)key; (void)value; return 0; }
    size_t putULong64(const char *key, uint64_t value) { (void)key; (void)value; return 0; }
    size_t putString(const char *key, const char *value) { (void)key; (void)value; return 0; }
    size_t putBytes(const char *key, const void *value, size_t len) { (void)key; (void)value; (void)len; return 0; }

    PreferenceType getType(const char *key) { (void)key; return PreferenceType::PT_INVALID; }
    int8_t getChar(const char *key, int8_t defaultValue = 0) { (void)key; return defaultValue; }
    uint8_t getUChar(const char *key, uint8_t defaultValue = 0) { (void)key; return defaultValue; }
    int16_t getShort(const char *key, int16_t defaultValue = 0) { (void)key; return defaultValue; }
    uint16_t getUShort(const char *key, uint16_t defaultValue = 0) { (void)key; return defaultValue; }
    int32_t getInt(const char *key, int32_t defaultValue = 0) { (void)key; return defaultValue; }
    uint32_t getUInt(const char *key, uint32_t defaultValue = 0) { (void)key; return defaultValue; }
    int64_t getLong64(const char *key, int64_t defaultValue = 0) { (void)key; return defaultValue; }
    uint64_t getULong64(const char *key, uint64_t defaultValue = 0) { (void)key; return defaultValue; }
    String getString(const char *key, const char *defaultValue = "") { (void)key; return String(defaultValue); }
    size_t getBytesLength(const char *key) { (void)key; return 0; }
    size_t getBytes(const char *key, void *buffer, size_t maxLen) { (void)key; (void)buffer; (void)maxLen; return 0; }
    size_t freeEntries() { return 0; }
};

#endif

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
    Preferences() : _read_only(false) {}

    bool begin(const char *name, bool readOnly = false);
    bool begin(const char *name, bool readOnly, const char *partition);
    void end();
    bool clear();
    bool remove(const char *key);

    size_t putChar(const char *key, int8_t value);
    size_t putUChar(const char *key, uint8_t value);
    size_t putShort(const char *key, int16_t value);
    size_t putUShort(const char *key, uint16_t value);
    size_t putInt(const char *key, int32_t value);
    size_t putUInt(const char *key, uint32_t value);
    size_t putLong64(const char *key, int64_t value);
    size_t putULong64(const char *key, uint64_t value);
    size_t putString(const char *key, const char *value);
    size_t putBytes(const char *key, const void *value, size_t len);

    PreferenceType getType(const char *key);
    int8_t getChar(const char *key, int8_t defaultValue = 0);
    uint8_t getUChar(const char *key, uint8_t defaultValue = 0);
    int16_t getShort(const char *key, int16_t defaultValue = 0);
    uint16_t getUShort(const char *key, uint16_t defaultValue = 0);
    int32_t getInt(const char *key, int32_t defaultValue = 0);
    uint32_t getUInt(const char *key, uint32_t defaultValue = 0);
    int64_t getLong64(const char *key, int64_t defaultValue = 0);
    uint64_t getULong64(const char *key, uint64_t defaultValue = 0);
    String getString(const char *key, const char *defaultValue = "");
    size_t getBytesLength(const char *key);
    size_t getBytes(const char *key, void *buffer, size_t maxLen);
    size_t freeEntries();

private:
    String makeKey(const char *key) const;
    size_t putValue(const char *key, PreferenceType type,
                    const void *value, size_t len);
    bool getValue(const char *key, PreferenceType type,
                  void *value, size_t *len);

    String _ns;
    bool _read_only;
};

#endif

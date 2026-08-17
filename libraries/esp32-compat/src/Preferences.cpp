#include "Preferences.h"
#include "lfs/easyflash.h"

extern "C" {
#include "lfs/bflb_mtd.h"
}

#include <stdlib.h>
#include <string.h>

/* Blob layout: [type u8][len u32 little-endian][payload] */
static const size_t VALUE_HEADER = 5;

static bool s_ef_inited = false;

static bool ensure_easyflash()
{
    if (s_ef_inited) {
        return true;
    }
    /* EasyFlash's lfs_xip_init opens the PSM partition via MTD; MTD needs the
     * boot2 partition table loaded first (normally done by SPIFFS.begin, but
     * Preferences must work standalone). bflb_boot2_init is re-entrant. */
    bflb_mtd_init();
    if (easyflash_init() != EF_NO_ERR) {
        return false;
    }
    s_ef_inited = true;
    return true;
}

static void put_le32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)((v >> 8) & 0xFF);
    dst[2] = (uint8_t)((v >> 16) & 0xFF);
    dst[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

/* Read the full stored blob (header + payload) into a heap buffer. */
static uint8_t *read_blob(const char *key, size_t *total)
{
    size_t saved = 0;
    ef_get_env_blob(key, nullptr, 0, &saved);
    if (saved < VALUE_HEADER) {
        return nullptr;
    }
    uint8_t *blob = (uint8_t *)malloc(saved);
    if (blob == nullptr) {
        return nullptr;
    }
    size_t got = ef_get_env_blob(key, blob, saved, &saved);
    if (got == 0 || saved < VALUE_HEADER) {
        free(blob);
        return nullptr;
    }
    *total = saved;
    return blob;
}

static char *dup_key(const char *key)
{
    size_t len = strlen(key);
    char *copy = (char *)malloc(len + 1);
    if (copy == nullptr) {
        return nullptr;
    }
    memcpy(copy, key, len + 1);
    return copy;
}

bool Preferences::begin(const char *name, bool readOnly)
{
    return begin(name, readOnly, nullptr);
}

bool Preferences::begin(const char *name, bool readOnly, const char *partition)
{
    (void)partition;
    if (name == nullptr || name[0] == '\0' || strlen(name) > 15) {
        return false;
    }
    if (!ensure_easyflash()) {
        return false;
    }
    _ns = name;
    _read_only = readOnly;
    return true;
}

void Preferences::end()
{
    _ns = String();
}

String Preferences::makeKey(const char *key) const
{
    if (key == nullptr || _ns.length() == 0) {
        return String();
    }
    return _ns + "/" + key;
}

size_t Preferences::putValue(const char *key, PreferenceType type,
                             const void *value, size_t len)
{
    if (_read_only || _ns.length() == 0 || value == nullptr || len > 0xFFFFFF) {
        return 0;
    }
    String k = makeKey(key);
    if (k.length() == 0 || k.length() >= EF_ENV_NAME_MAX) {
        return 0;
    }
    size_t total = VALUE_HEADER + len;
    uint8_t *blob = (uint8_t *)malloc(total);
    if (blob == nullptr) {
        return 0;
    }
    blob[0] = (uint8_t)type;
    put_le32(blob + 1, (uint32_t)len);
    memcpy(blob + VALUE_HEADER, value, len);
    EfErrCode rc = ef_set_env_blob(k.c_str(), blob, total);
    free(blob);
    return rc == EF_NO_ERR ? len : 0;
}

bool Preferences::getValue(const char *key, PreferenceType type,
                           void *value, size_t *len)
{
    if (_ns.length() == 0 || value == nullptr || len == nullptr) {
        return false;
    }
    String k = makeKey(key);
    if (k.length() == 0) {
        return false;
    }
    size_t total = 0;
    uint8_t *blob = read_blob(k.c_str(), &total);
    if (blob == nullptr) {
        return false;
    }
    if (blob[0] != (uint8_t)type) {
        free(blob);
        return false;
    }
    size_t payload = get_le32(blob + 1);
    if (payload != *len || total != VALUE_HEADER + payload) {
        free(blob);
        return false;
    }
    memcpy(value, blob + VALUE_HEADER, payload);
    *len = payload;
    free(blob);
    return true;
}

size_t Preferences::putChar(const char *key, int8_t value)
{ return putValue(key, PT_I8, &value, 1); }
size_t Preferences::putUChar(const char *key, uint8_t value)
{ return putValue(key, PT_U8, &value, 1); }
size_t Preferences::putShort(const char *key, int16_t value)
{ return putValue(key, PT_I16, &value, 2); }
size_t Preferences::putUShort(const char *key, uint16_t value)
{ return putValue(key, PT_U16, &value, 2); }
size_t Preferences::putInt(const char *key, int32_t value)
{ return putValue(key, PT_I32, &value, 4); }
size_t Preferences::putUInt(const char *key, uint32_t value)
{ return putValue(key, PT_U32, &value, 4); }
size_t Preferences::putLong64(const char *key, int64_t value)
{ return putValue(key, PT_I64, &value, 8); }
size_t Preferences::putULong64(const char *key, uint64_t value)
{ return putValue(key, PT_U64, &value, 8); }

size_t Preferences::putString(const char *key, const char *value)
{
    if (value == nullptr) {
        return 0;
    }
    return putValue(key, PT_STR, value, strlen(value));
}

size_t Preferences::putBytes(const char *key, const void *value, size_t len)
{
    return putValue(key, PT_BLOB, value, len);
}

PreferenceType Preferences::getType(const char *key)
{
    if (_ns.length() == 0) {
        return PT_INVALID;
    }
    String k = makeKey(key);
    if (k.length() == 0) {
        return PT_INVALID;
    }
    size_t total = 0;
    uint8_t *blob = read_blob(k.c_str(), &total);
    if (blob == nullptr) {
        return PT_INVALID;
    }
    uint8_t type = blob[0];
    free(blob);
    return (type <= PT_BLOB) ? (PreferenceType)type : PT_INVALID;
}

int8_t Preferences::getChar(const char *key, int8_t defaultValue)
{ int8_t v; size_t l = 1; return getValue(key, PT_I8, &v, &l) ? v : defaultValue; }
uint8_t Preferences::getUChar(const char *key, uint8_t defaultValue)
{ uint8_t v; size_t l = 1; return getValue(key, PT_U8, &v, &l) ? v : defaultValue; }
int16_t Preferences::getShort(const char *key, int16_t defaultValue)
{ int16_t v; size_t l = 2; return getValue(key, PT_I16, &v, &l) ? v : defaultValue; }
uint16_t Preferences::getUShort(const char *key, uint16_t defaultValue)
{ uint16_t v; size_t l = 2; return getValue(key, PT_U16, &v, &l) ? v : defaultValue; }
int32_t Preferences::getInt(const char *key, int32_t defaultValue)
{ int32_t v; size_t l = 4; return getValue(key, PT_I32, &v, &l) ? v : defaultValue; }
uint32_t Preferences::getUInt(const char *key, uint32_t defaultValue)
{ uint32_t v; size_t l = 4; return getValue(key, PT_U32, &v, &l) ? v : defaultValue; }
int64_t Preferences::getLong64(const char *key, int64_t defaultValue)
{ int64_t v; size_t l = 8; return getValue(key, PT_I64, &v, &l) ? v : defaultValue; }
uint64_t Preferences::getULong64(const char *key, uint64_t defaultValue)
{ uint64_t v; size_t l = 8; return getValue(key, PT_U64, &v, &l) ? v : defaultValue; }

String Preferences::getString(const char *key, const char *defaultValue)
{
    if (_ns.length() == 0) {
        return String(defaultValue);
    }
    String k = makeKey(key);
    if (k.length() == 0) {
        return String(defaultValue);
    }
    size_t total = 0;
    uint8_t *blob = read_blob(k.c_str(), &total);
    if (blob == nullptr || blob[0] != PT_STR) {
        free(blob);
        return String(defaultValue);
    }
    size_t len = get_le32(blob + 1);
    if (total != VALUE_HEADER + len) {
        free(blob);
        return String(defaultValue);
    }
    char *tmp = (char *)malloc(len + 1);
    if (tmp == nullptr) {
        free(blob);
        return String(defaultValue);
    }
    memcpy(tmp, blob + VALUE_HEADER, len);
    tmp[len] = '\0';
    String result(tmp);
    free(tmp);
    free(blob);
    return result;
}

size_t Preferences::getBytesLength(const char *key)
{
    if (_ns.length() == 0) {
        return 0;
    }
    String k = makeKey(key);
    if (k.length() == 0) {
        return 0;
    }
    size_t total = 0;
    uint8_t *blob = read_blob(k.c_str(), &total);
    if (blob == nullptr || blob[0] != PT_BLOB) {
        free(blob);
        return 0;
    }
    size_t payload = get_le32(blob + 1);
    free(blob);
    return total == VALUE_HEADER + payload ? payload : 0;
}

size_t Preferences::getBytes(const char *key, void *buffer, size_t maxLen)
{
    if (_ns.length() == 0 || buffer == nullptr) {
        return 0;
    }
    String k = makeKey(key);
    if (k.length() == 0) {
        return 0;
    }
    size_t total = 0;
    uint8_t *blob = read_blob(k.c_str(), &total);
    if (blob == nullptr || blob[0] != PT_BLOB) {
        free(blob);
        return 0;
    }
    size_t payload = get_le32(blob + 1);
    if (total != VALUE_HEADER + payload || payload > maxLen) {
        free(blob);
        return 0;
    }
    memcpy(buffer, blob + VALUE_HEADER, payload);
    free(blob);
    return payload;
}

struct ns_scan_ctx {
    const char *ns;
    size_t ns_len;
    char **keys;
    size_t count;
    size_t cap;
};

static bool ns_match(const struct ns_scan_ctx *ctx, const char *key)
{
    return strncmp(key, ctx->ns, ctx->ns_len) == 0 && key[ctx->ns_len] == '/';
}

static EfErrCode collect_cb(const char *key, void *arg)
{
    struct ns_scan_ctx *ctx = (struct ns_scan_ctx *)arg;
    if (!ns_match(ctx, key)) {
        return EF_NO_ERR;
    }
    if (ctx->count == ctx->cap) {
        size_t new_cap = ctx->cap ? ctx->cap * 2 : 8;
        char **new_keys = (char **)realloc(ctx->keys, new_cap * sizeof(char *));
        if (new_keys == nullptr) {
            return EF_ENV_FULL;
        }
        ctx->keys = new_keys;
        ctx->cap = new_cap;
    }
    char *copy = dup_key(key);
    if (copy == nullptr) {
        return EF_ENV_FULL;
    }
    ctx->keys[ctx->count++] = copy;
    return EF_NO_ERR;
}

static EfErrCode count_cb(const char *key, void *arg)
{
    struct ns_scan_ctx *ctx = (struct ns_scan_ctx *)arg;
    if (ns_match(ctx, key)) {
        ctx->count++;
    }
    return EF_NO_ERR;
}

bool Preferences::clear()
{
    if (_ns.length() == 0) {
        return false;
    }
    struct ns_scan_ctx ctx = { _ns.c_str(), _ns.length(), nullptr, 0, 0 };
    ef_foreach_env(collect_cb, &ctx);
    /* Deleting during iteration is unsafe in LittleFS; delete after collect. */
    for (size_t i = 0; i < ctx.count; i++) {
        ef_del_env(ctx.keys[i]);
        free(ctx.keys[i]);
    }
    free(ctx.keys);
    return true;
}

bool Preferences::remove(const char *key)
{
    if (_ns.length() == 0 || key == nullptr) {
        return false;
    }
    String k = makeKey(key);
    if (k.length() == 0) {
        return false;
    }
    return ef_del_env(k.c_str()) == EF_NO_ERR;
}

size_t Preferences::freeEntries()
{
    if (_ns.length() == 0) {
        return 0;
    }
    struct ns_scan_ctx ctx = { _ns.c_str(), _ns.length(), nullptr, 0, 0 };
    ef_foreach_env(count_cb, &ctx);
    return ctx.count;
}

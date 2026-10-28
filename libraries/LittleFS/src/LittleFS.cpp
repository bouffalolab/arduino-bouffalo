#include "LittleFS.h"
#include <string.h>
#include <new>
extern "C" {
#include "bflb_flash.h"
#include "bflb_boot2.h"
}

LittleFSClass LittleFS;

struct fs::File::Handle {
    LittleFSClass *owner;
    lfs_file_t file = {};
    unsigned refs = 1;
    bool open = false;
    int error = 0;
    int flags = 0;
    explicit Handle(LittleFSClass *fs) : owner(fs) {}
};

// This is a partition-scoped NOR adapter, not an SDK MTD/VFS or a public device API.
static int filesystemPartition(uint32_t &address, uint32_t &capacity) {
    address = capacity = 0;
    if (bflb_boot2_init() != 0) return LFS_ERR_IO;
    const auto *table = bflb_boot2_get_pt_config();
    if (table->pt_table.entryCnt > PT_ENTRY_MAX) return LFS_ERR_INVAL;
    unsigned found = 0;
    for (unsigned i = 0; i < table->pt_table.entryCnt; ++i) {
        const auto &p = table->pt_entries[i];
        if (memcmp(p.name, "lfs", 4)) continue;
        if (p.device || p.active_index || p.max_len[1]) return LFS_ERR_INVAL;
        address = p.start_address[0];
        capacity = p.max_len[0];
        ++found;
    }
    if (found != 1) return LFS_ERR_NOENT;
    const uint32_t flashSize = bflb_flash_get_size();
    if (address < 0x10000 || address % 4096 || capacity < 8192 || capacity % 4096 ||
        address >= flashSize || capacity > flashSize - address) return LFS_ERR_INVAL;
    for (unsigned i = 0; i < table->pt_table.entryCnt; ++i) {
        const auto &p = table->pt_entries[i];
        if (!memcmp(p.name, "lfs", 4) || p.device) continue;
        for (unsigned n = 0; n < 2; ++n) {
            if (!p.max_len[n]) continue;
            if (p.start_address[n] >= flashSize ||
                p.max_len[n] > flashSize - p.start_address[n]) return LFS_ERR_INVAL;
            if (p.start_address[n] < address + capacity &&
                p.start_address[n] + p.max_len[n] > address) return LFS_ERR_INVAL;
        }
    }
    return 0;
}

static bool validAccess(const lfs_config *c, lfs_block_t b, lfs_off_t o, lfs_size_t n) {
    return b < c->block_count && o < c->block_size && n &&
           n <= c->block_size - o && o % c->prog_size == 0 && n % c->prog_size == 0;
}

bool LittleFSClass::configure() {
    _error = filesystemPartition(_address, _capacity);
    if (_error) return false;
    _config = {};
    _config.context = &_address;
    _config.read = [](const lfs_config *c, lfs_block_t b, lfs_off_t o, void *p, lfs_size_t n) -> int {
        if (!p || !validAccess(c, b, o, n)) return LFS_ERR_INVAL;
        uint32_t address = *static_cast<const uint32_t *>(c->context) + b * c->block_size + o;
        return bflb_flash_read(address, static_cast<uint8_t *>(p), n) ? LFS_ERR_IO : 0;
    };
    _config.prog = [](const lfs_config *c, lfs_block_t b, lfs_off_t o, const void *p, lfs_size_t n) -> int {
        if (!p || !validAccess(c, b, o, n)) return LFS_ERR_INVAL;
        uint32_t address = *static_cast<const uint32_t *>(c->context) + b * c->block_size + o;
        // Never pass an XIP source pointer while the SDK suspends XIP.
        uint8_t page[256];
        for (lfs_size_t pos = 0; pos < n; pos += sizeof(page)) {
            memcpy(page, static_cast<const uint8_t *>(p) + pos, sizeof(page));
            if (bflb_flash_write(address + pos, page, sizeof(page))) return LFS_ERR_IO;
        }
        return 0;
    };
    _config.erase = [](const lfs_config *c, lfs_block_t b) -> int {
        if (b >= c->block_count) return LFS_ERR_INVAL;
        uint32_t address = *static_cast<const uint32_t *>(c->context) + b * c->block_size;
        return bflb_flash_erase(address, c->block_size) ? LFS_ERR_IO : 0;
    };
    // SDK operations block until complete and restore XIP before returning.
    _config.sync = [](const lfs_config *) { return 0; };
    _config.read_size = _config.prog_size = _config.cache_size = 256;
    _config.block_size = 4096;
    _config.block_count = _capacity / _config.block_size;
    _config.block_cycles = 500;
    _config.lookahead_size = 16;
    return true;
}

bool LittleFSClass::begin() {
    if (_mounted) { _error = 0; return true; }
    if (!configure()) return false;
    _error = lfs_mount(&_fs, &_config);
    _mounted = _error == 0;
    return _mounted;
}

bool LittleFSClass::format() {
    if (_mounted) { _error = LFS_ERR_BADF; return false; }
    if (!configure()) return false;
    _error = lfs_format(&_fs, &_config);
    return _error == 0;
}

bool LittleFSClass::end() {
    if (_openFiles) { _error = LFS_ERR_BADF; return false; }
    _error = _mounted ? lfs_unmount(&_fs) : 0;
    if (_error) return false;
    _mounted = false;
    _address = _capacity = 0;
    return true;
}

bool LittleFSClass::pathValid(const char *p) {
    _error = 0;
    if (!_mounted) { _error = LFS_ERR_BADF; return false; }
    if (!p || p[0] != '/' || !p[1]) { _error = LFS_ERR_INVAL; return false; }
    // Keep all names rooted; reject parent traversal rather than aliasing it.
    for (const char *s = p + 1; *s; ++s) {
        if ((s == p + 1 || s[-1] == '/') && s[0] == '.' &&
            s[1] == '.' && (s[2] == '/' || s[2] == 0)) {
            _error = LFS_ERR_INVAL; return false;
        }
    }
    return true;
}

File LittleFSClass::open(const char *path, const char *mode) {
    File result;
    if (!pathValid(path)) return result;
    int flags;
    if (!mode) { _error = LFS_ERR_INVAL; return result; }
    if (!strcmp(mode, "r")) flags = LFS_O_RDONLY;
    else if (!strcmp(mode, "r+")) flags = LFS_O_RDWR;
    else if (!strcmp(mode, "w")) flags = LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;
    else if (!strcmp(mode, "w+")) flags = LFS_O_RDWR | LFS_O_CREAT | LFS_O_TRUNC;
    else if (!strcmp(mode, "a")) flags = LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND;
    else if (!strcmp(mode, "a+")) flags = LFS_O_RDWR | LFS_O_CREAT | LFS_O_APPEND;
    else { _error = LFS_ERR_INVAL; return result; }
    auto *h = new(std::nothrow) fs::File::Handle(this);
    if (!h) { _error = LFS_ERR_NOMEM; return result; }
    _error = lfs_file_open(&_fs, &h->file, path, flags);
    if (_error) { delete h; return result; }
    h->flags = flags;
    h->open = true;
    result._handle = h;
    ++_openFiles;
    return result;
}

bool LittleFSClass::exists(const char *path) {
    if (!pathValid(path)) return false;
    lfs_info info;
    _error = lfs_stat(&_fs, path, &info);
    return _error == 0;
}
bool LittleFSClass::remove(const char *path) {
    if (!pathValid(path)) return false;
    lfs_info info;
    _error = lfs_stat(&_fs, path, &info);
    if (_error) return false;
    if (info.type != LFS_TYPE_REG) { _error = LFS_ERR_ISDIR; return false; }
    _error = lfs_remove(&_fs, path);
    return _error == 0;
}
bool LittleFSClass::rmdir(const char *path) {
    if (!pathValid(path)) return false;
    lfs_info info;
    _error = lfs_stat(&_fs, path, &info);
    if (_error) return false;
    if (info.type != LFS_TYPE_DIR) { _error = LFS_ERR_NOTDIR; return false; }
    _error = lfs_remove(&_fs, path);
    return _error == 0;
}
bool LittleFSClass::rename(const char *from, const char *to) {
    if (!pathValid(from) || !pathValid(to)) return false;
    _error = lfs_rename(&_fs, from, to);
    return _error == 0;
}
bool LittleFSClass::mkdir(const char *path) {
    if (!pathValid(path)) return false;
    _error = lfs_mkdir(&_fs, path);
    return _error == 0;
}
size_t LittleFSClass::totalBytes() const { return _mounted ? _capacity : 0; }
size_t LittleFSClass::usedBytes() {
    if (!_mounted) { _error = LFS_ERR_BADF; return 0; }
    lfs_ssize_t n = lfs_fs_size(&_fs);
    _error = n < 0 ? n : 0;
    return n < 0 ? 0 : n * _config.block_size;
}

fs::File::File(const File &other) : _handle(other._handle) { if (_handle) ++_handle->refs; }
fs::File &fs::File::operator=(const File &other) {
    if (this != &other) { release(); _handle = other._handle; if (_handle) ++_handle->refs; }
    return *this;
}
fs::File::~File() { release(); }
void fs::File::release() {
    if (_handle && --_handle->refs == 0) { close(); delete _handle; }
    _handle = nullptr;
}
fs::File::operator bool() const { return _handle && _handle->open; }
int fs::File::lastError() const { return _handle ? _handle->error : LFS_ERR_BADF; }
bool fs::File::close() {
    if (!_handle) return false;
    if (!_handle->open) return _handle->error == 0;
    _handle->error = lfs_file_close(&_handle->owner->_fs, &_handle->file);
    _handle->owner->_error = _handle->error;
    _handle->open = false;
    --_handle->owner->_openFiles;
    return _handle->error == 0;
}
bool fs::File::sync() {
    if (!*this) { if (_handle) _handle->error = LFS_ERR_BADF; return false; }
    _handle->error = lfs_file_sync(&_handle->owner->_fs, &_handle->file);
    _handle->owner->_error = _handle->error;
    return _handle->error == 0;
}
size_t fs::File::write(const uint8_t *data, size_t size) {
    if (!*this) { if (_handle) _handle->error = LFS_ERR_BADF; return 0; }
    if (!(_handle->flags & LFS_O_WRONLY)) { _handle->error = LFS_ERR_BADF; return 0; }
    lfs_ssize_t n = size && !data ? LFS_ERR_INVAL : lfs_file_write(&_handle->owner->_fs, &_handle->file, data, size);
    _handle->error = n < 0 ? n : 0;
    _handle->owner->_error = _handle->error;
    return n < 0 ? 0 : n;
}
int fs::File::read(uint8_t *data, size_t size) {
    if (!*this) { if (_handle) _handle->error = LFS_ERR_BADF; return -1; }
    if (!(_handle->flags & LFS_O_RDONLY)) { _handle->error = LFS_ERR_BADF; return -1; }
    lfs_ssize_t n = size && !data ? LFS_ERR_INVAL : lfs_file_read(&_handle->owner->_fs, &_handle->file, data, size);
    _handle->error = n < 0 ? n : 0;
    _handle->owner->_error = _handle->error;
    return n < 0 ? -1 : n;
}
int fs::File::read() { uint8_t b; return read(&b, 1) == 1 ? b : -1; }
size_t fs::File::size() {
    if (!*this) { if (_handle) _handle->error = LFS_ERR_BADF; return 0; }
    lfs_soff_t n = lfs_file_size(&_handle->owner->_fs, &_handle->file);
    _handle->error = n < 0 ? n : 0;
    return n < 0 ? 0 : n;
}
size_t fs::File::position() {
    if (!*this) { if (_handle) _handle->error = LFS_ERR_BADF; return 0; }
    lfs_soff_t n = lfs_file_tell(&_handle->owner->_fs, &_handle->file);
    _handle->error = n < 0 ? n : 0;
    return n < 0 ? 0 : n;
}
bool fs::File::seek(uint32_t position) {
    if (!*this) { if (_handle) _handle->error = LFS_ERR_BADF; return false; }
    lfs_soff_t n = position > 0x7fffffff ? LFS_ERR_INVAL :
        lfs_file_seek(&_handle->owner->_fs, &_handle->file, position, LFS_SEEK_SET);
    _handle->error = n < 0 ? n : 0;
    return n >= 0;
}
int fs::File::available() {
    if (!*this) { if (_handle) _handle->error = LFS_ERR_BADF; return 0; }
    size_t end = size(), pos = position();
    return end > pos ? static_cast<int>(end - pos) : 0;
}
int fs::File::peek() {
    if (!*this) { if (_handle) _handle->error = LFS_ERR_BADF; return -1; }
    size_t p = position();
    int b = read();
    return seek(p) ? b : -1;
}

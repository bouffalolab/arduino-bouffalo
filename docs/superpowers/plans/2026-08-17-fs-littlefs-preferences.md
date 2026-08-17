# FS LittleFS + Preferences EasyFlash 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 esp32-compat 库中落地 FS/File（LittleFS on `media`）、newlib `fopen`（挂载点 `/spiffs`）与 Preferences（EasyFlash on `PSM`），并用 StorageTest 固件在 BL616CL 上验证。

**Architecture:** SDK 存储组件以源码形式 vendored 进 `libraries/esp32-compat/src/lfs/`，随 Arduino 构建编译。`SPIFFSClass` 通过 `lfs_port`（MTD 查分区）挂载 LittleFS；`fopen` 经裁剪后的 newlib syscalls 按挂载点分发到同一实例；`Preferences` 走 EasyFlash 的 `ef_set/get_env_blob`。

**Tech Stack:** Bouffalo SDK LittleFS + `lfs_xip_flash` 移植 + EasyFlash + newlib syscalls + `bflb_mtd`/`bflb_boot2`；Arduino platform `bouffalo:bl616cl:unor4_bl616cl`；验证用 FTDI UART0 控制台。

---

## 文件结构

新增/修改（全部在 `arduino-bouffalo/`）：

- `libraries/esp32-compat/src/lfs/` —— vendored SDK 源码（见 Task 1）
- `libraries/esp32-compat/src/FS.h`、`FS.cpp` —— File 类（lfs 后端）
- `libraries/esp32-compat/src/SPIFFS.h`、`SPIFFS.cpp` —— 挂载 `media`，挂载点 `/spiffs`
- `libraries/esp32-compat/src/Preferences.h`、`Preferences.cpp` —— EasyFlash 后端
- `libraries/esp32-compat/src/syscalls_lfs.c` —— 裁剪版 newlib 文件系统调用
- `libraries/esp32-compat/src/esp32_compat.cpp` —— 删除重复的 `SPIFFSClass SPIFFS;`
- `examples/StorageTest/StorageTest.ino` —— 硬件验证固件

SDK 源路径（`bouffalo_sdk_full/bouffalo_sdk/components/`）：

| vendored 目标 | SDK 源 |
|---|---|
| `lfs/lfs.c`、`lfs_util.c`、`lfs.h`、`lfs_util.h` | `fs/littlefs/littlefs/` |
| `lfs/lfs_port.h`、`lfs_xip_flash.c` | `fs/littlefs/port/` |
| `lfs/easyflash.h`、`lfs_easyflash.c` | `fs/littlefs/easyflash_port/` |
| `lfs/port_file_fd.h`、`port_file_littlefs.h`、`port_file_littlefs.c` | `libc/newlib/` |
| `lfs/bflb_mtd.h`、`bflb_boot2.h`、`bflb_mtd.c`、`bflb_boot2.c` | `utils/bflb_mtd/` |

## 关键决策（来自 spec）

- FS 数据区：`media`（0x378000，452 KB）；NVM：`PSM`（0x3E9000，32 KB）。
- 挂载点 `/spiffs`；`SPIFFS.begin(formatOnFail)` 遵循 ESP32 语义（不自动格式化）。
- `LFS_THREADSAFE` 在 vendored `lfs_port.h` 顶部定义。
- 裁剪 syscalls：**不**定义 `_sbrk_r`/`_gettimeofday_r`（liblibc.a 已有强符号）。
- 桥接固件 `fopen("/spiffs/...")` 与 `SPIFFS` API 共用同一个 LittleFS 实例。

---

## Task 1: Vendored SDK 源码 + 编译冒烟

**Files:**
- Create: `libraries/esp32-compat/src/lfs/`（全部 vendored 文件）
- Create: `libraries/esp32-compat/src/lfs/README.md`

- [ ] **Step 1: 拷贝 SDK 源码**

```bash
SRC=/Volumes/DataStorage/workspace/arduino/bouffalo_sdk_full/bouffalo_sdk/components
DEST=/Volumes/DataStorage/workspace/arduino/arduino-bouffalo/libraries/esp32-compat/src/lfs
mkdir -p "$DEST"
cp "$SRC/fs/littlefs/littlefs/lfs.c" "$SRC/fs/littlefs/littlefs/lfs_util.c" \
   "$SRC/fs/littlefs/littlefs/lfs.h" "$SRC/fs/littlefs/littlefs/lfs_util.h" "$DEST/"
cp "$SRC/fs/littlefs/port/lfs_port.h" "$SRC/fs/littlefs/port/lfs_xip_flash.c" "$DEST/"
cp "$SRC/fs/littlefs/easyflash_port/easyflash.h" "$SRC/fs/littlefs/easyflash_port/lfs_easyflash.c" "$DEST/"
cp "$SRC/libc/newlib/port_file_fd.h" "$SRC/libc/newlib/port_file_littlefs.h" \
   "$SRC/libc/newlib/port_file_littlefs.c" "$DEST/"
cp "$SRC/utils/bflb_mtd/include/bflb_mtd.h" "$SRC/utils/bflb_mtd/include/bflb_boot2.h" \
   "$SRC/utils/bflb_mtd/bflb_mtd.c" "$SRC/utils/bflb_mtd/bflb_boot2.c" "$DEST/"
```

- [ ] **Step 2: 在 `lfs/lfs_port.h` 顶部加线程安全宏**

在 `#ifndef _LFS_PORT_H` 之后追加：

```c
#ifndef LFS_THREADSAFE
#define LFS_THREADSAFE 1
#endif
```

- [ ] **Step 3: 写 vendored 目录 README**

`libraries/esp32-compat/src/lfs/README.md` 内容：来源为 Bouffalo SDK 2.3.30-local-wl80211
的对应组件（逐文件列上游路径），许可证保留各文件头；同步策略：优先使用 SDK 原文件，
仅允许 `lfs_port.h` 的 `LFS_THREADSAFE` 宏与必要的构建适配。

- [ ] **Step 4: 编译 bridge 固件冒烟**

```bash
cd /Volumes/DataStorage/workspace/arduino
.tools/arduino-cli compile --config-file arduino-cli.yaml \
  --fqbn=bouffalo:bl616cl:unor4_bl616cl uno-r4-wifi-usb-bridge/UNOR4USBBridge 2>&1 | tail -8
```

预期：`Sketch uses ...` 两行输出，无编译错误。若报错（缺 include、宏冲突），在本任务内修复
（优先在 vendored 文件加 `#ifndef` 保护，不改 SDK 逻辑）。

- [ ] **Step 5: 提交**

```bash
git -C /Volumes/DataStorage/workspace/arduino/arduino-bouffalo add libraries/esp32-compat/src/lfs
git -C /Volumes/DataStorage/workspace/arduino/arduino-bouffalo commit -m "vendor: Bouffalo SDK LittleFS/EasyFlash/MTD/posix sources"
```

---

## Task 2: File + SPIFFS（LittleFS on media）

**Files:**
- Modify: `libraries/esp32-compat/src/FS.h`（替换 stub）
- Create: `libraries/esp32-compat/src/FS.cpp`
- Modify: `libraries/esp32-compat/src/SPIFFS.h`（替换 stub）
- Create: `libraries/esp32-compat/src/SPIFFS.cpp`
- Modify: `libraries/esp32-compat/src/esp32_compat.cpp`（删除 `SPIFFSClass SPIFFS;` 与 `#include "SPIFFS.h"`）

- [ ] **Step 1: 替换 `FS.h`**

```cpp
#ifndef BL616CL_ESP32_COMPAT_FS_H_
#define BL616CL_ESP32_COMPAT_FS_H_

#include <Arduino.h>
#include <WString.h>
#include <memory>

#define FILE_READ   "r"
#define FILE_WRITE  "w"
#define FILE_APPEND "a"

enum SeekMode {
    SeekSet = 0,
    SeekCur = 1,
    SeekEnd = 2
};

class FS;
struct FileImpl;

class File {
public:
    File();
    explicit File(std::shared_ptr<FileImpl> impl);

    operator bool() const;
    size_t write(uint8_t value);
    size_t write(const uint8_t *buffer, size_t size);
    int available();
    int read();
    int read(uint8_t *buffer, size_t size);
    int peek();
    void flush();
    void close();
    String name() const;
    String path() const;
    size_t size();
    bool seek(uint32_t position);
    bool seek(uint32_t position, SeekMode mode);
    size_t position();

    bool isDirectory();
    File openNextFile();
    void rewindDirectory();

private:
    std::shared_ptr<FileImpl> _impl;
};

class FS {
public:
    virtual ~FS() {}
    virtual File open(const char *path, const char *mode = FILE_READ) = 0;
    virtual bool exists(const char *path) = 0;
    virtual bool remove(const char *path) = 0;
    virtual bool rename(const char *from, const char *to) = 0;
    virtual bool mkdir(const char *path) = 0;
    virtual bool rmdir(const char *path) = 0;
};

#endif
```

- [ ] **Step 2: 创建 `FS.cpp`**

```cpp
#include "FS.h"
#include "lfs.h"

#include <string.h>

struct FileImpl {
    lfs_t *lfs = nullptr;
    lfs_file_t file;
    lfs_dir_t dir;
    bool open = false;
    bool is_dir = false;
    String full_path;
    String entry_name;
};

File::File() : _impl(nullptr) {}

File::File(std::shared_ptr<FileImpl> impl) : _impl(std::move(impl)) {}

File::operator bool() const
{
    return _impl != nullptr && _impl->open;
}

size_t File::write(uint8_t value)
{
    return write(&value, 1);
}

size_t File::write(const uint8_t *buffer, size_t size)
{
    if (!*this || _impl->is_dir || buffer == nullptr || size == 0) {
        return 0;
    }
    lfs_ssize_t n = lfs_file_write(_impl->lfs, &_impl->file, buffer, size);
    return n > 0 ? static_cast<size_t>(n) : 0;
}

int File::available()
{
    if (!*this || _impl->is_dir) {
        return 0;
    }
    return static_cast<int>(size() - position());
}

int File::read()
{
    uint8_t value = 0;
    return read(&value, 1) == 1 ? value : -1;
}

int File::read(uint8_t *buffer, size_t size)
{
    if (!*this || _impl->is_dir || buffer == nullptr || size == 0) {
        return -1;
    }
    lfs_ssize_t n = lfs_file_read(_impl->lfs, &_impl->file, buffer, size);
    return n > 0 ? static_cast<int>(n) : (n == 0 ? 0 : -1);
}

int File::peek()
{
    if (!*this || _impl->is_dir) {
        return -1;
    }
    uint8_t value = 0;
    if (lfs_file_read(_impl->lfs, &_impl->file, &value, 1) != 1) {
        return -1;
    }
    lfs_file_seek(_impl->lfs, &_impl->file, -1, LFS_SEEK_CUR);
    return value;
}

void File::flush()
{
    if (*this && !_impl->is_dir) {
        lfs_file_sync(_impl->lfs, &_impl->file);
    }
}

void File::close()
{
    if (_impl == nullptr || !_impl->open) {
        return;
    }
    if (_impl->is_dir) {
        lfs_dir_close(_impl->lfs, &_impl->dir);
    } else {
        lfs_file_close(_impl->lfs, &_impl->file);
    }
    _impl->open = false;
}

String File::name() const
{
    if (_impl == nullptr) {
        return String();
    }
    return _impl->is_dir && _impl->entry_name.length() > 0
               ? _impl->entry_name
               : _impl->full_path;
}

String File::path() const
{
    return _impl == nullptr ? String() : _impl->full_path;
}

size_t File::size()
{
    if (!*this || _impl->is_dir) {
        return 0;
    }
    lfs_soff_t n = lfs_file_size(_impl->lfs, &_impl->file);
    return n > 0 ? static_cast<size_t>(n) : 0;
}

bool File::seek(uint32_t position)
{
    return seek(position, SeekSet);
}

bool File::seek(uint32_t position, SeekMode mode)
{
    if (!*this || _impl->is_dir) {
        return false;
    }
    int whence = (mode == SeekCur) ? LFS_SEEK_CUR
                 : (mode == SeekEnd) ? LFS_SEEK_END
                                     : LFS_SEEK_SET;
    return lfs_file_seek(_impl->lfs, &_impl->file, position, whence) >= 0;
}

size_t File::position()
{
    if (!*this || _impl->is_dir) {
        return 0;
    }
    lfs_soff_t pos = lfs_file_tell(_impl->lfs, &_impl->file);
    return pos > 0 ? static_cast<size_t>(pos) : 0;
}

bool File::isDirectory()
{
    return *this && _impl->is_dir;
}

File File::openNextFile()
{
    if (!*this || !_impl->is_dir) {
        return File();
    }
    struct lfs_info info;
    while (lfs_dir_read(_impl->lfs, &_impl->dir, &info) > 0) {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }
        auto child = std::make_shared<FileImpl>();
        child->lfs = _impl->lfs;
        child->full_path = _impl->full_path;
        if (!child->full_path.endsWith("/")) {
            child->full_path += "/";
        }
        child->full_path += info.name;
        child->entry_name = info.name;
        if (info.type == LFS_TYPE_DIR) {
            child->is_dir = true;
            child->open = lfs_dir_open(child->lfs, &child->dir,
                                       child->full_path.c_str()) == LFS_ERR_OK;
        } else {
            child->is_dir = false;
            child->open = lfs_file_open(child->lfs, &child->file,
                                        child->full_path.c_str(),
                                        LFS_O_RDONLY) == LFS_ERR_OK;
        }
        return File(child);
    }
    return File();
}

void File::rewindDirectory()
{
    if (*this && _impl->is_dir) {
        lfs_dir_rewind(_impl->lfs, &_impl->dir);
    }
}
```

- [ ] **Step 3: 替换 `SPIFFS.h`**

```cpp
#ifndef BL616CL_ESP32_COMPAT_SPIFFS_H_
#define BL616CL_ESP32_COMPAT_SPIFFS_H_

#include "FS.h"

class SPIFFSClass : public FS {
public:
    bool begin(bool formatOnFail = false);
    void end();
    bool format();
    bool exists(const char *path) override;
    bool remove(const char *path) override;
    bool rename(const char *from, const char *to) override;
    File open(const char *path, const char *mode = FILE_READ) override;
    bool mkdir(const char *path) override;
    bool rmdir(const char *path) override;
};

extern SPIFFSClass SPIFFS;

#endif
```

- [ ] **Step 4: 创建 `SPIFFS.cpp`**

```cpp
#include "SPIFFS.h"
#include "FS.h"
#include "lfs.h"
#include "port_file_littlefs.h"
#include "bflb_mtd.h"

#include <string.h>

static lfs_t s_lfs;
static struct lfs_port_context s_ctx;
static struct lfs_config s_cfg;
static bool s_mounted = false;
static bool s_mtd_init = false;

static String fs_path(const char *path)
{
    String p = String("/spiffs");
    if (path == nullptr || path[0] == '\0') {
        return p;
    }
    if (path[0] != '/') {
        p += "/";
    }
    p += path;
    return p;
}

static bool mount_lfs(bool format_on_fail)
{
    if (s_mounted) {
        return true;
    }
    if (!s_mtd_init) {
        bflb_mtd_init();
        s_mtd_init = true;
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.partition_name = "media";
    memset(&s_cfg, 0, sizeof(s_cfg));
    if (lfs_port_init(&s_lfs, &s_ctx, &s_cfg) != 0) {
        return false;
    }
    int err = lfs_mount(&s_lfs, &s_cfg);
    if (err != LFS_ERR_OK) {
        if (!format_on_fail) {
            return false;
        }
        if (lfs_format(&s_lfs, &s_cfg) != LFS_ERR_OK) {
            return false;
        }
        if (lfs_mount(&s_lfs, &s_cfg) != LFS_ERR_OK) {
            return false;
        }
    }
    if (lfs_port_register_fs(&s_lfs, "/spiffs") != 0) {
        return false;
    }
    lfs_port_set_default(&s_lfs);
    s_mounted = true;
    return true;
}

static void unmount_lfs()
{
    if (s_mounted) {
        lfs_port_unregister_fs(&s_lfs);
        lfs_unmount(&s_lfs);
        s_mounted = false;
    }
}

bool SPIFFSClass::begin(bool formatOnFail)
{
    return mount_lfs(formatOnFail);
}

void SPIFFSClass::end()
{
    unmount_lfs();
}

bool SPIFFSClass::format()
{
    unmount_lfs();
    if (!s_mtd_init) {
        bflb_mtd_init();
        s_mtd_init = true;
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.partition_name = "media";
    memset(&s_cfg, 0, sizeof(s_cfg));
    if (lfs_port_init(&s_lfs, &s_ctx, &s_cfg) != 0) {
        return false;
    }
    return lfs_format(&s_lfs, &s_cfg) == LFS_ERR_OK;
}

bool SPIFFSClass::exists(const char *path)
{
    if (!s_mounted) {
        return false;
    }
    struct lfs_info info;
    return lfs_stat(&s_lfs, fs_path(path).c_str(), &info) == LFS_ERR_OK;
}

bool SPIFFSClass::remove(const char *path)
{
    if (!s_mounted) {
        return false;
    }
    return lfs_remove(&s_lfs, fs_path(path).c_str()) == LFS_ERR_OK;
}

bool SPIFFSClass::rename(const char *from, const char *to)
{
    if (!s_mounted) {
        return false;
    }
    return lfs_rename(&s_lfs, fs_path(from).c_str(), fs_path(to).c_str()) == LFS_ERR_OK;
}

File SPIFFSClass::open(const char *path, const char *mode)
{
    if (!s_mounted) {
        return File();
    }
    String p = fs_path(path);
    struct lfs_info info;
    bool is_dir = (lfs_stat(&s_lfs, p.c_str(), &info) == LFS_ERR_OK) &&
                  (info.type == LFS_TYPE_DIR);

    auto impl = std::make_shared<FileImpl>();
    impl->lfs = &s_lfs;
    impl->full_path = p;

    if (is_dir) {
        if (lfs_dir_open(&s_lfs, &impl->dir, p.c_str()) != LFS_ERR_OK) {
            return File();
        }
        impl->is_dir = true;
        impl->open = true;
        return File(impl);
    }

    int flags = LFS_O_RDONLY;
    if (mode != nullptr) {
        switch (mode[0]) {
            case 'w':
                flags = LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;
                break;
            case 'a':
                flags = LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND;
                break;
            case 'r':
            default:
                flags = LFS_O_RDONLY;
                break;
        }
    }
    if (lfs_file_open(&s_lfs, &impl->file, p.c_str(), flags) != LFS_ERR_OK) {
        return File();
    }
    impl->is_dir = false;
    impl->open = true;
    return File(impl);
}

bool SPIFFSClass::mkdir(const char *path)
{
    if (!s_mounted) {
        return false;
    }
    return lfs_mkdir(&s_lfs, fs_path(path).c_str()) == LFS_ERR_OK;
}

bool SPIFFSClass::rmdir(const char *path)
{
    if (!s_mounted) {
        return false;
    }
    return lfs_remove(&s_lfs, fs_path(path).c_str()) == LFS_ERR_OK;
}

SPIFFSClass SPIFFS;
```

- [ ] **Step 5: 从 `esp32_compat.cpp` 删除重复实例**

删除 `SPIFFSClass SPIFFS;` 与 `#include "SPIFFS.h"`。

- [ ] **Step 6: 编译冒烟 + 提交**

```bash
cd /Volumes/DataStorage/workspace/arduino
.tools/arduino-cli compile --config-file arduino-cli.yaml \
  --fqbn=bouffalo:bl616cl:unor4_bl616cl uno-r4-wifi-usb-bridge/UNOR4USBBridge 2>&1 | tail -8
git -C /Volumes/DataStorage/workspace/arduino/arduino-bouffalo add libraries/esp32-compat/src
git -C /Volumes/DataStorage/workspace/arduino/arduino-bouffalo commit -m "feat: FS/File over LittleFS on the media partition"
```

预期：编译通过（`Sketch uses ...`）。若 `FileImpl` 访问受限，把 `struct FileImpl` 移到
`FS.h` 中（见 Task 2 备注：`SPIFFS.cpp` 需要 `make_shared<FileImpl>`，故 `FileImpl`
定义放 `FS.h`，字段与 lfs 类型无关，仅声明 `lfs_file_t file; lfs_dir_t dir;` 需要前向
声明——lfs 类型完整定义仅 FS.cpp 需要，因此把 `FileImpl` 定义放进 `FS.cpp`，并在
`FS.h` 声明 `struct FileImpl;` 即可；`SPIFFS.cpp` 改为调用 `File` 提供的内部工厂）。

> 修正（保持 pimpl 不泄漏 lfs 类型）：`SPIFFS.cpp` 不直接构造 `FileImpl`，改为
> `FS.h` 增加友元工厂声明 `File _openImpl(...)` 不可行时，简单方案：在 `FS.h` 的
> `File` 上增加静态工厂 `static File fromImpl(std::shared_ptr<FileImpl>);` 并将
> `struct FileImpl` 完整定义放入 `FS.h`，但字段只用 `void*`（`lfs_file_t`/`lfs_dir_t`
> 以 `void*` 存储并在 FS.cpp 中 cast），保持头文件不含 lfs。

- [ ] **Step 7（按 Step 6 备注落地）**：把 `FileImpl` 定义改为 void* 持有：

```cpp
struct FileImpl {
    void *lfs = nullptr;      /* lfs_t* */
    void *file = nullptr;     /* lfs_file_t* */
    void *dir = nullptr;      /* lfs_dir_t* */
    bool open = false;
    bool is_dir = false;
    String full_path;
    String entry_name;
};
```

`FS.cpp` 内部用 `static lfs_file_t* as_file(FileImpl& i) { return (lfs_file_t*)i.file; }`
等 helper 转换后调用 lfs API；`FileImpl` 构造时用 `new lfs_file_t`/`new lfs_dir_t`，
析构/close 时 `delete`。

---

## Task 3: newlib FILE 层（fopen → LittleFS）

**Files:**
- Create: `libraries/esp32-compat/src/syscalls_lfs.c`

- [ ] **Step 1: 创建 `syscalls_lfs.c`**

```c
#include <errno.h>
#include <reent.h>
#include <sys/stat.h>
#include <sys/fcntl.h>

#include "port_file_fd.h"
#include "port_file_littlefs.h"

/* Trimmed newlib syscall layer for the Arduino platform.
 *
 * liblibc.a already provides _sbrk_r/_gettimeofday_r (strong) and weak
 * _read_r/_write_r stubs; the SDK's full syscalls.c cannot be linked as-is
 * (duplicate _sbrk_r/_gettimeofday_r).  We only provide the file/console
 * dispatch that the LittleFS POSIX layer needs.  FDs 0-2 stay on the
 * console stubs (the SDK log writes to UART directly, so returning EBADF
 * preserves current behavior); LittleFS fds (0x5000+) go to the LFS port.
 */

static int tty_open_r(struct _reent *reent, const char *path, int flags, int mode)
{
    (void)path; (void)flags; (void)mode;
    reent->_errno = EBADF;
    return -1;
}

static int tty_close_r(struct _reent *reent, int fd)
{
    (void)fd;
    reent->_errno = EBADF;
    return -1;
}

static _ssize_t tty_read_r(struct _reent *reent, int fd, void *ptr, size_t len)
{
    (void)fd; (void)ptr; (void)len;
    reent->_errno = EBADF;
    return -1;
}

static _ssize_t tty_write_r(struct _reent *reent, int fd, const void *ptr, size_t len)
{
    (void)fd; (void)ptr; (void)len;
    reent->_errno = EBADF;
    return -1;
}

static int tty_fstat_r(struct _reent *reent, int fd, struct stat *st)
{
    (void)fd;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR;
    return 0;
}

static int tty_stat_r(struct _reent *reent, const char *path, struct stat *st)
{
    (void)path; (void)st;
    reent->_errno = ENOENT;
    return -1;
}

int _isatty_r(struct _reent *reent, int fd)
{
    (void)reent;
    return !IS_FILE_FD(fd);
}

int _open_r(struct _reent *reent, const char *path, int flags, int mode)
{
    if (lfs_port_find_by_path(path, NULL) != NULL) {
        return _open_file_lfs_r(reent, path, flags, mode);
    }
    return tty_open_r(reent, path, flags, mode);
}

int _close_r(struct _reent *reent, int fd)
{
    if (LFS_FD_IS(fd)) {
        return _close_file_lfs_r(reent, fd);
    }
    return tty_close_r(reent, fd);
}

_ssize_t _read_r(struct _reent *reent, int fd, void *ptr, size_t len)
{
    if (LFS_FD_IS(fd)) {
        return _read_file_lfs_r(reent, fd, ptr, len);
    }
    return tty_read_r(reent, fd, ptr, len);
}

_ssize_t _write_r(struct _reent *reent, int fd, const void *ptr, size_t len)
{
    if (LFS_FD_IS(fd)) {
        return _write_file_lfs_r(reent, fd, ptr, len);
    }
    return tty_write_r(reent, fd, ptr, len);
}

_off_t _lseek_r(struct _reent *reent, int fd, _off_t offset, int whence)
{
    if (LFS_FD_IS(fd)) {
        return _lseek_file_lfs_r(reent, fd, offset, whence);
    }
    reent->_errno = ESPIPE;
    return -1;
}

int _fstat_r(struct _reent *reent, int fd, struct stat *st)
{
    if (LFS_FD_IS(fd)) {
        return _fstat_file_lfs_r(reent, fd, st);
    }
    return tty_fstat_r(reent, fd, st);
}

int _stat_r(struct _reent *reent, const char *path, struct stat *st)
{
    if (lfs_port_find_by_path(path, NULL) != NULL) {
        return _stat_file_lfs_r(reent, path, st);
    }
    return tty_stat_r(reent, path, st);
}

int _unlink_r(struct _reent *reent, const char *path)
{
    if (lfs_port_find_by_path(path, NULL) != NULL) {
        return _unlink_file_lfs_r(reent, path);
    }
    reent->_errno = ENOENT;
    return -1;
}

int _rename_r(struct _reent *reent, const char *oldname, const char *newname)
{
    if (lfs_port_find_by_path(oldname, NULL) != NULL ||
        lfs_port_find_by_path(newname, NULL) != NULL) {
        return _rename_file_lfs_r(reent, oldname, newname);
    }
    reent->_errno = ENOENT;
    return -1;
}

int _mkdir_r(struct _reent *reent, const char *path, int mode)
{
    if (lfs_port_find_by_path(path, NULL) != NULL) {
        return _mkdir_file_lfs_r(reent, path, mode);
    }
    reent->_errno = ENOENT;
    return -1;
}

int _rmdir_r(struct _reent *reent, const char *path)
{
    if (lfs_port_find_by_path(path, NULL) != NULL) {
        return _rmdir_file_lfs_r(reent, path);
    }
    reent->_errno = ENOENT;
    return -1;
}
```

> 若 `port_file_littlefs.h` 中 `_*_file_lfs_r` 的原型与参数名不同，以头文件为准微调；
> `lfs_port_find_by_path` 第二个参数传 NULL（只探测是否存在挂载点匹配）。

- [ ] **Step 2: 编译 + 提交**

```bash
cd /Volumes/DataStorage/workspace/arduino
.tools/arduino-cli compile --config-file arduino-cli.yaml \
  --fqbn=bouffalo:bl616cl:unor4_bl616cl uno-r4-wifi-usb-bridge/UNOR4USBBridge 2>&1 | tail -8
git -C /Volumes/DataStorage/workspace/arduino/arduino-bouffalo add libraries/esp32-compat/src/syscalls_lfs.c
git -C /Volumes/DataStorage/workspace/arduino/arduino-bouffalo commit -m "feat: route fopen to LittleFS via newlib syscalls"
```

预期：编译通过。若与 liblibc 的弱 `_read_r`/`_write_r` 冲突，检查链接输出；强符号应
优先生效（本文件在库对象中，无重复定义错误）。

---

## Task 4: Preferences（EasyFlash on PSM）

**Files:**
- Modify: `libraries/esp32-compat/src/Preferences.h`（替换 stub）
- Create: `libraries/esp32-compat/src/Preferences.cpp`

- [ ] **Step 1: 替换 `Preferences.h`**

```cpp
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
```

- [ ] **Step 2: 创建 `Preferences.cpp`**

```cpp
#include "Preferences.h"
#include "easyflash.h"

#include <stdlib.h>
#include <string.h>

/* Blob layout: [type u8][len u32 little-endian][payload] */
static const size_t VALUE_HEADER = 5;

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
    if (easyflash_init() != EF_NO_ERR) {
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
    size_t saved = 0;
    size_t got = ef_get_env_blob(k.c_str(), value, *len + VALUE_HEADER, &saved);
    if (got == 0 || saved < VALUE_HEADER) {
        return false;
    }
    uint8_t *blob = (uint8_t *)value;
    if (blob[0] != (uint8_t)type) {
        return false;
    }
    size_t payload = get_le32(blob + 1);
    if (payload != *len || saved != VALUE_HEADER + payload) {
        return false;
    }
    memmove(value, blob + VALUE_HEADER, payload);
    *len = payload;
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
    uint8_t type = 0;
    size_t saved = 0;
    if (ef_get_env_blob(k.c_str(), &type, 1, &saved) == 0 || saved < 1) {
        return PT_INVALID;
    }
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
    size_t saved = 0;
    size_t got = ef_get_env_blob(k.c_str(), nullptr, 0, &saved);
    if (got == 0 || saved < VALUE_HEADER) {
        return String(defaultValue);
    }
    uint8_t *blob = (uint8_t *)malloc(saved);
    if (blob == nullptr) {
        return String(defaultValue);
    }
    got = ef_get_env_blob(k.c_str(), blob, saved, &saved);
    if (got == 0 || saved < VALUE_HEADER || blob[0] != PT_STR) {
        free(blob);
        return String(defaultValue);
    }
    size_t len = get_le32(blob + 1);
    String result((const char *)(blob + VALUE_HEADER), len);
    free(blob);
    return result;
}

size_t Preferences::getBytesLength(const char *key)
{
    if (_ns.length() == 0) {
        return 0;
    }
    String k = makeKey(key);
    size_t saved = 0;
    if (ef_get_env_blob(k.c_str(), nullptr, 0, &saved) == 0 || saved < VALUE_HEADER) {
        return 0;
    }
    uint8_t header[VALUE_HEADER];
    ef_get_env_blob(k.c_str(), header, sizeof(header), &saved);
    return get_le32(header + 1);
}

size_t Preferences::getBytes(const char *key, void *buffer, size_t maxLen)
{
    size_t len = maxLen;
    return getValue(key, PT_BLOB, buffer, &len) ? len : 0;
}

struct clear_ctx {
    const char *ns;
    size_t ns_len;
    size_t count;
};

static EfErrCode clear_cb(const char *key, void *arg)
{
    struct clear_ctx *ctx = (struct clear_ctx *)arg;
    if (strncmp(key, ctx->ns, ctx->ns_len) == 0 && key[ctx->ns_len] == '/') {
        ef_del_env(key);
        ctx->count++;
    }
    return EF_NO_ERR;
}

static EfErrCode count_cb(const char *key, void *arg)
{
    struct clear_ctx *ctx = (struct clear_ctx *)arg;
    if (strncmp(key, ctx->ns, ctx->ns_len) == 0 && key[ctx->ns_len] == '/') {
        ctx->count++;
    }
    return EF_NO_ERR;
}

bool Preferences::clear()
{
    if (_ns.length() == 0) {
        return false;
    }
    struct clear_ctx ctx = { _ns.c_str(), _ns.length(), 0 };
    ef_foreach_env(clear_cb, &ctx);
    return true;
}

bool Preferences::remove(const char *key)
{
    if (_ns.length() == 0 || key == nullptr) {
        return false;
    }
    String k = makeKey(key);
    return ef_del_env(k.c_str()) == EF_NO_ERR;
}

size_t Preferences::freeEntries()
{
    if (_ns.length() == 0) {
        return 0;
    }
    struct clear_ctx ctx = { _ns.c_str(), _ns.length(), 0 };
    ef_foreach_env(count_cb, &ctx);
    return ctx.count;
}
```

- [ ] **Step 3: 编译 + 提交**

```bash
cd /Volumes/DataStorage/workspace/arduino
.tools/arduino-cli compile --config-file arduino-cli.yaml \
  --fqbn=bouffalo:bl616cl:unor4_bl616cl uno-r4-wifi-usb-bridge/UNOR4USBBridge 2>&1 | tail -8
git -C /Volumes/DataStorage/workspace/arduino/arduino-bouffalo add libraries/esp32-compat/src/Preferences.*
git -C /Volumes/DataStorage/workspace/arduino/arduino-bouffalo commit -m "feat: Preferences NVM over EasyFlash on the PSM partition"
```

> `ef_foreach_env` 若在迭代中删除 key 不安全，改为先收集再删除（实现时按
> `lfs_easyflash.c` 的遍历实现确认；若安全则直接用，否则收集到数组后统一删除）。

---

## Task 5: StorageTest 硬件验证

**Files:**
- Create: `examples/StorageTest/StorageTest.ino`

- [ ] **Step 1: 创建测试固件**

```cpp
#include <Arduino.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *name, bool ok)
{
    Serial.printf("[%s] %s\r\n", ok ? "PASS" : "FAIL", name);
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
    }
}

static void test_fs()
{
    Serial.println("== FS ==");
    check("begin(false) after format", SPIFFS.format() && SPIFFS.begin(false));

    File f = SPIFFS.open("/hello.txt", FILE_WRITE);
    check("open write", (bool)f);
    if (f) {
        check("write", f.write((const uint8_t *)"hello lfs", 9) == 9);
        f.close();
    }
    check("exists", SPIFFS.exists("/hello.txt"));

    f = SPIFFS.open("/hello.txt", FILE_READ);
    check("open read", (bool)f);
    if (f) {
        check("size", f.size() == 9);
        char buf[16] = {0};
        check("read", f.read((uint8_t *)buf, 9) == 9 && strcmp(buf, "hello lfs") == 0);
        check("seek 0", f.seek(0));
        check("read after seek", f.read() == 'h');
        f.close();
    }

    check("rename", SPIFFS.rename("/hello.txt", "/renamed.txt"));
    check("old gone", !SPIFFS.exists("/hello.txt"));
    check("new exists", SPIFFS.exists("/renamed.txt"));
    check("remove", SPIFFS.remove("/renamed.txt"));
    check("removed gone", !SPIFFS.exists("/renamed.txt"));

    check("mkdir", SPIFFS.mkdir("/dir"));
    File d = SPIFFS.open("/dir", FILE_READ);
    check("open dir", (bool)d && d.isDirectory());
    if (d) {
        File e = SPIFFS.open("/dir/item.txt", FILE_WRITE);
        if (e) {
            e.write((const uint8_t *)"x", 1);
            e.close();
        }
        File child = d.openNextFile();
        check("openNextFile", (bool)child && child.name() == "/dir/item.txt");
        if (child) {
            child.close();
        }
        d.close();
    }
    check("rmdir", SPIFFS.rmdir("/dir/item.txt") && SPIFFS.rmdir("/dir"));
}

static void test_fopen()
{
    Serial.println("== fopen ==");
    FILE *fp = fopen("/spiffs/fp.txt", "w");
    check("fopen w", fp != nullptr);
    if (fp) {
        check("fwrite", fwrite("via fopen", 1, 9, fp) == 9);
        fclose(fp);
    }
    fp = fopen("/spiffs/fp.txt", "r");
    check("fopen r", fp != nullptr);
    if (fp) {
        char buf[16] = {0};
        check("fread", fread(buf, 1, 9, fp) == 9 && strcmp(buf, "via fopen") == 0);
        fclose(fp);
    }
    remove("/spiffs/fp.txt");
}

static void test_prefs()
{
    Serial.println("== Preferences ==");
    Preferences prefs;
    check("begin", prefs.begin("tns"));
    prefs.clear();

    check("putChar", prefs.putChar("c", -5) == 1);
    check("getChar", prefs.getChar("c", 0) == -5);
    check("putInt", prefs.putInt("i", 123456) == 4);
    check("getInt", prefs.getInt("i", 0) == 123456);
    check("putUInt", prefs.putUInt("u", 4000000000UL) == 4);
    check("getUInt", prefs.getUInt("u", 0) == 4000000000UL);
    check("putLong64", prefs.putLong64("l", -9000000000LL) == 8);
    check("getLong64", prefs.getLong64("l", 0) == -9000000000LL);
    check("putString", prefs.putString("s", "hello world") == 11);
    check("getString", prefs.getString("s", "") == "hello world");
    check("getType", prefs.getType("s") == PT_STR);
    check("getType invalid", prefs.getType("nope") == PT_INVALID);

    uint8_t blob[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    check("putBytes", prefs.putBytes("b", blob, 4) == 4);
    check("getBytesLength", prefs.getBytesLength("b") == 4);
    uint8_t out[8] = {0};
    check("getBytes", prefs.getBytes("b", out, 8) == 4 &&
                      memcmp(out, blob, 4) == 0);

    check("remove", prefs.remove("c"));
    check("removed", prefs.getType("c") == PT_INVALID);
    check("freeEntries", prefs.freeEntries() == 4); /* i,u,l,s,b -> b,i,l,s,u */
    prefs.end();
}

static void test_persist()
{
    Serial.println("== Persistence ==");
    Preferences prefs;
    check("begin p", prefs.begin("persist"));
    int32_t boot_count = prefs.getInt("boots", 0);
    Serial.printf("boot count: %d\r\n", boot_count);
    check("increment", prefs.putInt("boots", boot_count + 1) == 4);
    check("readback", prefs.getInt("boots", 0) == boot_count + 1);
    prefs.end();
}

void setup()
{
    Serial.begin(2000000);
    delay(200);
    Serial.println("== StorageTest ==");

    test_fs();
    test_fopen();
    test_prefs();
    test_persist();

    Serial.printf("RESULT: %d pass, %d fail\r\n", g_pass, g_fail);
}

void loop()
{
    delay(1000);
}
```

- [ ] **Step 2: 编译**

```bash
cd /Volumes/DataStorage/workspace/arduino
.tools/arduino-cli compile --config-file arduino-cli.yaml \
  --fqbn=bouffalo:bl616cl:unor4_bl616cl arduino-bouffalo/examples/StorageTest 2>&1 | tail -8
```

预期：编译通过。若 `Serial.printf` 不支持（CONFIG_LIBC_FLOAT=0 不影响 %s/%d），
改用 `Serial.print` 拼接；`String(name) == "/dir/item.txt"` 用 `strcmp` 兼容。

- [ ] **Step 3: 烧录并运行**

```bash
pkill -f picocom 2>/dev/null
arduino-bouffalo/tools/bouffalo_flash_cube/BLFlashCommand-macos \
  --interface=uart --chipname=bl616cl --port=/dev/cu.usbserial-BG02DE1J \
  --baudrate=2000000 \
  --firmware=arduino-bouffalo/examples/StorageTest/build/bouffalo.bl616cl.unor4_bl616cl/StorageTest.ino.bin
```

然后开控制台观察：

```bash
picocom -b 2000000 --lower-rts --raise-dtr /dev/tty.usbserial-BG02DE1J
```

预期：串口输出 `== StorageTest ==` 及全部 `[PASS]`，结尾
`RESULT: 40 pass, 0 fail`（具体计数以实现为准）。首次运行 `boot count: 0`；
复位后再跑一次应递增（验证持久化）。

- [ ] **Step 4: 修复问题并迭代**（若有 FAIL：定位是实现 bug 还是分区/MTD 问题，
      修复后重编译重烧；直到全 PASS + 持久化计数递增）

- [ ] **Step 5: 提交**

```bash
git -C /Volumes/DataStorage/workspace/arduino/arduino-bouffalo add examples/StorageTest
git -C /Volumes/DataStorage/workspace/arduino/arduino-bouffalo commit -m "test: StorageTest firmware for FS/fopen/Preferences verification"
```

---

## Task 6: 回归 + 文档 + TODO

**Files:**
- Modify: `arduino-bouffalo/TODO.md`
- Modify: `arduino-bouffalo/docs/CHANGES-non-arduino-bouffalo.md`（如需）

- [ ] **Step 1: bridge 回归编译**

```bash
cd /Volumes/DataStorage/workspace/arduino
.tools/arduino-cli compile --config-file arduino-cli.yaml \
  --fqbn=bouffalo:bl616cl:unor4_bl616cl uno-r4-wifi-usb-bridge/UNOR4USBBridge 2>&1 | tail -8
```

预期：编译通过。随后烧录 bridge 固件并跑 `tools/at_smoke/test_net.py` + `test_tls.py`
确认 AT 冒烟不回归（可选，若时间允许）。

- [ ] **Step 2: 更新 TODO.md**

把第四阶段前两项标记完成：

```markdown
- [x] 在 BL616CL 上实现 `SPIFFS`/`FS` 兼容层：SDK LittleFS vendored 进
      esp32-compat，`SPIFFS.begin` 挂载 `media` 分区（挂载点 `/spiffs`），
      `File` 支持读写/seek/目录遍历；newlib `fopen("/spiffs/...")` 经裁剪
      syscalls 分发到同一实例；StorageTest 全 PASS + 复位持久化验证
- [x] 实现 `Preferences` NVM 后端：对接 EasyFlash（底层 LittleFS on PSM），
      put/get 全类型 + getType/remove/clear/freeEntries，StorageTest round-trip
```

- [ ] **Step 3: 提交**

```bash
git -C /Volumes/DataStorage/workspace/arduino/arduino-bouffalo add TODO.md
git -C /Volumes/DataStorage/workspace/arduino/arduino-bouffalo commit -m "TODO: mark FS LittleFS and Preferences EasyFlash done"
```

---

## 自检记录

- **Spec 覆盖**：FS/File（Task 2）、fopen（Task 3）、Preferences（Task 4）、
  验证（Task 5）、回归与记录（Task 6）；分区 `media`/`PSM`、挂载点 `/spiffs`
  均落实。
- **占位符**：无 TBD；vendored 文件用精确 cp 命令。
- **类型一致**：`FileImpl` 字段统一为 void* 方案；`SPIFFS.open` 返回 `File`；
  `Preferences` 方法签名与 ESP32 一致。

#include "SPIFFS.h"
#include "FS.h"
#include "lfs/lfs.h"
#include "lfs/port_file_littlefs.h"

extern "C" {
#include "lfs/bflb_mtd.h"
}

#include <string.h>

static lfs_t s_lfs;
static struct lfs_port_context s_ctx;
static struct lfs_config s_cfg;
static bool s_mounted = false;
static bool s_ctx_inited = false;

/*
 * LittleFS has no mount-point concept: every path is rooted at the filesystem
 * root. The "/spiffs" prefix is only a POSIX-layer notion (stripped by
 * port_file_littlefs for fopen). So SPIFFS API paths are normalized to
 * root-relative lfs paths directly.
 */
static String fs_path(const char *path)
{
    if (path == nullptr || path[0] == '\0') {
        return String("/");
    }
    if (path[0] == '/') {
        return String(path);
    }
    return String("/") + path;
}

static bool ensure_ctx()
{
    if (s_ctx_inited) {
        return true;
    }
    bflb_mtd_init();
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.partition_name = "media";
    if (lfs_port_init(&s_lfs, &s_ctx, &s_cfg) != 0) {
        return false;
    }
    s_ctx_inited = true;
    return true;
}

static bool mount_lfs(bool format_on_fail)
{
    if (s_mounted) {
        return true;
    }
    if (!ensure_ctx()) {
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
        lfs_unmount(&s_lfs);
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
    if (!ensure_ctx()) {
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
    return lfs_rename(&s_lfs, fs_path(from).c_str(),
                      fs_path(to).c_str()) == LFS_ERR_OK;
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
        impl->dir = new lfs_dir_t();
        if (lfs_dir_open(&s_lfs, reinterpret_cast<lfs_dir_t *>(impl->dir),
                         p.c_str()) != LFS_ERR_OK) {
            delete reinterpret_cast<lfs_dir_t *>(impl->dir);
            impl->dir = nullptr;
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
    impl->file = new lfs_file_t();
    if (lfs_file_open(&s_lfs, reinterpret_cast<lfs_file_t *>(impl->file),
                      p.c_str(), flags) != LFS_ERR_OK) {
        delete reinterpret_cast<lfs_file_t *>(impl->file);
        impl->file = nullptr;
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

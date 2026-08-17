#include "FS.h"
#include "lfs/lfs.h"

#include <string.h>

/* ---- type helpers (FileImpl stores opaque pointers) ---- */

static lfs_t *as_lfs(FileImpl &i)
{
    return reinterpret_cast<lfs_t *>(i.lfs);
}

static lfs_file_t *as_file(FileImpl &i)
{
    return reinterpret_cast<lfs_file_t *>(i.file);
}

static lfs_dir_t *as_dir(FileImpl &i)
{
    return reinterpret_cast<lfs_dir_t *>(i.dir);
}

/* ---- FileImpl lifecycle ---- */

FileImpl::~FileImpl()
{
    if (lfs == nullptr) {
        return;
    }
    if (file != nullptr) {
        if (open && !is_dir) {
            lfs_file_close(as_lfs(*this), as_file(*this));
        }
        delete as_file(*this);
        file = nullptr;
    }
    if (dir != nullptr) {
        if (open && is_dir) {
            lfs_dir_close(as_lfs(*this), as_dir(*this));
        }
        delete as_dir(*this);
        dir = nullptr;
    }
    open = false;
}

/* ---- File ---- */

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
    lfs_ssize_t n = lfs_file_write(as_lfs(*_impl), as_file(*_impl), buffer, size);
    return n > 0 ? static_cast<size_t>(n) : 0;
}

int File::available()
{
    if (!*this || _impl->is_dir) {
        return 0;
    }
    lfs_soff_t size = lfs_file_size(as_lfs(*_impl), as_file(*_impl));
    lfs_soff_t pos = lfs_file_tell(as_lfs(*_impl), as_file(*_impl));
    if (size < 0 || pos < 0 || pos > size) {
        return 0;
    }
    return static_cast<int>(size - pos);
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
    lfs_ssize_t n = lfs_file_read(as_lfs(*_impl), as_file(*_impl), buffer, size);
    return n > 0 ? static_cast<int>(n) : (n == 0 ? 0 : -1);
}

int File::peek()
{
    if (!*this || _impl->is_dir) {
        return -1;
    }
    uint8_t value = 0;
    if (lfs_file_read(as_lfs(*_impl), as_file(*_impl), &value, 1) != 1) {
        return -1;
    }
    if (lfs_file_seek(as_lfs(*_impl), as_file(*_impl), -1, LFS_SEEK_CUR) < 0) {
        return -1;
    }
    return value;
}

void File::flush()
{
    if (*this && !_impl->is_dir) {
        lfs_file_sync(as_lfs(*_impl), as_file(*_impl));
    }
}

void File::close()
{
    if (_impl == nullptr || !_impl->open) {
        return;
    }
    if (_impl->is_dir) {
        if (as_dir(*_impl) != nullptr) {
            lfs_dir_close(as_lfs(*_impl), as_dir(*_impl));
            delete as_dir(*_impl);
            _impl->dir = nullptr;
        }
    } else {
        if (as_file(*_impl) != nullptr) {
            lfs_file_close(as_lfs(*_impl), as_file(*_impl));
            delete as_file(*_impl);
            _impl->file = nullptr;
        }
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
    lfs_soff_t n = lfs_file_size(as_lfs(*_impl), as_file(*_impl));
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
    return lfs_file_seek(as_lfs(*_impl), as_file(*_impl), position, whence) >= 0;
}

size_t File::position()
{
    if (!*this || _impl->is_dir) {
        return 0;
    }
    lfs_soff_t pos = lfs_file_tell(as_lfs(*_impl), as_file(*_impl));
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
    while (lfs_dir_read(as_lfs(*_impl), as_dir(*_impl), &info) > 0) {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }
        auto child = std::make_shared<FileImpl>();
        child->lfs = _impl->lfs;
        child->full_path = _impl->full_path;
        if (child->full_path.length() == 0 ||
            child->full_path[child->full_path.length() - 1] != '/') {
            child->full_path += "/";
        }
        child->full_path += info.name;
        child->entry_name = info.name;
        if (info.type == LFS_TYPE_DIR) {
            child->dir = new lfs_dir_t();
            if (lfs_dir_open(as_lfs(*child), as_dir(*child),
                             child->full_path.c_str()) != LFS_ERR_OK) {
                delete as_dir(*child);
                child->dir = nullptr;
                continue;
            }
            child->is_dir = true;
        } else {
            child->file = new lfs_file_t();
            if (lfs_file_open(as_lfs(*child), as_file(*child),
                              child->full_path.c_str(),
                              LFS_O_RDONLY) != LFS_ERR_OK) {
                delete as_file(*child);
                child->file = nullptr;
                continue;
            }
            child->is_dir = false;
        }
        child->open = true;
        return File(child);
    }
    return File();
}

void File::rewindDirectory()
{
    if (*this && _impl->is_dir) {
        lfs_dir_rewind(as_lfs(*_impl), as_dir(*_impl));
    }
}

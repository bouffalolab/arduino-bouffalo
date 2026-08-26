#include <errno.h>
#include <reent.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <string.h>

#include "lfs/port_file_fd.h"
#include "lfs/port_file_littlefs.h"

/* Trimmed newlib syscall layer for the Arduino platform.
 *
 * liblibc.a already provides strong _sbrk_r/_gettimeofday_r and weak
 * _read_r/_write_r aliases to the UART console; the SDK's full syscalls.c
 * cannot be linked as-is.  We provide the file/console dispatch that the
 * LittleFS POSIX layer needs:
 *
 *  - LittleFS fds (0x5000+) go to the LFS port (_*_file_lfs_r).
 *  - Console fds (0-2) keep the existing UART behavior by forwarding to
 *    liblibc's _console_write_r/_console_read_r, so newlib printf/getchar
 *    continue to work exactly as before.
 *  - Other tty operations (open/close/lseek) return the same errors the
 *    previous default stubs produced.
 */

/* Provided by liblibc.a (syscalls_simple_io.c) when CONFIG_CONSOLE_WO is off. */
extern ssize_t _console_write_r(struct _reent *reent, int fd, const void *ptr, size_t size);
extern ssize_t _console_read_r(struct _reent *reent, int fd, void *ptr, size_t size);

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
    return _console_read_r(reent, fd, ptr, len);
}

static _ssize_t tty_write_r(struct _reent *reent, int fd, const void *ptr, size_t len)
{
    return _console_write_r(reent, fd, ptr, len);
}

static int tty_fstat_r(struct _reent *reent, int fd, struct stat *st)
{
    (void)reent; (void)fd;
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

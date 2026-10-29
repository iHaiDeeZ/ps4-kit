// The *at file functions (fstatat, openat, unlinkat, mkdirat, renameat, faccessat, readlinkat,
// fchmodat, utimensat) on top of the path ones, and realpath, readlink, statvfs and fstatvfs.
// The OpenOrbis C library's fstatat, unlinkat, mkdirat, renameat, faccessat, readlinkat, fchmodat
// and utimensat only fail with ENOSYS (the PS4 has no such system calls for apps), and the
// sysroot's Boost.Filesystem (built with the POSIX *at APIs) and libc++'s std::filesystem go
// through them for every status, directory listing and removal: Vita3K v00.02 died at its first
// create_directories ("Function not implemented").
// With the linker's --wrap (ps4-kit.cmake) a name relative to a directory descriptor is resolved
// against the path that descriptor was opened on (open and openat remember each directory's until
// it's closed), and stat, lstat, open, unlink, rmdir, mkdir, rename, access, chmod and utimes,
// which the kernel has, do the work.
//
// The path functions themselves (open, close, stat, lstat, mkdir, rmdir, unlink, rename, access) are
// wrapped here as well: the network shares under /net (ps4_netfs.cpp) are answered from them, and
// every name the kernel gets is absolute and tidy (below).
//
// The working directory is kept here too: the C library's getcwd and fchdir are ENOSYS stubs, and
// the kernel turns relative names down even after a chdir (SSBU v00.04: "cannot open
// dump/modules/main.reloc.bin" after chdir("/data/SSBU")). chdir and fchdir set it, getcwd answers
// it ("/" until the app sets one), and the names relative to it are made absolute here.
//
// Every path made here is tidied by name ("." and ".." taken out): the PS4's file systems have no
// links, and the kernel walks ".." only through folders that exist (SSBU v00.25's saves went to
// "dump/modules/../../save" and were lost on consoles without dump/modules).

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

#include <limits.h>
#include <stdlib.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

#include "ps4_kit_io.h"

// ps4_crashlog.cpp's boot trace. A do-nothing stand-in when it isn't linked: a weak reference left
// undefined would become an import, and create-fself refuses those ("missing module for symbol").
extern "C" __attribute__((weak)) void ps4_boot_trace(const char*) {}

// Stand-ins for ps4_netfs.cpp when a build leaves the network shares out (ps4-kit.cmake
// PS4_KIT_NETFS=OFF): the wraps below still call them. ps4_netfs.cpp's own definitions win when it's
// linked.
extern "C" __attribute__((weak)) int ps4_netfs_is_path(const char*) {
    return 0;
}
extern "C" __attribute__((weak)) int ps4_netfs_open(const char*, int) {
    errno = ENOENT;
    return -1;
}
extern "C" __attribute__((weak)) int ps4_netfs_close(int) {
    return 0;
}
extern "C" __attribute__((weak)) int ps4_netfs_stat(const char*, struct stat*) {
    errno = ENOENT;
    return -1;
}

// ps4_crashlog.cpp's open-file tracking (a descriptor leak names its opener); do-nothing stand-ins
// when it isn't linked.
extern "C" __attribute__((weak)) void ps4_diag_note_open(int, const char*) {}
extern "C" __attribute__((weak)) void ps4_diag_note_close(int) {}

extern "C" {
int __real_open(const char* path, int flags, ...);
int __real_close(int fd);
int __real_stat(const char* path, struct stat* st);
int __real_lstat(const char* path, struct stat* st);
int __real_mkdir(const char* path, mode_t mode);
int __real_rmdir(const char* path);
int __real_unlink(const char* path);
int __real_rename(const char* from, const char* to);
int __real_access(const char* path, int mode);
int __real_chdir(const char* path);
}

namespace {

// The directories openat opened: descriptor -> path (made on first use: static constructors may
// list directories before this file's run)
struct Dirs {
    std::mutex lock;
    std::unordered_map<int, std::string> paths;
    std::string cwd = "/";  // the working directory (absolute, tidy)
};

Dirs& dirs() {
    static Dirs s_dirs;
    return s_dirs;
}

constinit std::atomic<int> s_count{0};

void remember(int fd, const std::string& path) {
    Dirs& d = dirs();
    std::lock_guard lock(d.lock);
    if (d.paths.insert_or_assign(fd, path).second)
        s_count++;
}

// An absolute path with ".", "..", "//" and a trailing "/" taken out ("/" stays "/").
std::string tidy(std::string full) {
    if (full.find("/.") == std::string::npos && full.find("//") == std::string::npos &&
        (full.size() <= 1 || full.back() != '/'))
        return full;  // already tidy: most paths
    std::string out;
    out.reserve(full.size());
    for (size_t i = 0; i < full.size();) {
        while (i < full.size() && full[i] == '/')
            i++;
        const size_t start = i;
        while (i < full.size() && full[i] != '/')
            i++;
        const size_t length = i - start;
        if (length == 0 || (length == 1 && full[start] == '.'))
            continue;
        if (length == 2 && full[start] == '.' && full[start + 1] == '.') {
            const size_t slash = out.rfind('/');
            out.resize(slash == std::string::npos ? 0 : slash);
            continue;
        }
        out += '/';
        out.append(full, start, length);
    }
    return out.empty() ? "/" : out;
}

void traceBadDescriptor(int dirfd, const char* path) {
    static std::atomic<int> s_logged{0};
    if (s_logged.fetch_add(1) < 8) {
        char line[200];
        snprintf(line, sizeof(line), "fileat: descriptor %d isn't a directory openat opened (%s)", dirfd, path);
        ps4_boot_trace(line);
    }
}

// The path a name relative to dirfd stands for; false (with errno) when dirfd isn't a directory
// openat opened.
bool resolve(int dirfd, const char* path, std::string& out) {
    if (!path) {
        errno = EFAULT;
        return false;
    }
    if (dirfd == AT_FDCWD && !*path) {
        out.clear();  // the kernel's answer for "" (ENOENT)
        return true;
    }
    if (path[0] == '/') {
        out = tidy(path);
        return true;
    }
    Dirs& d = dirs();
    if (dirfd == AT_FDCWD) {
        // The kernel turns relative names down (EINVAL, where elsewhere they'd simply not be
        // there): Vita3K v00.03 died on Boost's status("ux0/app"). They're the working directory's.
        std::string full;
        {
            std::lock_guard lock(d.lock);
            full = d.cwd;
        }
        full += '/';
        full += path;
        out = tidy(std::move(full));
        return true;
    }
    {
        std::lock_guard lock(d.lock);
        const auto it = d.paths.find(dirfd);
        if (it != d.paths.end()) {
            std::string full = it->second;
            if (*path) {
                full += '/';
                full += path;
            }
            out = tidy(std::move(full));
            return true;
        }
    }
    traceBadDescriptor(dirfd, path);
    errno = EBADF;
    return false;
}

// The path to hand the kernel: as it is when it's absolute and tidy (nearly every call: no copy),
// else made so in keep. nullptr (with errno) for a NULL path; "" stays "" (the kernel's ENOENT).
const char* kernelPath(const char* path, std::string& keep) {
    if (!path) {
        errno = EFAULT;
        return nullptr;
    }
    if (!*path || (path[0] == '/' && !strstr(path, "/.") && !strstr(path, "//")))
        return path;
    resolve(AT_FDCWD, path, keep);  // (can't fail for AT_FDCWD)
    return keep.c_str();
}

// statvfs: the C library's asks the kernel (fstatfs) and then fills nothing in, so Boost's space()
// (games asking how much room there is) got whatever was on the stack.
extern "C" int _fstatfs(int fd, void* buf);  // libkernel

int statfsInto(int fd, struct statvfs* out) {
    // The kernel's struct statfs (FreeBSD's): the counts come after version, type and flags. The
    // rest is as long as FreeBSD 12's, whichever size the kernel writes.
    struct KernelStatfs {
        uint32_t version;
        uint32_t type;
        uint64_t flags;
        uint64_t bsize;  // the unit of the block counts
        uint64_t iosize;
        uint64_t blocks;
        uint64_t bfree;
        int64_t bavail;
        uint64_t files;
        int64_t ffree;
        uint8_t rest[2344 - 72];
    };
    KernelStatfs fs{};
    if (_fstatfs(fd, &fs) != 0)
        return -1;
    memset(out, 0, sizeof(*out));
    out->f_bsize = out->f_frsize = fs.bsize;
    out->f_blocks = fs.blocks;
    out->f_bfree = fs.bfree;
    out->f_bavail = fs.bavail > 0 ? fs.bavail : 0;
    out->f_files = fs.files;
    out->f_ffree = out->f_favail = fs.ffree > 0 ? fs.ffree : 0;
    out->f_namemax = 255;
    return 0;
}

}  // namespace

extern "C" int ps4_kit_tidy_path(const char* path, char* out, size_t size) {
    std::string full;
    if (!resolve(AT_FDCWD, path, full))
        return -1;
    if (full.size() >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, full.c_str(), full.size() + 1);
    return static_cast<int>(full.size());
}

// --- The path functions themselves ---------------------------------------------------------------
// Relative and untidy names made absolute and tidy, /net paths to the network shares
// (ps4_netfs.cpp), the rest to the kernel. ps4_crashlog.cpp's weak open/close wraps give way to
// these, which report to its open-file tracking themselves.

extern "C" __attribute__((noinline)) int __wrap_open(const char* path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    const int mode = va_arg(args, int);  // always passed on: harmless when the call had none
    va_end(args);
    std::string keep;
    const char* full = kernelPath(path, keep);
    if (!full)
        return -1;
    const int fd = ps4_netfs_is_path(full) ? ps4_netfs_open(full, flags) : __real_open(full, flags, mode);
    if (fd >= 0) {
        // a directory, for the *at functions relative to it
        if ((flags & O_DIRECTORY) && *full)
            remember(fd, full);
        ps4_diag_note_open(fd, full);
    }
    return fd;
}

extern "C" int __wrap_close(int fd) {
    // (first: the number may be handed out again as soon as it's closed)
    ps4_fileat_closed(fd);
    ps4_diag_note_close(fd);
    if (ps4_netfs_close(fd))
        return 0;
    return __real_close(fd);
}

extern "C" int __wrap_stat(const char* path, struct stat* st) {
    std::string keep;
    const char* full = kernelPath(path, keep);
    if (!full)
        return -1;
    return ps4_netfs_is_path(full) ? ps4_netfs_stat(full, st) : __real_stat(full, st);
}

// (the shares have no links: their stat is their lstat)
extern "C" int __wrap_lstat(const char* path, struct stat* st) {
    std::string keep;
    const char* full = kernelPath(path, keep);
    if (!full)
        return -1;
    return ps4_netfs_is_path(full) ? ps4_netfs_stat(full, st) : __real_lstat(full, st);
}

namespace {
// The shares are read-only; changing anything there is EROFS.
template <typename Kernel>
int change(const char* path, Kernel kernel) {
    std::string keep;
    const char* full = kernelPath(path, keep);
    if (!full)
        return -1;
    if (ps4_netfs_is_path(full)) {
        errno = EROFS;
        return -1;
    }
    return kernel(full);
}
}  // namespace

extern "C" int __wrap_mkdir(const char* path, mode_t mode) {
    return change(path, [&](const char* full) { return __real_mkdir(full, mode); });
}

extern "C" int __wrap_rmdir(const char* path) {
    return change(path, [](const char* full) { return __real_rmdir(full); });
}

extern "C" int __wrap_unlink(const char* path) {
    return change(path, [](const char* full) { return __real_unlink(full); });
}

extern "C" int __wrap_rename(const char* from, const char* to) {
    std::string keep;
    const char* fullTo = kernelPath(to, keep);
    if (!fullTo)
        return -1;
    if (ps4_netfs_is_path(fullTo)) {
        errno = EROFS;
        return -1;
    }
    return change(from, [&](const char* fullFrom) { return __real_rename(fullFrom, fullTo); });
}

extern "C" int __wrap_access(const char* path, int mode) {
    std::string keep;
    const char* full = kernelPath(path, keep);
    if (!full)
        return -1;
    if (ps4_netfs_is_path(full)) {
        // read-only, and the kernel doesn't know them
        struct stat st;
        if (ps4_netfs_stat(full, &st) != 0)
            return -1;
        if (mode & W_OK) {
            errno = EROFS;
            return -1;
        }
        return 0;
    }
    return __real_access(full, mode);
}

// getcwd: the C library's only fails (ENOSYS); the working directory kept here. A NULL buffer gets
// one allocated, as in musl and glibc.
extern "C" char* __wrap_getcwd(char* buf, size_t size) {
    std::string cwd;
    {
        Dirs& d = dirs();
        std::lock_guard lock(d.lock);
        cwd = d.cwd;
    }
    if (!buf) {
        size = size ? size : cwd.size() + 1;
        buf = static_cast<char*>(malloc(size));
        if (!buf) {
            errno = ENOMEM;
            return nullptr;
        }
    } else if (size == 0) {
        errno = EINVAL;
        return nullptr;
    }
    if (cwd.size() + 1 > size) {
        errno = ERANGE;
        return nullptr;
    }
    memcpy(buf, cwd.c_str(), cwd.size() + 1);
    return buf;
}

namespace {
int changeDirectory(const std::string& full) {
    struct stat st;
    if (stat(full.c_str(), &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    Dirs& d = dirs();
    std::lock_guard lock(d.lock);
    d.cwd = full;
    return 0;
}
}  // namespace

extern "C" int __wrap_chdir(const char* path) {
    std::string full;
    if (!resolve(AT_FDCWD, path, full))
        return -1;
    if (changeDirectory(full) != 0)
        return -1;
    // the kernel's too (libkernel), though it doesn't use it for names; a share under /net isn't
    // one of its folders, so its answer doesn't count
    __real_chdir(full.c_str());
    return 0;
}

extern "C" int __wrap_fchdir(int fd) {
    std::string full;
    if (!resolve(fd, "", full))
        return -1;
    return changeDirectory(full);
}

extern "C" int __wrap_fstatat(int dirfd, const char* path, struct stat* st, int flags) {
    if (path && !*path && (flags & AT_EMPTY_PATH))
        return fstat(dirfd, st);
    std::string full;
    if (!resolve(dirfd, path, full))
        return -1;
    return (flags & AT_SYMLINK_NOFOLLOW) ? lstat(full.c_str(), st) : stat(full.c_str(), st);
}

extern "C" int __wrap_openat(int dirfd, const char* path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    std::string full;
    if (!resolve(dirfd, path, full))
        return -1;
    const int fd = open(full.c_str(), flags, mode);
    if (fd < 0)
        return fd;
    // Remembered when it's a directory. Only a read-only open without O_CREAT can be one, so a file
    // opened to be written (std::filesystem::copy_file) costs no extra fstat.
    if (flags & O_DIRECTORY) {
        remember(fd, full);
    } else if ((flags & O_ACCMODE) == O_RDONLY && !(flags & O_CREAT)) {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode))
            remember(fd, full);
    }
    return fd;
}

// From __wrap_open (ps4_crashlog.cpp): a directory opened by path. Boost opens the top of a
// directory walk that way (its open_directory), then each folder in it relative to its parent's
// descriptor: Vita3K v00.07 died walking an app folder ("Bad file descriptor").
extern "C" void ps4_fileat_opened(int fd, const char* path) {
    std::string full;
    if (resolve(AT_FDCWD, path, full))
        remember(fd, full);
}

extern "C" int __wrap_unlinkat(int dirfd, const char* path, int flags) {
    std::string full;
    if (!resolve(dirfd, path, full))
        return -1;
    return (flags & AT_REMOVEDIR) ? rmdir(full.c_str()) : unlink(full.c_str());
}

extern "C" int __wrap_mkdirat(int dirfd, const char* path, mode_t mode) {
    std::string full;
    if (!resolve(dirfd, path, full))
        return -1;
    return mkdir(full.c_str(), mode);
}

extern "C" int __wrap_renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath) {
    std::string from, to;
    if (!resolve(olddirfd, oldpath, from) || !resolve(newdirfd, newpath, to))
        return -1;
    return rename(from.c_str(), to.c_str());
}

// (AT_EACCESS changes nothing: apps have one identity on the PS4)
extern "C" int __wrap_faccessat(int dirfd, const char* path, int mode, int flags) {
    (void)flags;
    std::string full;
    if (!resolve(dirfd, path, full))
        return -1;
    return access(full.c_str(), mode);
}

// (no lchmod or lutimes on the PS4: links are followed, there are none in its file systems)
extern "C" int __wrap_fchmodat(int dirfd, const char* path, mode_t mode, int flags) {
    (void)flags;
    std::string full;
    if (!resolve(dirfd, path, full))
        return -1;
    return chmod(full.c_str(), mode);
}

extern "C" int __wrap_utimensat(int dirfd, const char* path, const struct timespec times[2], int flags) {
    (void)flags;
    std::string full;
    if (!resolve(dirfd, path, full))
        return -1;
    if (!times)
        return utimes(full.c_str(), nullptr);
    struct timeval tv[2];
    struct stat st;
    bool have_st = false;
    for (int i = 0; i < 2; i++) {
        if (times[i].tv_nsec == UTIME_NOW) {
            gettimeofday(&tv[i], nullptr);
        } else if (times[i].tv_nsec == UTIME_OMIT) {
            if (!have_st) {
                if (stat(full.c_str(), &st) != 0)
                    return -1;
                have_st = true;
            }
            const struct timespec& kept = i == 0 ? st.st_atim : st.st_mtim;
            tv[i].tv_sec = kept.tv_sec;
            tv[i].tv_usec = kept.tv_nsec / 1000;
        } else {
            tv[i].tv_sec = times[i].tv_sec;
            tv[i].tv_usec = times[i].tv_nsec / 1000;
        }
    }
    return utimes(full.c_str(), tv);
}

// realpath: the C library's opens the path and reads the link /proc/self/fd/N with readlink,
// which it only has as an ENOSYS stub (the PS4 has neither), so libc++'s canonical and
// weakly_canonical failed: psvpfstools' fs::relative, decrypting a NoNpDrm dump in Vita3K v00.08.
// The path made absolute and tidied (".", "..", "//") is it, if it's there.
extern "C" char* __wrap_realpath(const char* path, char* resolved) {
    if (!path) {
        errno = EINVAL;
        return nullptr;
    }
    if (!*path) {
        errno = ENOENT;
        return nullptr;
    }
    std::string full;
    if (!resolve(AT_FDCWD, path, full))
        return nullptr;
    struct stat st;
    if (stat(full.c_str(), &st) != 0)
        return nullptr;
    if (full.size() >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return nullptr;
    }
    if (!resolved) {
        resolved = static_cast<char*>(malloc(full.size() + 1));
        if (!resolved) {
            errno = ENOMEM;
            return nullptr;
        }
    }
    memcpy(resolved, full.c_str(), full.size() + 1);
    return resolved;
}

// readlink: an ENOSYS stub in the C library; the PS4's file systems have no links, so a file that's
// there isn't one (EINVAL), which is what callers expect of a plain file
namespace {
ssize_t notALink(int dirfd, const char* path) {
    std::string full;
    if (!path || !resolve(dirfd, path, full))
        return -1;
    struct stat st;
    if (lstat(full.c_str(), &st) == 0)
        errno = EINVAL;
    return -1;
}
}  // namespace

extern "C" ssize_t __wrap_readlink(const char* path, char* buf, size_t size) {
    (void)buf;
    (void)size;
    return notALink(AT_FDCWD, path);
}

extern "C" ssize_t __wrap_readlinkat(int dirfd, const char* path, char* buf, size_t size) {
    (void)buf;
    (void)size;
    return notALink(dirfd, path);
}

extern "C" int __wrap_statvfs(const char* path, struct statvfs* out) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    const int result = statfsInto(fd, out);
    const int saved = errno;
    close(fd);
    errno = saved;
    return result;
}

extern "C" int __wrap_fstatvfs(int fd, struct statvfs* out) {
    return statfsInto(fd, out);
}

// From __wrap_close (ps4_crashlog.cpp), before the descriptor goes: it no longer names a directory.
extern "C" void ps4_fileat_closed(int fd) {
    if (s_count.load(std::memory_order_relaxed) == 0)
        return;
    Dirs& d = dirs();
    std::lock_guard lock(d.lock);
    if (d.paths.erase(fd))
        s_count--;
}

// mkdirat, renameat, faccessat and readlinkat are defined outright rather than wrapped: no system
// library the app links exports them (mkdirat and renameat are only in libkernel_sys), and a --wrap
// leaves the wrapped name in the dynamic symbol table, which create-fself then refuses ("missing
// module for symbol (mkdirat)"). Defined here, the C library's ENOSYS members are never pulled in.
extern "C" int mkdirat(int dirfd, const char* path, mode_t mode) __attribute__((alias("__wrap_mkdirat")));
extern "C" int renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath)
    __attribute__((alias("__wrap_renameat")));
extern "C" int faccessat(int dirfd, const char* path, int mode, int flags) __attribute__((alias("__wrap_faccessat")));
extern "C" ssize_t readlinkat(int dirfd, const char* path, char* buf, size_t size)
    __attribute__((alias("__wrap_readlinkat")));

// ps4-kit: the file and network layer's public API (ps4_net.cpp, ps4_netfs.cpp, ps4_fileat.cpp,
// ps4_modules.cpp, ps4_bsd.c). Plain C, callable from C and C++.
//
// Most of the layer needs no call at all: it works through the linker's --wrap (getaddrinfo, the
// path functions, the file reads, the *at functions; the list is in
// docs/merge-notes/io-toolchain.md). What is here: the menus' calls for network shares, the hooks
// between the layer's files, and a few helpers an app may want directly.

#ifndef PS4_KIT_IO_H
#define PS4_KIT_IO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Host names (ps4_net.cpp) ------------------------------------------------------------------

// The IPv4 address of name as text ("a.b.c.d"), through libSceNet's resolver (the C library has no
// DNS on the PS4). Answers are kept for a minute, failures for 5 s. 1 when found, 0 when not.
int ps4_net_resolve(const char* name, char* text, size_t size);

// Forgets the kept answers (the network changed, a server moved).
void ps4_net_forget(void);

// --- Network shares under /net (ps4_netfs.cpp) -------------------------------------------------

// Mounts url (smb://[domain;][user@]host/share[/folder] or nfs://host/export[/folder]) read-only at
// /net/<name>; an empty or NULL url takes it away. user/password: SMB only, NULL or "" for guest.
// Connects on first use; mounting the same address and identity again keeps the connection.
void ps4_netfs_mount(const char* name, const char* url, const char* user, const char* password);

// Connects now (if it isn't) and lists the share: 1 and "Connected: 12 items" in message, or 0 and
// why not ("Not set", the server's error).
int ps4_netfs_check(const char* name, char* message, size_t size);

// Forgets what was learned about the shares' folders and files (a launcher's refresh).
void ps4_netfs_forget(void);

// Hooks for the path wraps in ps4_fileat.cpp (__wrap_open, __wrap_close, __wrap_stat), which hand
// /net paths over already absolute and tidy. Without ps4_netfs.cpp in the build (ps4-kit.cmake
// PS4_KIT_NETFS=OFF) weak stand-ins in ps4_fileat.cpp answer "not a share path".
struct stat;
int ps4_netfs_is_path(const char* path);              // 1 for /net and below
int ps4_netfs_open(const char* path, int flags);        // a descriptor, or -1 with errno
int ps4_netfs_close(int fd);                           // 1 when fd was a share's file (now closed)
int ps4_netfs_stat(const char* path, struct stat* st);  // 0, or -1 with errno

// --- Paths (ps4_fileat.cpp) --------------------------------------------------------------------

// ps4_fileat.cpp's own __wrap_open/__wrap_close call these; they're public for a port that keeps an
// open/close wrap of its own instead. A directory was opened by path (O_DIRECTORY), so names
// relative to fd resolve / fd no longer names a directory (call before the real close).
void ps4_fileat_opened(int fd, const char* path);
void ps4_fileat_closed(int fd);

// Writes path absolute (relative names are the working directory's) and tidied - ".", "..", "//"
// and a trailing "/" taken out by name, as the PS4's file systems have no links - into out.
// The kernel resolves ".." itself only through folders that exist: "dump/modules/../../save"
// failed on consoles without dump/modules and nothing was saved (SSBU v00.25). Returns the length,
// or -1 with errno (ENAMETOOLONG when it doesn't fit in size).
int ps4_kit_tidy_path(const char* path, char* out, size_t size);

// --- System modules (ps4_modules.cpp) ----------------------------------------------------------

// Loads a system module (ORBIS_SYSMODULE_INTERNAL_* id); a failure goes to the boot trace with
// name. Returns sceSysmoduleLoadModuleInternal's result (0: loaded or already there).
int ps4_kit_load_module(uint32_t id, const char* name);

// Optional, defined by the app (ps4_modules.cpp has an empty weak default): more modules to load before main, after the kit's own
// (SystemService, UserService, VideoOut, AudioOut, Pad). Return the array and set *count, e.g.
//   static const uint32_t ids[] = {0x8000001C /* ORBIS_SYSMODULE_INTERNAL_NET */};
//   const uint32_t* ps4_kit_extra_modules(size_t* count) { *count = 1; return ids; }
// (Calling into a system library whose module isn't loaded ends the app without a word.)
const uint32_t* ps4_kit_extra_modules(size_t* count);

#ifdef __cplusplus
}
#endif

#endif

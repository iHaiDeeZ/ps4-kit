# Merge notes: file/network layer and toolchain

Merged from the 12 ports' copies on 2026-10-25. Nothing in the ports was changed.

## Runtime (`runtime/`)

### ps4_net.cpp: host names through libSceNet's resolver

| Copy | Ports | What it had |
|---|---|---|
| 99 lines | CEMUPS4, Resonance, rcps3, xemu-ps4 | `__wrap_getaddrinfo` + `__wrap_gethostbyname` (calls `__real_gethostbyname` with the numeric address) |
| 125 lines | March33 | + AI_ADDRCONFIG/AI_V4MAPPED dropped from hints; `gethostbyname` defined outright |
| 125 lines | Conker = Vita3K, SMW = SSBU | same as March33; only the pool name and the comment prefixes differ |

Kit version = the 125-line one, plus:

- **Answers kept** (32 names, 60 s found / 5 s not found). netfs' reachability check and games' reconnect loops asked for the same name each time; a name without an answer cost 2 x 5 s every time.
- **`localhost`** answers 127.0.0.1 without the resolver (musl looks in an /etc/hosts the PS4 doesn't have).
- `gethostbyname` defined outright (M33: libc.a's imports a symbol no console module has, and create-fself refuses the link). A `__wrap_gethostbyname` is defined too, on the same internal lookup, so a build that still passes `--wrap=gethostbyname` (Crema's cmake) links. Neither calls the other through the symbol: with that wrap in force the call would come back.
- `ps4_net_resolve` / `ps4_net_forget` are public (ps4_kit_io.h).
- Pool name "ps4kit-dns" (was crema-/v3k-/m33-/smw-dns).
- The boot trace is reached through a weak do-nothing default definition, not a weak reference (see "Weak symbols" below).

### ps4_netfs.cpp: SMB/NFS shares under /net (from Crema)

All 7 copies (CEMUPS4, Conker, March33, Resonance, SMW, SSBU, Vita3K) were identical (md5 f1fb3325). Kit changes:

- **Sequential read-ahead**: a miss on the block right after the last ones fetched asks for 2, then 4 blocks (1 MiB) in one request; each extra block goes into the cache by itself. A random miss starts over at one block. Streaming a disc image in small reads was one server round trip per 256 KiB. The state lives in the file and is guarded by the mount's lock (`pread` doesn't take the file's lock).
- `keepBlock` replaces an existing copy of the same block instead of adding a duplicate (two threads missing the same block filled two slots).
- **Build switches** `PS4_KIT_NETFS_SMB2=0` / `PS4_KIT_NETFS_NFS=0` leave a protocol and its library out; mounting such an address fails with "SMB/NFS isn't in this build" in the menu's status. The whole file stays optional via ps4-kit.cmake's `PS4_KIT_NETFS`.
- `stat` is no longer wrapped here: ps4_fileat.cpp owns every path wrap and calls the new hook `ps4_netfs_stat` for /net paths (already absolute and tidy).
- `ps4_netfs_mount` refuses a NULL/empty name or one containing "/" (it was used as a map key and a path part).
- Weak boot-trace default (as above).

**Libraries**: `libsmb2.a` and `libnfs.a` (Ronnie Sahlberg, LGPL-2.1), headers `<smb2/smb2.h>`, `<smb2/libsmb2.h>`, `<nfsc/libnfs.h>` in `$PS4_SYSROOT/include`, archives in `$PS4_SYSROOT/lib`. Where the ports get them:

- CEMUPS4 `scripts/build-deps.sh` (`build_libsmb2`, `build_libnfs`): libsmb2 v6.0.0 and libnfs 8.0.0 tarballs, built with ps4-deps.cmake. Its libsmb2 patches matter: the PS4 branch's own `CMAKE_C_FLAGS` removed, `__bswapNN` left alone on OpenOrbis, and **libsmb2's AES functions renamed** (`smb2_AES128_*`): Cemu has a variable `AES128_ECB_encrypt`, and SMB3 signing jumped into data right after login. Any app with its own AES symbols needs that rename.
- Resonance `scripts/build-netfs.sh`: git clones (commits in Resonance's patches/BASE.txt), `-DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF`, the bswap fix as `-D` defines.
- Conker and SSBU link `${PS4_SYSROOT}/lib/libnfs.a ${PS4_SYSROOT}/lib/libsmb2.a` in their cmake; HENkaku's sysroot (`~/vita3k-build/sysroot`, the kit's seed) has both, so the seeded kit sysroot has them.
- Recommendation: put CEMUPS4's two build functions (with the AES rename) into the kit's deps script when one exists.

### ps4_fileat.cpp: *at functions, paths, working directory

Conker and Vita3K copies were identical (md5 85c4cb73). Kit changes:

- **More *at functions**: `mkdirat`, `renameat`, `faccessat`, `readlinkat` (all ENOSYS stubs in OpenOrbis libc.a; listed by scanning libc.a for the ENOSYS stub pattern, as xemu's trim-libc.py does), and **`fstatvfs`** (same member as the broken statvfs).
- **Path wraps moved here**: `open`, `close` (strong; ps4_crashlog.cpp's weak ones give way, as its comment expects, and these call `ps4_diag_note_open/close` themselves), `stat` (from netfs), and new `lstat`, `mkdir`, `rmdir`, `unlink`, `rename`, `access`. They route /net paths to netfs (read-only: EROFS for changes, `access` answered from netfs' stat) and the rest to the kernel.
- **Every path the kernel gets is absolute and tidy** ("." and ".." removed by name). The kernel only walks ".." through folders that exist: SSBU v00.25's saves under `dump/modules/../../save` were lost on consoles without `dump/modules`. The PS4's file systems have no links, so by-name is exact. Fast path: an absolute path without "/." or "//" is passed through without a copy.
- **Working directory**: libc's `getcwd` and `fchdir` are ENOSYS stubs (disassembled: getcwd always returns NULL), and the kernel refuses relative names even after `chdir` (SSBU v00.04). New wraps `getcwd`, `chdir`, `fchdir` keep it here ("/" until set; `chdir` also calls the kernel's), and relative names anywhere in the layer resolve against it. Before, relative names always resolved against "/" (getcwd always failed).
- `openat` skips its fstat for opens that can't be a directory (write access or O_CREAT) and for O_DIRECTORY.
- `ps4_kit_tidy_path` public.
- Weak stand-ins for netfs' hooks (`ps4_netfs_is_path/open/close/stat`) and for `ps4_diag_note_open/close`, so the file links without ps4_netfs.cpp or ps4_crashlog.cpp.

**Link order requirement** (unchanged, but now load-bearing for more functions): `stat`, `lstat`, `access`, `mkdir`, `rename`, `unlink`, `rmdir`, `chmod`, `utimes`, `chdir`, `open`, `close`, `_fstatfs` must bind to **libkernel** (`-lkernel` before `-lc`, as ps4.cmake and bin/ps4-cc do). libc.a's `lstat` is musl's, which calls `fstatat`: with `--wrap=fstatat` that would recurse through `__wrap_fstatat` -> `lstat`. Same for libc's `utime`/`futimesat` -> `utimensat`.

### ps4_modules.cpp: system modules before main

Conker, SSBU, Vita3K identical (md5 5b525086). Kept the five modules (SystemService, UserService, VideoOut, AudioOut, Pad) at constructor priority 102. Generalized:

- `ps4_kit_load_module(id, name)`: load one later, failures to the boot trace.
- `ps4_kit_extra_modules(size_t* count)`: optional app hook (weak empty default in ps4_modules.cpp) for more modules before main.

### ps4_bsd.c: getpagesizes (rcps3 only)

Judged generic: a FreeBSD libc function libkernel/musl lack, 16 KiB + 2 MiB page sizes are PS4 facts, no state. Its declaration went into prepare-sysroot.sh's `sys/mman.h` (rcps3 did that in rpcs3-overlay.sh). ps4-kit.cmake's source list needs `runtime/ps4_bsd.c` added.

### ps4_kit_io.h

Public API: `ps4_net_resolve`, `ps4_net_forget`, `ps4_netfs_mount`, `ps4_netfs_check`, `ps4_netfs_forget`, hooks `ps4_netfs_is_path/open/close/stat`, `ps4_fileat_opened/closed`, `ps4_kit_tidy_path`, `ps4_kit_load_module`, `ps4_kit_extra_modules`.

### Weak symbols

An undefined weak *reference* that nothing defines becomes a dynamic import, and create-fself then refuses the eboot ("missing module for symbol (ps4_boot_trace)": seen in this merge's test link). So these files use weak *default definitions* (`ps4_boot_trace`, the netfs hooks, `ps4_diag_note_*`, `ps4_kit_extra_modules`) that a strong definition overrides. FYI for ps4_runtime.cpp: it declares `ps4_boot_trace` as a weak reference; fine while ps4_crashlog.cpp is always linked, same failure otherwise. This relies on the runtime being an OBJECT library (as ps4-kit.cmake does): from a static archive the weak defaults could satisfy the references and the real files never be pulled.

## Every `__wrap_` the runtime files define

All must have a matching `--wrap=` on the app link, and every `--wrap=` below must be passed when its file is linked: the files reference `__real_<fn>`, which stays undefined without the flag.

| File | Wraps |
|---|---|
| ps4_net.cpp | `getaddrinfo`; `gethostbyname` defined outright (`__wrap_gethostbyname` only for builds still passing that flag: don't add it) |
| ps4_netfs.cpp | `read`, `_read`, `readv`, `_readv`, `pread`, `lseek`, `fstat`, `getdents` |
| ps4_fileat.cpp | `open`, `close`, `stat`, `lstat`, `mkdir`, `rmdir`, `unlink`, `rename`, `access`, `getcwd`, `chdir`, `fchdir`, `fstatat`, `openat`, `unlinkat`, `mkdirat`, `renameat`, `faccessat`, `readlinkat`, `fchmodat`, `utimensat`, `realpath`, `readlink`, `statvfs`, `fstatvfs` |

Changes needed in ps4-kit.cmake's lists (I didn't edit it):

- `PS4_KIT_APP_WRAPS` (always): add `stat lstat mkdir rmdir unlink rename access getcwd chdir fchdir mkdirat renameat faccessat readlinkat fstatvfs`.
- netfs-only list: remove `stat` (now in fileat, always).
- `getaddrinfo` sits in the process-wide list, but `__wrap_getaddrinfo` is only in the app's runtime objects: a CMake probe program that calls getaddrinfo would fail to link. Moving it to the app-only list is safer.
- Sources: add `runtime/ps4_bsd.c`. Libraries when `PS4_KIT_NETFS`: `${PS4_SYSROOT}/lib/libnfs.a ${PS4_SYSROOT}/lib/libsmb2.a`. ps4_net.cpp and ps4_modules.cpp need `-lSceNet -lSceSysmodule` (ps4-deps.cmake already adds both).

Checked: all files compile with clang 21 / libc++ 21 (`-Wall -Wextra` clean, netfs in all four switch combinations); a test program with all five files, libsmb2/libnfs and every wrap above links, and create-fself accepts it.

## Toolchain (`toolchain/`)

| File | Source | Kit changes |
|---|---|---|
| prepare-sysroot.sh | identical in 10 ports | installs the checked-in `libc-overlay/` at the end (was CEMUPS4/rpcs3 `scripts/install-overlay.sh`, which DolphinPS4's tree skipped), then runs ucontext-overlay.sh; `getpagesizes` in `sys/mman.h`; the 0x30 pad generated into `bits/signal.h` directly; DolphinPS4's two-line `sys/socket.h` dropped (the checked-in full header, which has SO_NOSIGPIPE, overwrote it anyway). Generated headers' md5s match the ports' sysroots except those changes. |
| ucontext-overlay.sh | Conker/Vita3K (differ only in PSB), = m33/smw/ssbu/rpcs3-overlay.sh's ucontext part | any port's pad name counts as done (each script only knew its own, so a sysroot seeded from another port's tree failed its assert: tested with SSBU's and DolphinPS4's trees); PSB from env.sh, no hard-coded build root; `mktemp` check file. |
| libc-overlay/ | 5 files identical in 9 ports | + `elf.h` (rpcs3: FreeBSD `Elf*_Auxinfo` names), + `sys/event.h` (xemu: kqueue/kevent, in libkernel), + `dirent.h` (xemu: `d_ino` alias for the PS4's `d_fileno`; kit adds: `_DIRENT_HAVE_D_OFF` removed, the struct has no d_off; `_DIRENT_HAVE_D_NAMLEN` added) |
| bin/ps4-cc, ps4-c++ | xemu's variant | bare `-v` only is version (CMake's ABI probe links with -v), `-shared` writes an empty file, hooks `PS4_CRT1`, `PS4_APP_LDFLAGS`, `PS4_APP_LDFLAGS_END`, `PS4_RT_LIB`, new `PS4_LINK_SCRIPT`; link script from `toolchain/ps4-link.x` (xemu generated one into the sysroot) |
| ps4-link.x | identical in 10 | + `.data.*`, `.tdata.*`, `.tbss.*` collected (ps4-kit.cmake's fix; tested: `.tdata.t` was an orphan next to .rodata with the old script, now inside `.tls`). Also keeps PSChrome's `.fini_array`/`.ctors`, which ps4-kit.cmake's generated script lacks: worth using this file there. |
| check-oelf.cmake | SMW's variant (`$ENV{SMW_LLVM}/bin`) | hints generalized to `$ENV{PS4_LLVM}/bin`, `$ENV{OPENORBIS}/bin`, `llvm-readelf-$ENV{CLANG_VER}`, only when set; tested on HENkaku v01.13's OELF |
| patch-crt1.sh | identical in 12 | `llvm-objcopy-${CLANG_VER:-21}`; note that ps4-kit.cmake's ps4_crt1.S supersedes it |
| build-libcxx.sh | identical in 10 | `LLVM_VER` overridable |
| build-runtime.sh, runtime/ps4_malloc.c | identical in 10 | unchanged (`libdolphinps4_rt.a` name kept: ps4-deps.cmake and ps4-cc refer to it) |

Left out (app-specific): xemu's `closefrom` declaration and libps4compat.a/libresolv.a/trim-libc.py/make-link-script.py (its own runtime), xemu's `SCM_CREDS`/`struct cmsgcred` (GIO only) and `SOCK_CLOEXEC`/`SOCK_NONBLOCK` (PSChrome's socket.h deliberately leaves `SOCK_CLOEXEC` undefined: the FreeBSD 9 kernel refuses the flags, and code that doesn't retry without them would lose its sockets), xemu ps4-cc's `-lSceNet` and `libps4compat.a` (pass through `PS4_APP_LDFLAGS[_END]`).

### env.sh variables the scripts use

Required: `PSB`, `OPENORBIS`, `PS4_SYSROOT`, `PS4_TARGET`, `CLANG_VER`, `PS4_CC`, `PS4_CXX`, `PS4_AR`, `PS4_RANLIB`, `PS4_BASE_FLAGS`, `JOBS`, `need()`. All are in the current kit env.sh. `PS4_LD`, `PS4_CXX_FLAGS` aren't used by these scripts (ps4-cc builds its C++ flags itself).

Optional (read from the environment): `LLVM_VER`, `LLVM_LOCAL_SRC` (build-libcxx.sh), `PS4_LLVM` (check-oelf.cmake), `PS4_CRT1`, `PS4_APP_LDFLAGS`, `PS4_APP_LDFLAGS_END`, `PS4_RT_LIB`, `PS4_LINK_SCRIPT` (bin/ps4-cc).

Note for the compile check in the brief: `$PS4_CXX_FLAGS` has to come before `$PS4_BASE_FLAGS` (libc++'s headers ahead of the C overlay), or `<cstddef>` errors out.

## Open questions / risks

- **Behaviour change, relative paths**: once an app calls `chdir`, relative names resolve against it instead of "/". Any port that called chdir and still relied on "/" would change. Absolute paths with ".." now go to the kernel tidied (fixes SSBU's case; differs from POSIX only through missing intermediate folders, which is the point).
- **open() now allocates** (a std::string) for relative or untidy paths; absolute tidy paths don't. An open from inside a signal handler with such a path would call malloc. ps4_crashlog.cpp uses `__real_open` for its own files.
- Read-ahead is untested on hardware. Worst case it reads up to 768 KiB more than needed at the end of a sequential run.
- The DNS failure cache (5 s) means an app retrying right after the network comes up can see one more failure.
- `getnameinfo` without NI_NUMERICHOST: musl would try DNS on 127.0.0.1:53 (no resolv.conf) and wait out its timeouts. Not seen on hardware, not wrapped.
- Not wrapped yet (would complete the relative/tidy/net path story): `chmod`, `utimes`. Covered through `open`: `opendir` (musl calls `open`) and `fopen`/std::ifstream (OpenOrbis' stdio reaches `open`; netfs already serves std::ifstream that way).
- create-fself's duplicated data PT_LOAD appeared on the merge's test link with both the old and new ps4-link.x, so it's the known size-dependent create-fself bug, not the script change; check-oelf caught it.

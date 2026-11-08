# Shared settings for the kit's PS4 toolchain (clang 21 + libc++ 21 on top of the OpenOrbis C
# runtime). Source this from the other scripts.
#
# The build tree lives in WSL (~/ps4kit-build), never on the Windows drive: builds there are several
# times faster, and toolchain flags (linker script paths) break on the space in "PS4 Projects", so the
# kit is also reached through the space-free link ~/ps4-kit (scripts/seed-build-tree.sh makes both).

export PS4_KIT="${PS4_KIT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
export PSB="${PSB:-$HOME/ps4kit-build}"
export OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"
export PS4_SYSROOT="${PS4_SYSROOT:-$PSB/sysroot}"                    # our libc++, ICU, ... install here
export PS4_MESA_BUILD="${PS4_MESA_BUILD:-$PSB/build/mesa}"
export PS4_TARGET="x86_64-pc-freebsd12-elf"
export CLANG_VER="${CLANG_VER:-21}"
export PS4_CC="clang-$CLANG_VER"
export PS4_CXX="clang++-$CLANG_VER"
export PS4_LD="ld.lld-$CLANG_VER"
export PS4_AR="llvm-ar-$CLANG_VER"
export PS4_RANLIB="llvm-ranlib-$CLANG_VER"

# The OpenOrbis C library is musl; __PS4__ tells our patched libc++ not to take its FreeBSD code paths.
# -march=btver2: the PS4's CPU (AMD Jaguar: SSE4.2, AVX, BMI1, F16C, MOVBE; no AVX2).
export PS4_BASE_FLAGS="--target=$PS4_TARGET -D_GNU_SOURCE -D__PS4__ -D__ORBIS__ -D__OPENORBIS__ -fPIC -funwind-tables \
 -march=btver2 --sysroot=$OPENORBIS -isystem $PS4_SYSROOT/libc-overlay -isystem $OPENORBIS/include"
export PS4_CXX_FLAGS="-nostdinc++ -isystem $PS4_SYSROOT/include/c++/v1"

export JOBS="${JOBS:-$(nproc)}"

# PacBrew's ld.lld links against libxml2.so.2; newer distros only ship libxml2.so.16 (same C API), so
# give it a private compat link if needed (from love-ps4's build.sh). The link is shared by every build
# on the machine: it is made only when it is missing or points elsewhere, and then by a rename, never
# removed first. A plain `ln -sf` in each script that sources this left an instant without it, and a
# CMake configure in another build loading ld.lld then failed its compiler check ("libxml2.so.2: cannot
# open shared object file", MP3 v00.51's first Xenos test build, 2026-11-08).
if [ -x "$OPENORBIS/bin/ld.lld" ] && [[ "$(ldd "$OPENORBIS/bin/ld.lld" 2>/dev/null)" == *"libxml2.so.2 => not found"* ]]; then
    _compat=""
    for _lib in /usr/lib/x86_64-linux-gnu/libxml2.so.[0-9]*; do
        if [[ "$_lib" =~ /libxml2\.so\.[0-9]+$ ]]; then _compat="$_lib"; break; fi
    done
    if [ -n "$_compat" ]; then
        mkdir -p "$PSB/host-compat"
        if [ "$(readlink "$PSB/host-compat/libxml2.so.2" 2>/dev/null)" != "$_compat" ]; then
            ln -sfn "$_compat" "$PSB/host-compat/.libxml2.so.2.$$"
            mv -fT "$PSB/host-compat/.libxml2.so.2.$$" "$PSB/host-compat/libxml2.so.2"
        fi
        case ":${LD_LIBRARY_PATH:-}:" in *":$PSB/host-compat:"*) ;; *)
            export LD_LIBRARY_PATH="$PSB/host-compat${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;; esac
    fi
    unset _compat _lib
fi

need() {
    for t in "$@"; do
        command -v "$t" >/dev/null 2>&1 || { echo "missing tool: $t (see README.md)" >&2; exit 1; }
    done
}

# CMake with the kit's toolchain file. CFLAGS/CXXFLAGS/LDFLAGS from the environment would be prepended
# to the toolchain's own flags (PacBrew's ps4vars.sh exports ones that break <cmath>), so clear them.
ps4_cmake() {
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
        cmake -DCMAKE_TOOLCHAIN_FILE="$PS4_KIT/toolchain/ps4-kit-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@"
}

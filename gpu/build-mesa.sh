#!/usr/bin/env bash
# Mesa's RADV (Vulkan for AMD GCN) for the PS4, built as the static libraries ps4_kit_link_mesa links.
#
# Source: Mesa 26.0.8 + gpu/mesa-patches (the PS4 series: the amdgpu kernel interface over GNM in
# src/amd/common/ac_ps4_drm.c, GPU submissions on their own thread, the Liverpool workarounds, the
# shader cache on the PS4's file system), cloned to ~/ps4kit-build/src/mesa. The series is the union
# of every port's Mesa (Dolphin's, Crema's, SSBU's): they were one linear history.
# Build: ~/ps4kit-build/build/mesa, with Mesa's shader cache on (games hitch on every new shader
# without it). The cache's identity is -Dradv-build-id: the hash of the patch series, so a build whose
# shader code may differ never reads another build's cache.
#
#   gpu/build-mesa.sh            build (clones and configures the first time)
#   gpu/build-mesa.sh setup      reconfigure from scratch
#   gpu/build-mesa.sh export     after committing changes in ~/ps4kit-build/src/mesa: rewrite
#                                gpu/mesa-patches from it (the kit keeps the series, not the tree)
#
# Host tools: meson + mako + PyYAML (a venv in ~/ps4kit-build/mesa-venv), glslangValidator (Mesa's
# built-in shaders), libdrm's headers only (there is no DRM on the PS4).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../toolchain/env.sh"
SRC="$PSB/src/mesa"
B="$PS4_MESA_BUILD"
BASE_TAG=mesa-26.0.8
# Where to clone Mesa from: any local Mesa git tree that has the tag saves a 1 GB download.
MESA_ORIGIN="${MESA_ORIGIN:-https://gitlab.freedesktop.org/mesa/mesa.git}"
LIBDRM_ORIGIN="${LIBDRM_ORIGIN:-https://gitlab.freedesktop.org/mesa/drm.git}"
mkdir -p "$PSB/src" "$PSB/build" "$PSB/logs" "$PSB/host-tools/bin"

if [ "${1:-}" = export ]; then
    rm -f "$HERE/mesa-patches/"*.patch
    git -C "$SRC" format-patch -q -o "$HERE/mesa-patches" "$BASE_TAG..HEAD"
    echo "gpu/mesa-patches: $(ls "$HERE/mesa-patches" | wc -l) patches on $BASE_TAG"
    exit 0
fi

# --- host tools ---
VENV="$PSB/mesa-venv"
if [ ! -x "$VENV/bin/meson" ]; then
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install -q "meson>=1.4" mako pyyaml packaging
fi
if ! command -v glslangValidator >/dev/null && [ ! -x "$PSB/host-tools/bin/glslangValidator" ]; then
    for g in "$HOME"/*-build/host-tools/bin/glslangValidator; do
        [ -x "$g" ] && { cp "$g" "$PSB/host-tools/bin/"; break; }
    done
fi
export PATH="$PSB/host-tools/bin:$VENV/bin:$PATH"
need meson ninja glslangValidator "$PS4_CC"

# --- sources ---
if [ ! -d "$SRC/.git" ]; then
    git clone -q --no-checkout "$MESA_ORIGIN" "$SRC"
    git -C "$SRC" fetch -q origin "refs/tags/$BASE_TAG:refs/tags/$BASE_TAG" 2>/dev/null || true
    git -C "$SRC" checkout -q -b ps4-kit "$BASE_TAG"
    git -C "$SRC" -c user.name=ps4-kit -c user.email=ps4-kit@localhost am -q "$HERE"/mesa-patches/*.patch
fi
if [ ! -d "$PSB/src/libdrm" ]; then
    git clone -q --depth 1 "$LIBDRM_ORIGIN" "$PSB/src/libdrm"
fi
mkdir -p "$PSB/mesa-pkgconfig"
cat > "$PSB/mesa-pkgconfig/libdrm_amdgpu.pc" <<EOF
Name: libdrm_amdgpu
Description: libdrm headers only (PS4 has no DRM)
Version: 2.4.125
Cflags: -I$PSB/src/libdrm -I$PSB/src/libdrm/include/drm -I$PSB/src/libdrm/amdgpu
Libs:
EOF

# --- cross file ---
# The system's lld for meson's link checks (PacBrew's needs a libxml2 newer distros no longer ship;
# nothing else links: Mesa is built as static libraries). gpu/mesa-overlay: FreeBSD headers Mesa
# includes that OpenOrbis lacks (sys/ioccom.h).
CROSS="$PSB/mesa-ps4-cross.ini"
flags="'-target', 'x86_64-pc-freebsd12-elf', '-D_GNU_SOURCE', '-D__PS4__', '-D__OPENORBIS__',
  '-D__ORBIS__', '-DPS4', '-D__BSD_VISIBLE', '-D_BSD_SOURCE', '-fPIC', '-funwind-tables',
  '-march=btver2', '-isysroot', '$OPENORBIS', '-isystem', '$PS4_SYSROOT/libc-overlay',
  '-isystem', '$HERE/mesa-overlay', '-I$PSB/src/libdrm', '-I$PSB/src/libdrm/include/drm',
  '-isystem', '$OPENORBIS/include', '-I$OPENORBIS/usr/include'"
link="'-fuse-ld=/usr/bin/ld.lld-$CLANG_VER', '-nostdlib', '-Wl,-m,elf_x86_64', '-Wl,-pie',
  '-Wl,--eh-frame-hdr', '-Wl,--script,$OPENORBIS/link.x', '-L$OPENORBIS/lib',
  '$OPENORBIS/lib/crt1.o', '-lkernel', '-lc', '-lclang_rt.builtins-x86_64', '-lSceLibcInternal'"
cat > "$CROSS" <<EOF
[binaries]
c = '$PS4_CC'
cpp = '$PS4_CXX'
ar = '$PS4_AR'
strip = 'llvm-strip-$CLANG_VER'
pkg-config = 'pkg-config'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$PSB/mesa-pkgconfig'

[built-in options]
c_args = [$flags]
cpp_args = ['-nostdinc++', '-isystem', '$PS4_SYSROOT/include/c++/v1', $flags]
c_link_args = [$link]
cpp_link_args = [$link, '$PS4_SYSROOT/lib/libc++.a', '$PS4_SYSROOT/lib/libc++abi.a', '$OPENORBIS/lib/libunwind.a']

[host_machine]
system = 'freebsd'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

# --- configure + build ---
BUILD_ID="$(cat "$HERE"/mesa-patches/*.patch | grep -v '^From [0-9a-f]\{40\} ' | sha1sum | cut -c1-40)"
[ "${1:-}" = setup ] && rm -rf "$B"
if [ ! -f "$B/build.ninja" ]; then
    meson setup "$B" "$SRC" -Dbuildtype=release -Db_ndebug=true -Ddefault_library=static -Dvulkan-drivers=amd \
        -Dgallium-drivers= -Dplatforms= -Dllvm=disabled -Damd-use-llvm=false -Dopengl=false -Dgles1=disabled \
        -Dgles2=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dzstd=disabled -Dexpat=disabled \
        -Dxmlconfig=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false \
        -Dshader-cache=enabled -Dvulkan-layers= -Dtools= -Dvideo-codecs= -Dspirv-tools=disabled -Dperfetto=false \
        -Dselinux=false -Dradv-build-id="$BUILD_ID" --cross-file="$CROSS" > "$PSB/logs/mesa-setup.log" \
        || { tail -30 "$PSB/logs/mesa-setup.log"; exit 1; }
elif [ "$(cat "$B/.ps4kit-build-id" 2>/dev/null)" != "$BUILD_ID" ]; then
    # The patch series changed: a new cache identity, or the console would read shaders an older build
    # compiled differently.
    meson configure "$B" -Dradv-build-id="$BUILD_ID" > /dev/null
fi
echo "$BUILD_ID" > "$B/.ps4kit-build-id"
# Only the libraries: Mesa's tools (ac_ib_parser among them) do not link for the PS4.
ninja -C "$B" -j"$JOBS" src/amd/vulkan/libvulkan_radeon.a src/amd/common/libamd_common.a src/amd/compiler/libaco.a \
    src/amd/addrlib/libaddrlib.a src/vulkan/util/libvulkan_util.a src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a \
    src/compiler/libcompiler.a src/util/libxmlconfig.a src/util/libmesa_util.a src/util/libmesa_util_simd.a \
    src/util/libmesa_util_clflush.a src/util/libmesa_util_clflushopt.a src/util/libparson.a \
    src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a
echo "mesa: $(git -C "$SRC" log --oneline "$BASE_TAG..HEAD" | wc -l) PS4 patches on $BASE_TAG, build id ${BUILD_ID:0:12}, in $B"

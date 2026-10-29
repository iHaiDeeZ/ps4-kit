#!/usr/bin/env bash
# The kit's RADV built for the PC, to measure and profile its CPU side without a console: the same
# source tree (~/ps4kit-build/src/mesa) built natively with drm-shim standing in for the GPU and
# AC_PS4_HOST_IDENTITY, so that it sees the PS4's device (Liverpool / Bonaire, ac_ps4_info.h) and
# records command buffers exactly as the console's RADV does. Frame pointers on, so the profiler in
# gpu/host/record-bench.cpp can walk stacks. (From SSBU's scripts/build-mesa-host.sh.)
#   gpu/host/build-mesa-host.sh          -> ~/ps4kit-build/build/mesa-host
# Needs libdrm for the PC (HOST_LIBDRM, default Crema's ~/cemups4-build/host-radv/prefix).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../../toolchain/env.sh"
SRC="$PSB/src/mesa"
B="$PSB/build/mesa-host"
LIBDRM="${HOST_LIBDRM:-$HOME/cemups4-build/host-radv/prefix}"
export PATH="$PSB/host-tools/bin:$PSB/mesa-venv/bin:$PATH"
export PKG_CONFIG_PATH="$LIBDRM/lib/x86_64-linux-gnu/pkgconfig"
export CC=clang-$CLANG_VER CXX=clang++-$CLANG_VER
[ -d "$SRC/.git" ] || { echo "no $SRC: run gpu/build-mesa.sh first" >&2; exit 1; }

if [ ! -f "$B/build.ninja" ]; then
    # btver2 without SSE4a (Intel CPUs lack it): the console's code generation, near enough.
    flags="-march=btver2 -mno-sse4a -g -fno-omit-frame-pointer -DAC_PS4_HOST_IDENTITY"
    meson setup "$B" "$SRC" -Dbuildtype=release -Db_ndebug=true -Dvulkan-drivers=amd -Dgallium-drivers= \
        -Dplatforms= -Dllvm=disabled -Damd-use-llvm=false -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
        -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dzstd=disabled -Dexpat=disabled -Dxmlconfig=disabled \
        -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false -Dshader-cache=disabled \
        -Dvulkan-layers= -Dtools=drm-shim -Dvideo-codecs= -Dspirv-tools=disabled -Dperfetto=false -Dselinux=false \
        -Dc_args="$flags" -Dcpp_args="$flags" > "$PSB/logs/mesa-host-setup.log" \
        || { tail -30 "$PSB/logs/mesa-host-setup.log"; exit 1; }
fi
ninja -C "$B" -j"$JOBS" src/amd/vulkan/libvulkan_radeon.so src/amd/drm-shim/libamdgpu_noop_drm_shim.so
ls -la "$B/src/amd/vulkan/libvulkan_radeon.so" "$B/src/amd/drm-shim/libamdgpu_noop_drm_shim.so"

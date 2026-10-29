#!/usr/bin/env bash
# Build record-bench against the host RADV and run it: CPU cost per draw with and without pipeline
# switches, plus a function profile of the switching run.
#   gpu/host/run-bench.sh [draws] [repeats]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../../toolchain/env.sh"
M="$PSB/build/mesa-host"
B="$PSB/build/record-bench"
mkdir -p "$B"
export PATH="$PSB/host-tools/bin:$PATH"
for s in quad.vert quad.frag quad2.frag; do
    glslangValidator -V --quiet --vn "${s/./_}" -o "$B/$s.h" "$HERE/../../samples/kit-test/shaders/$s"
done
clang++-$CLANG_VER -std=c++20 -O2 -g -fno-omit-frame-pointer -I"$PSB/src/mesa/include" -I"$B" \
    "$HERE/record-bench.cpp" -o "$B/record-bench" -ldl
LD_PRELOAD="$M/src/amd/drm-shim/libamdgpu_noop_drm_shim.so" AMDGPU_GPU_ID=bonaire \
    "$B/record-bench" "$M/src/amd/vulkan/libvulkan_radeon.so" "$B/samples.txt" "${1:-5000}" "${2:-200}"
python3 -I "$HERE/profile-report.py" "$B/samples.txt" --top "${TOP:-35}"

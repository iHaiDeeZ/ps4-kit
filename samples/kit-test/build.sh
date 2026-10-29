#!/usr/bin/env bash
# Build kit-test and package it as out/PS4Kit-Test-vXX.YY.pkg (title PKIT00001). Run in WSL.
#   KIT_TEST_VERSION=00.01 samples/kit-test/build.sh [clean]
# Then, from Windows (Git Bash): scripts/upload.sh out/PS4Kit-Test-vXX.YY.pkg <console-ip>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../../toolchain/env.sh"
VERSION="${KIT_TEST_VERSION:-00.00}"
B="$PSB/build/kit-test"
[ "${1:-}" = clean ] && rm -rf "$B"
export PATH="$PSB/host-tools/bin:$PATH"
need cmake ninja glslangValidator
[ -f "$PS4_MESA_BUILD/src/amd/vulkan/libvulkan_radeon.a" ] || bash "$PS4_KIT/gpu/build-mesa.sh"

ps4_cmake -S "$HERE" -B "$B" -G Ninja -DKIT_TEST_VERSION="$VERSION" > "$PSB/logs/kit-test-configure.log" \
    || { tail -30 "$PSB/logs/kit-test-configure.log"; exit 1; }
ninja -C "$B" -j"$JOBS" > "$PSB/logs/kit-test-build.log" 2>&1 \
    || { grep -E "error|undefined" "$PSB/logs/kit-test-build.log" | head -40; exit 1; }

STAGE="$B/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys"
cp "$B/kit_test_eboot/eboot.bin" "$STAGE/eboot.bin"
cp "$HERE/sce_sys/icon0.png" "$STAGE/sce_sys/"
PKG="$(NO_RIGHT_SPRX=1 bash "$PS4_KIT/scripts/make-pkg.sh" "$STAGE" PKIT00001 "PS4 Kit Test" "$VERSION" PS4KITTEST "$PSB/out" | tail -1)"
mkdir -p "$PS4_KIT/out"
NAME="PS4Kit-Test-v$VERSION.pkg"
cp "$PKG" "$PS4_KIT/out/$NAME"
# Symbols of every packaged build: crash logs are symbolized against the exact binary.
cp "$B/kit_test_eboot/kit_test.oelf" "$PSB/oelf/kit-test-v$VERSION.oelf"
cp "$B/kit_test" "$PSB/oelf/kit-test-v$VERSION.elf"
ls -la "$PS4_KIT/out/$NAME"

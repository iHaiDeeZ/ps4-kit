#!/usr/bin/env bash
# One-time: create the kit's WSL build tree ~/ps4kit-build and the space-free link ~/ps4-kit to this
# repo (toolchain flags - linker script paths - break on the space in "PS4 Projects").
#
# The sysroot (libc++ 21 with the libc overlay, Boost 1.89, OpenSSL, curl, zlib, zstd, libzip, FFmpeg
# 7.1, SDL3, glslang, libsmb2, libnfs) is copied from an existing port's tree (SEED_SYSROOT, default
# HENkaku's, the most complete one) rather than rebuilt: toolchain/prepare-sysroot.sh and
# build-libcxx.sh rebuild it from source when needed. The kit's libc overlay and PS4 ucontext layout
# then replace the copy's.
#   scripts/seed-build-tree.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
PSB="${PSB:-$HOME/ps4kit-build}"
SEED_SYSROOT="${SEED_SYSROOT:-$HOME/vita3k-build/sysroot}"
mkdir -p "$PSB/src" "$PSB/build" "$PSB/out" "$PSB/logs" "$PSB/oelf" "$PSB/host-tools/bin"
[ -e "$HOME/ps4-kit" ] || ln -s "$REPO" "$HOME/ps4-kit"
if [ ! -d "$PSB/sysroot" ]; then
    [ -d "$SEED_SYSROOT" ] || { echo "no sysroot to seed from at $SEED_SYSROOT (set SEED_SYSROOT)" >&2; exit 1; }
    cp -a "$SEED_SYSROOT" "$PSB/sysroot"
fi
# The kit's libc overlay over the copy's, then the PS4's signal context layout into it (JITs' fault
# handlers read the faulting context through it).
if [ -d "$REPO/toolchain/libc-overlay" ]; then
    cp -r "$REPO/toolchain/libc-overlay/." "$PSB/sysroot/libc-overlay/"
fi
if [ -f "$REPO/toolchain/ucontext-overlay.sh" ]; then
    PSB="$PSB" bash "$REPO/toolchain/ucontext-overlay.sh"
fi
du -sh "$PSB"/* 2>/dev/null

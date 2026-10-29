#!/bin/sh
# Makes a copy of OpenOrbis' crt1.o that declares an explicit system libc heap size.
#
# The PS4's system C library (libSceLibcInternal) serves every malloc of the system modules -
# Piglet's program links, shader compiles and command buffers included - from a heap sized by the
# app's sceLibcHeapSize. OpenOrbis declares 0xFFFFFFFFFFFFFFFF ("default"), which measured ~12.7
# MiB on hardware: Dolphin exhausted it within minutes ("Internal Memory is running out") and
# crashed in the next shader compile.
#
# (ps4-kit.cmake links runtime/ps4_crt1.S instead, which sends the system modules' mallocs to the
# app's own heap: a bigger declared size alone had no effect on hardware. This stays for builds that
# keep OpenOrbis' crt1.o.)
#
# usage: patch-crt1.sh <crt1.o in> <crt1.o out> <heap MiB>
set -e
in=$1
out=$2
heap_mib=$3

# crt1.o .data (0x28 bytes): +0 sceLibcHeapExtendedAlloc (u32, 1), +8 sceLibcHeapSize (u64),
# +0x10 sce_libc_heap_delayed_alloc (u32), +0x14 sce_libc_heap_extended_alloc (u32), +0x18.. 0.
data=$(mktemp)
python3 - "$data" "$heap_mib" <<'EOF'
import struct, sys
path, mib = sys.argv[1], int(sys.argv[2])
blob = struct.pack("<IIQII", 1, 0, mib * 1024 * 1024, 0, 0) + bytes(0x28 - 24)
open(path, "wb").write(blob)
EOF
"llvm-objcopy-${CLANG_VER:-21}" --update-section .data="$data" "$in" "$out"
rm -f "$data"

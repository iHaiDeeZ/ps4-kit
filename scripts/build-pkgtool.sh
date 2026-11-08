#!/usr/bin/env bash
# PkgTool from LibOrbisPkg's source with three changes, published to $PSB/tools/pkgtool-fixed (scripts/make-pkg.sh
# uses it when it is there):
#  - room to spare for the PlayGo chunk hashes: the toolchain's PkgTool sizes them from an estimate of the package
#    that falls short past ~4 GB ("Playgo Chunk hash file was not allocated enough space": SMW's package with the
#    game data in it, MP3's one-file package);
#  - PKG_EXTRA_CONTENT_FLAGS (hex, from the environment) OR'd into the header's content flags (DawnStation: PS2
#    Classics carry REMASTER, 0x400000; LibOrbisPkg derives the flags from the volume type only);
#  - compressed (PFSC) sectors inflated with a loop. Upstream does one DeflateStream.Read per 64 KB sector, which on
#    .NET 6+ may return less than the sector and leaves the rest of the buffer holding the previous one: files
#    extracted from compressed packages came out silently corrupted (DawnStation's Bully core).
# From SMW's and DawnStation's build-pkgtool.sh. Needs a .NET SDK (DOTNET_ROOT, default $PSB/tools/dotnet).
#   scripts/build-pkgtool.sh [output folder]      default $PSB/tools/pkgtool-fixed
# LIBORBISPKG=<a LibOrbisPkg checkout> builds from that one instead of cloning maxton/LibOrbisPkg into $PSB/tools.
set -euo pipefail
PSB="${PSB:-$HOME/ps4kit-build}"
T="$PSB/tools"
OUT="${1:-$T/pkgtool-fixed}"
SRC="${LIBORBISPKG:-$T/LibOrbisPkg}"
DOTNET_ROOT="${DOTNET_ROOT:-$T/dotnet}"
[ -x "$DOTNET_ROOT/dotnet" ] || { echo "no .NET SDK at $DOTNET_ROOT (set DOTNET_ROOT)" >&2; exit 1; }
mkdir -p "$T"
[ -d "$SRC" ] || git clone -q https://github.com/maxton/LibOrbisPkg.git "$SRC"
cd "$SRC"
git checkout -q -- .
sed -i 's|e.DataSize = (uint)(pkgSize / 0x10000L) \* 4;|e.DataSize = (uint)(pkgSize / 0x10000L) * 4 + 0x10000;  // slack: the estimate fell short for a 4 GB image|' \
    LibOrbisPkg/PKG/PkgBuilder.cs
sed -i 's|content_flags = ContentFlags.Unk_x8000000 \| VolTypeToContentFlags(volType),|content_flags = ContentFlags.Unk_x8000000 \| VolTypeToContentFlags(volType) \| (ContentFlags)System.Convert.ToUInt32(System.Environment.GetEnvironmentVariable("PKG_EXTRA_CONTENT_FLAGS") ?? "0", 16),|' \
    LibOrbisPkg/PKG/PkgBuilder.cs
sed -i 's|          ds.Read(output, 0, hdr.BlockSz);|          for (int got = 0, n; got < hdr.BlockSz \&\& (n = ds.Read(output, got, hdr.BlockSz - got)) > 0; got += n) { }  // DeflateStream may return a partial block|' \
    LibOrbisPkg/PFS/PFSCReader.cs
grep -q "slack: the estimate" LibOrbisPkg/PKG/PkgBuilder.cs || { echo "the PlayGo sizing line moved: patch by hand" >&2; exit 1; }
grep -q "PKG_EXTRA_CONTENT_FLAGS" LibOrbisPkg/PKG/PkgBuilder.cs || { echo "the content_flags line moved: patch by hand" >&2; exit 1; }
grep -q "may return a partial block" LibOrbisPkg/PFS/PFSCReader.cs || { echo "the PFSC inflate line moved: patch by hand" >&2; exit 1; }
sed -i 's|<TargetFramework>netcoreapp3.0</TargetFramework>|<TargetFramework>net10.0</TargetFramework>|' \
    PkgTool.Core/PkgTool.Core.csproj LibOrbisPkg.Core/LibOrbisPkg.Core.csproj
export DOTNET_ROOT DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 DOTNET_NOLOGO=1
"$DOTNET_ROOT/dotnet" publish PkgTool.Core/PkgTool.Core.csproj -c Release -o "$OUT" | tail -1

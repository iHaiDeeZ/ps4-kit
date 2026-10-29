#!/usr/bin/env bash
# Puts a package in the console's /data/pkg, then removes older versions of the same package there:
# <Name>-vXX.YY.pkg replaces every other <Name>-vXX.YY.pkg (other apps' packages are left alone).
#   scripts/upload.sh out/<Name>-vXX.YY.pkg [console-ip]
# The console comes from the argument or PS4_HOST - there is no default, as our ports go to two
# consoles (192.168.0.90 and 192.168.0.212). FTP port: PS4_FTP_PORT, default 2121.
#
# Run it from Windows (Git Bash), not from WSL: on 2026-10-12 every upload sent from inside WSL (even
# through Windows' curl.exe) was undone on the console a minute later (the old package back, the new
# one gone), while the same steps from Windows stayed.
set -euo pipefail
PKG="${1:?package path}"
CONSOLE="${2:-${PS4_HOST:-}}"
[ -n "$CONSOLE" ] || { echo "which console? give its IP as the 2nd argument or in PS4_HOST" >&2; exit 1; }
NAME="$(basename "$PKG")"
HOST="ftp://$CONSOLE:${PS4_FTP_PORT:-2121}"
SIZE="$(stat -c %s "$PKG")"
export MSYS_NO_PATHCONV=1

listing() { curl -sS --list-only "$HOST/data/pkg/" | tr -d '\r'; }
present() { curl -sS "$HOST/data/pkg/" | tr -d '\r' | awk -v n="$NAME" -v s="$SIZE" '$NF == n && $5 == s { f = 1 } END { exit !f }'; }
# the new package stays in the listing for 30 s
settled() { for _ in 1 2 3; do sleep 10; present || return 1; done; }

for try in 1 2 3; do
    curl -sS -T "$PKG" "$HOST/data/pkg/"
    echo "uploaded $NAME to $CONSOLE"
    settled && break
    echo "console: $NAME didn't stay, uploading again"
done
FAMILY="${NAME%-v[0-9][0-9].[0-9][0-9]*.pkg}"
if [ "$FAMILY" != "$NAME" ]; then
    for old in $(listing | grep -E "^${FAMILY}-v[0-9]{2}[.][0-9]{2}[^/]*[.]pkg\$" | grep -vx "$NAME"); do
        if curl -sS "$HOST/" -Q "DELE /data/pkg/$old" > /dev/null; then echo "console: removed old $old"; else echo "console: could not remove $old"; fi
        sleep 15
    done
    settled || { echo "console: $NAME went missing after the cleanup, uploading again"; curl -sS -T "$PKG" "$HOST/data/pkg/"; settled; }
    listing | grep -E "^${FAMILY}-" | sed 's/^/console has: /'
fi

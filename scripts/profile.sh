#!/usr/bin/env bash
# Fetches the profilers' files from the console and names them (profile.py): samples.log (the 1 kHz
# sampler), profile.log (the hot-spot profiler) and, with -b, profiles/profile_<n>.txt (the burst
# profiler; named copies are written beside them). Symbolized against the build that ran.
#   profile.sh [-c kit.conf] [-H host] [-P port] [-d /data/App] [-e elf] [-o dir] [-n] [-b]
#              [-- profile.py options, e.g. --top 40 --window 60-120]
# Settings: diag.conf.example. In WSL (llvm-symbolizer-21, curl, python3).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=diag-env.sh
source "$HERE/diag-env.sh"
if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    sed -n '2,7p' "$0"
    diag_usage_common
    echo "  -b        also the burst profiles (profiles/*.txt)"
    exit 0
fi
burst=0
args=()
extra=()
while [ $# -gt 0 ]; do
    case "$1" in
    -b) burst=1 ;;
    --) shift; extra=("$@"); break ;;
    *) args+=("$1") ;;
    esac
    shift
done
diag_parse_args "${args[@]+"${args[@]}"}"
OUT="$PS4_LOGS_DIR"

diag_get samples.log "$OUT/samples.log" 120
diag_get profile.log "$OUT/profile.log" 120
diag_get boot-trace.log "$OUT/boot-trace.log"
files=()
[ -s "$OUT/samples.log" ] && files+=("$OUT/samples.log")
[ -s "$OUT/profile.log" ] && files+=("$OUT/profile.log")
if [ "$burst" = 1 ]; then
    mkdir -p "$OUT/profiles"
    for f in $(diag_list profiles | grep -E '^profile_[0-9]+\.txt$' || true); do
        diag_get "profiles/$f" "$OUT/profiles/$f" 60
    done
    for f in "$OUT"/profiles/profile_*.txt; do
        case "$f" in *.named.txt) ;; *) [ -s "$f" ] && files+=("$f") ;; esac
    done
fi
[ ${#files[@]} -gt 0 ] || { echo "no profiles on the console (is a profiler switched on?)"; exit 1; }
jit=()
if [ -n "${PS4_JIT_MAP:-}" ]; then
    diag_get "$PS4_JIT_MAP" "$OUT/$(basename "$PS4_JIT_MAP")" 120
    [ -s "$OUT/$(basename "$PS4_JIT_MAP")" ] && jit=(--jit-map "$OUT/$(basename "$PS4_JIT_MAP")")
fi
diag_pick_elf "$OUT/boot-trace.log"
echo "== profile from ${DIAG_ELF:-(no ELF found)}"
elf=()
[ -n "${DIAG_ELF:-}" ] && elf=(--elf "$DIAG_ELF")
PS4_SYMBOLIZER="$PS4_SYMBOLIZER" python3 "$HERE/profile.py" "${elf[@]+"${elf[@]}"}" "${jit[@]+"${jit[@]}"}" \
    "${extra[@]+"${extra[@]}"}" "${files[@]}"

#!/usr/bin/env bash
# Fetches the app's diagnostics from the console - boot-trace.log (and the previous run's,
# boot-trace-prev.log / boot-trace-game.log), crash.log, hang.log, the app's own logs, the GPU
# driver's fault files - and symbolizes the crash and thread-dump addresses against the ELF of the
# build that ran (runtime/ps4_crashlog.cpp, ps4_sampler.cpp write them).
#   crashlog.sh [-c kit.conf] [-H host] [-P port] [-d /data/App] [-e elf] [-o dir] [-n] [elf]
# Settings: diag.conf.example. In WSL (llvm-symbolizer-21, curl).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=diag-env.sh
source "$HERE/diag-env.sh"
if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    sed -n '2,7p' "$0"
    diag_usage_common
    exit 0
fi
diag_parse_args "$@"
[ ${#DIAG_REST[@]} -gt 0 ] && PS4_ELF="${DIAG_REST[0]}"   # (the old "crashlog.sh <elf>")
OUT="$PS4_LOGS_DIR"

for f in boot-trace.log boot-trace-prev.log boot-trace-game.log crash.log hang.log; do
    diag_get "$f" "$OUT/$f"
done
for f in ${PS4_APP_LOGS:-}; do
    diag_get "$f" "$OUT/$(basename "$f")" 60
done
diag_pick_elf "$OUT/boot-trace.log"
echo "== symbols from ${DIAG_ELF:-(no ELF found: set PS4_ELF_DIR or -e)}"

# A thread dump (ps4_dump_thread_stacks: hang.log, the end of a crash) from stdin, symbolized.
print_thread_dump() {
    while IFS= read -r line; do
        case "$line" in
        "-- "*)
            echo "$line"
            rip="$(echo "$line" | grep -oE "rip elf 0x[0-9a-f]+" | grep -oE "0x[0-9a-f]+$" || true)"
            [ -n "$rip" ] && echo "     rip  $(diag_symbolize "$rip")"
            ;;
        *"stack:"*)
            for addr in $(echo "$line" | grep -oE "0x[0-9a-f]+" | head -14); do
                echo "     $addr  $(diag_symbolize "$addr")"
            done
            ;;
        *) echo "$line" ;;
        esac
    done
}

# Screenshots the app takes during a run
if [ -n "${PS4_SCREENSHOTS:-}" ] && [ "$DIAG_FETCH" = 1 ]; then
    shots="$(diag_list "$PS4_SCREENSHOTS" | grep -iE '\.(png|jpg)$' || true)"
    if [ "${PS4_SCREENSHOTS_DOWNLOAD:-0}" = 1 ] && [ -n "$shots" ]; then
        rm -rf "$OUT/screenshots"; mkdir -p "$OUT/screenshots"
        for f in $shots; do diag_get "$PS4_SCREENSHOTS/$f" "$OUT/screenshots/$f" 60; done
    fi
    [ -n "$shots" ] && echo "== screenshots: $(echo "$shots" | tr '\n' ' ')"
fi

echo "== boot-trace.log"; cat "$OUT/boot-trace.log" 2>/dev/null || echo "(none)"
# a game that stopped and went back to the menu: its own trace (the menu's is above)
if [ -s "$OUT/boot-trace-game.log" ] && [ -n "${PS4_GAME_STOP_PATTERN:-}" ] &&
    grep -q "$PS4_GAME_STOP_PATTERN" "$OUT/boot-trace.log" 2>/dev/null; then
    echo "== boot-trace-game.log (the game that stopped)"; grep -v "mmap:" "$OUT/boot-trace-game.log" | tail -40
fi
for f in ${PS4_APP_LOGS:-}; do
    log="$OUT/$(basename "$f")"
    [ -s "$log" ] || continue
    if [ -n "${PS4_APP_LOG_ERRORS:-}" ]; then
        echo "== $(basename "$f") (errors)"; grep -E "$PS4_APP_LOG_ERRORS" "$log" | head -40 || true
    fi
    echo "== $(basename "$f") (last 60 lines)"; tail -60 "$log"
done

# crash.log collects every run's crashes: only the last one, and only if this run (or the game run
# before it) crashed
if [ -s "$OUT/crash.log" ] && grep -qE "crashed|fatal signal|uncaught exception" "$OUT/boot-trace.log" "$OUT/boot-trace-game.log" 2>/dev/null; then
    echo "== crash.log (last crash)"
    last="$(awk '/^=== /{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' "$OUT/crash.log")"
    # the crashing thread (before the other threads' dump)
    crashed="$(printf '%s\n' "$last" | sed '/^==== /,$d')"
    printf '%s\n' "$crashed" | grep -v "^  \[rsp"
    echo "== symbolized"
    printf '%s\n' "$crashed" | grep -oE "(rip 0x[0-9a-f]+ elf |^  \[rsp\+0x[0-9a-f]+\] )0x[0-9a-f]+" | grep -oE "0x[0-9a-f]+$" |
        while read -r addr; do
            printf '%s  ' "$addr"
            diag_symbolize "$addr"
        done
    # the app's own reading of the crashing thread (a guest code decoder)
    if [ -n "${PS4_CRASH_HOOK:-}" ]; then
        printf '%s\n' "$crashed" > "$OUT/crash-block.txt"
        echo "== $PS4_CRASH_HOOK"
        $PS4_CRASH_HOOK "$OUT/crash-block.txt" || true
    fi
    # every other thread at the crash (ps4_dump_thread_stacks), symbolized like hang.log
    if printf '%s\n' "$last" | grep -q "^==== "; then
        echo "== the other threads at the crash"
        printf '%s\n' "$last" | sed -n '/^==== /,$p' |
            awk -v p="${PS4_GUEST_SECTION:-}" 'p != "" && index($0, p) == 1 {exit} {print}' | print_thread_dump
    fi
    # the app's side (ps4_crash_guest_info) follows in crash.log
    if [ -n "${PS4_GUEST_SECTION:-}" ]; then
        printf '%s\n' "$last" | awk -v p="$PS4_GUEST_SECTION" 'index($0, p) == 1 {on = 1} on'
    fi
else
    echo "== crash.log: no crash in this run"
fi

# The GPU driver's files (RADV on PS4, in <data dir>/gpu): its log, a GPU fault report, and with the
# app's GPU fault recorder on, the flight recorder (progress.log: the last submission and per-draw
# trace id the GPU reached; ib-N.bin: the last command streams, decode-flight.py reads them)
if [ -n "${PS4_GPU_DIR:-}" ]; then
    mkdir -p "$OUT/gpu"
    if [ "$DIAG_FETCH" = 1 ]; then
        rm -f "$OUT"/gpu/*
        for f in radv-ps4.log gpu-fault.txt; do diag_get "$PS4_GPU_DIR/$f" "$OUT/gpu/$f"; done
        for f in $(diag_list "$PS4_GPU_DIR/flight" | grep -E '\.(log|bin)$' || true); do
            diag_get "$PS4_GPU_DIR/flight/$f" "$OUT/gpu/$f" 60
        done
    fi
    [ -s "$OUT/gpu/gpu-fault.txt" ] && { echo "== gpu-fault.txt"; head -40 "$OUT/gpu/gpu-fault.txt"; }
    if [ -s "$OUT/gpu/progress.log" ]; then
        echo "== GPU flight recorder (last lines of progress.log; command streams in $OUT/gpu, see decode-flight.py)"
        tail -15 "$OUT/gpu/progress.log"
    fi
fi

# hang.log: the app's watchdog's dumps of every watched thread
if [ -s "$OUT/hang.log" ]; then
    echo "== hang.log (last dump, symbolized)"
    awk '/^==== /{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' "$OUT/hang.log" | print_thread_dump
fi

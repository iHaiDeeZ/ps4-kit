#!/usr/bin/env bash
# Shared by crashlog.sh and profile.sh: the settings (diag.conf.example lists them) from the
# config file, then the environment, then the command line; and the FTP and symbolizer helpers.
# Source it, then call diag_parse_args "$@".

DIAG_KEYS="PS4_HOST PS4_FTP_PORT PS4_DATA_DIR PS4_TITLE_ID PS4_APP_NAME PS4_BUILD_ROOT PS4_ELF_DIR
PS4_LOGS_DIR PS4_ELF PS4_APP_LOGS PS4_APP_LOG_ERRORS PS4_GUEST_SECTION PS4_CRASH_HOOK
PS4_GAME_STOP_PATTERN PS4_SCREENSHOTS PS4_SCREENSHOTS_DOWNLOAD PS4_GPU_DIR PS4_JIT_MAP PS4_SYMBOLIZER"

diag_usage_common() {
    cat <<'EOF'
  -c FILE   settings file (default: $KIT_CONF, ./kit.conf, or kit.conf beside this script's folder)
  -H HOST   console address (192.168.0.90 / 192.168.0.212)
  -P PORT   FTP port (2121)
  -d DIR    the app's folder on the console (/data/<App>)
  -e ELF    symbolize against this ELF (default: the build that ran, from PS4_ELF_DIR)
  -o DIR    where fetched files go (PS4_LOGS_DIR)
  -n        no fetching: use the files already in the output folder
EOF
}

# Loads the settings file; environment values set before win over it.
diag_load_conf() {
    local conf="$1" key
    declare -A saved=()
    for key in $DIAG_KEYS; do
        [ -n "${!key+x}" ] && saved[$key]="${!key}"
    done
    if [ -n "$conf" ] && [ -f "$conf" ]; then
        # shellcheck disable=SC1090
        source "$conf"
    fi
    for key in "${!saved[@]}"; do
        printf -v "$key" '%s' "${saved[$key]}"
    done
}

# Sets DIAG_* from the options; leaves the remaining arguments in DIAG_REST.
diag_parse_args() {
    local conf="${KIT_CONF:-}" here
    here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    if [ -z "$conf" ]; then
        if [ -f ./kit.conf ]; then conf=./kit.conf; elif [ -f "$here/../kit.conf" ]; then conf="$here/../kit.conf"; fi
    fi
    # -c first (the others override what it sets)
    local args=("$@") i
    for ((i = 0; i < ${#args[@]}; i++)); do
        [ "${args[$i]}" = "-c" ] && conf="${args[$((i + 1))]:-}"
    done
    diag_load_conf "$conf"
    DIAG_FETCH=1
    DIAG_REST=()
    while [ $# -gt 0 ]; do
        case "$1" in
        -c) shift ;;
        -H) PS4_HOST="$2"; shift ;;
        -P) PS4_FTP_PORT="$2"; shift ;;
        -d) PS4_DATA_DIR="$2"; shift ;;
        -e) PS4_ELF="$2"; shift ;;
        -o) PS4_LOGS_DIR="$2"; shift ;;
        -n) DIAG_FETCH=0 ;;
        *) DIAG_REST+=("$1") ;;
        esac
        shift
    done
    PS4_FTP_PORT="${PS4_FTP_PORT:-2121}"
    PS4_SYMBOLIZER="${PS4_SYMBOLIZER:-llvm-symbolizer-21}"
    PS4_LOGS_DIR="${PS4_LOGS_DIR:-${PS4_BUILD_ROOT:+$PS4_BUILD_ROOT/logs}}"
    PS4_LOGS_DIR="${PS4_LOGS_DIR:-./logs}"
    PS4_ELF_DIR="${PS4_ELF_DIR:-${PS4_BUILD_ROOT:+$PS4_BUILD_ROOT/oelf}}"
    if [ "$DIAG_FETCH" = 1 ]; then
        [ -n "${PS4_HOST:-}" ] || { echo "no console address: -H, PS4_HOST or kit.conf (see diag.conf.example)" >&2; exit 2; }
        [ -n "${PS4_DATA_DIR:-}" ] || { echo "no data folder: -d, PS4_DATA_DIR or kit.conf" >&2; exit 2; }
    fi
    PS4_DATA_DIR="${PS4_DATA_DIR:-}"
    PS4_DATA_DIR="${PS4_DATA_DIR%/}"
    DIAG_URL="ftp://${PS4_HOST:-}:${PS4_FTP_PORT}${PS4_DATA_DIR:-}"
    mkdir -p "$PS4_LOGS_DIR"
}

# diag_get <path in the data folder> <local file> [seconds]: fetched, or removed if it isn't there
diag_get() {
    [ "$DIAG_FETCH" = 1 ] || return 0
    curl -s --max-time "${3:-30}" -o "$2" "$DIAG_URL/$1" || rm -f "$2"
    [ -s "$2" ] || rm -f "$2"
}

# diag_list <folder in the data folder>: its file names
diag_list() {
    [ "$DIAG_FETCH" = 1 ] || return 0
    curl -s --max-time 30 --list-only "$DIAG_URL/$1/" 2>/dev/null | tr -d '\r' || true
}

# The ELF to symbolize against: -e / PS4_ELF, else the version the boot trace names
# ("<PS4_APP_NAME> XX.YY starting") from PS4_ELF_DIR, else the newest there.
diag_pick_elf() {
    local trace="$1" ran=""
    if [ -n "${PS4_ELF:-}" ]; then
        DIAG_ELF="$PS4_ELF"
        return
    fi
    DIAG_ELF="$(ls -t "${PS4_ELF_DIR:-.}"/v*.elf 2>/dev/null | head -1 || true)"
    if [ -n "${PS4_APP_NAME:-}" ] && [ -f "$trace" ]; then
        ran="$(grep -oE "$PS4_APP_NAME [0-9]{2}\.[0-9]{2} starting" "$trace" 2>/dev/null | head -1 | awk '{print $2}' || true)"
        [ -n "$ran" ] && [ -f "$PS4_ELF_DIR/v$ran.elf" ] && DIAG_ELF="$PS4_ELF_DIR/v$ran.elf"
    fi
    # .text's ELF address (0 with OpenOrbis' link.x): the logs' addresses are relative to it
    DIAG_TEXT_START=0
    if [ -n "$DIAG_ELF" ] && [ -f "$DIAG_ELF" ]; then
        local nm="${PS4_SYMBOLIZER/symbolizer/nm}" start
        start="$("$nm" "$DIAG_ELF" 2>/dev/null | awk '$3 == "__text_start" {print $1; exit}' || true)"
        [ -n "$start" ] && DIAG_TEXT_START=$((16#$start))
    fi
}

# diag_symbolize <.text-relative address>: "function file:line"
diag_symbolize() {
    if [ -z "${DIAG_ELF:-}" ] || [ ! -f "$DIAG_ELF" ]; then
        echo "(no ELF)"
        return
    fi
    "$PS4_SYMBOLIZER" --obj="$DIAG_ELF" --functions=linkage --demangle --no-inlines \
        "$(printf '0x%x' $(($1 + ${DIAG_TEXT_START:-0})))" | head -2 | paste -sd' '
}

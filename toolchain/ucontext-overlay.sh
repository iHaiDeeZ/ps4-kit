#!/usr/bin/env bash
# The PS4 signal context in the sysroot's libc overlay (idempotent; prepare-sysroot.sh runs it, and
# it upgrades a sysroot copied from a port's tree):
#
# - ucontext_t: on the PS4 the FreeBSD machine context starts at byte 0x40 of the signal context
#   (measured on hardware by DolphinPS4's probe; DeCast-S4 v01.04), not at 0x10 where the overlay's
#   FreeBSD layout put it. Fault handlers that read and edit registers through uc_mcontext.mc_*
#   (PPSSPP's fast memory, RPCS3's access violations, Vita3K's and the recompilers' fault handlers)
#   need the type itself to match the hardware.
# - machine/fpu.h: FreeBSD's struct savefpu (the XMM registers in mc_fpstate), which the same
#   handlers use for faulting SSE loads/stores (from rpcs3).
#
# Each port added the padding under its own name (__m33_ps4_pad, __smw_ps4_pad, __rpcs3_ps4_pad)
# and only recognised its own, so its script failed ("unexpected bits/signal.h") on a sysroot seeded
# from another port's tree. Any of them counts as done here.
set -euo pipefail
source "$(dirname "$0")/env.sh"
O="$PS4_SYSROOT/libc-overlay"
[ -f "$O/bits/signal.h" ] || { echo "no $O/bits/signal.h: run prepare-sysroot.sh first" >&2; exit 1; }

if ! grep -qE '__[A-Za-z0-9_]*(ps4_pad|ps4_mcontext_pad)\[0x30\]' "$O/bits/signal.h"; then
    python3 - "$O/bits/signal.h" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = "\tsigset_t uc_sigmask;\n\tmcontext_t uc_mcontext;\n"
new = ("\tsigset_t uc_sigmask;\n"
       "\tchar __ps4_mcontext_pad[0x30]; /* the PS4 kernel's mcontext is at byte 0x40 (ucontext-overlay.sh) */\n"
       "\tmcontext_t uc_mcontext;\n")
assert s.count(old) == 1, "unexpected bits/signal.h"
open(p, "w").write(s.replace(old, new))
EOF
fi

mkdir -p "$O/machine"
cat > "$O/machine/fpu.h" <<'EOF'
/* ps4-kit (from rpcs3): FreeBSD/amd64 <machine/fpu.h> layout of the FXSAVE area in mcontext_t.mc_fpstate. */
#ifndef _PS4KIT_MACHINE_FPU_H
#define _PS4KIT_MACHINE_FPU_H
#include <stdint.h>
struct envxmm {
	uint16_t en_cw;
	uint16_t en_sw;
	uint8_t en_tw;
	uint8_t en_zero;
	uint16_t en_opcode;
	uint64_t en_rip;
	uint64_t en_rdp;
	uint32_t en_mxcsr;
	uint32_t en_mxcsr_mask;
};
struct fpacc87 {
	uint8_t fp_bytes[10];
};
struct xmmacc {
	uint8_t xmm_bytes[16];
};
struct savefpu {
	struct envxmm sv_env;
	struct {
		struct fpacc87 fp_acc;
		uint8_t fp_pad[6];
	} sv_fp[8];
	struct xmmacc sv_xmm[16];
	uint8_t sv_pad[96];
} __attribute__((aligned(16)));
#endif
EOF

# The layout the code will see, checked by the compiler (a private file: several builds may run
# this at once)
check="$(mktemp --suffix=.c)"
trap 'rm -f "$check"' EXIT
cat > "$check" <<'EOF'
#include <stddef.h>
#include <signal.h>
#include <machine/fpu.h>
_Static_assert(offsetof(ucontext_t, uc_mcontext) == 0x40, "uc_mcontext must be at 0x40");
_Static_assert(offsetof(ucontext_t, uc_mcontext.mc_rip) == 0x40 + 20 * 8, "mc_rip must be word 20");
_Static_assert(offsetof(struct savefpu, sv_xmm) == 160, "sv_xmm at 160");
int main(void) { return 0; }
EOF
# shellcheck disable=SC2086
"$PS4_CC" $PS4_BASE_FLAGS -fsyntax-only "$check" && echo "overlay: ucontext/savefpu layout OK"

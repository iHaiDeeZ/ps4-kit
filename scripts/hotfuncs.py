#!/usr/bin/env python3
"""The busiest translated guest functions (the sampler's hotfuncs.bin) with their code: what the
guest does there and what the JIT made of it (x86), with the sampler's samples per 16 bytes of x86
from samples.log ("jit 0x.. guest <function> +<offset>" lines).

  python3 hotfuncs.py hotfuncs.bin [samples.log] [--functions N] [--thread FILTER] [--triple T]

hotfuncs.bin is written by the port's ps4_jit_dump_function (runtime/ps4_kit_diag.h): records of
u32 'HFNC', u32 guest address, u32 guest bytes, u32 x86 bytes, u32 samples, u64 x86 address, then
the guest code and the x86 code. --triple names the guest's instruction set for llvm-mc (default
powerpc: Crema's Wii U; e.g. mipsel for a PSP, armv7 for a Vita); the guest code is disassembled as
it lies in guest memory.
"""
import argparse
import collections
import re
import struct
import subprocess
import tempfile

MC = "llvm-mc-21"
OBJDUMP = "llvm-objdump-21"
OBJCOPY = "llvm-objcopy-21"


def read_functions(path):
    data = open(path, "rb").read()
    funcs, at = [], 0
    while at + 28 <= len(data):
        magic, guest, guest_size, x86_size, samples = struct.unpack_from("<5I", data, at)
        if magic != 0x434E4648:  # 'HFNC'
            break
        (x86,) = struct.unpack_from("<Q", data, at + 20)
        at += 28
        code = data[at:at + guest_size]
        at += guest_size
        x86_code = data[at:at + x86_size]
        at += x86_size
        funcs.append(dict(guest=guest, guest_size=guest_size, x86=x86, x86_size=x86_size, samples=samples,
                          code=code, x86_code=x86_code))
    return funcs


def guest_disasm(triple, base, code):
    """[(address, mnemonic, text)] of guest code, through llvm-mc (any instruction set it knows)."""
    text = " ".join("0x%02x" % b for b in code)
    out = subprocess.run([MC, "--disassemble", "-triple=" + triple, "--show-encoding=false"], input=text,
                         capture_output=True, text=True).stdout
    result, address = [], base
    width = 4 if triple.startswith(("powerpc", "ppc", "mips", "arm", "aarch64")) else 0
    for line in out.splitlines():
        line = line.strip()
        if not line or line.startswith("."):
            continue
        result.append((address, line.split()[0], line))
        address += width
    return result


def x86_disasm(code):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(code)
    subprocess.run([OBJCOPY, "-I", "binary", "-O", "elf64-x86-64", "--rename-section", ".data=.text,code", f.name,
                    f.name + ".o"], check=True)
    out = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", f.name + ".o"], capture_output=True, text=True).stdout
    result = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s*(.*)", line)
        if m:
            result.append((int(m.group(1), 16), m.group(2)))
    return result


def kind(triple, mnemonic):
    """A rough instruction class (PowerPC's by name; others: the mnemonic itself)."""
    if not triple.startswith(("powerpc", "ppc")):
        return mnemonic
    if mnemonic.startswith(("ps_", "psq_")):
        return "paired single"
    if mnemonic.startswith(("lf", "stf")):
        return "float load/store"
    if mnemonic.startswith("f"):
        return "float"
    if mnemonic.startswith(("l", "st")) and not mnemonic.startswith(("li", "lis")):
        return "load/store"
    if mnemonic.startswith("b"):
        return "branch"
    if mnemonic.startswith(("cmp", "cr", "mfcr", "mtcr")):
        return "compare/cr"
    return "integer"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("hotfuncs")
    ap.add_argument("samples", nargs="?")
    ap.add_argument("--functions", type=int, default=10)
    ap.add_argument("--thread", help="only samples.log windows of threads whose name contains this")
    ap.add_argument("--triple", default="powerpc")
    args = ap.parse_args()
    funcs = read_functions(args.hotfuncs)

    # samples per (function, 16-byte x86 bucket), from the thread windows that match the filter
    buckets = collections.defaultdict(collections.Counter)
    totals = collections.Counter()
    if args.samples:
        keep = True
        for line in open(args.samples, errors="replace"):
            m = re.match(r"== [\d.]+ s, (.+?): (\d+) samples", line)
            if m:
                keep = args.thread is None or args.thread in m.group(1)
                continue
            m = re.match(r"\s+([\d.]+)% jit 0x[0-9a-f]+ (?:guest|ppc) ([0-9a-f]+) \+([0-9a-f]+)", line)
            if m and keep:
                buckets[int(m.group(2), 16)][int(m.group(3), 16) & ~0xF] += float(m.group(1))
                totals[int(m.group(2), 16)] += float(m.group(1))

    order = sorted(funcs, key=lambda f: -(totals.get(f["guest"], 0) if totals else f["samples"]))
    print("%-10s %8s %7s %7s %6s  %s" % ("function", "samples", "guest B", "x86 B", "x86/g", "guest mix"))
    for f in order:
        insns = guest_disasm(args.triple, f["guest"], f["code"])
        mix = collections.Counter(kind(args.triple, i[1]) for i in insns)
        total = sum(mix.values()) or 1
        mix_text = ", ".join("%s %d%%" % (k, 100 * v // total) for k, v in mix.most_common(4))
        f["insns"] = insns
        print("%08x   %8s %7d %7d %6.1f  %s" % (f["guest"], "%.1f%%" % totals[f["guest"]] if totals else f["samples"],
                                             f["guest_size"], f["x86_size"], f["x86_size"] / max(f["guest_size"], 1),
                                             mix_text))

    for f in order[:args.functions]:
        print("\n==== %08x (%d bytes of guest code, %d of x86)" % (f["guest"], f["guest_size"], f["x86_size"]))
        hot = buckets.get(f["guest"], {})
        if hot:
            print("-- hottest x86 (samples per 16 bytes):")
            x86 = x86_disasm(f["x86_code"])
            for bucket, share in sorted(hot.items(), key=lambda kv: -kv[1])[:6]:
                print("  +%05x  %.2f%%" % (bucket, share))
                for offset, text in x86:
                    if bucket - 0x20 <= offset < bucket + 0x30:
                        print("      %s+%05x: %s" % (">" if bucket <= offset < bucket + 16 else " ", offset, text))
        print("-- guest code:")
        for address, _mnemonic, text in f["insns"][:400]:
            print("  %08x  %s" % (address, text))


if __name__ == "__main__":
    main()

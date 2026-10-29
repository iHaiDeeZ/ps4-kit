#!/usr/bin/env python3
"""Function profile from record-bench's samples: self time (the sampled function itself) and total
time (the function anywhere on the stack), symbolized with llvm-symbolizer.
    profile-report.py <samples> [--top N]
"""
import collections
import subprocess
import sys


def main():
    path = sys.argv[1]
    top = int(sys.argv[sys.argv.index("--top") + 1]) if "--top" in sys.argv else 40
    samples, cur = [], []
    for line in open(path):
        line = line.rstrip("\n")
        if not line:
            if cur:
                samples.append(cur)
            cur = []
            continue
        mod, off = line.split("\t")
        cur.append((mod, off))
    if cur:
        samples.append(cur)

    # Symbolize every distinct (module, offset) once per module.
    names = {}
    by_mod = collections.defaultdict(set)
    for s in samples:
        for f in s:
            by_mod[f[0]].add(f[1])
    for mod, offs in by_mod.items():
        offs = sorted(offs)
        if mod == "?":
            for o in offs:
                names[(mod, o)] = "?" + o
            continue
        out = subprocess.run(["llvm-symbolizer-21", "--obj=" + mod, "--functions=short", "--no-inlines"],
                             input="\n".join("0x" + o for o in offs), capture_output=True, text=True).stdout
        blocks = [b.split("\n") for b in out.strip().split("\n\n")]
        short = mod.rsplit("/", 1)[-1]
        for o, b in zip(offs, blocks):
            fn = b[0] if b and b[0] not in ("??", "") else "%s+0x%s" % (short, o)
            names[(mod, o)] = fn

    total = len(samples)
    self_count = collections.Counter()
    incl = collections.Counter()
    for s in samples:
        self_count[names[s[0]]] += 1
        for fn in set(names[f] for f in s):
            incl[fn] += 1
    print("%d samples" % total)
    print("\n-- self --")
    for fn, n in self_count.most_common(top):
        print("%6.2f%%  %s" % (100.0 * n / total, fn))
    print("\n-- total (anywhere on the stack) --")
    for fn, n in incl.most_common(top):
        print("%6.2f%%  %s" % (100.0 * n / total, fn))


if __name__ == "__main__":
    main()

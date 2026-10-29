#!/usr/bin/env python3
"""Names the PS4 profilers' samples (runtime/ps4_sampler.cpp, ps4_profile.cpp) and adds them up
by function.

Usage (in WSL):
  profile.py [--elf ELF] [--top N] [--window FROM-TO] [--jit-map FILE]
             [--regions FILE] [--ro-log LOG --ro-dir DIR] FILE...

Reads, telling them apart by their contents:
  samples.log           the 1 kHz sampler: per thread, where its time went over the whole run
                        (or --window seconds), then what its waits / sleeps were called from
  profile.log           the hot-spot profiler: per thread, how often it ran and its busiest code
  profile_<n>.txt       the burst profiler: written out named as profile_<n>.named.txt beside it

Addresses: "elf" ones are relative to the eboot's .text (named against --elf, the build that ran);
"sys"/"mod" ones are in Sony's modules (named by module offset, and libkernel's by its nearest entry
point when the log lists them; "wait" marks a thread waiting in the kernel - libkernel 0x...3710 at
the top is a restarted system call, i.e. a wait); "jit"/"abs" ones are code made at run time: named
by --jit-map ("address size name" lines, e.g. rpcs3's jit.map), by the guest function the port
reported ("guest 8200a0c0 +1c"), or by --regions ("0xSTART SIZE NAME [OBJECT]" lines: code loaded
at run time, with an ELF/.so to symbolize offsets against) / --ro-log (SSBU: ssbu.log's "[ro] <name>'s
code: N MiB at 0x..." lines, objects <name>.so in --ro-dir).
"""
import argparse
import bisect
import collections
import os
import re
import subprocess
import sys

SYMBOLIZER = os.environ.get("PS4_SYMBOLIZER", "llvm-symbolizer-21")
NM = SYMBOLIZER.replace("symbolizer", "nm")
# libkernel entry points where a thread waits rather than works
WAITS = {"_umtx_op", "nanosleep", "usleep", "sceKernelUsleep", "sceKernelNanosleep", "pthread_cond_wait",
         "pthread_cond_timedwait", "scePthreadCondWait", "scePthreadCondTimedwait", "pthread_mutex_lock",
         "scePthreadMutexLock", "pthread_mutex_timedlock", "sem_wait", "sem_timedwait", "sceKernelWaitSema",
         "sceKernelWaitEventFlag", "sceKernelWaitEqueue", "kevent", "poll", "select", "sigsuspend", "sigwait",
         "sched_yield", "pthread_join", "pthread_rwlock_rdlock", "pthread_rwlock_wrlock"}


def text_start(elf):
    """.text's ELF address (0 with OpenOrbis' link.x): the logs' addresses are relative to it."""
    if not elf:
        return 0
    out = subprocess.run([NM, elf], capture_output=True, text=True).stdout
    m = re.search(r"^([0-9a-f]+) \S __text_start$", out, re.M)
    return int(m.group(1), 16) if m else 0


def symbolize(obj, addresses):
    """{address: function name} for addresses in one object."""
    addresses = sorted(set(addresses))
    if not addresses or not obj or not os.path.isfile(obj):
        return {}
    r = subprocess.run([SYMBOLIZER, f"--obj={obj}", "--functions=linkage", "--demangle", "--no-inlines",
                        "--output-style=GNU"], input="\n".join(hex(a) for a in addresses) + "\n",
                       capture_output=True, text=True)
    lines = r.stdout.splitlines()
    names = {}
    for i, a in enumerate(addresses):  # (GNU style: two lines an address)
        name = lines[2 * i] if 2 * i < len(lines) else "??"
        names[a] = name if name != "??" else f"?{a:#x}"
    return names


def short(name, width=110):
    """The function's name without its parameters (unless that leaves nothing: "void (*f(...))()")."""
    cut = re.sub(r"\(.*", "", name).strip()
    return (cut if len(cut) > 5 else name)[:width]


class Modules:
    """Sony's modules from a log's "module" lines, and libkernel's entry points ("symbol" lines)."""

    def __init__(self):
        self.ranges = []   # (start, end, name)
        self.symbols = []  # (address, name), sorted

    def read(self, line):
        m = re.match(r"module (\S+)((?: seg 0x[0-9a-f]+\+0x[0-9a-f]+/-?\d+)+)", line)
        if m:
            for start, size, prot in re.findall(r" seg 0x([0-9a-f]+)\+0x([0-9a-f]+)/(-?\d+)", m.group(2)):
                self.ranges.append((int(start, 16), int(start, 16) + int(size, 16), m.group(1)))
            return True
        m = re.match(r"module (\S+) 0x([0-9a-f]+)-0x([0-9a-f]+)", line)
        if m:
            self.ranges.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1)))
            return True
        m = re.match(r"symbol (\S+) (?:0x)?([0-9a-f]+)", line)
        if m:
            bisect.insort(self.symbols, (int(m.group(2), 16), m.group(1)))
            return True
        return False

    def name(self, address):
        """("[module+0x.. near entry]", waiting?) for a system address."""
        module = next(((s, n) for s, e, n in self.ranges if s <= address < e), None)
        where = f"{module[1]}+{address - module[0]:#x}" if module else f"sys {address:#x}"
        i = bisect.bisect_right(self.symbols, (address, "\xff")) - 1
        if i >= 0 and address - self.symbols[i][0] < 0x2000:
            sym = self.symbols[i][1]
            return f"[{module[1] if module else 'sys'}: near {sym}]", sym in WAITS
        return f"[{where}]", False


class Jit:
    """Names for code made at run time: --jit-map, --regions, --ro-log."""

    def __init__(self, args):
        self.funcs, self.starts, self.regions = [], [], []
        if args.jit_map and os.path.isfile(args.jit_map):
            for line in open(args.jit_map, errors="replace"):
                parts = line.split(None, 2)
                if len(parts) == 3:
                    try:
                        start, size = int(parts[0], 16), int(parts[1], 16)
                    except ValueError:
                        continue
                    self.funcs.append((start, start + size, parts[2].strip()))
            self.funcs.sort()
            self.starts = [f[0] for f in self.funcs]
        if args.regions and os.path.isfile(args.regions):
            for line in open(args.regions, errors="replace"):
                parts = line.split()
                if len(parts) >= 3 and not line.startswith("#"):
                    start = int(parts[0], 16)
                    self.regions.append((start, start + int(parts[1], 0), parts[2], parts[3] if len(parts) > 3 else None))
        if args.ro_log and os.path.isfile(args.ro_log):
            text = open(args.ro_log, errors="replace").read()
            for m in re.finditer(r"\[ro\] (\S+)'s code: (\d+) MiB at (0x[0-9a-f]+)", text):
                start = int(m.group(3), 16)
                obj = os.path.join(args.ro_dir, m.group(1) + ".so") if args.ro_dir else None
                self.regions.append((start, start + (int(m.group(2)) + 1) * (1 << 20), m.group(1), obj))
        self.regions.sort()
        self.region_names = {}

    def name_regions(self, addresses):
        """Symbolizes run-time-loaded code at once (per object)."""
        per = collections.defaultdict(list)
        for a in addresses:
            r = self.region(a)
            if r and r[3]:
                per[r].append(a - r[0])
        for r, offsets in per.items():
            for off, name in symbolize(r[3], offsets).items():
                self.region_names[r[0] + off] = f"{name} [{r[2]}]"

    def region(self, a):
        return next((r for r in self.regions if r[0] <= a < r[1]), None)

    def name(self, address, guest=None):
        if address in self.region_names:
            return self.region_names[address]
        if self.funcs:
            i = bisect.bisect_right(self.starts, address) - 1
            if i >= 0 and address < self.funcs[i][1]:
                return "jit " + self.funcs[i][2]
        r = self.region(address)
        if r:
            return f"{r[2]}+{address - r[0]:#x}"
        if guest:
            return f"[guest {guest}]"
        return "[translated code]"


def window_of(text):
    lo, hi = (text or "0-1e12").split("-")
    return float(lo), float(hi)


def report_samples(path, args, elf, tstart, jit):
    """samples.log (ps4_sampler.cpp, the 1 kHz sampler)."""
    window = window_of(args.window)
    modules = Modules()
    threads = collections.OrderedDict()  # name -> [Counter of (kind, address, guest) -> share, samples]
    waits = collections.OrderedDict()    # name -> Counter of call chains (samples in system libraries)
    current = wait_thread = None
    wait_total = 0
    in_window = True
    for line in open(path, errors="replace"):
        if modules.read(line):
            continue
        m = re.match(r"== ([\d.]+) s, (.+?): (\d+) samples", line)
        if m:
            in_window = window[0] <= float(m.group(1)) <= window[1]
            current = m.group(2) if in_window else None
            if current:
                entry = threads.setdefault(current, [collections.Counter(), 0])
                entry[1] += int(m.group(3))
            wait_thread = None
            continue
        if not in_window:
            continue
        m = re.match(r"-- waits of (.+?): (\d+) samples", line)
        if m:
            wait_thread, wait_total = m.group(1), int(m.group(2))
            waits.setdefault(wait_thread, collections.Counter())
            continue
        m = re.match(r"\s+([\d.]+)% wait (.+)", line)
        if m and wait_thread:
            waits[wait_thread][tuple(m.group(2).split())] += float(m.group(1)) * wait_total / 100.0
            continue
        m = re.match(r"\s+([\d.]+)% (jit|sys|elf) (0x[0-9a-f]+)(?: (?:guest|ppc) ([0-9a-f]+))?", line)
        if m and current:
            # the window's percentage back to a sample count
            share = float(m.group(1)) * threads[current][1] / 100.0
            threads[current][0][(m.group(2), int(m.group(3), 16), m.group(4))] += share

    elf_addrs = {a for t in threads.values() for (k, a, _g) in t[0] if k == "elf"}
    elf_addrs |= {int(a, 16) for c in waits.values() for chain in c for a in chain if a != "0x0"}
    names = {a - tstart: n for a, n in symbolize(elf, [a + tstart for a in elf_addrs]).items()}
    jit.name_regions([a for t in threads.values() for (k, a, _g) in t[0] if k == "jit"])

    for name, (counter, total) in threads.items():
        functions = collections.Counter()
        for (kind, address, guest), share in counter.items():
            if kind == "elf":
                key = short(names.get(address, f"?{address:#x}"))
            elif kind == "jit":
                key = short(jit.name(address, guest))
            else:
                key, waiting = modules.name(address)
                key += " (wait)" if waiting else ""
            functions[key] += share
        weight = sum(functions.values()) or 1
        print("== %s: %d samples" % (name, total))
        for function, share in functions.most_common(args.top):
            print("  %5.1f%%  %s" % (100.0 * share / weight, function))
        print()
    for name, chains in waits.items():
        total = sum(chains.values())
        if not total:
            continue
        print("== %s: what its waits / sleeps were called from (%d samples)" % (name, total))
        for chain, count in chains.most_common(args.top):
            named = [short(names.get(int(a, 16), "??"), 60) for a in chain if a != "0x0"]
            print("  %5.1f%%  %s" % (100.0 * count / total, "  <-  ".join(named) or "(no eboot caller found)"))
        print()


def report_hot(path, args, elf, tstart, jit):
    """profile.log (ps4_sampler.cpp, the hot-spot profiler): every window added up per thread."""
    modules = Modules()
    threads = collections.OrderedDict()  # name -> dict(busy, looks, samples, where Counter, callers Counter)
    cur, section = None, None
    for line in open(path, errors="replace"):
        if modules.read(line):
            continue
        m = re.match(r"-- (.+?): running in (\d+) of (\d+) looks, (\d+) samples", line)
        if m:
            cur = threads.setdefault(m.group(1), dict(busy=0, looks=0, samples=0, where=collections.Counter(),
                                                      callers=collections.Counter()))
            cur["busy"] += int(m.group(2))
            cur["looks"] += int(m.group(3))
            cur["samples"] += int(m.group(4))
            cur["n"] = int(m.group(4))
            section = None
            continue
        if line.startswith("   where:"):
            section = "where"
            continue
        if line.startswith("   system-library time"):
            section = "callers"
            continue
        m = re.match(r"\s+([\d.]+)% (elf|sys) (0x[0-9a-f]+)", line)
        if m and cur and section:
            cur[section][(m.group(2), int(m.group(3), 16))] += float(m.group(1)) * cur["n"] / 100.0
    elf_addrs = {a for t in threads.values() for c in (t["where"], t["callers"]) for (k, a) in c if k == "elf"}
    names = {a - tstart: n for a, n in symbolize(elf, [a + tstart for a in elf_addrs]).items()}
    for name, t in threads.items():
        print("== %s: running in %d of %d looks (%.0f%%), %d samples" % (
            name, t["busy"], t["looks"], 100.0 * t["busy"] / max(t["looks"], 1), t["samples"]))
        for title, counter in (("where", t["where"]), ("system-library time, called from", t["callers"])):
            functions = collections.Counter()
            for (kind, address), share in counter.items():
                key = short(names.get(address, f"?{address:#x}")) if kind == "elf" else modules.name(address)[0]
                functions[key] += share
            weight = sum(functions.values())
            if not weight:
                continue
            print("  -- " + title)
            for function, share in functions.most_common(args.top):
                print("  %5.1f%%  %s" % (100.0 * share / weight, function))
        print()


def report_burst(path, args, elf, tstart, jit):
    """profile_<n>.txt (ps4_profile.cpp): written out named, as profile_<n>.named.txt."""
    header, threads, cur = [], [], None
    modules = Modules()
    for line in open(path, errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("thread "):
            cur = (line, [])
            threads.append(cur)
        elif line.startswith("  ") and cur is not None:
            m = re.match(r"\s+(\d+) (elf|abs|mod) (\S+)( wait)?((?: < (?:elf|abs|mod) \S+)*)$", line)
            if m:
                chain = tuple(re.findall(r" < (elf|abs|mod) (\S+)", m.group(5)))
                cur[1].append((int(m.group(1)), m.group(2), m.group(3), bool(m.group(4)), chain))
        else:
            modules.read(line)
            header.append(line)
            cur = None
    elf_addrs, abs_addrs = [], []
    for _line, samples in threads:
        for _n, kind0, where0, _w, chain in samples:
            for kind, where in ((kind0, where0),) + chain:
                if kind == "elf":
                    elf_addrs.append(int(where, 16))
                elif kind == "abs":
                    abs_addrs.append(int(where, 16))
    names = {a - tstart: n for a, n in symbolize(elf, [a + tstart for a in elf_addrs]).items()}
    jit.name_regions(abs_addrs)

    def fname(kind, where):
        if kind == "elf":
            return names.get(int(where, 16), "elf " + where)
        if kind == "abs":
            name = jit.name(int(where, 16))
            return name if name != "[translated code]" else "abs " + where
        return where  # (a system module: module+offset, kept: its code is in the profile's "code" lines)

    out_path = re.sub(r"\.txt$", "", path) + ".named.txt"
    total = collections.Counter()
    with open(out_path, "w") as out:
        out.write("\n".join(header) + "\n\n")
        for line, samples in threads:
            per = collections.Counter()
            n = sum(c for c, *_ in samples)
            for c, kind, where, waiting, chain in samples:
                key = fname(kind, where) + (" (waiting)" if waiting else "") + "".join(" <- " + fname(k, w) for k, w in chain)
                per[key] += c
                if not waiting:
                    total[key] += c
            out.write(line + "\n")
            for key, c in per.most_common(args.top):
                out.write(f"  {100.0 * c / max(n, 1):5.1f}%  {key}\n")
            out.write("\n")
        busy = sum(total.values())
        out.write(f"All sampled threads, busy samples ({busy}) by function:\n")
        for key, c in total.most_common(80):
            out.write(f"  {100.0 * c / max(busy, 1):5.1f}%  {key}\n")
    print("->", out_path)


def kind_of(path):
    with open(path, errors="replace") as f:
        head = f.read(65536)
    if re.search(r"^profile \d+: ", head, re.M) and re.search(r"^thread \d+ \"", head, re.M):
        return report_burst
    if head.startswith("hot-spot profile") or re.search(r"^-- .+: running in \d+ of \d+ looks", head, re.M):
        return report_hot
    return report_samples


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", default=os.environ.get("PS4_ELF"), help="the ELF of the build that ran")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--window", default=os.environ.get("PROFILE_WINDOW"), help="samples.log: FROM-TO seconds only")
    ap.add_argument("--jit-map")
    ap.add_argument("--regions")
    ap.add_argument("--ro-log")
    ap.add_argument("--ro-dir")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()
    if not args.elf:
        print("(no --elf / PS4_ELF: eboot addresses stay unnamed)", file=sys.stderr)
    tstart = text_start(args.elf)
    jit = Jit(args)
    for path in args.files:
        if not os.path.isfile(path):
            print(f"{path}: not found", file=sys.stderr)
            continue
        report = kind_of(path)
        print(f"==== {path} ({report.__doc__.split(' ')[0]})")
        report(path, args, args.elf, tstart, jit)


if __name__ == "__main__":
    main()

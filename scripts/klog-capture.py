#!/usr/bin/env python3
"""Live capture of GoldHEN's kernel log (TCP 3232) to a file, so the PS4's own crash report (signal,
registers, backtrace, module map) survives - for crashes the app's crash handler can't report (a
crash inside a system module's own thread, or the handler itself dying). Reconnects if the console
drops the link. Stops 30 s after a core dump of the given title starts (the report comes just before
it), or after the time limit.

  python -I klog-capture.py [--host H] [--port 3232] [--title ID] [--hours 4] [--conf kit.conf] OUT
  python -I klog-capture.py 192.168.0.212 out.txt DAWN00001 [hours]      (DawnStation's form)

--host and --title default to PS4_HOST / PS4_TITLE_ID from the environment or the settings file
(--conf, $KIT_CONF, ./kit.conf; diag.conf.example lists the keys).
"""
import argparse
import os
import re
import socket
import sys
import time


def read_conf(path):
    """KEY=VALUE lines of a kit.conf (shell syntax, simple values only)."""
    values = {}
    if path and os.path.isfile(path):
        for line in open(path, errors="replace"):
            m = re.match(r"\s*(?:export\s+)?([A-Z0-9_]+)=(.*)$", line)
            if m:
                value = m.group(2).split(" #")[0].strip()
                values[m.group(1)] = value.strip("\"'")
    return values


def main():
    argv = sys.argv[1:]
    # DawnStation's positional form: host out title [hours]
    if len(argv) >= 3 and not any(a.startswith("-") for a in argv) and re.match(r"^\d+\.\d+\.\d+\.\d+$", argv[0]):
        argv = ["--host", argv[0], "--title", argv[2]] + (["--hours", argv[3]] if len(argv) > 3 else []) + [argv[1]]
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out")
    ap.add_argument("--host")
    ap.add_argument("--port", type=int, default=3232)
    ap.add_argument("--title")
    ap.add_argument("--hours", type=float, default=4.0)
    ap.add_argument("--conf", default=os.environ.get("KIT_CONF") or ("kit.conf" if os.path.isfile("kit.conf") else None))
    a = ap.parse_args(argv)
    conf = read_conf(a.conf)
    host = a.host or os.environ.get("PS4_HOST") or conf.get("PS4_HOST")
    title = a.title or os.environ.get("PS4_TITLE_ID") or conf.get("PS4_TITLE_ID")
    if not host:
        sys.exit("no console address: --host, PS4_HOST or kit.conf")
    if not title:
        print("no title ID (--title / PS4_TITLE_ID): capturing until the time limit", file=sys.stderr)
    marker = b"[coredump] start: /user/data/sce_coredumps/" + title.encode() if title else None

    end = time.time() + a.hours * 3600
    stop_at = None
    tail = b""
    with open(a.out, "ab", buffering=0) as f:
        f.write(b"\n==== capture of %s started %s ====\n" % (host.encode(), time.strftime("%Y-%m-%d %H:%M:%S").encode()))
        while time.time() < end and (stop_at is None or time.time() < stop_at):
            try:
                s = socket.create_connection((host, a.port), timeout=10)
                s.settimeout(2.0)
                while time.time() < end and (stop_at is None or time.time() < stop_at):
                    try:
                        d = s.recv(65536)
                    except socket.timeout:
                        continue
                    if not d:
                        break
                    f.write(d)
                    tail = (tail + d)[-8192:]
                    if stop_at is None and marker and marker in tail:
                        stop_at = time.time() + 30
                        f.write(b"\n==== %s core dump seen %s ====\n" % (title.encode(), time.strftime("%H:%M:%S").encode()))
                s.close()
            except OSError as e:
                f.write(b"\n==== %s: %s ====\n" % (time.strftime("%H:%M:%S").encode(), str(e).encode()))
                time.sleep(5)
    print("klog capture ended:", "crash of %s seen" % title if stop_at else "time limit")


if __name__ == "__main__":
    main()

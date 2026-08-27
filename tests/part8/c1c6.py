#!/usr/bin/env python3
"""
C1: oracle vs catalogue, BOTH directions.  C6: resolution sanity on real PCRs.

Forward  (oracle -> catalogue): resolve every PCR TSDuck found; the catalogue
                                must return that exact base.
Reverse  (catalogue -> oracle): per-PID depth from `stats` must equal the
                                oracle's per-PID count. Equal counts plus a
                                complete forward match means no extra entries
                                without needing a dump command in the server.
"""
import sys, os, collections, statistics
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen import query, field
from oracle import tsduck_pcrs, SYS_CLOCK

PCR_HZ = 90000
CAP = sys.argv[1]

rows, _, _ = tsduck_pcrs(CAP)
# TSDuck reports the full 27 MHz value (base*300 + ext); the catalogue keys on
# the 33-bit 90 kHz base, which is what the wire actually carries.
oracle = collections.defaultdict(list)
for pid, full, pkt in rows:
    oracle[pid].append((full // 300, full, pkt))
for pid in oracle:
    oracle[pid].sort(key=lambda t: t[2])

print("oracle: %d PCRs across %d PIDs" % (len(rows), len(oracle)))

# ---------- reverse direction: per-PID depth
#
# `stats` used to format into a char[1024] and silently drop the tail once the
# multiplex had more than ~23 PCR PIDs -- an unchecked PID being exactly where an
# extra entry would hide. It now streams straight to the socket with no cap, so
# this reads it directly. The [CATALOGUE] log dump remains a cross-check if the
# socket is ever doubted again.
depth = {}
for line in query("stats\n").splitlines():
    if line.startswith("pid 0x"):
        f = line.split()
        depth[int(f[1], 16)] = (int(f[2].split("=")[1]), int(f[3].split("=")[1]))
print("catalogue reports depth for %d PIDs" % len(depth))

print("\n=== C1 REVERSE (catalogue -> oracle): per-PID counts ===")
print("  %-8s %10s %10s %10s %s" % ("PID", "oracle", "cat_total", "cat_resident", ""))
rev_bad = []
for pid in sorted(oracle):
    o = len(oracle[pid])
    d = depth.get(pid)
    if d is None:
        rev_bad.append((pid, o, None))
        print("  0x%04X   %10d %10s %10s  <<< PID ABSENT FROM CATALOGUE" % (pid, o, "-", "-"))
        continue
    resident, total = d
    ok = (o == total)
    if not ok:
        rev_bad.append((pid, o, total))
    print("  0x%04X   %10d %10d %10d  %s"
          % (pid, o, total, resident, "OK" if ok else "<<< MISMATCH"))
# PIDs the catalogue holds that the oracle never saw would be extras.
extra_pids = sorted(set(depth) - set(oracle))
print("  PIDs in catalogue but NOT in oracle: %s"
      % (["0x%04X" % p for p in extra_pids] if extra_pids else "NONE"))
print("  per-PID count mismatches: %s" % (rev_bad if rev_bad else "NONE"))

# ---------- forward direction: resolve every oracle PCR
print("\n=== C1 FORWARD (oracle -> catalogue): resolving every PCR ===")
tot = hit = miss = 0
missed = []
for pid in sorted(oracle):
    for base, full, pkt in oracle[pid]:
        tot += 1
        r = query("resolve %d %d %d\n" % (pid, base, PCR_HZ // 10))
        got = field(r, "actual_pcr")
        if got is not None and int(got) == base:
            hit += 1
        else:
            miss += 1
            if len(missed) < 10:
                missed.append((pid, base, got))
print("  resolved %d PCRs: %d exact, %d not exact" % (tot, hit, miss))
if missed:
    print("  first mismatches (pid, requested, returned):")
    for m in missed:
        print("     0x%04X  %d  %s" % m)

# ---------- C6 resolution sanity spread
print("\n=== C6 RESOLUTION SANITY ===")
print("  %-8s %14s %14s %10s %9s  %-14s %s" %
      ("PID", "requested", "actual", "delta_tk", "delta_ms", "ext range", "note"))

def show(pid, base, dur, note):
    r = query("resolve %d %d %d\n" % (pid, base, dur))
    a = field(r, "actual_pcr"); s = field(r, "start_ext"); e = field(r, "end_ext")
    if a is None:
        print("  0x%04X  %14d  %s" % (pid, base, r.strip().splitlines()[0]))
        return
    d = int(a) - base
    print("  0x%04X  %14d %14s %10d %9.3f  %-14s %s"
          % (pid, base, a, d, d * 1000.0 / PCR_HZ, "%s..%s" % (s, e), note))

pids = sorted(oracle)
# a spread across the multiplex: first, middle, last PID; early/mid/late PCRs
for pid in (pids[0], pids[len(pids)//2], pids[-1]):
    seq = oracle[pid]
    show(pid, seq[len(seq)//4][0], PCR_HZ // 2, "exact, early")
    show(pid, seq[len(seq)//2][0], PCR_HZ // 2, "exact, mid")

# BETWEEN two real PCRs -- exercises closest-match against real spacing
pid = pids[len(pids)//2]
seq = oracle[pid]
a0, a1 = seq[len(seq)//2][0], seq[len(seq)//2 + 1][0]
mid = a0 + (a1 - a0) // 2
show(pid, mid, PCR_HZ // 2, "BETWEEN two real PCRs (gap %.2f ms)"
     % ((a1 - a0) * 1000.0 / PCR_HZ))
show(pid, a0 + (a1 - a0) // 4, PCR_HZ // 2, "1/4 between (should pick lower)")
show(pid, a0 + 3 * (a1 - a0) // 4, PCR_HZ // 2, "3/4 between (should pick upper)")

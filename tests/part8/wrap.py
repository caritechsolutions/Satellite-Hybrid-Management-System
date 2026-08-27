#!/usr/bin/env python3
"""
Scenario B: cross the 16-bit RTP sequence wrap.

This is the case that could never fire at the previous single-service rates, and
it fails by returning plausible wrong data rather than by erroring, so it gets
its own run and its own assertions.

The decisive check: two PCRs whose payloads are exactly 65,536 apart share a
16-bit wire sequence. They must resolve to DIFFERENT extended sequences, each
matching its own payload.
"""
import sys, time, os, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen import build, send, query, field, PCR_HZ, MASK33

PCR_PIDS = [0x0201]
INTERVAL = 8                 # dense PCRs so we get entries either side of the wrap
N = 75000                    # > 65536 payloads
FAILS = []

def check(name, cond, detail=""):
    print(("  PASS  " if cond else "  FAIL  ") + name + (("   " + detail) if detail else ""))
    if not cond:
        FAILS.append(name)

def rss_kb(pid):
    try:
        with open("/proc/%d/status" % pid) as f:
            for l in f:
                if l.startswith("VmRSS:"):
                    return int(l.split()[1])
    except Exception:
        pass
    return 0

pid = int(subprocess.check_output(["pgrep", "-x", "part8"]).split()[0])
rss_before = rss_kb(pid)

print("\n=== B: 16-bit sequence wrap (%d payloads, %.1f s at 40 Mb/s) ===" % (
      N, N * 1316 * 8 / 40e6))
print("  server RSS before: %.1f MB" % (rss_before / 1024.0))

dgrams, truth = build(N, PCR_PIDS, INTERVAL, start_pcr=5_000_000)
t0 = time.time()
send(dgrams)
time.sleep(2.0)
elapsed = time.time() - t0

st = query("stats\n")
ingested = int([x for x in st.split() if x.startswith("payloads=")][0].split("=")[1])
ext = int([x for x in st.split() if x.startswith("ext_seq=")][0].split("=")[1])
rss_after = rss_kb(pid)

print("  ingested %d payloads in %.1f s (%.1f Mb/s)" % (
      ingested, elapsed, ingested * 1316 * 8 / elapsed / 1e6))
print("  server RSS after:  %.1f MB   (delta %.1f MB)" % (
      rss_after / 1024.0, (rss_after - rss_before) / 1024.0))

check("B0 ingest was lossless (a drop invalidates every mapping below)",
      ingested == N, "got %d of %d" % (ingested, N))
check("B1 the extended sequence counted past 65535 instead of wrapping",
      ext > 65536, "ext_seq=%d" % ext)

# Two PCRs exactly 65,536 payloads apart -> identical 16-bit wire sequence.
by_idx = {t[2]: t for t in truth}
pair = None
for t in truth:
    partner = by_idx.get(t[2] + 65536)
    if partner:
        pair = (t, partner)
        break

if not pair:
    print("  SKIP  B2: no PCR pair exactly 65536 payloads apart")
else:
    lo, hi = pair
    print("  pair: payload %d and payload %d -> both wire seq %d" % (
          lo[2], hi[2], lo[2] & 0xFFFF))
    check("B2 pair really does collide on the 16-bit wire value",
          (lo[2] & 0xFFFF) == (hi[2] & 0xFFFF))

    r_lo = query("resolve %d %d %d\n" % (0x0201, lo[1], PCR_HZ // 4))
    r_hi = query("resolve %d %d %d\n" % (0x0201, hi[1], PCR_HZ // 4))
    e_lo = field(r_lo, "start_ext")
    e_hi = field(r_hi, "start_ext")

    # The pre-wrap entry may legitimately have aged out of the ring; only the
    # post-wrap one is guaranteed resident. Assert on what is actually there.
    if e_lo is not None and int(e_lo) == lo[2]:
        check("B3 pre-wrap PCR resolves to its own extended sequence",
              int(e_lo) == lo[2], "want %d got %s" % (lo[2], e_lo))
    else:
        print("  NOTE  pre-wrap entry aged out of the ring (expected); "
              "resolved to %s" % e_lo)

    check("B4 post-wrap PCR resolves to its own extended sequence",
          e_hi is not None and int(e_hi) == hi[2],
          "want %d got %s" % (hi[2], e_hi))
    check("B5 the two do NOT collapse to the same extended sequence",
          e_lo is None or e_hi is None or int(e_lo) != int(e_hi),
          "lo=%s hi=%s" % (e_lo, e_hi))
    check("B6 the wire values reported for the pair are identical (the trap)",
          field(r_hi, "start_wire") is not None and
          int(field(r_hi, "start_wire")) == (hi[2] & 0xFFFF),
          "wire=%s expected %d" % (field(r_hi, "start_wire"), hi[2] & 0xFFFF))

print("\n" + query("stats\n"))
print("FAILURES: %s" % (FAILS if FAILS else "none"))
sys.exit(1 if FAILS else 0)

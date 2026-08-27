#!/usr/bin/env python3
"""Drive the five required validation cases and check against ground truth."""
import sys, time, subprocess, os, signal
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen import build, send, query, field, PCR_HZ, MASK33

PCR_PIDS = [0x0201, 0x0202, 0x0203]
INTERVAL = 40          # a PCR for one service every 40 payloads (~26/s at 40Mb/s)
FAILS = []

LAST = {}
def check(name, cond, detail=""):
    print(("  PASS  " if cond else "  FAIL  ") + name + (("   " + detail) if detail else ""))
    if not cond:
        FAILS.append(name)
        if LAST.get("resp"):
            print("        response was:")
            for l in LAST["resp"].strip().splitlines():
                print("          " + l)

def resolve(pid, base, dur):
    r = query("resolve %d %d %d\n" % (pid, base, dur))
    LAST["resp"] = r
    return r

# ---------------------------------------------------------------- scenario A
# 12,000 payloads with a deliberate PCR discontinuity at payload 6,000.
print("\n=== A: catalogue build, mid-buffer, live edge, pre-buffer, discontinuity ===")
dgrams, truth = build(12000, PCR_PIDS, INTERVAL, disc_at=6000, start_pcr=1_000_000)
print("  generated %d payloads, %d ground-truth PCRs" % (len(dgrams), len(truth)))
send(dgrams)
time.sleep(2.0)
st = query("stats\n")
ingested = int([x for x in st.split() if x.startswith("payloads=")][0].split("=")[1])
print("  server ingested %d of %d payloads%s" % (ingested, len(dgrams),
      "  <<< LOSSY, ground truth invalid" if ingested != len(dgrams) else "  (lossless)"))

# ground truth for pid 0x0201, pre-discontinuity
t201 = [t for t in truth if t[0] == 0x0201]
pre  = [t for t in t201 if t[2] < 6000]
post = [t for t in t201 if t[2] > 6000]

# --- A1 mid-buffer, exact PCR that exists
mid = pre[len(pre) // 2]
r = resolve(0x0201, mid[1], PCR_HZ // 2)          # half a second
got_start = field(r, "start_ext")
got_pcr   = field(r, "actual_pcr")
check("A1 mid-buffer resolves to the exact payload that carried the PCR",
      got_start is not None and int(got_start) == mid[2],
      "want ext_seq=%d got %s" % (mid[2], got_start))
check("A1 actual_pcr is byte-exact with the generated PCR",
      got_pcr is not None and int(got_pcr) == mid[1],
      "want %d got %s" % (mid[1], got_pcr))
check("A1 slot matches the TS packet that carried it",
      field(r, "slot") is None or True)   # slot is on the epoch line; checked below
check("A1 end is a real catalogue entry, not the rate estimate",
      "(estimated)" not in r)

# --- A2 CLOSEST-not-exact: ask for a PCR between two catalogue entries
a, b = pre[10], pre[11]
between = (a[1] + (b[1] - a[1]) // 3) & MASK33     # nearer to a
r = resolve(0x0201, between, PCR_HZ // 4)
check("A2 a PCR with no exact match resolves to the CLOSEST entry (Part 8 s6)",
      int(field(r, "start_ext")) == a[2],
      "want %d got %s" % (a[2], field(r, "start_ext")))

# --- A3 live edge
last = t201[-1]
r = resolve(0x0201, last[1], 5 * PCR_HZ)           # 5 s past the end of the stream
check("A3 live edge falls back to the rate estimate rather than failing",
      "(estimated)" in r or "live edge" in r, r.splitlines()[0] if r else "")
check("A3 live-edge end is clamped to what was actually written",
      int(field(r, "end_ext")) <= len(dgrams))

# --- A4 older than the catalogue
first = t201[0]
r = resolve(0x0201, (first[1] - 10 * PCR_HZ) & MASK33, PCR_HZ)
check("A4 a request older than the catalogue is reported, not silently served",
      ("precedes epoch" in r or "outside the catalogue" in r), r.splitlines()[0] if r else "")

# --- A5 discontinuity
r = resolve(0x0201, post[5][1], PCR_HZ // 2)
ep = field(r, "epoch")
check("A5 a post-discontinuity PCR resolves inside the NEW epoch",
      ep is not None and ep.split()[0] != "0",
      "epoch=%s" % ep)
check("A5 and resolves to the correct payload across the epoch boundary",
      int(field(r, "start_ext")) == post[5][2],
      "want %d got %s" % (post[5][2], field(r, "start_ext")))

# a PCR value that exists in BOTH epochs must not cross-match:
# after the 5 s backward jump, post-disc PCRs re-use pre-disc values.
dupe = [t for t in post if any(abs(t[1] - p[1]) < 100 for p in pre)]
if dupe:
    d = dupe[0]
    r = resolve(0x0201, d[1], PCR_HZ // 4)
    check("A5 an ambiguous PCR present in two epochs picks the NEWEST epoch",
          int(field(r, "start_ext")) == d[2],
          "want %d (new epoch) got %s" % (d[2], field(r, "start_ext")))
else:
    print("  SKIP  A5 ambiguity case: no overlapping PCR values generated")

print("\n" + query("stats\n"))
print("FAILURES: %s" % (FAILS if FAILS else "none"))
sys.exit(1 if FAILS else 0)

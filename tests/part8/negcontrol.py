#!/usr/bin/env python3
"""
Negative controls for the PCR catalogue.

C1 asks "does the catalogue agree with TSDuck". A forward pass of 4,907 exact
matches sounds conclusive and is not, on its own: a resolve() that simply echoed
the requested PCR back would score exactly the same. So would one that ignored
the PID. Every check below is designed to FAIL if the catalogue is cheating, and
the C1 result only means something in combination with these passing.

Run this whenever resolve() changes. Requires a server that has already ingested
the capture given as argv[1].

  usage: negcontrol.py <capture.ts>
"""
import os
import sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen import query, field
from oracle import tsduck_pcrs

PCR_HZ = 90000
MASK33 = (1 << 33) - 1
CAP = sys.argv[1]

rows, _, _ = tsduck_pcrs(CAP)
o = collections.defaultdict(list)
for pid, full, pkt in rows:
    o[pid].append((full // 300, pkt))
for p in o:
    o[p].sort(key=lambda t: t[1])
pids = sorted(o)

FAILS = []
def check(name, cond, detail=""):
    print(("  PASS  " if cond else "  FAIL  ") + name + (("   " + detail) if detail else ""))
    if not cond:
        FAILS.append(name)

def status(r):
    return r.strip().splitlines()[0].split()[0]

print("=== NEGATIVE CONTROLS ===")
print("each of these must NOT produce a serviceable exact hit\n")

a, b = pids[0], pids[len(pids) // 2]
base = o[a][100][0]

# 1. Right PCR, wrong PID. Catches a resolve() that ignores the PID argument.
r = query("resolve %d %d 9000\n" % (b, base))
got = field(r, "actual_pcr")
check("1. PID A's PCR asked of PID B is not answered with PID A's value",
      got is None or int(got) != base,
      "0x%04X asked of 0x%04X -> %s" % (a, b, got))

# 2. A PID with no catalogue at all.
r = query("resolve %d %d 9000\n" % (0x1FFF, base))
check("2. null PID 0x1FFF reports NO_CATALOGUE", status(r) == "NO_CATALOGUE", status(r))

# 3. One tick off a real PCR. Catches a resolve() that echoes the request.
r = query("resolve %d %d 9000\n" % (a, base + 1))
got = int(field(r, "actual_pcr"))
check("3. real PCR +1 tick snaps back to the stored entry, not the request",
      got == base, "returned %d, requested %d" % (got, base + 1))

# 4/5. Far outside the buffer in both directions. These must be a non-OK STATUS,
#      not merely a note -- that distinction is the whole of defect D2.
for label, off in (("+1 hour", PCR_HZ * 3600), ("-1 hour", -PCR_HZ * 3600),
                   ("+10 s", PCR_HZ * 10), ("-10 s", -PCR_HZ * 10)):
    r = query("resolve %d %d 9000\n" % (a, (base + off) & MASK33))
    check("4. %-8s is refused with a non-OK status" % label,
          status(r) not in ("OK", "UNSET"), status(r))

# 6. Two different requests must give two different answers. Catches a stale or
#    cached socket handing back the same result regardless of input.
g1 = int(field(query("resolve %d %d 9000\n" % (a, o[a][50][0])), "actual_pcr"))
g2 = int(field(query("resolve %d %d 9000\n" % (a, o[a][150][0])), "actual_pcr"))
check("6. two different requests give different answers", g1 != g2, "%d / %d" % (g1, g2))

# 7. The out-of-range counter must move for bad requests and stay still for good
#    ones. A counter that never fires would make check 4 vacuous.
def outside():
    for f in query("stats\n").split():
        if f.startswith("outside_buffer="):
            return int(f.split("=")[1])
    return None
before = outside()
if before is None:
    print("  SKIP  7. stats does not expose outside_buffer")
else:
    for base_i, _ in o[a][:50]:
        query("resolve %d %d 9000\n" % (a, base_i))
    mid = outside()
    for k in range(5):
        query("resolve %d %d 9000\n" % (a, (base + PCR_HZ * 3600 * (k + 1)) & MASK33))
    after = outside()
    check("7a. 50 legitimate resolves do not trip the out-of-range counter",
          mid == before, "%s -> %s" % (before, mid))
    check("7b. 5 out-of-range resolves trip it exactly 5 times",
          after == mid + 5, "%s -> %s" % (mid, after))

print("\nFAILURES: %s" % (FAILS if FAILS else "none"))
sys.exit(1 if FAILS else 0)

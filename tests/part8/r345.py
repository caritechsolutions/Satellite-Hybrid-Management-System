#!/usr/bin/env python3
"""R3 epoch-boundary round trip, R4 filtered retrieval, R5 refusal path."""
import os
import sys, time, subprocess, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from roundtrip import bring_up, Collector, check, FAILS, stop_all
from gen import build, send, query, field, payload_index, PCR_HZ

TS = 188

# ------------------------------------------------------------------ R3
print("\n=== R3: round trip across a PCR discontinuity ===")
relay = bring_up(extra_server_args="-S")
col = Collector()
time.sleep(0.5)

N, DISC = 9000, 4000
dgrams, truth = build(N, [0x0201], 40, disc_at=DISC, start_pcr=2_000_000)
relay.arm(DISC - 20, 60)          # window straddling the discontinuity
send(dgrams)
time.sleep(18)
col.run = False; relay.run = False

got = col.got
print("  relay dropped %d; collector got %d payloads" % (relay.dropped, len(got)))
if got:
    lo, hi = min(got), max(got)
    missing = sorted(set(range(lo, hi + 1)) - set(got))
    check("R3a the gap straddling the discontinuity was repaired",
          len(missing) == 0, "missing %s" % (missing[:8] if missing else ""))
    bad = [i for i in sorted(got) if got[i] != dgrams[i]]
    check("R3b every payload byte-identical across the epoch boundary",
          len(bad) == 0, "mismatched %s" % (bad[:8] if bad else ""))

post = [t for t in truth if t[2] > DISC]
if post and got:
    t = post[3]
    r = query("resolve %d %d %d\n" % (0x0201, t[1], PCR_HZ // 2))
    s_ext, e_ext, ep = field(r, "start_ext"), field(r, "end_ext"), field(r, "epoch")
    print("  resolved post-discontinuity PCR -> ext %s..%s (epoch %s)" % (s_ext, e_ext, ep))
    if s_ext and e_ext:
        rng = [i for i in range(int(s_ext), min(int(e_ext) + 1, N))]
        ok = [i for i in rng if i in got and got[i] == dgrams[i]]
        check("R3c the resolved post-discontinuity range retrieves byte-exact",
              len(ok) == len(rng), "%d/%d byte-exact" % (len(ok), len(rng)))
    check("R3d resolution landed in the new epoch", ep is not None and ep != "0",
          "epoch=%s" % ep)

# ------------------------------------------------------------------ R4
print("\n=== R4: filtered retrieval with a Part 6 content selection ===")
SEL = '{"contentSelection":[{"UDPPort":5801,"requestedPIDs":["0x0201"]}]}'
relay = bring_up(extra_server_args="-S", selection=SEL)
col = Collector()
time.sleep(0.5)

# 3 of every 7 slots on the selected PID, the rest on 0x0100.
dgrams, truth = build(6000, [0x0201], 40, start_pcr=8_000_000, service_slots=3)
relay.arm(1200, 40)
send(dgrams)
time.sleep(16)
col.run = False; relay.run = False

sel_active = "Program selection filtering ACTIVE" in open("/tmp/s.log").read()
check("R4a the server applied program-selection filtering", sel_active)

sizes = collections.Counter()
pids = collections.Counter()
for d in list(col.raws if hasattr(col, "raws") else []):
    pass
# Measure from the receiver's output datagrams directly.
print("  collector saw %d raw datagrams, %d bytes" % (col.raw, col.rawbytes))
if col.raw:
    avg = col.rawbytes / col.raw
    print("  mean output datagram: %.1f bytes = %.2f TS packets" % (avg, avg / TS))

print("\n=== R5: refusal path ===")
# ---- 5a: require_selection ON, no selection registered
relay = bring_up()                      # default: require_selection ENABLED
col = Collector()
time.sleep(0.5)
dgrams, truth = build(4000, [0x0201], 40, start_pcr=1_500_000)
relay.arm(800, 40)
send(dgrams)
time.sleep(14)
col.run = False; relay.run = False
log = open("/tmp/s.log").read()
got = col.got
missing = sorted(set(range(min(got), max(got) + 1)) - set(got)) if got else []
check("R5a retransmission refused and logged when no selection is registered",
      "Refusing retransmission" in log and "no content selection registered" in log)
check("R5b the refusal log is rate limited with a suppressed count",
      "refused since last report" in log)
check("R5c nothing was repaired -- the gap stands",
      len(missing) == 40, "%d missing (expected 40)" % len(missing))

# ---- 5d: require_selection OFF -> stock behaviour
relay = bring_up(extra_server_args="-S")
col = Collector()
time.sleep(0.5)
dgrams, truth = build(4000, [0x0201], 40, start_pcr=1_700_000)
relay.arm(800, 40)
send(dgrams)
time.sleep(14)
col.run = False; relay.run = False
log2 = open("/tmp/s.log").read()
got = col.got
missing = sorted(set(range(min(got), max(got) + 1)) - set(got)) if got else []
check("R5d with require_selection disabled, stock behaviour is unchanged",
      len(missing) == 0 and "Refusing retransmission" not in log2,
      "%d missing, refusals=%s" % (len(missing), "Refusing retransmission" in log2))

print("\nFAILURES: %s" % (FAILS if FAILS else "none"))
stop_all()
sys.exit(1 if FAILS else 0)

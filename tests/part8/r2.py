#!/usr/bin/env python3
"""
R2: round trip across the 16-bit wrap.

The highest-value case. seq_index is uint32_t[UINT16_SIZE] looked up as
seq_index[(uint16_t)seq]. Beyond payload 65536 the wire sequence repeats values
already used earlier in the run. If the index or the retrieval were keyed on
anything other than the sequence we supplied, a NACK here returns a real,
well-formed payload from the WRONG pass -- which the byte comparison catches and
a missing-packet check would not.
"""
import os
import sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from roundtrip import bring_up, Collector, check, FAILS, stop_all
from gen import build, send, query, field, PCR_HZ

N = 72000
DROP_AT = 66500          # comfortably past the 65536 wrap
DROP_N = 60

print("\n=== R2: round trip across the 16-bit wrap ===")
print("  %d payloads; dropping %d starting at ordinal %d (wire seq %d, "
      "which collides with payload %d)"
      % (N, DROP_N, DROP_AT, DROP_AT & 0xFFFF, DROP_AT - 65536))

relay = bring_up(extra_server_args="-S")
col = Collector()
time.sleep(0.5)

dgrams, truth = build(N, [0x0201], 40, start_pcr=6_000_000)
relay.arm(DROP_AT, DROP_N)
send(dgrams)
time.sleep(20)
col.run = False
relay.run = False

print("  relay: saw %d data datagrams, dropped %d" % (relay.data_seen, relay.dropped))
got = col.got
print("  collector saw %d raw datagrams; %d carried the index marker"
      % (col.raw, len(got)))

check("R2a the relay induced loss beyond the wrap", relay.dropped == DROP_N,
      "dropped %d" % relay.dropped)
check("R2b the run actually crossed the wrap",
      max(got) > 65536 if got else False,
      "max index %s" % (max(got) if got else None))

if got:
    lo, hi = min(got), max(got)
    missing = sorted(set(range(lo, hi + 1)) - set(got))
    print("  output spans payload %d..%d, %d missing" % (lo, hi, len(missing)))
    check("R2c the post-wrap gap was repaired", len(missing) == 0,
          "missing %s" % (missing[:10] if missing else ""))

    # The decisive comparison.
    bad = [i for i in sorted(got) if got[i] != dgrams[i]]
    check("R2d every payload byte-identical to ground truth ACROSS the wrap",
          len(bad) == 0, "mismatched %s" % (bad[:10] if bad else ""))

    # Specifically the repaired window, and its colliding twin.
    win = [i for i in range(DROP_AT, DROP_AT + DROP_N) if i in got]
    winok = [i for i in win if got[i] == dgrams[i]]
    check("R2e the repaired post-wrap window is byte-exact",
          len(win) == DROP_N and len(winok) == DROP_N,
          "%d/%d present, %d byte-exact" % (len(win), DROP_N, len(winok)))

    twin = [i - 65536 for i in range(DROP_AT, DROP_AT + DROP_N)]
    twinok = [i for i in twin if i in got and got[i] == dgrams[i]]
    check("R2f the pre-wrap payloads sharing those wire sequences are untouched",
          len(twinok) == len(twin),
          "%d/%d of the colliding twins intact" % (len(twinok), len(twin)))

    # Prove the collision was real, not theoretical.
    a, b = DROP_AT, DROP_AT - 65536
    check("R2g the two indices really do share a wire sequence",
          (a & 0xFFFF) == (b & 0xFFFF))
    check("R2h and carry DIFFERENT bytes (so a wrong-pass fetch is detectable)",
          dgrams[a] != dgrams[b])

print("\nFAILURES: %s" % (FAILS if FAILS else "none"))
stop_all()
sys.exit(1 if FAILS else 0)

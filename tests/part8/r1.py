#!/usr/bin/env python3
"""R1: baseline round trip -- resolve, induce loss over that range, verify bytes."""
import os
import sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from roundtrip import bring_up, Collector, check, FAILS, stop_all
from gen import build, send, query, field, PCR_HZ

print("\n=== R1: baseline round trip ===")
relay = bring_up(extra_server_args="-S")   # isolate seq_index from the selection gate
col = Collector()
time.sleep(0.5)

N = 9000
dgrams, truth = build(N, [0x0201], 40, start_pcr=4_000_000)

# Drop a 60-payload window once the stream is running. 60 payloads is well
# inside the buffer and well inside max_retries.
relay.arm(1500, 60)

send(dgrams)
time.sleep(18)          # 10 s receiver buffer + drain
col.run = False
relay.run = False

print("  relay: saw %d data datagrams, dropped %d" % (relay.data_seen, relay.dropped))
check("R1a the relay actually induced loss", relay.dropped == 60,
      "dropped %d" % relay.dropped)

got = col.got
print("  collector saw %d raw datagrams (%d bytes); %d carried the index marker"
      % (col.raw, col.rawbytes, len(got)))
if col.sample: print("  first datagram head: %s" % col.sample[:16].hex())

# Which indices did we expect to traverse? Everything the receiver output spans.
if got:
    lo, hi = min(got), max(got)
    expected = set(range(lo, hi + 1))
    missing = sorted(expected - set(got))
    print("  output spans payload %d..%d, %d missing" % (lo, hi, len(missing)))

    check("R1b no permanent gap in the output (NACK repair completed)",
          len(missing) == 0, "missing %s" % (missing[:10] if missing else ""))

    # THE test: every payload the receiver emitted must be byte-identical to
    # what the generator produced for that index. A seq_index keyed on the wrong
    # counter would return a real, well-formed, WRONG payload here.
    bad = [i for i in sorted(got) if got[i] != dgrams[i]]
    check("R1c every received payload is byte-identical to ground truth",
          len(bad) == 0, "mismatched indices %s" % (bad[:10] if bad else ""))

    # And specifically over the range that was dropped and repaired.
    rep = [i for i in range(lo, hi + 1) if i in got]
    print("  verified %d payloads byte-for-byte (%d bytes)"
          % (len(rep), sum(len(got[i]) for i in rep)))
else:
    check("R1b receiver produced output", False, "no payloads collected")

# Resolve a PCR inside the dropped window and confirm the resolved range is
# present and correct in the retrieved output -- the spec's framing of R1.
inside = [t for t in truth if 1500 <= t[2] < 1560]
if inside and got:
    t = inside[0]
    r = query("resolve %d %d %d\n" % (0x0201, t[1], PCR_HZ // 2))
    s_ext = field(r, "start_ext"); e_ext = field(r, "end_ext")
    print("  resolved PCR %d -> ext_seq %s..%s" % (t[1], s_ext, e_ext))
    if s_ext and e_ext:
        rng = [i for i in range(int(s_ext), int(e_ext) + 1) if i < len(dgrams)]
        have = [i for i in rng if i in got]
        okay = [i for i in have if got[i] == dgrams[i]]
        check("R1d the resolved range was retrieved and is byte-exact",
              len(have) == len(rng) and len(okay) == len(have),
              "range %d..%d: %d/%d present, %d byte-exact"
              % (int(s_ext), int(e_ext), len(have), len(rng), len(okay)))

print("\nFAILURES: %s" % (FAILS if FAILS else "none"))
stop_all()
sys.exit(1 if FAILS else 0)

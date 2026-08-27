#!/usr/bin/env python3
"""
R4: filtered retrieval, and the number Milestone 2 must be built against.

With a Part 6 content selection registered, the server filters retransmissions
to the selected PIDs. Filtering replaces unwanted PIDs with NULL packets and then
runs NPD to strip them, so a filtered RTP payload carries FEWER THAN 7 TS
packets. The box side must not assume 7 on recovered data. This measures it.
"""
import os
import sys, time, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from roundtrip import bring_up, Collector, check, FAILS, stop_all
from gen import build, send, query, payload_index

TS = 188
SEL = '{"contentSelection":[{"UDPPort":5801,"requestedPIDs":["0x0201"]}]}'

print("\n=== R4: filtered retrieval with a Part 6 content selection ===")
print("  selection: only PID 0x0201; stream carries 3 of 7 slots on 0x0201")

relay = bring_up(extra_server_args="-S", selection=SEL)
col = Collector()
time.sleep(0.5)

dgrams, truth = build(6000, [0x0201], 40, start_pcr=8_000_000, service_slots=3)
relay.arm(1200, 40)
send(dgrams)
time.sleep(16)
col.run = False
relay.run = False

slog = open("/tmp/s.log").read()
rlog = open("/tmp/r.log").read()

check("R4a the receiver registered a content selection",
      "Content selection:" in rlog)
check("R4b the server applied program-selection filtering",
      "Program selection filtering ACTIVE" in slog)

print("  collector saw %d raw datagrams, %d bytes" % (col.raw, col.rawbytes))
if col.raw:
    avg = col.rawbytes / col.raw
    print("  MEAN OUTPUT DATAGRAM: %.1f bytes = %.2f TS packets" % (avg, avg / TS))
    hist = collections.Counter()
    for n, b in col.sizes.items():
        hist[n] = b
    for size, cnt in sorted(hist.items()):
        print("     %4d bytes (%d TS packets): %d datagrams" % (size, size // TS, cnt))
    check("R4c receiver OUTPUT is a full 7 TS packets (nulls reinstated)",
          abs(avg / TS - 7.0) < 0.01, "mean %.2f packets" % (avg / TS))
else:
    check("R4b receiver produced output", False, "nothing collected")

print("\n  ON THE WIRE (server -> receiver), data datagram sizes:")
for size, cnt in sorted(relay.wire_sizes.items()):
    print("     %4d bytes: %d datagrams" % (size, cnt))
if relay.wire_sizes:
    tot = sum(k*v for k,v in relay.wire_sizes.items())
    n = sum(relay.wire_sizes.values())
    print("     mean %.1f bytes/datagram over %d datagrams" % (tot/n, n))

print("\nFAILURES: %s" % (FAILS if FAILS else "none"))
stop_all()
sys.exit(1 if FAILS else 0)

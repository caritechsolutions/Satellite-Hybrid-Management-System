#!/usr/bin/env python3
"""
C8: what a payload that filters to NOTHING does to the wire and to the
sequence space.

This matters to Milestone 2 and nothing else answers it. The mux interleaves 35
services, so a 7-TS-packet payload almost never holds two packets of the same
service -- at a realistic single-service selection ratio the MAJORITY of
payloads contain zero selected packets. Whether librist then sends a stub or
skips the payload entirely decides whether the box sees a dense sequence space
with tiny packets, or a sparse one riddled with permanent holes it must learn
not to wait for.

Run against the real capture, at the real selection ratio, with real NACKs.
"""
import sys, os, time, subprocess, collections, socket, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from roundtrip import DropRelay, Collector, start, stop_all, check, FAILS, LD, RX, SERVER
from gen import query, field

TS   = 188
CAP  = sys.argv[1]
RATE = 59145890
TSBIN = os.environ.get("P8_TSDUCK_BIN", "/home/user/tsduck/tsduck/bin/release-x86_64-vm")

# BET, service 0x0730: 1.83 Mb/s of 59.1 Mb/s = 3.10% of the multiplex. This is
# a REALISTIC single-service selection, not a contrived one.
BET = [0x0730, 0x0731, 0x0732, 0x0736, 0x0737, 0x0739]

# TR-06-4 Part 6 Section 4.3 mandatory PIDs -- these pass regardless of the
# selection and must NOT be counted as leaks. Taken from the capture itself
# (PAT -> PMT PIDs, CAT -> EMM PIDs), not from the filter's own opinion.
PMT_PIDS = {0x0027,0x006A,0x008D,0x008E,0x0672,0x067C,0x0730,0x078A,0x07D0,
            0x07DA,0x07E4,0x07EE,0x07F8,0x080C,0x0816,0x0820,0x082A,0x0834,
            0x083E,0x0848,0x085C,0x0866,0x0870,0x0884,0x088E,0x0898,0x08A2,
            0x08AC,0x08B6,0x08C0,0x08CA,0x08D4,0x08FC,0x0906}
EMM_PIDS = {0x0583, 0x0584, 0x0585, 0x0586}
def mandatory(pid):
    return pid <= 0x1F or pid in PMT_PIDS or pid in EMM_PIDS
SEL = ('{"contentSelection":[{"UDPPort":5801,"requestedPIDs":[%s]}]}'
       % ",".join('"0x%04X"' % p for p in BET))


class SeeAllRelay(DropRelay):
    """
    DropRelay tallies only datagrams >300 B, because that is how it tells data
    from RTCP. For C8 that filter would hide precisely the thing under test, so
    this records EVERY server->receiver datagram and separates RTP from RTCP by
    the second header byte: RTCP packet types are 200-206, which cannot collide
    with an RTP payload type (0-127 plus the marker bit).
    """
    def __init__(self):
        self.all_rtp = collections.Counter()
        self.all_rtcp = collections.Counter()
        self.seqs = []
        DropRelay.__init__(self)

    def _b(self):
        while self.run:
            try:
                d, _ = self.down.recvfrom(65536)
            except socket.timeout:
                continue
            if not self.client:
                continue
            if len(d) >= 2 and (d[0] & 0xC0) == 0x80:
                pt = d[1] & 0x7F
                if 72 <= pt <= 76 or d[1] >= 200:
                    self.all_rtcp[len(d)] += 1
                else:
                    self.all_rtp[len(d)] += 1
                    if len(d) >= 4:
                        self.seqs.append((d[2] << 8) | d[3])
                    # Drop by RTP-ness, NOT by size. The parent class uses
                    # >300 B to tell data from RTCP; with filtering on most data
                    # datagrams are stubs far below that, so a size rule would
                    # arm the drop and then never fire.
                    nn = self.data_seen
                    self.data_seen += 1
                    self.wire_sizes[len(d)] = self.wire_sizes.get(len(d), 0) + 1
                    if self.drop_from is not None and self.drop_from <= nn < self.drop_to:
                        self.dropped += 1
                        continue
            try:
                self.up.sendto(d, self.client)
            except OSError:
                pass


class RawCollector(Collector):
    """Collector keyed on the synthetic P8ID marker; real TS has none, so this
    keeps a bounded sample of the raw output bytes to inspect PIDs directly."""
    LIMIT = 3000

    def __init__(self):
        self.raws = []
        Collector.__init__(self)

    def _loop(self):
        while self.run:
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            self.raw += 1
            self.rawbytes += len(d)
            self.sizes[len(d)] = self.sizes.get(len(d), 0) + 1
            if len(self.raws) < self.LIMIT:
                self.raws.append(d)


print("=== C8: payloads that filter to nothing ===")
print("selection: BET service 0x0730, PIDs %s" % ["0x%04X" % p for p in BET])
print("           1,830,720 b/s of 59,145,890 b/s = 3.10%% of the multiplex\n")

# ---------------------------------------------------------------- ground truth
d = open(CAP, "rb").read(); n = len(d) // TS
pids = [((d[i*TS+1] & 0x1F) << 8) | d[i*TS+2] for i in range(n)]
sel = set(BET)
gt = collections.Counter()
for g in range(n // 7):
    gt[sum(1 for p in pids[g*7:g*7+7] if p in sel)] += 1
tot_pl = sum(gt.values())
print("GROUND TRUTH (independent of librist, from the capture bytes):")
for k in sorted(gt):
    print("   %d selected TS packets: %6d payloads (%5.2f%%)" % (k, gt[k], 100.0*gt[k]/tot_pl))
print("   -> %.2f%% of payloads contain NO selected packet\n" % (100.0*gt[0]/tot_pl))

# --------------------------------------------------------------------- bring up
stop_all()
subprocess.run("rm -f /tmp/part8_recovery.sock", shell=True)
start(f'{LD} {SERVER} -i "udp://@:5800" '
      f'-o "rist://@127.0.0.1:5799?weight=1000&buffer=4000" -b 4000 '
      f'-d /tmp/part8_recovery.sock -S', "/tmp/c8s.log")
time.sleep(1.5)
relay = SeeAllRelay()
time.sleep(0.3)
start(f"{LD} {RX} -i \"rist://127.0.0.1:5798?weight=0&buffer=4000,"
      f"rist://127.0.0.1:5899?weight=1000&buffer=4000\" "
      f"-o \"udp://127.0.0.1:5801\" --content-selection '{SEL}'", "/tmp/c8r.log")

for _ in range(40):
    time.sleep(0.5)
    if "FSR ENABLE" in open("/tmp/c8s.log").read():
        print("FSR engaged after %.1f s" % (_ * 0.5))
        break
else:
    print("FSR did NOT engage -- results below are not meaningful")

col = RawCollector()
time.sleep(0.5)

# ------------------------------------------------------------------- replay
# Drop a run of data datagrams part-way through so NACKs fire and the
# RETRANSMIT path is exercised, not just the live path.
threading.Timer(1.5, lambda: relay.arm(400, 300)).start()
print("\nreplaying %d TS packets at %d b/s ..." % (n, RATE))
t0 = time.time()
subprocess.run(
    f"LD_LIBRARY_PATH={TSBIN} {TSBIN}/tsp -I file {CAP} "
    f"-P regulate --bitrate {RATE} -O ip 127.0.0.1:5800 "
    f"--packet-burst 7 --enforce-burst",
    shell=True, capture_output=True, timeout=300)
print("replay done in %.2f s; draining 8 s for retransmissions" % (time.time()-t0))
time.sleep(8)
col.run = False; relay.run = False
time.sleep(0.5)

slog = open("/tmp/c8s.log").read()
rlog = open("/tmp/c8r.log").read()

check("C8a receiver registered the content selection", "Content selection:" in rlog)
check("C8b server applied program-selection filtering",
      "Program selection filtering ACTIVE" in slog)
print("  relay: %d data datagrams seen, %d dropped" % (relay.data_seen, relay.dropped))

# ------------------------------------------------------- what went on the wire
print("\n=== ON THE WIRE (server -> receiver), EVERY datagram by size ===")
rtp, rtcp = relay.all_rtp, relay.all_rtcp
tot_rtp = sum(rtp.values())
print("   RTP data datagrams : %d" % tot_rtp)
print("   RTCP datagrams     : %d" % sum(rtcp.values()))
for s, c in sorted(rtp.items()):
    ts_pkts = (s - 12 - 8) / TS          # 12 B RTP hdr + 8 B "RI" extension
    print("     %5d bytes: %6d datagrams (%5.2f%%)   ~%.2f TS packets"
          % (s, c, 100.0*c/tot_rtp if tot_rtp else 0, max(ts_pkts, 0)))

# ---- the question C8 actually asks
print("\n=== SEQUENCE SPACE ===")
sq = relay.seqs
if sq:
    span = 0
    prev = sq[0]
    holes = 0
    for x in sq[1:]:
        step = (x - prev) & 0xFFFF
        if step > 1:
            holes += step - 1
        span += step
        prev = x
    print("   RTP packets observed : %d" % len(sq))
    print("   sequence span        : %d" % span)
    print("   missing sequence nos : %d  (relay deliberately dropped %d)"
          % (holes, relay.dropped))
    print("   -> %s"
          % ("DENSE: librist SENDS a stub for an empty payload; the receiver "
             "sees no permanent holes."
             if holes <= relay.dropped
             else "SPARSE: %d sequence numbers never appear at all -- librist "
                  "SKIPS empty payloads and the box must not wait for them."
                  % (holes - relay.dropped)))
else:
    print("   no RTP seen")

# ---- what the box would actually decode
# Collector.got is keyed on the synthetic P8ID marker, which real transport
# stream has none of, so it is empty here. RawCollector keeps the bytes instead.
print("\n=== RECEIVER OUTPUT CONTENT (the PIDs the box actually gets) ===")
outp = collections.Counter()
for dg in col.raws:
    for i in range(len(dg) // TS):
        p = dg[i*TS:(i+1)*TS]
        if p[0] == 0x47:
            outp[((p[1] & 0x1F) << 8) | p[2]] += 1
tot_out = sum(outp.values())
print("   %d TS packets sampled from %d output datagrams" % (tot_out, len(col.raws)))
for pid, c in outp.most_common(12):
    tag = ("SELECTED" if pid in sel else
           "null" if pid == 0x1FFF else
           "mandatory (Part 6 4.3)" if mandatory(pid) else "<<< LEAKED")
    print("     0x%04X: %7d (%5.2f%%)  %s" % (pid, c, 100.0*c/tot_out, tag))
leak = sum(c for pid, c in outp.items()
           if pid not in sel and pid != 0x1FFF and not mandatory(pid))
mand = sum(c for pid, c in outp.items() if pid not in sel and mandatory(pid))
print("   mandatory PSI/PMT/EMM passed through: %d packets (%.2f%%)"
      % (mand, 100.0*mand/tot_out if tot_out else 0))
check("C8c no UNSELECTED, NON-MANDATORY PID reaches the box", leak == 0,
      "%d leaked packets" % leak)
check("C8d unselected content was replaced by nulls, not removed",
      outp.get(0x1FFF, 0) > tot_out * 0.5,
      "nulls are %.2f%% of output" % (100.0*outp.get(0x1FFF, 0)/tot_out if tot_out else 0))

print("\n=== RECEIVER OUTPUT (after expand_null_packets) ===")
print("   raw datagrams: %d, bytes: %d" % (col.raw, col.rawbytes))
for s, c in sorted(col.sizes.items()):
    print("     %5d bytes (%.2f TS packets): %6d datagrams" % (s, s/TS, c))
if col.raw:
    avg = col.rawbytes / col.raw
    print("   mean %.1f bytes = %.3f TS packets" % (avg, avg/TS))

print("\nFAILURES: %s" % (FAILS if FAILS else "none"))

#!/usr/bin/env python3
"""
C4 structure and C5 discontinuities, plus an independent read of the 0x03E8
continuity anomaly.

The CC analysis here is deliberately NOT TSDuck's -- the question is whether
TSDuck's continuity-error report means packet loss or interleaved counters, and
answering that with the same tool that raised it proves nothing. This walks the
raw packets and reconstructs each PID's counter behaviour directly.
"""
import sys, os, collections, subprocess

TS = 188
CAP = sys.argv[1]
TSBIN = os.environ.get("P8_TSDUCK_BIN", "/home/user/tsduck/tsduck/bin/release-x86_64-vm")
ENV = dict(os.environ, LD_LIBRARY_PATH=TSBIN)

data = open(CAP, "rb").read()
n = len(data) // TS
print("packets: %d" % n)

# ---- walk every packet once
cc_last = {}
cc_errs = collections.defaultdict(list)      # pid -> [(idx, expected, got, has_payload)]
disc_flag = collections.defaultdict(list)    # pid -> [idx]
pcr_pids = set()
pid_count = collections.Counter()
dup_count = collections.Counter()

for i in range(n):
    p = data[i*TS:(i+1)*TS]
    if p[0] != 0x47:
        continue
    pid = ((p[1] & 0x1F) << 8) | p[2]
    pid_count[pid] += 1
    afc = (p[3] >> 4) & 0x03
    cc = p[3] & 0x0F
    has_payload = bool(afc & 0x01)

    # adaptation field: discontinuity_indicator and PCR presence
    if afc & 0x02 and p[4] > 0:
        if p[5] & 0x80:
            disc_flag[pid].append(i)
        if p[5] & 0x10 and p[4] >= 7:
            pcr_pids.add(pid)

    if pid == 0x1FFF:
        continue
    if pid in cc_last:
        prev = cc_last[pid]
        # per H.222.0: CC increments only when a payload is present
        exp = (prev + 1) & 0x0F if has_payload else prev
        if cc != exp:
            if cc == prev and has_payload:
                dup_count[pid] += 1          # legal single duplicate
            else:
                cc_errs[pid].append((i, exp, cc, has_payload))
    cc_last[pid] = cc

print("\n=== C4 STRUCTURE ===")
print("distinct PIDs carrying packets : %d" % len(pid_count))
print("PIDs with PCRs (independent)   : %d" % len(pcr_pids))

# service -> PMT -> PCR PID, via TSDuck tables (structure only, not PCR values)
out = subprocess.run([f"{TSBIN}/tstables", CAP, "--pid", "0", "--pid", "16",
                      "--max-tables", "40"], env=ENV, capture_output=True,
                     text=True, timeout=600)
svc = subprocess.run([f"{TSBIN}/tsanalyze", CAP, "--service-analysis"],
                     env=ENV, capture_output=True, text=True, timeout=900)

print("\n=== C5 DISCONTINUITIES ===")
tot_disc = sum(len(v) for v in disc_flag.values())
print("packets with discontinuity_indicator SET: %d" % tot_disc)
for pid in sorted(disc_flag):
    idxs = disc_flag[pid]
    print("   PID 0x%04X: %d  (first at packet %d, t=%.3f s)"
          % (pid, len(idxs), idxs[0], idxs[0] * TS * 8 / 59_145_890))
if tot_disc == 0:
    print("   NONE. The corrected epoch logic is therefore UNEXERCISED against")
    print("   real data in this capture -- proven only against synthetic (A5/R3).")

print("\n=== CONTINUITY COUNTER ANOMALY ===")
print("PIDs with CC errors: %d" % len(cc_errs))
for pid in sorted(cc_errs, key=lambda p: -len(cc_errs[p])):
    errs = cc_errs[pid]
    nopay = sum(1 for e in errs if not e[3])
    print("   PID 0x%04X: %d errors (%d of them with NO payload)"
          % (pid, len(errs), nopay))

# The specific claim: two interleaved counters on one PID.
target = 0x03E8
if target in cc_errs:
    print("\n--- PID 0x%04X examined directly ---" % target)
    idxs = [i for i in range(n)
            if data[i*TS] == 0x47 and (((data[i*TS+1] & 0x1F) << 8) | data[i*TS+2]) == target]
    seq = [(i, data[i*TS+3] & 0x0F, (data[i*TS+3] >> 4) & 0x03) for i in idxs]
    print("   packets on this PID: %d" % len(seq))
    print("   first 24 (idx, cc, afc):")
    print("     " + " ".join("%d:%X/%d" % (a, b, c) for a, b, c in seq[:24]))

    # Test the interleave hypothesis: split alternate packets into two streams
    # and see whether each is individually continuous.
    for stride in (2, 3):
        ok = True
        for off in range(stride):
            sub = [c for (_, c, _) in seq[off::stride]]
            bad = sum(1 for a, b in zip(sub, sub[1:]) if b != ((a + 1) & 0x0F))
            if bad > len(sub) * 0.02:
                ok = False
        print("   de-interleaving by %d: %s" % (stride,
              "EVERY substream continuous -> two interleaved counters CONFIRMED"
              if ok else "substreams still discontinuous -> NOT a clean interleave"))

    # Missing-count pairing: do gap sizes pair to 15?
    gaps = []
    for a, b in zip(seq, seq[1:]):
        exp = (a[1] + 1) & 0x0F if (a[2] & 0x01) else a[1]
        if b[1] != exp:
            gaps.append((b[1] - exp) & 0x0F)
    pair_sums = [x + y for x, y in zip(gaps, gaps[1:])]
    print("   gap sizes (first 20): %s" % gaps[:20])
    if pair_sums:
        n15 = sum(1 for s in pair_sums if s == 15 or s == 16)
        print("   consecutive gaps summing to 15/16: %d of %d pairs (%.0f%%)"
              % (n15, len(pair_sums), 100.0 * n15 / len(pair_sums)))

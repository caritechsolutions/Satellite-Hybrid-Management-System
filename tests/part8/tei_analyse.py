#!/usr/bin/env python3
"""
Decide whether the GX6631 demux passes TEI-marked packets to a TS-output slot.

Reads a capture of the STB's demux output and reports, per PID, how many packets
carry transport_error_indicator and how many continuity discontinuities occurred.

WHY BOTH NUMBERS, AND WHY THAT IS THE WHOLE TEST
------------------------------------------------
The two candidate behaviours produce MUTUALLY EXCLUSIVE evidence from the same
damage:

  demux PASSES TEI    -> the damaged packet still arrives, flagged.
                         TEI count rises. CC stays continuous (nothing missing).
  demux DISCARDS it   -> the packet never arrives.
                         TEI count stays zero. CC shows a discontinuity.

So "TEI rose" and "CC broke" are not two ways of measuring the same thing -- each
is the signature of one answer. That is also why either one appearing is proof
the impairment actually damaged the stream, which is the positive control the
result depends on. If NEITHER moves, the water did nothing and the run says
nothing; fall back to the recorded SNR/BER to tell which.

THE ASYMMETRY IS THE CONTROL
----------------------------
In app_ts_record.c the audio slot is allocated with DMX_ERR_DISCARD_EN and the
video slot without it. Same service, same impairment, same instant. If video
shows TEI while audio shows CC breaks instead, both questions are answered at
once: the demux does pass TEI, and the flag is what suppresses it.

BASELINE IS NOT OPTIONAL IN PRACTICE
------------------------------------
CC discontinuities are NOT zero on a healthy stream. On this transponder PID
0x03E8 carries two interleaved continuity counters and produces 164 CC errors in
four clean seconds with nothing wrong. Judging "damage occurred" against zero
therefore reports a perfect capture as damaged -- which this script did, before
--baseline existed. The verdict is computed against a clean capture taken the
same day on the same service, and PIDs whose CC is untrustworthy are named so
their contribution is discounted rather than believed.

  usage: tei_analyse.py <capture.ts> [--baseline clean.ts]
                        [--video PID] [--audio PID]
         PIDs may be decimal or 0x hex; if omitted they are inferred from the PMT.
"""
import sys, os, subprocess, collections

TS = 188
TSBIN = os.environ.get("P8_TSDUCK_BIN",
                       "/home/user/tsduck/tsduck/bin/release-x86_64-vm")


def parse_pid(s):
    return int(s, 16) if s.lower().startswith("0x") else int(s)


def walk(path):
    """One pass: TEI count, CC discontinuities and packet count per PID.

    Deliberately not TSDuck: the question is what the bytes say, and answering it
    with the same tool that will later be used to cross-check would leave a shared
    failure mode. TSDuck is used afterwards as an independent second opinion.
    """
    tei = collections.Counter()
    disc = collections.Counter()
    total = collections.Counter()
    last_cc = {}
    bad_sync = 0

    with open(path, "rb") as f:
        while True:
            p = f.read(TS)
            if len(p) < TS:
                break
            if p[0] != 0x47:
                bad_sync += 1
                continue
            pid = ((p[1] & 0x1F) << 8) | p[2]
            total[pid] += 1
            if p[1] & 0x80:
                tei[pid] += 1
                # A TEI packet's CC is untrustworthy by definition, so it is not
                # fed into the continuity check below.
                continue
            if pid == 0x1FFF:
                continue
            cc = p[3] & 0x0F
            has_payload = bool(p[3] & 0x10)
            if pid in last_cc:
                exp = (last_cc[pid] + 1) & 0x0F if has_payload else last_cc[pid]
                if cc != exp and not (cc == last_cc[pid] and has_payload):
                    disc[pid] += 1
            last_cc[pid] = cc
    return tei, disc, total, bad_sync


def pmt_pids(path):
    """video/audio PIDs from the PMT, so the operator need not supply them."""
    try:
        r = subprocess.run([f"{TSBIN}/tstables", path, "--max-tables", "40"],
                           env=dict(os.environ, LD_LIBRARY_PATH=TSBIN),
                           capture_output=True, text=True, timeout=300)
    except Exception:
        return None, None
    v = a = None
    for line in r.stdout.splitlines():
        if "Elementary stream" not in line:
            continue
        low = line.lower()
        pid = None
        if "pid:" in low:
            try:
                pid = int(line.split("PID:")[1].split("(")[0].strip(), 16)
            except Exception:
                pid = None
        if pid is None:
            continue
        if v is None and "video" in low:
            v = pid
        elif a is None and "audio" in low:
            a = pid
    return v, a


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    path = sys.argv[1]
    vpid = apid = None
    for i, a in enumerate(sys.argv):
        if a == "--video" and i + 1 < len(sys.argv): vpid = parse_pid(sys.argv[i + 1])
        if a == "--audio" and i + 1 < len(sys.argv): apid = parse_pid(sys.argv[i + 1])

    base = None
    for i, a in enumerate(sys.argv):
        if a == "--baseline" and i + 1 < len(sys.argv):
            base = sys.argv[i + 1]

    size = os.path.getsize(path)
    print("capture : %s" % path)
    print("          %.1f MB, %d TS packets%s\n"
          % (size / 1e6, size // TS,
             "" if size % TS == 0 else "  <<< NOT a multiple of 188"))

    tei, disc, total, bad_sync = walk(path)

    btei = bdisc = btotal = None
    if base:
        btei, bdisc, btotal, _ = walk(base)
        # CC error RATE per 1000 packets, so a baseline of a different length
        # still calibrates correctly.
        print("baseline: %s" % base)
        noisy = [p for p in bdisc
                 if bdisc[p] > 0 and btotal.get(p, 0) > 0]
        if noisy:
            print("  PIDs with CC errors when HEALTHY (their CC cannot be trusted):")
            for p in sorted(noisy, key=lambda x: -bdisc[x]):
                print("    0x%04X  %d CC breaks in %d packets (%.2f per 1000)"
                      % (p, bdisc[p], btotal[p], 1000.0 * bdisc[p] / btotal[p]))
        if sum(btei.values()):
            print("  WARNING: baseline already contains %d TEI packets -- it is not "
                  "clean" % sum(btei.values()))
        print()

    if vpid is None or apid is None:
        v2, a2 = pmt_pids(path)
        vpid = vpid if vpid is not None else v2
        apid = apid if apid is not None else a2
    print("video PID: %s    audio PID: %s%s\n"
          % (("0x%04X" % vpid) if vpid is not None else "unknown",
             ("0x%04X" % apid) if apid is not None else "unknown",
             "   (pass --video/--audio to override)"))

    tot_tei = sum(tei.values())
    tot_disc = sum(disc.values())
    print("=== PER-PID ===")
    print("  %-8s %10s %10s %12s" % ("PID", "packets", "TEI", "CC breaks"))
    for pid in sorted(total):
        if pid == 0x1FFF:
            continue
        mark = ""
        if pid == vpid: mark = "  <- video, NO discard flag"
        if pid == apid: mark = "  <- audio, HAS discard flag"
        print("  0x%04X   %10d %10d %12d%s"
              % (pid, total[pid], tei.get(pid, 0), disc.get(pid, 0), mark))
    if bad_sync:
        print("  %d packets with a bad sync byte (framing damage)" % bad_sync)

    print("\n=== TOTALS ===")
    print("  TEI packets        : %d" % tot_tei)
    print("  CC discontinuities : %d" % tot_disc)

    # Excess CC over the baseline RATE, per PID. A PID with no baseline entry is
    # compared against zero; a PID known noisy is discounted by its own rate.
    excess = 0
    if btotal is not None:
        for pid in total:
            if pid == 0x1FFF or total[pid] == 0:
                continue
            brate = (bdisc.get(pid, 0) / btotal[pid]) if btotal.get(pid) else 0.0
            expected = brate * total[pid]
            excess += max(0.0, disc.get(pid, 0) - expected)
        print("  CC breaks above baseline rate: %.0f  (raw %d)" % (excess, tot_disc))
        print("  TEI above baseline           : %d  (raw %d)"
              % (tot_tei - sum(btei.values()), tot_tei))
    else:
        excess = tot_disc
        print("  NO BASELINE GIVEN -- CC is judged against zero, which this")
        print("  transponder does not satisfy even when healthy. Pass --baseline.")

    print("\n=== VERDICT ===")
    if tot_tei == 0 and excess < 1:
        print("  NO DAMAGE DETECTED. This run establishes nothing about TEI.")
        print("  The positive control did not fire: neither signature appeared, so")
        print("  the impairment either did not reach the demodulator or was not")
        print("  severe enough. Check the recorded SNR/BER. Repeat with more water")
        print("  or a longer soak before drawing any conclusion.")
        return

    print("  Damage confirmed: at least one signature fired, so the impairment")
    print("  did reach the stream. The run is valid.\n")

    vt = tei.get(vpid, 0) if vpid is not None else 0
    at = tei.get(apid, 0) if apid is not None else 0
    vd = disc.get(vpid, 0) if vpid is not None else 0
    ad = disc.get(apid, 0) if apid is not None else 0

    if vt > 0 and at == 0:
        print("  *** BEST CASE ***")
        print("  Video (no discard flag) shows %d TEI; audio (flag set) shows none" % vt)
        print("  but %d CC breaks. The demux PASSES TEI when the flag is clear," % ad)
        print("  and DMX_ERR_DISCARD_EN is what suppresses it.")
        print("  -> TEI is available AND selectable per slot. Detector branch (a).")
    elif vt > 0 and at > 0:
        print("  TEI present on BOTH slots (%d video, %d audio)." % (vt, at))
        print("  The demux passes TEI regardless of the flag, which therefore")
        print("  governs something else (CRC or descrambling).")
        print("  -> TEI is available. Detector branch (a), without per-slot control.")
    elif tot_tei == 0 and excess >= 1:
        print("  NO TEI anywhere, but %d CC discontinuities." % tot_disc)
        print("  Damage occurred and arrived as MISSING packets, not flagged ones.")
        print("  The demux appears to drop errored packets at input regardless of")
        print("  the flag -- note video had no discard flag and still shows no TEI.")
        print("  -> Detector branch (b). Before accepting this, clear")
        print("     DMX_ERR_DISCARD_EN from the audio slot (app_ts_record.c:1679),")
        print("     rebuild and repeat: if TEI then appears, the demux does pass it.")
    elif vt == 0 and at > 0:
        print("  UNEXPECTED: TEI on audio (flag set) but not video (flag clear).")
        print("  This contradicts the flag's name. Do not design against this")
        print("  result -- re-examine the slot configuration first.")
    else:
        print("  Mixed result. video TEI=%d CC=%d, audio TEI=%d CC=%d." % (vt, vd, at, ad))
        print("  Report the per-PID table rather than drawing a conclusion here.")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
C1-C3 oracle: TSDuck as an INDEPENDENT PCR extractor.

The point of this file is that nothing in it calls the catalogue's own
extraction path. PCRs come out of TSDuck's pcrextract plugin; the catalogue's
view comes back over the debug socket. A shared bug cannot hide in the overlap
because there is no shared code.

Usage:  oracle.py <capture.ts>            # C1/C2/C3 oracle only, no server
"""
import sys, os, subprocess, statistics, collections, re

TSBIN = os.environ.get("P8_TSDUCK_BIN", "/home/user/tsduck/tsduck/bin/release-x86_64-vm")
ENV = dict(os.environ, LD_LIBRARY_PATH=TSBIN)
TS = 188
PCR_HZ = 90000
SYS_CLOCK = 27_000_000          # PCR full resolution: base*300 + ext


def tsduck_pcrs(path):
    """
    Every PCR in the file, via TSDuck. Returns [(pid, pcr_27mhz, ts_packet_index)].

    pcrextract --csv emits, per the header it prints:
      PID, Packet index in TS, Packet index in PID, Type, Count in PID,
      Value, Value offset in PID, Offset from PCR
    PID is DECIMAL and Type may be PCR, PTS or DTS -- filter to PCR only, or
    presentation timestamps get counted as clock references.
    """
    out = subprocess.run(
        [f"{TSBIN}/tsp", "-I", "file", path,
         "-P", "pcrextract", "--csv", "--noheader",
         "-O", "drop"],
        env=ENV, capture_output=True, text=True, timeout=1800)
    rows = []
    for line in (out.stdout + out.stderr).splitlines():
        f = line.split(",")
        if len(f) < 6 or f[3].strip() != "PCR":
            continue
        try:
            rows.append((int(f[0]), int(f[5]), int(f[1])))
        except ValueError:
            continue
    return rows, out.stdout, out.stderr


def tsduck_analyze(path):
    out = subprocess.run(
        [f"{TSBIN}/tsanalyze", path],
        env=ENV, capture_output=True, text=True, timeout=1800)
    return out.stdout


def rate_from_pcr(rows):
    """
    C2: transport rate derived from PCR deltas against byte offsets between them.

    Independent of socket throughput and more accurate than it, and because it
    uses both the PCR values and the packet indices it cross-checks the oracle's
    own extraction at the same time -- a mis-parsed PCR shows up as an absurd
    rate rather than passing quietly.
    """
    per_pid = collections.defaultdict(list)
    for pid, pcr, pkt in rows:
        if pkt is not None:
            per_pid[pid].append((pkt, pcr))
    rates = []
    for pid, seq in per_pid.items():
        seq.sort()
        for (p0, c0), (p1, c1) in zip(seq, seq[1:]):
            dp, dc = p1 - p0, c1 - c0
            if dp <= 0 or dc <= 0:
                continue                      # wrap or discontinuity; skip
            secs = dc / SYS_CLOCK
            if secs <= 0:
                continue
            rates.append(dp * TS * 8 / secs)
    if not rates:
        return None, 0
    rates.sort()
    return statistics.median(rates), len(rates)


def intervals(rows):
    """C3: PCR interval distribution per PID, in milliseconds."""
    per_pid = collections.defaultdict(list)
    for pid, pcr, _ in rows:
        per_pid[pid].append(pcr)
    stats = {}
    for pid, seq in per_pid.items():
        d = [(b - a) / SYS_CLOCK * 1000.0
             for a, b in zip(seq, seq[1:]) if b > a]
        if not d:
            continue
        stats[pid] = dict(
            n=len(d), min=min(d), max=max(d), mean=statistics.fmean(d),
            sd=statistics.pstdev(d) if len(d) > 1 else 0.0)
    return stats


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    path = sys.argv[1]
    size = os.path.getsize(path)
    print("capture: %s  %.1f MB  %d TS packets  (%s)" % (
        path, size / 1e6, size // TS,
        "whole multiple of 188" if size % TS == 0
        else "NOT a multiple of 188 -- check for a 4-byte timestamp prefix"))

    rows, so, se = tsduck_pcrs(path)
    print("TSDuck pcrextract: %d PCRs across %d PIDs"
          % (len(rows), len({r[0] for r in rows})))
    if not rows:
        print("NO PCRs PARSED -- dumping first output lines so the parse can be fixed:")
        for l in (so + se).splitlines()[:15]:
            print("   " + l)
        sys.exit(1)

    rate, n = rate_from_pcr(rows)
    if rate:
        print("\nC2 rate from PCR deltas vs byte offsets: %.3f Mb/s (median of %d intervals)"
              % (rate / 1e6, n))
        rtp = rate / (TS * 8) / 7
        print("    -> %.0f TS pkt/s, %.0f RTP payloads/s" % (rate / (TS * 8), rtp))
        for buf in (10, 12, 15, 17, 20):
            resident = rtp * buf
            print("    buffer %2d s -> %7.0f payloads, %5.1f%% of 65536%s"
                  % (buf, resident, resident * 100 / 65536,
                     "   <-- OVER CEILING" if resident >= 65536 else
                     "   <-- tripwire A" if resident >= 65536 * 0.75 else ""))
        print("    tripwire A (75%%) fires at %.1f s; ceiling reached at %.1f s"
              % (65536 * 0.75 / rtp, 65536 / rtp))

    print("\nC3 PCR interval distribution per PID (ms):")
    st = intervals(rows)
    print("    %-8s %7s %8s %8s %8s %8s" % ("PID", "n", "min", "max", "mean", "sd"))
    for pid in sorted(st):
        s = st[pid]
        print("    0x%04X   %7d %8.2f %8.2f %8.2f %8.2f"
              % (pid, s["n"], s["min"], s["max"], s["mean"], s["sd"]))
    print("    %d PCR-bearing PIDs" % len(st))
    worst = max((s["max"], p) for p, s in st.items())
    print("    worst-case gap back to a valid reference PCR: %.2f ms (PID 0x%04X)"
          % (worst[0], worst[1]))

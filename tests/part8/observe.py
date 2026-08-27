#!/usr/bin/env python3
"""
Long-run observer for the deployed Part 8 recovery server (O1-O5).

Runs for hours against the live feed and records what a four-second paced replay
in a container cannot show. Writes a CSV sample line per interval and a running
human-readable log, and -- the part that matters -- reacts to a PCR discontinuity
the moment one appears rather than leaving it to be noticed later.

  usage: part8-observe [--socket PATH] [--interval SEC] [--out DIR] [--unit NAME]

Safe to leave running. It only reads: the debug socket, /proc, and journald.
"""
import argparse, json, os, re, socket, subprocess, sys, time

AP = argparse.ArgumentParser()
AP.add_argument("--socket", default="/run/part8-recovery/debug.sock")
AP.add_argument("--interval", type=float, default=30.0)
AP.add_argument("--out", default="/var/log/part8-observe")
AP.add_argument("--unit", default="part8-recovery")
A = AP.parse_args()

PCR_HZ = 90000
MEASURED_BPS = 59145890          # C2, agreed three ways
PPS = MEASURED_BPS / (188 * 8) / 7


def query(cmd, timeout=5.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(A.socket)
        s.sendall(cmd.encode())
        buf = b""
        while True:
            try:
                d = s.recv(65536)
            except socket.timeout:
                break
            if not d:
                break
            buf += d
        return buf.decode(errors="replace")
    finally:
        s.close()


def kv(text):
    """Parse the space-separated key=value header line."""
    out = {}
    for f in text.splitlines()[0].split() if text.strip() else []:
        if "=" in f:
            k, v = f.split("=", 1)
            out[k] = v
    return out


def main_pid():
    try:
        return int(subprocess.run(["systemctl", "show", "-p", "MainPID", "--value", A.unit],
                                  capture_output=True, text=True, timeout=10).stdout.strip())
    except Exception:
        return 0


def proc_stats(pid):
    """O5: the things a container could not show -- threads, fds, memory."""
    d = {"rss_kb": 0, "threads": 0, "fds": 0, "vm_kb": 0}
    if not pid:
        return d
    try:
        for line in open(f"/proc/{pid}/status"):
            if line.startswith("VmRSS:"):
                d["rss_kb"] = int(line.split()[1])
            elif line.startswith("VmSize:"):
                d["vm_kb"] = int(line.split()[1])
            elif line.startswith("Threads:"):
                d["threads"] = int(line.split()[1])
        d["fds"] = len(os.listdir(f"/proc/{pid}/fd"))
    except Exception:
        pass
    return d


def journal_bytes():
    """O5: log volume, so a chatty build is noticed before it fills a disk."""
    try:
        r = subprocess.run(["journalctl", "-u", A.unit, "--output=export", "--no-pager"],
                           capture_output=True, timeout=60)
        return len(r.stdout)
    except Exception:
        return -1


def parse_events(text):
    evs = []
    for line in text.splitlines():
        if not line.startswith("event "):
            continue
        e = {}
        for f in line.split()[1:]:
            if "=" in f:
                k, v = f.split("=", 1)
                e[k] = v
        evs.append(e)
    return evs


def probe_around(pid_hex, pcr_before, pcr_after):
    """
    O3: resolve either side of a discontinuity.

    This is the whole reason for catching the event live. The epoch logic has
    only ever been proven synthetically; these four resolves are what turn a real
    discontinuity into evidence about it. Recorded verbatim, pass or fail -- an
    unexpected answer here is the finding, not an error to be smoothed over.
    """
    pid = int(pid_hex, 16)
    out = {}
    probes = [
        ("before_exact", pcr_before),
        ("before_minus_1s", (pcr_before - PCR_HZ) & ((1 << 33) - 1)),
        ("after_exact", pcr_after),
        ("after_plus_1s", (pcr_after + PCR_HZ) & ((1 << 33) - 1)),
    ]
    for label, base in probes:
        if base is None:
            continue
        try:
            r = query("resolve %d %d %d\n" % (pid, base, PCR_HZ // 2))
            head = r.strip().splitlines()[0] if r.strip() else "(empty)"
            rec = {"status": head.split()[0], "head": head}
            m = re.search(r"actual_pcr\s*=\s*(\d+)", r)
            if m:
                rec["actual_pcr"] = int(m.group(1))
                rec["error_ticks"] = rec["actual_pcr"] - base
            m = re.search(r"epoch\s*=\s*(\d+)", r)
            if m:
                rec["epoch"] = int(m.group(1))
            m = re.search(r"start_ext\s*=\s*(\d+)", r)
            if m:
                rec["start_ext"] = int(m.group(1))
            out[label] = rec
        except Exception as ex:
            out[label] = {"error": str(ex)}
    return out


os.makedirs(A.out, exist_ok=True)
csv_path = os.path.join(A.out, "samples.csv")
evt_path = os.path.join(A.out, "discontinuities.jsonl")
log_path = os.path.join(A.out, "observe.log")

new_csv = not os.path.exists(csv_path)
csv = open(csv_path, "a", buffering=1)
evt = open(evt_path, "a", buffering=1)
log = open(log_path, "a", buffering=1)

if new_csv:
    csv.write("wall,uptime_s,ts_in,payloads,ext_seq,sync_err,bytes_rate_mbps,"
              "queue_now,queue_max,queue_ms,queue_mb,pct_ceiling,outside_buffer,"
              "tripwires,pids,pid_entries,rss_kb,vm_kb,threads,fds,journal_bytes,"
              "disc_total,disc_nopcr\n")


def say(msg):
    line = "%s  %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg)
    print(line, flush=True)
    log.write(line + "\n")


say("observer starting: socket=%s interval=%.0fs out=%s" % (A.socket, A.interval, A.out))

t0 = time.time()
seen_events = 0
prev = None
prev_pids = set()
journal_every = max(1, int(600 / A.interval))     # journal size is expensive; every ~10 min
tick = 0
last_journal = -1

while True:
    tick += 1
    now = time.time()
    try:
        st_text = query("stats\n")
    except Exception as ex:
        say("STATS QUERY FAILED: %s (server down or socket gone)" % ex)
        time.sleep(A.interval)
        continue

    st = kv(st_text)
    pid_lines = [l for l in st_text.splitlines() if l.startswith("pid 0x")]
    pids = set()
    entries = 0
    for l in pid_lines:
        f = l.split()
        pids.add(f[1])
        try:
            entries += int(f[3].split("=")[1])
        except Exception:
            pass

    p = main_pid()
    ps = proc_stats(p)

    if tick % journal_every == 1:
        last_journal = journal_bytes()

    def gi(k):
        try:
            return int(st.get(k, 0))
        except Exception:
            return 0

    ts_in = gi("ts_in")
    payloads = gi("payloads")
    qnow = gi("queue_now")
    qmax = gi("queue_max")
    qms = gi("queue_ms")
    qbytes = gi("queue_bytes")
    ceiling = gi("ceiling") or 65536

    # O1: measured ingest rate over the sample interval, from the server's own
    # TS counter rather than from anything this script guesses.
    rate = 0.0
    if prev and now > prev["t"]:
        rate = (ts_in - prev["ts_in"]) * 188 * 8 / (now - prev["t"]) / 1e6

    csv.write("%d,%.0f,%d,%d,%d,%d,%.3f,%d,%d,%d,%.2f,%.2f,%d,%s,%d,%d,%d,%d,%d,%d,%d,%s,%s\n"
              % (now, now - t0, ts_in, payloads, gi("ext_seq"),
                 gi("sync_err"), rate, qnow, qmax, qms, qbytes / 1048576.0,
                 100.0 * qnow / ceiling, gi("outside_buffer"),
                 st.get("tripwires", "--"), len(pids), entries,
                 ps["rss_kb"], ps["vm_kb"], ps["threads"], ps["fds"], last_journal,
                 st.get("disc_total", "?"), st.get("disc_nopcr", "?")))

    # O4: a PID appearing or disappearing is a catalogue-health event, not noise.
    if prev_pids and pids != prev_pids:
        gone = sorted(prev_pids - pids)
        came = sorted(pids - prev_pids)
        if gone:
            say("O4 PID(s) GONE from catalogue: %s (now %d PIDs)" % (", ".join(gone), len(pids)))
        if came:
            say("O4 PID(s) NEW in catalogue: %s (now %d PIDs)" % (", ".join(came), len(pids)))
    prev_pids = pids

    if st.get("tripwires", "--") != "--":
        say("TRIPWIRE FIRED: %s  queue_now=%d (%.1f%% of ceiling) queue_ms=%d"
            % (st["tripwires"], qnow, 100.0 * qnow / ceiling, qms))

    if gi("sync_err") and (not prev or gi("sync_err") != prev["sync_err"]):
        say("O1 sync errors now %d (was %s)" % (gi("sync_err"),
                                                prev["sync_err"] if prev else "-"))

    # ---- O3: react to a discontinuity the moment it shows up
    try:
        ev_text = query("events\n")
    except Exception:
        ev_text = ""
    ev_hdr = kv(ev_text)
    try:
        total = int(ev_hdr.get("disc_total", 0))
    except Exception:
        total = 0

    if total > seen_events:
        evs = parse_events(ev_text)
        fresh = evs[-(total - seen_events):] if total - seen_events <= len(evs) else evs
        say("O3 *** %d NEW DISCONTINUITY EVENT(S) *** (total %d)" % (total - seen_events, total))
        for e in fresh:
            say("O3   pid=%s kind=%s epoch=%s delta=%s ticks (%.1f ms) ext_seq=%s"
                % (e.get("pid"), e.get("kind"), e.get("epoch"), e.get("delta"),
                   int(e.get("delta", 0)) * 1000.0 / PCR_HZ, e.get("ext_seq")))
            rec = {"observed_at": time.time(), "event": e}
            if e.get("kind") != "indicator-no-pcr":
                try:
                    rec["probes"] = probe_around(e["pid"],
                                                 int(e.get("pcr_before", 0)),
                                                 int(e.get("pcr_after", 0)))
                    for lbl, pr in rec["probes"].items():
                        say("O3     %-16s -> %s epoch=%s err=%s ext=%s"
                            % (lbl, pr.get("status"), pr.get("epoch"),
                               pr.get("error_ticks"), pr.get("start_ext")))
                except Exception as ex:
                    rec["probe_error"] = str(ex)
                    say("O3     probe failed: %s" % ex)
            evt.write(json.dumps(rec) + "\n")
        seen_events = total

    # periodic human-readable summary
    if tick % max(1, int(1800 / A.interval)) == 1:
        say("O1/O2 up %.1f h | in %.2f Mb/s | ts=%d payloads=%d sync_err=%d | "
            "queue %d payloads %d ms %.1f MB (%.1f%% of %d) | rss %d kB threads %d fds %d | "
            "%d PIDs %d entries | disc %s"
            % ((now - t0) / 3600.0, rate, ts_in, payloads, gi("sync_err"),
               qnow, qms, qbytes / 1048576.0, 100.0 * qnow / ceiling, ceiling,
               ps["rss_kb"], ps["threads"], ps["fds"], len(pids), entries,
               st.get("disc_total", "?")))

    prev = {"t": now, "ts_in": ts_in, "sync_err": gi("sync_err")}
    time.sleep(A.interval)

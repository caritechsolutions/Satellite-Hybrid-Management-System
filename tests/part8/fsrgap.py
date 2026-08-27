#!/usr/bin/env python3
"""
Measure the FSR switchover gap instead of assuming it.

The question is narrow: when FSR engages, does the server resume from the
receiver's last good sequence (backfilling out of the 4 s buffer), or from
wherever the live edge happens to be? Part 7's FSR Enable message carries no
resume point -- proto/rtp.c writes only flags/ptype/len/ssrc/"RIST" -- so the
prediction is "live edge, no backfill", and the consequence is that the gap is
set by the OUTAGE, not bounded by the buffer.

Method: get the stream running first, attach the receiver late. Whatever the
receiver fails to collect between stream start and its first delivered payload
IS the gap. Nothing here assumes a number; the ext_seq counter at the moment
FSR engages is read from the server itself.
"""
import sys, os, time, subprocess, socket, threading, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from roundtrip import start, stop_all, Collector, LD, RX, SERVER
from gen import query, field

TS = 188
CAP = sys.argv[1]
RATE = 59145890
LATE = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0
TSBIN = os.environ.get("P8_TSDUCK_BIN", "/home/user/tsduck/tsduck/bin/release-x86_64-vm")

PPS = RATE / (TS * 8) / 7          # payloads per second, from C2's measured rate

stop_all()
subprocess.run("rm -f /tmp/part8_recovery.sock", shell=True)
start(f'{LD} {SERVER} -i "udp://@:5800" '
      f'-o "rist://@127.0.0.1:5799?weight=1000&buffer=4000" -b 4000 '
      f'-d /tmp/part8_recovery.sock -S', "/tmp/fg_s.log")
time.sleep(1.5)

# Feed continuously so there IS a live edge to fall behind.
rep = subprocess.Popen(
    f"LD_LIBRARY_PATH={TSBIN} {TSBIN}/tsp -I file {CAP} --infinite "
    f"-P regulate --bitrate {RATE} -O ip 127.0.0.1:5800 "
    f"--packet-burst 7 --enforce-burst",
    shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    preexec_fn=os.setsid)

t_stream = time.time()
print("stream running; attaching the receiver %.1f s late\n" % LATE)
time.sleep(LATE)

seq_before = int(field(query("stats\n"), "ext_seq") or 0) if False else None
st = query("stats\n").split()
seq_at_attach = int([x for x in st if x.startswith("ext_seq=")][0].split("=")[1])
t_attach = time.time()
print("at receiver attach: server had already written ext_seq = %d  (%.2f s of stream)"
      % (seq_at_attach, seq_at_attach / PPS))

col = Collector()
start(f'{LD} {RX} -i "rist://127.0.0.1:5798?weight=0&buffer=4000,'
      f'rist://127.0.0.1:5799?weight=1000&buffer=4000" '
      f'-o "udp://127.0.0.1:5801"', "/tmp/fg_r.log")

# Wait for the server to log FSR engage, and read ext_seq at that instant.
seq_at_fsr = None
t_fsr = None
for _ in range(80):
    time.sleep(0.25)
    if "FSR ENABLE" in open("/tmp/fg_s.log").read():
        st = query("stats\n").split()
        seq_at_fsr = int([x for x in st if x.startswith("ext_seq=")][0].split("=")[1])
        t_fsr = time.time()
        break
print("at FSR engage      : server ext_seq = %s  (%.2f s after attach)"
      % (seq_at_fsr, (t_fsr - t_attach) if t_fsr else -1))

time.sleep(6)
st = query("stats\n").split()
seq_end = int([x for x in st if x.startswith("ext_seq=")][0].split("=")[1])
col.run = False
try:
    os.killpg(os.getpgid(rep.pid), 15)
except Exception:
    pass
time.sleep(0.5)

rlog = open("/tmp/fg_r.log").read()

print("\n=== RESULT ===")
print("  server wrote            : %d payloads total" % seq_end)
print("  receiver collected      : %d output datagrams" % col.raw)
if seq_at_fsr is not None:
    live_after_fsr = seq_end - seq_at_fsr
    print("  written after FSR engage: %d payloads" % live_after_fsr)
    backfill = col.raw - live_after_fsr
    print("  receiver got %d, live-since-FSR was %d -> %+d"
          % (col.raw, live_after_fsr, backfill))
    print()
    if backfill > PPS * 0.5:
        print("  BACKFILL: the receiver got ~%.2f s MORE than the live edge alone,"
              % (backfill / PPS))
        print("  so FSR does replay out of the buffer and the gap is bounded by it.")
    else:
        print("  NO BACKFILL: the receiver got essentially only what was produced")
        print("  AFTER FSR engaged. FSR resumes at the LIVE EDGE.")
        print("  -> the switchover gap is set by the OUTAGE + detection latency,")
        print("     NOT bounded by the %d ms buffer. The %d payloads (%.2f s)"
              % (4000, seq_at_fsr, seq_at_fsr / PPS))
        print("     already resident were never sent.")

# librist's own accounting of the hole
import re
m = re.findall(r'"flow_cumulative_stats":\{"flow_id":\d+,"received":(\d+),'
               r'"recovered":(\d+),"lost":(\d+)\}', rlog)
if m:
    r, rec, lost = m[-1]
    print("\n  receiver flow stats: received=%s recovered=%s lost=%s" % (r, rec, lost))
stop_all()

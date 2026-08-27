#!/usr/bin/env python3
"""
Milestone 1b: close the round trip.

The claim under test is narrow and specific: seq_index is populated from the
sequence the CALLER supplied via RIST_DATA_FLAGS_USE_SEQ, not from librist's own
ctx->common.seq_rtp++ counter. If it were populated from the internal counter,
every resolution would be a correct mapping and a wrong retrieval -- and the only
way to see that is to pull bytes back out.

Method: a UDP drop-relay sits between the receiver and the server, so loss is
targeted rather than random. Each generated payload carries its own index, so a
repaired payload can be compared to the exact bytes generated for it. If
seq_index were keyed on the wrong counter, the repair would deliver a DIFFERENT
payload -- which shows up immediately as an index or byte mismatch, not as a
missing packet.
"""
import sys, socket, threading, time, subprocess, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen import build, send, query, field, payload_index, PCR_HZ, MASK33

SERVER_PORT = 5799
RELAY_PORT  = 5899
OUT_PORT    = 5801
IN_PORT     = 5800

FAILS = []
def check(name, cond, detail=""):
    print(("  PASS  " if cond else "  FAIL  ") + name + (("   " + detail) if detail else ""))
    if not cond:
        FAILS.append(name)


class DropRelay:
    """Bidirectional UDP relay, server->receiver direction droppable by ordinal."""
    def __init__(self):
        self.up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.up.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.up.bind(("127.0.0.1", RELAY_PORT))
        self.up.settimeout(0.2)
        self.down = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.down.settimeout(0.2)
        self.client = None
        self.run = True
        self.drop_from = None
        self.drop_to = None
        self.data_seen = 0
        self.dropped = 0
        self.wire_sizes = {}
        threading.Thread(target=self._a, daemon=True).start()
        threading.Thread(target=self._b, daemon=True).start()

    def arm(self, first, count):
        """Drop `count` consecutive large (data) datagrams starting at ordinal `first`."""
        self.drop_from, self.drop_to = first, first + count
        self.dropped = 0

    def disarm(self):
        self.drop_from = self.drop_to = None

    def _a(self):                      # receiver -> server
        while self.run:
            try:
                d, a = self.up.recvfrom(65536)
            except socket.timeout:
                continue
            self.client = a
            try:
                self.down.sendto(d, ("127.0.0.1", SERVER_PORT))
            except OSError:
                pass

    def _b(self):                      # server -> receiver
        while self.run:
            try:
                d, _ = self.down.recvfrom(65536)
            except socket.timeout:
                continue
            if not self.client:
                continue
            # Only count/drop DATA-sized datagrams; RTCP must always pass or the
            # peer dies and nothing is repaired.
            if len(d) > 300:
                n = self.data_seen
                self.data_seen += 1
                self.wire_sizes[len(d)] = self.wire_sizes.get(len(d), 0) + 1
                if self.drop_from is not None and self.drop_from <= n < self.drop_to:
                    self.dropped += 1
                    continue
            try:
                self.up.sendto(d, self.client)
            except OSError:
                pass


class Collector:
    def __init__(self):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16 << 20)
        self.s.bind(("127.0.0.1", OUT_PORT))
        self.s.settimeout(0.5)
        self.got = {}
        self.raw = 0
        self.rawbytes = 0
        self.sample = None
        self.sizes = {}
        self.run = True
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while self.run:
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            self.raw += 1
            self.rawbytes += len(d)
            self.sizes[len(d)] = self.sizes.get(len(d), 0) + 1
            if self.sample is None:
                self.sample = d[:32]
            i = payload_index(d)
            if i is not None:
                self.got[i] = d


def start(cmd, log):
    return subprocess.Popen(cmd, shell=True, stdout=open(log, "w"),
                            stderr=subprocess.STDOUT, preexec_fn=os.setsid)


def stop_all():
    # SIGKILL, not SIGTERM. part8 catches SIGTERM, leaves its main loop, then
    # blocks forever in pthread_join() on a debug thread parked in a blocking
    # accept(). A polite kill leaves the process alive still holding :5800 and
    # :5799, and the next run silently talks to the corpse. This is a workaround
    # for a defect in the server, NOT a fix for it.
    subprocess.run("pkill -KILL -x part8; pkill -KILL -x ristreceiver", shell=True)
    time.sleep(1)


# Point these at the librist build under test. NEVER at a packaged librist:
# this is a modified fork carrying VSF TR-06-4 Part 6 (program selection) and
# Part 7 (FSR), and an upstream build will not have them.
LIBRIST = os.environ.get("P8_LIBRIST", "/tmp/shms/librist/bh2")
LD = "LD_LIBRARY_PATH=" + LIBRIST
RX = os.environ.get("P8_RISTRECEIVER", LIBRIST + "/tools/ristreceiver")
SERVER = os.environ.get("P8_SERVER", "/tmp/part8")


def bring_up(extra_server_args="", selection=None):
    """Server + relay + receiver, FSR engaged, ready to stream."""
    stop_all()
    subprocess.run("rm -f /tmp/part8_recovery.sock", shell=True)
    start(f'{LD} {SERVER} -i "udp://@:{IN_PORT}" '
          f'-o "rist://@127.0.0.1:{SERVER_PORT}?weight=1000&buffer=10000" '
          f'-d /tmp/part8_recovery.sock {extra_server_args}', "/tmp/s.log")
    time.sleep(1)
    relay = DropRelay()
    time.sleep(0.3)
    sel = ""
    if selection:
        sel = " --content-selection '%s'" % selection
    start(f'{LD} {RX} -i "rist://127.0.0.1:5798?weight=0&buffer=10000,'
          f'rist://127.0.0.1:{RELAY_PORT}?weight=1000&buffer=10000" '
          f'-o "udp://127.0.0.1:{OUT_PORT}"{sel}', "/tmp/r.log")
    # wait for FSR enable to reach the server
    for _ in range(40):
        time.sleep(0.5)
        if "FSR ENABLE" in open("/tmp/s.log").read():
            return relay
    return relay

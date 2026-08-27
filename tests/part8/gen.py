#!/usr/bin/env python3
"""
Part 8 Milestone 1 validation harness.

Generates a synthetic MPTS with KNOWN ground truth, streams it to the running
recovery server over UDP as 1316-byte datagrams (exactly one RTP payload each,
so payload index == datagram index and the mapping under test is unambiguous),
then queries the debug socket and checks every answer against ground truth.
"""
import socket, struct, sys, time, os

TS = 188
PER = 7
PAYLOAD = TS * PER
PCR_HZ = 90000
MASK33 = (1 << 33) - 1

def ts_packet(pid, cc, pcr=None, disc=False, payload_byte=0x00):
    """One TS packet. With pcr set, carries an adaptation field holding it."""
    p = bytearray(TS)
    p[0] = 0x47
    p[1] = (pid >> 8) & 0x1F
    p[2] = pid & 0xFF
    if pcr is None:
        p[3] = 0x10 | (cc & 0x0F)              # AFC=1, payload only
        for i in range(4, TS):
            p[i] = payload_byte
    else:
        p[3] = 0x30 | (cc & 0x0F)              # AFC=3, adaptation + payload
        p[4] = 7                               # af_length: flags + 6 PCR bytes
        p[5] = 0x10 | (0x80 if disc else 0x00) # PCR_flag (+ discontinuity)
        base = pcr & MASK33
        p[6]  = (base >> 25) & 0xFF
        p[7]  = (base >> 17) & 0xFF
        p[8]  = (base >>  9) & 0xFF
        p[9]  = (base >>  1) & 0xFF
        p[10] = ((base & 1) << 7) | 0x7E       # 6 reserved bits set
        p[11] = 0x00                           # extension low
        for i in range(12, TS):
            p[i] = payload_byte
    return bytes(p)


def build(n_payloads, pcr_pids, pcr_interval_payloads, disc_at=None, start_pcr=0,
          service_slots=0, service_pid=0x0201):
    """
    Returns (datagrams, truth) where truth is a list of
    (pid, pcr_base, payload_index, slot) in emission order.
    """
    dgrams, truth = [], []
    cc = {pid: 0 for pid in pcr_pids}
    cc[0x0100] = 0
    epoch_shift = 0
    for idx in range(n_payloads):
        pkts = []
        for slot in range(PER):
            # service_slots puts the first N slots of every payload on the
            # selected service PID, so a filtered payload carries a measurable
            # number of packets rather than one every 40 payloads.
            pid_here = service_pid if slot < service_slots else 0x0100
            pcr_here, disc_here = None, False
            # place a PCR for one service every pcr_interval_payloads payloads
            for k, pid in enumerate(pcr_pids):
                if idx % pcr_interval_payloads == 0 and slot == k:
                    ticks = int(idx * (PAYLOAD * 8) * PCR_HZ / 40_000_000)
                    pcr_here = (start_pcr + ticks + epoch_shift) & MASK33
                    pid_here = pid
                    if disc_at is not None and idx == disc_at and k == 0:
                        # a real discontinuity: jump backwards AND flag it
                        epoch_shift = (epoch_shift - 5 * PCR_HZ) & MASK33
                        pcr_here = (start_pcr + ticks + epoch_shift) & MASK33
                        disc_here = True
                    truth.append((pid_here, pcr_here, idx, slot))
                    break
            cc[pid_here] = (cc.get(pid_here, 0) + 1) & 0x0F
            pkts.append(ts_packet(pid_here, cc[pid_here], pcr_here, disc_here,
                                  payload_byte=idx & 0xFF))
        blob = bytearray(b"".join(pkts))
        # Self-identifying payload: a magic + big-endian payload index written
        # into the first TS packet's payload area. Comparing whole streams is
        # useless here because the receiver starts mid-stream and may reorder;
        # what we need is to identify each received payload and compare it to
        # the exact bytes generated for that index.
        blob[20:24] = b"P8ID"
        blob[24:28] = idx.to_bytes(4, "big")
        dgrams.append(bytes(blob))
    return dgrams, truth


def payload_index(dgram):
    """Recover the generator index from a received payload, or None."""
    if len(dgram) < 28 or dgram[20:24] != b"P8ID":
        return None
    return int.from_bytes(dgram[24:28], "big")


def send(dgrams, host="127.0.0.1", port=5800, mbps=40.0):
    """
    Paced to a realistic rate. Blasting is not a shortcut here: ext_seq is
    assigned per RECEIVED payload, so a single dropped datagram shifts the
    mapping under test and every ground-truth comparison afterwards is
    meaningless. Losslessness is a precondition of the test, not a nicety.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 8 << 20)
    per = (PAYLOAD * 8) / (mbps * 1e6)          # seconds per datagram
    burst = 32
    t0 = time.perf_counter()
    for i, d in enumerate(dgrams):
        s.sendto(d, (host, port))
        if i % burst == 0:
            target = t0 + (i + 1) * per
            now = time.perf_counter()
            if target > now:
                time.sleep(target - now)
    s.close()


def query(cmd, path="/tmp/part8_recovery.sock"):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(path)
    s.sendall(cmd.encode())
    out = b""
    while True:
        try:
            b = s.recv(4096)
        except socket.timeout:
            break
        if not b:
            break
        out += b
    s.close()
    return out.decode(errors="replace")


def field(resp, key):
    import re
    m = re.search(re.escape(key) + r"\s*=\s*(\S+)", resp)
    return m.group(1) if m else None

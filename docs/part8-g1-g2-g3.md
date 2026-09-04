# G1 / G2 / G3 — the final gate

**G1 passes. G3 resolves, but only because G1 passes — and it corrects the
numbering rule. G2 I cannot complete: the box half needs hardware I do not
have.**

---

## G1 — recovered-payload PCR readback. PASSES.

**The box gets raw TS bytes on recovered payloads, so the PCR is readable and
the scheme is robust to a missed PCR.**

The receive path, `rist-common.c:3110-3132`:

```c
payload_offset += sizeof(*rtp);                 /* RTP header stripped      */
uint8_t *data_payload = &recv_buf[payload_offset];
payload.size = recv_bufsize - payload_offset;
if (CHECK_BIT(rtp->flags, 4)) {                 /* NPD extension present    */
    payload.size -= sizeof(*hdr_ext);
    data_payload += sizeof(*hdr_ext);
    if (CHECK_BIT(hdr_ext->flags, 7))
        if (expand_null_packets(data_payload, data_payload_out, &payload.size,
                                hdr_ext->npd_bits))
            payload.data = (void *)data_payload_out;   /* nulls reinstated  */
}
payload.type = RIST_PAYLOAD_TYPE_DATA_RAW;
```

By the time it reaches the output queue it is **plain 188-byte TS packets** —
adaptation fields and PCRs intact, nulls already expanded back.

**Recovered and normally-received payloads are indistinguishable at output.**
Both are inserted into `f->receiver_queue[]` by sequence index and the output
loop (`rist-common.c:958-995`) emits `b->data` without reference to origin:

```c
uint8_t *payload = b->data;
struct rist_data_block *block = new_data_block(NULL, b,
        &payload[RIST_MAX_PAYLOAD_OFFSET], f->flow_id, flags);
```

and `output_buffer->seq = b->seq` (`:848`). So the box receives **(sequence, raw
TS bytes)** together and can compare the PCR it reads against the PCR it expected
for that sequence, on every payload including recovered ones.

**Bonus, and it matters for G3:** `RIST_DATA_FLAGS_DISCONTINUITY` is set on the
first block after an unrecovered hole (`:959-961`). That is a free re-anchor
trigger — the box is told exactly where its numbering may have diverged.

---

## G3 — the crux, settled. And the numbering rule in the brief is wrong.

### A payload is one datagram of at most 7 packets. An interval is several.

Measured on the filtered streams:

| service | packets/interval | payloads/interval (greedy 7) | 0-packet intervals |
|---|---|---|---|
| NCN-Guyana | min 1, max 49, mean 22.1 | min 1, max 7, mean **3.59** | 0 |
| BET | min 24, max 79, mean 47.9 | min 4, max 12, mean **7.28** | 0 |

So "one payload per PCR" was never achievable and the MTU question answers
itself: **the datagram stays capped at 7 packets (1316 bytes) and an interval is
4–12 datagrams.** Nothing ever exceeds MTU.

### Does the PCR boundary pin all boundaries, or only interval starts?

**All of them — conditionally.** The rule is:

1. Interval start is pinned by the PCR packet itself, identically on both sides.
2. Within the interval, split greedily at 7 packets from that start. Given the
   same ordered packet list, both sides compute identical splits.

The original drift problem was that phase was set once at process start and never
re-established, so any disagreement was **permanent**. Under this scheme phase is
re-established 25–52 times per second, so a disagreement is **confined to one
interval**.

The condition is that both sides hold the same packet list for that interval —
which is exactly G2.

### The numbering rule as stated does not hold

> "sequence = anchor_seq + (PCRs seen since anchor)"

**Sequence advances per payload, not per PCR**, and there are 3.6–7.3 payloads
per interval. The correct rule is:

```
sequence = anchor_seq + (payloads emitted since anchor)
payloads in interval K = ceil(packets_in_interval_K / 7)
```

So the box must know the interval's **true packet count**, including packets it
lost. That is the D3/TEI dependency again.

### And the mod-16 escape does not work here

I expected interval scope to make continuity-counter reconstruction safe, because
an interval is small. It is not:

```
MAX packets of ONE pid within a single interval:  NCN 48,  BET 79
```

Both far exceed 16, so a burst loss of exactly 16 packets on one PID inside one
interval is invisible to CC and the box's count is silently short.

### Why the scheme survives anyway — G1 is the backstop

Two things rescue it, and they compose:

- **A miscount inside a withheld interval never reaches the wire.** If the box
  lost packets in interval K it withholds K entirely, so its own (wrong) packing
  of K is never emitted; the server fills every payload of K. The only lasting
  consequence is the counter offset carried into K+1.
- **That offset is detected and corrected at the next interval by G1.** The box
  reads the PCR out of the recovered payloads, compares it with the PCR it
  expected for that sequence, and re-anchors on mismatch. Plus
  `RIST_DATA_FLAGS_DISCONTINUITY` marks exactly where to look.

**So the requirement is not perfect counting — it is detectable, correctable
counting, and the design has that.** Worth stating plainly because it changes what
must be built: the box needs a PCR-readback check on every recovered payload, not
merely at startup. That check is load-bearing, not a nicety.

### Edge cases, both sides must agree

| case | rule | observed |
|---|---|---|
| two adjacent PCRs (empty interval) | emit nothing, do not advance the sequence | 0 occurrences in either service |
| interval of 1 packet | one 188-byte payload | 38 in NCN, 0 in BET |
| interval > 7 packets | greedy 7 from the interval start, short remainder last | the normal case |
| PCR packet itself | starts the payload, is packet 0 of it | — |

---

## G3 — scoping both feeders

### Box: `_rist_reader()` (`app_rist_capture.c:857-910`)

Today it accumulates into `sbuf` and emits fixed `RIST_DGRAM` (1316) chunks,
carrying the remainder across reads (`:902-910`). The change:

- scan the accumulated bytes at 188 boundaries rather than emitting blindly;
- on a packet whose PID is the PCR PID and which carries a PCR
  (`AFC ∈ {2,3}`, `adaptation_field_length > 0`, `PCR_flag` set), flush whatever
  is pending **before** appending it, so the PCR packet begins the next payload;
- otherwise flush at 7 packets as now.

**Finding the PCR PID:** from the captured PMT. The box already has it —
`GxBus_PmProgGetById()` fills `prog.pcr_pid`, used at
`app_ts_record.c:1230`. It should come from the same record the slot set is
derived from, so the feeder and the slot set cannot disagree.

Scope: contained, one function, ours.

### Server: not stock `tsp`

A fixed 7-packer cannot produce PCR-aligned boundaries, so the feeder is new
work. Cleanest shape, in order of preference:

1. **A small custom feeder** reading `tsp -P filter --pid … -O file -` on stdout,
   applying the identical boundary rule, and calling `rist_sender_data_write()`
   directly. It shares the rule with the box as one specification, and it is the
   only new server-side component.
2. A TSDuck output plugin doing the same — more machinery, no advantage here.

Either way the boundary rule must be **one written specification implemented
twice**, since the two feeders cannot share code. That is the residual risk in
this design and it should be tested by diffing payload boundaries, not just
packet streams.

---

## G2 — I cannot complete this. The box half needs hardware.

I flagged last pass that the real diff needs a box capture, and that has not
changed: I have no access to the STB. **What follows is the server half, done,
plus the exact commands for the box half.**

### Use NCN-Guyana, not BET or PARAMOUNT

You asked me to confirm BET/PARAMOUNT are unshared and clear. **They are
unshared but they are NOT clear** — BET's 0x0731 and 0x0732 are both scrambled
(`S`), PARAMOUNT has three scrambled PIDs. That does not break the diff, since
the comparison is about which packets arrive in what order and scrambled bytes
compare fine, but it is not the "no CAS" condition you asked for.

**NCN-Guyana (service 0x0002) is the right choice**: 3 PIDs, all clear, none
shared, no 0x03E8, realistic 1.74 Mb/s.

```
PMT PID 0x008E   PCR PID / video 0x0022   audio 0x0023
```

### Server side — done

```
tsp -I file <transponder>.ts \
    -P filter --pid 0x0000 --pid 0x0011 --pid 0x008E --pid 0x0022 --pid 0x0023 \
    -O file ncn_server.ts
```

Result: **4,725 packets** — PAT 20, SDT 80, PMT 20, video 4,241, audio 364.

### Box side — you run this

```
curl -s http://<stb-ip>:8998/play_file          # find NCN-Guyana's prog_id
curl -s http://<stb-ip>:8999/stream=<prog>.ts > ncn_box.ts
```

Then diff with `tests/part8/tei_analyse.py`-style packet walking, or simply
compare the PID histograms and the ordered packet list.

### What to expect, and what would send us back to per-service buffers

- **PID set mismatch** — the box slots PCR/PMT/ES via
  `_ts_rec_demux_ext_slot_alloc()` but **never PID 0**, because everything goes
  through `VALID_PID`, which excludes it. So the box capture will be **missing
  the PAT** until that is added. Expect this; it is the known gap the occupancy
  fix in `b8659b1` was preparing for, not a surprise.
- **SDT** — the box does not slot 0x0011 today either. Same category.
- **Order or count divergence on the ES PIDs** would be the serious result. That
  is the one that sends us back to per-service buffers, and it is what this diff
  exists to find.
- `user_pmt` must be **off** for the box capture, or the box will inject a
  generated PAT/PMT that the server never had.

---

## Verdict

| gate | result |
|---|---|
| **G1** | **Passes.** Raw TS delivered on recovered payloads with the sequence; PCR readable; discontinuity flag free. |
| **G2** | **Incomplete — needs the box.** Server half done (4,725 packets, NCN-Guyana). Expect the box to be missing PAT and SDT until slotted. |
| **G3** | **Resolves.** All boundaries pin, conditional on matching packet lists. But the numbering rule is per-payload not per-PCR, CC cannot always reconstruct the count, and the design therefore **depends on G1's PCR readback as an active correction mechanism**, not just a startup anchor. |

The one thing that changed in the design: **the PCR readback check is
load-bearing and must be built in from the start.**

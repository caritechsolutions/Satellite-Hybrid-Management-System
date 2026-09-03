# V1/V2 — verifying the single-sequence assumption

Both gates pass, but each carries a correction to the framing.

---

## V2 — the grouping phase. Correction: librist does NOT group.

**The framing says "it is one implementation run twice". That is true of
librist and false of the thing that decides the grouping.**

`grep` for `1316`, `7 * 188` or `188 * 7` across `src/` and `tools/` returns
**nothing**. librist has no concept of TS packets at all. The sender path is:

```c
/* tools/ristsender.c:241-288 */
recv_bufsize = udpsocket_recvfrom(callback_object->sd, recv_buf + ipheader_bytes,
                                  RIST_MAX_PACKET_SIZE, MSG_DONTWAIT, ...);
...
data_block.payload_len = recv_bufsize - offset;
rist_sender_data_write(callback_object->sender_ctx->ctx, &data_block);
```

**One UDP datagram in = one RTP payload out.** No grouping, no splitting, no
re-framing. `rist_sender_data_write()` assigns exactly one sequence number per
call (`rist.c:670`).

So the 7-packet grouping happens **before** librist, in the two feeders — and
those are two different implementations:

| side | what decides the boundary | citation |
|---|---|---|
| box | `_rist_reader()` accumulates into `sbuf`, emits `RIST_DGRAM` (1316) chunks, carries the remainder across reads | `app_rist_capture.c:857-910` |
| server | `tsp -O ip` packs a fixed 7 packets per datagram | TSDuck |

Both start their accumulator empty at process/thread start, so **phase is
whatever TS packet each happened to see first — arbitrary on both sides, and it
re-randomises on every zap** (the box's reader thread restarts per capture).

### Can it be pinned? Yes — and better than by communicating it

Do not put an offset in the anchor. **Make the boundary a function of the
content**: start a new payload at every packet carrying a PCR on the service's
PCR PID.

Both sides then compute identical boundaries from identical bytes. Phase is
never transmitted, cannot drift, and re-synchronises every PCR by construction.
It also makes a PCR interval an exact whole number of payloads, so the
PCR-to-PCR repair unit becomes exactly `sequence S_n … S_{n+1}-1` and the anchor
carries only **(PCR, sequence)** — no third field.

**Measured cost** on TLC-GUYANA's filtered stream (4 s):

```
packets between PCRs in the filtered stream: min 2  max 65  mean 23.1
=> 3.3 payloads of 7 per PCR interval; 14% of intervals are under 7 packets
```

So roughly one short payload per 3.3 — about 30% more payloads than pure
7-packing, and 14% of intervals collapse to a single sub-7 payload. librist
tolerates variable lengths (`payload_len` is just `recv_bufsize`; the NPD gate is
`filtered_len <= 7 * 204`), so this is a bandwidth-efficiency cost, not a
correctness one.

### What this costs in changes

Neither feeder does this today.

- **Box side** is `_rist_reader()` — ours, and a small change.
- **Server side is not `tsp -O ip`.** A fixed 7-packet packer cannot produce
  PCR-aligned boundaries, so the server needs a custom output stage or a TSDuck
  output plugin.

**The claim that "the sender needs almost no change" does not survive this.**
Part 6's numbering needs no change — that was Q1 and it held — but the
packetiser feeding it does.

---

## V1 — the TS diff. Passes, with one command constraint.

### Result

Independent PID-filter model of the demux vs `tsp -P filter --pid …`, over
TLC-GUYANA (service 0x0001, all-clear FTA, PMT 0x008D, PCR 0x0021, ES 0x03E8):

```
box-model packets : 4799
tsp filter packets: 4799
byte-identical    : YES, all 4799 packets, same order
```

### But the obvious plugin breaks it three ways

`tsp -P zap 1` is the natural choice for "extract one service" and it is wrong
here. First packet of each table, original vs each variant:

```
PAT (0x0000)
  original : 474000180000b0950005c500000000e0100001e08d0002e0
  zap      : 474000100000b00d0005c300000001e08d96c94721ffffff   *** REWRITTEN ***
  filter   : 474000180000b0950005c500000000e0100001e08d0002e0   IDENTICAL
PMT (0x008D)
  original : 47408d180002b0230001c10000e021f00024e021f0060504
  zap      : 47408d100002b0230001c10000e021f00024e021f0060504   *** REWRITTEN ***
  filter   : 47408d180002b0230001c10000e021f00024e021f0060504   IDENTICAL
SDT (0x0011)
  original : 474011100042b2c60005c300000005ff0001fc8011480f01
  zap      : 474011100042f0220005c300000005ff0001fc8011480f01   *** REWRITTEN ***
  filter   : 474011100042b2c60005c300000005ff0001fc8011480f01   IDENTICAL
```

1. **PAT regenerated** — section length `0x95` → `0x0d`, version `c5` → `c3`,
   one service listed, tail padded with `0xff`.
2. **SDT regenerated** — `b2c6` → `f022`, rewritten to list only this service.
3. **Continuity counters reset** — byte 3 `18` → `10` on *both* PAT and PMT.

The third is the dangerous one and it is easy to miss because the payload of the
PMT is otherwise identical. A CC that starts from zero on the server side while
the box carries the broadcast CC would make the box's CC-based loss detection
see phantom discontinuities on the PSI PIDs indefinitely.

`zap` also drops packets: 4,733 vs 4,799.

**So the server must use `filter --pid`, never `zap`.** That is the whole V1
answer, and it is a one-line constraint on the sender command.

### The known differences, reconciled

- **Nulls: neither side ever has them, no work needed.** The demux only delivers
  slotted PIDs and 0x1FFF is never slotted; `filter --pid` without 0x1FFF emits
  none. They match automatically — nothing to strip on either side.
- **CC on 0x03E8: not a diff.** 171 discontinuities in this service's PID set,
  all on 0x03E8, and identical on both sides by construction since the streams
  are byte-identical.
- **PSI/SI presence is transponder-dependent and must be checked per deployment.**
  In this capture: PAT 20 packets, SDT 80, **EIT 627**, and **NIT (0x0010) and
  TDT/TOT (0x0014) are entirely absent**. Whatever §4.3 set is chosen must be
  identical on both sides, and a table that is absent at start and appears later
  is a divergence risk if one side's list is static while the other's is derived
  from the PMT.

### Two caveats on this result

**This is a model of the box, not the box.** I ran an independent PID filter over
the same capture rather than a real dmx2 capture. That is a valid test of the two
*filtering rules* — which is where PSI regeneration shows up, and it found it —
but it does not exercise the demux itself: ordering under load,
`DMX_ERR_DISCARD_EN`, TEI handling. **The real diff still needs a box capture of
the same content**, and that is bench-testable now via the HTTP route.

**Do not use TLC-GUYANA for the real bench test.** Its ES PID 0x03E8 is both the
two-interleaved-continuity-counter PID and **shared with another service**
(`C+` in tsanalyze). Pick a service whose PIDs are unshared and which excludes
0x03E8, so a diff failure means what it says.

---

## Verdict

| gate | result |
|---|---|
| **V1 — can the TS be made identical?** | **Yes**, with `filter --pid`. `zap` fails three ways including a silent CC reset. Verified byte-identical over 4,799 packets. |
| **V2 — can the grouping phase be pinned?** | **Yes**, by deriving boundaries from PCR position rather than transmitting an offset. But the grouping is in the two feeders, not in librist, and both need changing. |

The single-sequence scheme holds. The correction to carry forward is that the
work is in the **packetisers**, not in librist: the server cannot use a stock
`tsp -O ip`, and the box's reader needs PCR-aligned emission.

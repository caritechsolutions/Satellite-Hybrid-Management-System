# Part 8 Milestone 2 — design constraints established by measurement

These are not opinions. Each one was measured during Milestone 1c against a real
59.145 Mb/s, 35-service transponder capture, and each one changes what the
Milestone 2 receiver and request handler must do. The harness that produced them
is in `tests/part8/`; re-run it rather than trusting this file.

---

## 1. FSR Enable must carry a resume point — REQUIRED, not polish

**Measured** (`tests/part8/fsrgap.py`): with the stream already running and the
receiver attached late, the receiver collected `received=33706` against `33739`
payloads produced *after* FSR engaged — `recovered=0`, `lost=0`. It received
essentially nothing that predated FSR engage. The 40,562 payloads (7.22 s) the
server already held resident were never sent.

**Cause**: Part 7's FSR Enable message carries no resume point. `proto/rtp.c`
writes only flags, ptype, len, ssrc and the `"RIST"` tag. The server therefore
has no way to know where the box stopped being able to decode, so it can only
serve forward from wherever the live edge happens to be.

**Consequence**: the switchover gap equals **outage duration + detection
latency**, and is **not bounded by the buffer**. Enlarging the buffer cannot
shrink it — the resident payloads are simply never requested. This is the
mechanism behind the field switchover gap seen in Part 7, and it also means the
resident buffer contributes nothing during a fade, which is precisely when it
should be earning its keep.

**Design in**: the box states where to restart; the server serves forward from
the buffer instead of from the live edge. That makes the 4 s resident buffer
useful during a fade and closes the Part 7 field issue.

**Corollary**: 4 s is settled as the buffer target on the **RTT budget alone**.
It was never a switchover-gap lever, so do not size it as one.

---

## 2. The recovered stream is always 7 TS packets — never assume otherwise

`expand_null_packets()` reinstates stripped nulls at the receiver *before*
output. Measured: **22,472 output datagrams, every one 1316 bytes = 7 TS
packets**, regardless of how aggressively the payload was filtered.

Build the Milestone 2 receiver against **7 packets with unselected PIDs replaced
by nulls**, not against a variable packet count.

---

## 3. The sequence space is dense — do not expect permanent holes

At a realistic single-service selection (~3.1% of the multiplex) **~78% of
payloads contain no selected TS packet at all**, because the mux interleaves 35
services and a 7-packet window rarely holds two packets of one service.

librist **sends these anyway**. There is no early-out: `send_data:` in
`send_filtered_data_to_peer()` is unconditional. An all-empty payload goes out as
an **8-byte NPD stub** (RTP extension header with `npd_bits = 0x7F`, no TS
packets); one selected packet gives 196 bytes.

Confirmed three ways: librist's own log (`1316 -> 8 bytes`), receiver flow stats
(`received=22471, recovered=0, lost=0`), and the full output datagram count.

**So every sequence number is used.** A receiver that treats a gap as a permanent
hole, or that tries to predict which sequence numbers "should" be absent under a
selection, will be wrong.

---

## 4. Bandwidth: separate FSR fallback cost from ordinary repair cost

Keep this distinction explicit in anything written up — they are different costs
with different triggers.

**FSR full-stream delivery** carries a per-payload overhead floor that does not
shrink with the selection, because every empty payload still costs an RTP header,
an extension header and a UDP/IP frame:

| selection | service bitrate | recovery wire | ratio | of which empty stubs |
|---|---|---|---|---|
| BET 0x0730 | 1.83 Mb/s | 5.22 Mb/s | 2.85× | 2.08 Mb/s (40%) |
| PARAMOUNT 0x07D0 | 1.74 Mb/s | 5.13 Mb/s | 2.95× | 2.11 Mb/s (41%) |
| Vacation 0x0672 | 0.08 Mb/s | 3.47 Mb/s | 43.9× | 2.64 Mb/s (76%) |

That **~2.1 Mb/s floor is the cost of a box being in fallback**, not the cost of
ordinary repair. **Targeted STC-NACK repairs are bounded ranges and do not carry
it.** Do not budget WAN capacity as though every box paid it continuously.

---

## 5. Resolution API: only `RESOLVE_OK` means "serve this"

`resolve()` returns an `enum resolve_status`, not a bool, and `RESOLVE_OK` is
deliberately non-zero so a zeroed struct can never look serviceable. Test
`resolve_serviceable()`. A nearest-PCR search on a non-empty ring *always* finds
something, so a populated `start_ext` is not evidence that the range may be sent;
`BEFORE_EPOCH` and `OUTSIDE_BUFFER` both come back with diagnostic fields filled
in and must not be served.

---

## 6. Numbers Milestone 2 can rely on

| quantity | value | source |
|---|---|---|
| transport rate | 59.145 Mb/s | three independent methods agree |
| TS packets / s | 39,325 | derived |
| RTP payloads / s | 5,618 | derived |
| worst gap back to a reference PCR | 40.94 ms (PID 0x08FD) | 33 PCR PIDs measured |
| resident at 4 s buffer | ~24,900 payloads, 4,430 ms, 31.3 MB | receiver attached, steady state |
| that as a fraction of the 16-bit space | 38% of 65,536 | — |
| first tripwire / ceiling | ~7.9 s / ~10.5 s nominal buffer | — |
| server RSS | ~37 MB | — |

The 4,430 ms resident against a 4,000 ms nominal confirms librist's
nominal + 2×min_rtt sizing (`min_rtt = buffer / max_retries` = 200 ms).

---

## 7. Still owed before Milestone 2 closes

The capture used so far contains **zero** packets with `discontinuity_indicator`
set. The epoch handling in `catalogue_insert()` and the nearest-across-all-epochs
search in `resolve()` are therefore validated **only synthetically** (A5, R3) and
have never met a real discontinuity. A longer capture containing one is
outstanding.

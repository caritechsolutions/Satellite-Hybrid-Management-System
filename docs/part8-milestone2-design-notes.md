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

Measured on the live headend: **4.07 Mb/s for BET**, against 5.22 Mb/s predicted
from the capture. The floor is real but the bench figure overstated it; use the
measured number. Note this was taken with **FSR ENGAGED** -- the bench receiver's
dead weight-0 peer makes it declare its satellite path failed, which is the only
way live data passes the FSR gate. So 4.07 Mb/s is the **full-stream fallback**
cost of one box, not the cost of ordinary repair, and capacity planning must keep
those two apart.

---

## 5. Resolution API: only `RESOLVE_OK` means "serve this"

`resolve()` returns an `enum resolve_status`, not a bool, and `RESOLVE_OK` is
deliberately non-zero so a zeroed struct can never look serviceable. Test
`resolve_serviceable()`. A nearest-PCR search on a non-empty ring *always* finds
something, so a populated `start_ext` is not evidence that the range may be sent.

Four non-OK statuses, and the distinction between the last two matters:

| status | meaning |
|---|---|
| `NO_CATALOGUE` | nothing has ever been catalogued for this PID |
| `BEFORE_EPOCH` | the request predates the epoch it resolved into |
| `OUTSIDE_BUFFER` | the nearest PCR held is further from the request than the buffer could span — the request is nowhere near anything we have |
| `TOO_OLD` | **we have exactly what was asked for, and it has aged out of the retransmission buffer** |

To exercise `TOO_OLD` from the debug socket, use the `too_old_probe` value from
`bounds` -- not `oldest`, which is the eviction frontier and drifts at wall-clock
rate on a full ring.

`TOO_OLD` is the one the oversized ring exists to produce. It is checked against
the **live edge** using payload residency, not against the nearest PCR and not
against the clock: librist holds the newest N payloads, so anything older than
(live edge − N) is gone regardless of what the timestamps say.

That check was missing at first, and its absence was the most serious defect
found on hardware: an **exact** hit on a 40-second-old entry gave a distance of
zero, passed every check, and returned `RESOLVE_OK` with a sequence librist had
dropped long before. Milestone 2 must treat `TOO_OLD` as "your request was late",
distinct from `NO_CATALOGUE`'s "you asked about something we never had".

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

## 7. CLOSED — the epoch logic has now run against real data

A genuine discontinuity occurred on the live headend and the corrected epoch
semantics handled it:

```
0x03E8 backward-jump delta=-27,044,504 ticks (-300.5 s) -> epoch 1
0x03E8 forward-jump  delta=+27,045,604 ticks (+300.5 s)  99 payloads later
```

A real backward jump created epoch 1, which is what the nearest-across-all-epochs
search was written for and what had previously only been exercised synthetically
(A5, R3). **This item is closed.**

It is the same PID as the continuity-counter anomaly found in C4/C5: two PCR
timelines interleaved on one PID, matching the two independent continuity
counters the greedy-assignment analysis found. **One upstream defect, two
symptoms** — worth raising with whoever operates that encoder.

### Operational note: never point a second consumer at a running input port

A `tsp -I ip 0.0.0.0:5800` against a live instance's input port makes two
consumers compete for one UDP socket. The kernel gives datagrams to whichever
reads first, starving the server for as long as the second consumer runs. The
detector reports this correctly and unmistakably -- 32 simultaneous forward jumps
of ~10 s, one per catalogued PID -- but the feed really is interrupted. To
inspect a live input, tee it upstream instead.

## 8. Idle services move the PID count

Two services on this transponder currently carry no packets at all (0x0021 and
Vacation's 0x0673), so their PCR PIDs are absent from the catalogue. The
catalogue PID count is therefore a **live-content indicator**, not a health
signal: it moves for reasons that have nothing to do with Part 8. Do not alarm
on a change in it. A count of zero, or zero servable entries, is worth a warning;
a count that drops from 33 to 32 is a service going idle upstream.

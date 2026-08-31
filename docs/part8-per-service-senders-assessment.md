# Per-service filtered senders — assessment of A1–A7

Attacking the proposal, as asked. Verdict up front:

**The shape is right and I would build it. But step 8 is necessary and not
sufficient — there are four further divergences, and one of them makes "one
anchor holds indefinitely" false. None of them sends us back to STC-NACK; they
change the anchor from one-shot to periodic and self-validating. Separately, A5
contains a live librist units bug that would have made the whole withheld-hole
mechanism fail in the field in a way that looks like "the server never
answered".**

---

## A1 — Attacking the reasoning

Step 8 removes **the** divergence you identified, and it is the right fix for it.
`udp.c:931` and `udp.c:1080` confirm your step 7 exactly:

```c
buffer->seq = ctx->common.seq++;
```

Pure free-running count per payload sent, per context. So a per-service sender
has its own counter over its own filtered population, and the box counts the same
population. That part works.

Here is what it does not fix.

### D1 — The box injects PAT/PMT that the server has never seen

`user_pmt` is true today and `_ts_rec_user_info_generate()` (`app_ts_record.c:1611`)
**generates** PAT/PMT and injects them on a timer, default 100 ms, tunable via
`/tmp/ristpsims` (`app_ts_record.c:159-163`). Those packets exist only on the box.
The server, filtering the real multiplex, emits the **broadcast** PAT/PMT instead.

Two different packet populations from the first second. **`user_pmt` has to go**
and the box must pass the broadcast PAT/PMT through as slotted PIDs like any
other. That is a behaviour change with a consequence worth naming: the generated
PMT was there to make the downstream consumer's probe collapse fast, so zap-to-
picture may get slower when it is removed.

### D2 — The anchor gives you a sequence number but not a phase

This is the one I would most expect to be missed, because it is invisible until
you look at payload *contents* rather than payload *numbers*.

librist packs 7 TS packets per 1316-byte payload. Sequence alignment guarantees
that server payload N and box payload N carry the same *number*. It does not
guarantee they carry the same *seven packets*. If the box's capture begins
mid-group — offset by k packets relative to where the server's filtered stream
starts its grouping — then forever after:

```
server payload N = filtered packets [7N   .. 7N+6]
box    payload N = filtered packets [7N+k .. 7N+k+6]
```

Splice server payload N into the box's stream at position N and you insert seven
packets that overlap the neighbours by k and leave a k-packet hole. **You get
duplication and loss on every single repair**, and the numbers all look correct.

So the anchor must be (sequence, phase), or the filtered stream must be
packetised from a deterministic origin both sides can compute. This is cheap to
fix and expensive to discover later.

### D3 — Box-side loss must be countable, and that depends on the TEI answer

To keep counting in step with the server, the box must know **how many packets it
lost**, not merely that it lost some. Its counter has to advance by the true
population, including the packets that never arrived.

- If the demux **passes TEI** (branch a), the packet still arrives. The count
  never diverges; you mark it bad and withhold its payload. Clean.
- If the demux **discards** (branch b), the packet is gone and the count must be
  reconstructed from continuity counters — which are ambiguous modulo 16. A gap
  of exactly 16 on one PID is invisible and shifts the count by 16 permanently.

**This proposal therefore depends on the TEI field test more than the framing
suggests.** Under branch (b) the box cannot reliably count its own losses, and
counting is the entire basis of the alignment. Worth knowing before the dish trip
rather than after.

### D4 — Two independent receptions cannot stay in step indefinitely

This is the one that makes step 8 insufficient rather than merely incomplete.

The server's population is what **the headend's downlink** received. The box's is
what **the box's dish** received. They are two independent receptions of the same
transmission. Any packet the headend loses is absent from the server's stream,
so the server's counter does not advance for it — and the box's does.

**One lost packet at the headend offsets the anchor permanently**, and every
subsequent repair is then delivered into the wrong position. Silent.

We measured `sync_errors = 0` over 27.1M TS packets on the live headend, so this
is rare. Rare is not the same as never, and the shape here is the worst kind:
**low probability, permanent consequence, no symptom until pictures break in a
way that looks like a server fault.** A system that is correct for six hours and
then silently corrupt is harder to operate than one that fails immediately.

The same applies, more sharply, to a sender restart: the counter restarts, the
anchor is stale, and nothing announces it.

### D5 — Minor, but list them

- **Null policy.** TSOUT slots deliver only slotted PIDs, so the box's capture
  contains no 0x1FFF. The server must filter nulls identically. NPD operates
  below this and does not help — this is about packet population, not payload
  compression.
- **`DMX_ERR_DISCARD_EN` on the audio slot** (`app_ts_record.c:1679`) silently
  drops errored audio packets at the demux. That is a box-side population change
  the server cannot know about. It should be cleared along with `DES_EN` for this
  topology, giving `0x1a` on both slots.

### Verdict on A1

**Alignment cannot hold indefinitely from a single anchor.** D4 alone guarantees
eventual divergence, and D2 means it may never have been aligned in content to
begin with.

**But this does not send us back to STC-NACK.** The fix is proportionate:

1. Anchor **(sequence, phase)**, not sequence alone.
2. **Re-anchor periodically** rather than once — the same query, on a timer,
   riding the existing RTCP keepalive (A4). This bounds divergence to the
   re-anchor interval instead of letting it accumulate forever.
3. Make the anchor **self-validating**: carry enough content identity (a PCR, or
   a hash of the payload) that the box can *detect* misalignment rather than
   assume it away. Detecting it is what turns D4 from silent corruption into a
   re-sync event.

That is a small change to step 4, not a change of architecture. Steps 1–3, 5, 6
and 8 stand.

## A2 — The identical-filtering contract

**Yes, and it should be derived, not duplicated.** Two hand-maintained PID lists
in two codebases that must agree exactly, where disagreement is silent, is the
same defect class as D4.

The box already fetches a per-service record from the recovery API
(`app_rist_api.c`, cached by `service_id`; `s_rist.rec` carries `rist_url`,
`name`, `service_id`, `marker_pid`). **That record is the natural single source of
truth.** Extend it to carry the authoritative PID set — the ES PIDs, the PCR PID,
the PMT PID, and the Part 6 §4.3 mandatory passthrough (PSI/SI 0x00–0x1F, CAT,
EMM) — computed **server-side from the same selection the sender filters by**.

Then:

- the sender filters by that set because it produced it;
- the box builds its dmx2 slot set from the same fetched record, in
  `_ts_rec_prog_config()` (`app_ts_record.c:1645`) and
  `_ts_rec_demux_ext_slot_alloc()` (`:1127`), rather than from the PMT it parsed
  itself.

One list, one origin, and a version/generation number on it so the box can detect
that the sender's set changed under it — which happens whenever the broadcast PMT
changes, and is exactly when silent divergence would otherwise start.

This also removes a real hazard: the box currently derives its slot set from the
PMT it parses locally, and under damage it may parse a *different* PMT than the
server did.

## A3 — Layer over the existing ingest. Do not run N ingests.

**One ingest, one catalogue, N filtered senders.** The alternative is clearly
worse and the numbers are not close.

**N independent ingests:** 35 × 59.145 Mb/s = **2.07 Gb/s** of multicast
reception, 35 × 39,325 = 1.38 M packet parses/s, 35 copies of the ingest and 35
catalogues to keep consistent — and it throws away the ingest and catalogue that
are validated 4,907/4,907 against the TSDuck oracle and running live. No.

**Layered:** one ingest at 59.145 Mb/s exactly as today. Filtering is a PID test
per packet per sender — 35 × 39,325 = 1.38 M integer tests/s, which is nothing on
the headend x86. The validated ingest and catalogue are untouched.

**Memory.** Your ~40 MB is right for the per-service buffers (35 × ~1.1 MB at
4 s), but the honest total depends on one decision:

| arrangement | memory | note |
|---|---|---|
| senders hold their own filtered copies, multiplex buffer retained | ~31 + 38.5 = **~70 MB** | catalogue keeps working exactly as validated |
| senders hold copies, multiplex buffer dropped | **~38.5 MB** | catalogue must then be rebuilt over per-sender storage |
| senders index into the shared multiplex buffer | ~31 MB + indices | least memory, most coupling |

Against the ~37 MB RSS measured today, the first option roughly doubles the
server and changes nothing that was validated. **I would take it.** 70 MB on a
headend is not a constraint worth optimising against, and the second and third
options both put the validated catalogue at risk to save memory nobody needs.

**What happens to the catalogue.** It stays as-is — PCR → position in the
multiplex. Each sender needs one additional per-service index mapping multiplex
position → its own sequence number. That index is the new thing being built, and
it is small and independently testable against the same oracle.

## A4 — The startup anchor

**Put it on the RTCP app-specific carrier that already exists in our fork.** It
carries Part 6 content selection and Part 7 FSR today, with subtype dispatch and
a `"RIST"` name field validated at `rist-common.c:2379` and `:1665`. A sequence
anchor is the same kind of message and belongs on the same path — not on the
debug socket, which you are right to rule out for a fleet-facing box.

Two properties it should have, both following from D4:

- **Periodic, not one-shot.** It rides the keepalive, which is once per second by
  protocol design. Re-anchoring costs nothing and bounds divergence.
- **Content-identified.** The reply should be (sequence, phase, PCR) so the box
  can verify the anchor rather than trust it.

**If the anchor is lost or the sender restarts:** the sequence counter restarts
from zero and the box's numbering is instantly wrong. Nothing in the current
design announces this. The sender needs a **generation/epoch id** that changes on
restart, carried alongside the anchor; the box treats a change as "discard the
anchor and re-anchor", and withholds nothing until it has. That is the same
epoch mechanism the catalogue already has for PCR discontinuities, and it should
be the same concept, not a second one.

## A5 — The withheld-sequence NACK trigger

Three sub-answers. The routing one is good news; the timeout one is a bug.

### Where does the NACK go? To the server. Our fork already handles this.

`send_nack_group()` (`rist-common.c:1091-1160`) — the version compiled in our
fork, not the `#ifdef default_send_nack_group_function` one above it — does a
first pass specifically for weight-1000 peers:

```c
if (check->is_rtcp && !check->dead && check->config.weight == 1000)
    { recovery_agent = check; ... }
if (recovery_agent != NULL) peer = recovery_agent;
```

and sends **all** NACKs there, regardless of which peer the missing entry was
recorded against. So the Part 7 two-peer arrangement does route it to the
recovery peer directly. **You are not back to forwarding.**

Note the asymmetry though: `mb->peer` (`flow.c:27`) is still the peer that
delivered the surrounding packets — the *local* packetiser. The NACK is
transmitted to the recovery peer, but every *policy* decision about it reads the
local peer's config: `max_retries`, `recovery_mode`, `buffer_bloat_active`, and
the RTT used for retry pacing. That is what the next section is about.

### The loss timeout, and the bug

Two independent give-up conditions in `rist_process_nack()`
(`rist-common.c:724-793`):

```c
if (b->nack_count >= peer->config.max_retries)                    return 8;
if ((now - b->insertion_time) > (recovery_buffer_ticks * 1.1))    return 9;
```

`RIST_DEFAULT_MAX_RETRIES = 20`, and the age deadline is **1.1 × the recovery
buffer**, so ~4.4 s at a 4000 ms buffer. Separately the *output* deadline is
`target_output_time = packet_time + recovery_buffer_ticks` (`rist-common.c:441`),
i.e. the hole is released at one buffer depth. **The hold line is bounded by the
receiver buffer itself — ~4 s — not by a separate shorter timeout.** That is the
number you asked for and it is comfortable.

**Except that the retry budget is exhausted long before any of it is used.**

```c
uint64_t rtt = (peer->eight_times_rtt / 8);
if (rtt < peer->config.recovery_rtt_min)  rtt = peer->config.recovery_rtt_min;   /* 5   */
else if (rtt > peer->config.recovery_rtt_max) rtt = peer->config.recovery_rtt_max; /* 500 */
...
b->next_nack = now + (uint64_t)(rtt * 1.1);
```

`peer->last_rtt` and `eight_times_rtt` are in **NTP units** — `calculate_rtt_delay()`
(`proto/rist_time.c:91-98`) returns `response - request` with both operands NTP,
and `stats.c:82` divides by `RIST_CLOCK` to print milliseconds. But
`recovery_rtt_min`/`_max` are **millisecond** constants (5 and 500, `peer.h:34-35`).

So the clamp compares NTP units against milliseconds. A real RTT of 0.14 ms is
601,295 NTP units, so **every real RTT is clamped down to 500 NTP units = 116
nanoseconds.** Then `next_nack = now + 550` NTP units ≈ 128 ns — always already
in the past.

**Consequence: every missing packet is re-NACKed on every pass of
`receiver_nack_output()`, and `max_retries = 20` is burned in twenty passes** —
milliseconds — after which `rist_process_nack` returns 8 and the packet is
abandoned with ~4 s of buffer still unused.

The commented-out line immediately above it is the correct one:

```c
//b->next_nack = now + (uint64_t)rtt * (uint64_t)ratio * (uint64_t)RIST_CLOCK;
```

`RIST_CLOCK` is present there and absent from the live line. This is the **same
units defect family** we already found and fixed in `udp.c` (`5000000000ULL` →
`5000ULL * RIST_CLOCK`) — a millisecond constant meeting an NTP-unit value.

The same mixed comparison appears again at `rist-common.c:1979-1990`, governing
the reorder buffer, so the out-of-order-vs-missing decision is affected too.

**Why it has not bitten yet:** in Part 7 the recovery peer delivers full-stream
under FSR and ordinary NACK-driven repair is barely exercised — consistent with
the `recovered=0` we measured. This proposal makes NACK the *primary* mechanism,
so it moves from latent to load-bearing. It is fixable, it is small, and it must
be fixed before any of this is tested, or every result will look like "the server
did not answer in time".

### Will librist reliably NACK a withheld range?

Yes, with two constraints from the code:

- **`missing_count > 32768` returns immediately** (`rist-common.c:459-461`) —
  a wrap-around guard. Withheld ranges must stay far below half the sequence
  space, which at PCR granularity they trivially are.
- **`missing_counter_max`** (`rist-common.c:290`) caps outstanding missing
  packets at roughly `buffer_ms × max_bitrate_mbps / packet_size`. Exceed it and
  `receiver_mark_missing` stops queueing retries and logs "Retry buffer is
  already too large". At a filtered ~450 payloads/s this is a very long way off,
  but it is the ceiling on how much may be withheld at once.

At PCR granularity — worst-case 44.7 ms, typical 39.6 ms, so ~18–20 payloads per
PCR interval at 450/s — both limits are comfortable. The mechanism is sound; the
pacing bug is what breaks it.

## A6 — Zap as reconnect

**The connection setup is not the problem. The buffer fill is, and it is
structural: one full receiver buffer, ~4 s, every zap.**

`target_output_time = packet_time + f->recovery_buffer_ticks`
(`rist-common.c:441`) — the receiver holds every packet for the full buffer depth
before releasing it. That is not a warm-up that amortises; it is the steady-state
latency, paid again from zero on every new session. It is the same mechanism
behind the Part 7 ~8 s zap you are still fielding complaints about.

So yes, this is a problem, and per-service senders make it structural rather than
incidental — a zap is now necessarily a new session.

**But A7 is the fix, and it is the same mechanism.** See below.

## A7 — Fallback to normal play

**Achievable, the machinery already exists, and it should be built first — not
because it is the safety net, but because it is also the zap fix.**

The decision point is `app_rist_screen_enabled()` (`app_rist_capture.c:667`),
evaluated by `app_play_control.c:1165`, which today suppresses the live-tuner
decode when the chain is up. The predicate must become **"the Part 8 path is up
*and producing output*"**, not "the chain was started". Part 7 already built
exactly that machinery:

- `s_rist.screen_started` and the probe timer that polls the player until it
  reports RUNNING (`app_rist_capture.c:733`, `:711`);
- `s_rist.screen_failed` and the `failed_svc_id` latch, with the deliberate
  "zapping away and back is a genuine retry" semantics
  (`app_rist_capture.c:1156-1164`);
- the ordering guarantee that `app_rist_play_change()` runs before the decode
  block so the single video decoder is free for whichever path wins
  (`app_play_control.c:1148-1155`).

**The synthesis worth taking:** decode from dmx0 immediately on zap, and cut to
dmx3 only once the Part 8 chain has filled its buffer and is producing. That
makes the zap instant *and* makes Part 8 a switchable enhancement, from one
mechanism. It turns A6 from a regression into a non-issue, and it is the same
code path the fallback needs anyway.

### Where this depends on Q1 — and it does

You asked me not to let the proposal depend on Q1 unless it must. It does, in
exactly one place, and it is A7 rather than the repair path.

Per Q3, the CAS moves to dmx3 — one CA context, set by `dmx_id`/`ts_id` in
`app_cgcas_init()`. **That binding is exclusive: with the CAS on dmx3, dmx0
cannot descramble at all.** So for an encrypted service the fallback to normal
play gives a locked picture, not a working one, unless the CAS is re-initialised
back onto dmx0 — and `GxCas_Init` runs once, with a "Demux will be initialed
twice, may be lose some hardware resource" guard in the layer below.

Consequences, stated plainly:

- **FTA services: the fallback works unconditionally.** No Q1 dependency. This is
  also the whole set you can test today, since `CASCAM_SUPPORT` is 0.
- **Encrypted services: the fallback needs the CAS to move back to dmx0**, which
  is a re-init and not a switch, and its cost and reliability are unknown.
- If **Q1 comes back negative** — a descrambler cannot bind to a memory-fed
  instance — then dmx3 cannot descramble at all, the CAS stays on dmx0, and this
  whole topology is FTA-only unless option (b) carries the repair.

So: build and test the FTA path now, which is unblocked and is most of the work.
The CAS story stays gated on Q1, and the fallback for encrypted services is the
piece to design once Q1 is answered — not before.

---

## What I would do next, in order

1. **Fix the librist NACK pacing units bug** (A5). Everything downstream measures
   nothing until this is right, and it is a small, well-understood fix of a class
   we have already handled once.
2. **Answer TEI at the dish** (D3). It gates whether box-side loss is countable,
   which is the basis of the whole alignment.
3. **Build A7's fallback and cut-over first.** It de-risks the fleet, and it is
   also the zap fix.
4. Then the per-service senders as a layer (A3), with the derived PID contract
   (A2) and the periodic self-validating anchor (A1/A4).

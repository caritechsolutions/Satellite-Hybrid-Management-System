# Part 8 recovery server — test harness

Validation harness for `part8_recovery_server.c` (VSF TR-06-4 Part 8: a recovery
server that ingests an MPTS, catalogues PCRs, and serves retransmissions out of a
resident buffer).

Everything here exists to answer one kind of question: *does the catalogue
actually say what the stream actually contains, and can bytes be pulled back out
by sequence?* The scripts are kept because the method outlives any particular
result — re-run them against every future build.

## Running

The harness drives a real `part8_recovery_server` and a real `ristreceiver`
against real transport streams. Paths come from the environment:

| variable | default | meaning |
|---|---|---|
| `P8_LIBRIST` | `/tmp/shms/librist/bh2` | the librist build under test |
| `P8_RISTRECEIVER` | `$P8_LIBRIST/tools/ristreceiver` | receiver binary |
| `P8_SERVER` | `/tmp/part8` | the recovery server binary |
| `P8_TSDUCK_BIN` | `/home/user/tsduck/.../release-x86_64-vm` | TSDuck binaries |

**Never point `P8_LIBRIST` at a packaged or upstream librist.** This is a
modified fork carrying Part 6 (program selection) and Part 7 (FSR); an upstream
build has neither, and the tests will fail in ways that look like product bugs.

## Files

### Core

- **`gen.py`** — synthetic MPTS generator and the debug-socket client.
  Carries the **P8ID marker scheme**: every generated payload gets the literal
  `P8ID` plus a 4-byte payload index at bytes 20–27. That is what makes a repair
  checkable *by identity* rather than by count — if `seq_index` were keyed on
  librist's internal counter instead of the caller's `USE_SEQ` value, a NACK
  would return a *different* payload, and only a marker catches that. A missing
  packet is easy to see; a wrong packet is not.
  Also paces `send()` to a realistic rate: unpaced sends drop in the kernel, and
  because the extended sequence is assigned per *received* payload, a single
  drop shifts the whole mapping and silently invalidates ground truth.

- **`roundtrip.py`** — `DropRelay` (bidirectional UDP relay that drops
  server→receiver datagrams by ordinal, so loss is *targeted* rather than
  random), `Collector`, and `bring_up()`.
  `stop_all()` uses SIGKILL deliberately — see the note in the source.

- **`oracle.py`** — TSDuck as an **independent** PCR extractor. Nothing in it
  touches the catalogue's own extraction path: PCRs come from `tsp -P pcrextract`
  and the catalogue's view comes back over the debug socket, so a shared bug
  cannot hide in the overlap. Also derives transport rate from PCR deltas against
  byte offsets, which cross-checks the extraction at the same time (a mis-parsed
  PCR shows up as an absurd bitrate rather than passing quietly).

- **`negcontrol.py`** — **read this before trusting any C1 result.** A forward
  pass of "every PCR resolved exactly" is not evidence on its own: a `resolve()`
  that echoed the request, or ignored the PID, would score identically. These
  controls are built to fail if the catalogue is cheating, and they include a
  check that the out-of-range counter *moves for bad requests and stays still for
  good ones* — without which "out of range is refused" would itself be vacuous.

### Scenario scripts

| script | what it establishes |
|---|---|
| `run.py` | scenario A: ingest, catalogue, resolution |
| `wrap.py` | the 16-bit sequence wrap |
| `r1.py`–`r4.py` | the round trip: that `seq_index` is keyed on the caller's `USE_SEQ` sequence, proven by pulling exact bytes back |
| `c1c6.py` | catalogue vs oracle both directions; resolution sanity including a request landing *between* two real PCRs |
| `c45.py` | structure, discontinuities, and an independent continuity-counter analysis (deliberately *not* TSDuck's — answering "is TSDuck's CC error real?" with TSDuck proves nothing) |
| `c8.py` | what happens when a payload filters to **nothing** |
| `fsrgap.py` | measures the FSR switchover gap instead of assuming it |

## Results worth keeping in mind

Against a real 59.145 Mb/s / 35-service transponder capture:

- **`seq_index` is correct** under `RIST_DATA_FLAGS_USE_SEQ` — proven by byte-exact
  repair of a targeted gap, not by counting.
- **FSR resumes at the live edge with no backfill.** Measured, not assumed
  (`fsrgap.py`): the receiver got only what was produced *after* FSR engaged. So
  the switchover gap is set by outage + detection latency and **cannot** be
  shrunk by enlarging the buffer. A resume point in the FSR Enable message is the
  fix.
- **At a realistic single-service selection (~3% of the mux), ~78% of payloads
  filter to nothing** — the multiplex interleaves 35 services, so a 7-packet
  window rarely holds two packets of one service. librist **sends** these as an
  8-byte NPD stub rather than skipping them, so **the sequence space stays dense
  and the receiver must not expect permanent holes.** The receiver reinstates the
  nulls, so output is always 7 TS packets.
  The resulting **~2.1 Mb/s floor of per-payload overhead is the cost of FSR
  full-stream delivery** — it is what a box in fallback costs. It does **not**
  apply to targeted STC-NACK repairs, which are bounded ranges.
- **A 4 s buffer holds ~24,900 payloads = 38% of the 65,536 sequence space**, with
  the first tripwire at ~7.9 s and the ceiling at ~10.5 s.

## Do not point a second consumer at a running instance's input port

```
tsp -I ip 0.0.0.0:5800 ...        # NOT against a live instance
```

Two consumers on one UDP port compete, and the kernel hands each datagram to
only one of them. Doing this to a live instance starves the server for as long
as the second consumer runs. It is detected and reported accurately -- 32
simultaneous forward jumps of ~10 s, one per catalogued PID -- but the feed is
genuinely interrupted while it happens. Tee the stream upstream instead.

## Reading the catalogue's extent

`bounds [pid]` reports, per PID, the oldest and newest PCR held, which of them
are still servable, and the spans in ms. Any base between `oldest` and
`oldest_servable` must resolve `TOO_OLD`: still in the ring, past the buffer.

```
$ printf 'bounds 0x0731\n' | nc -U /run/part8-recovery/<instance>/debug.sock
live_ext_seq=22472 resident=6220 oldest_servable_ext=16252
pid 0x0731 count=101 servable=28 oldest=1868451840 newest=1868808345
    oldest_servable=1868712047 history_ms=3961 servable_ms=1070 epoch=0 epochs=1
```

Spans are measured **within the newest epoch only** -- a PCR difference across a
discontinuity is meaningless because the clock restarted -- and `epochs=` says
whether the ring currently spans more than one.

`resolve` accepts either `0x0731` or `1841`.

## Coverage note

The epoch/discontinuity logic was validated against real data on the live
headend: PID 0x03E8 produced a genuine backward jump of -300.5 s which created
epoch 1, and a matching forward jump 99 payloads later. It is the same PID as the
continuity-counter anomaly from C4/C5 -- two interleaved PCR timelines on one
PID, one upstream defect with two symptoms.

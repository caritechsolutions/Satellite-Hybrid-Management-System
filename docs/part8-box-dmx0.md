# Part 8 box — demux 0 fed from the RIST receiver, full time

```
frontend locks the NIM (unchanged)
  -> dmx2 reads the frontend TS -> cutter (0x0022) -> RIST sender -> receiver
  -> dmx0 opened MEMORY-FED (GXMEDIA_SOURCE_TS) from the receiver output
  -> decode, app_epg (SDT/EIT), app_time (TDT) all read the repaired stream
```

Buffer 2000 ms. `/tmp/ristp8tail = dmx0`. No runtime source switch — which
open path runs decides everything.

---

## Not a new implementation

`GxMediaApi_ModuleOpen3()` takes the demux modid as a parameter, so "memory-fed
demux 3" and "memory-fed demux 0" are the Step 2 code path with one number
changed: `_rist_p8_tail_start(int demux_modid)`, latched into `s_rist.p8_dmx` so
setup and teardown can never name different instances. The DVR-0 feed from the
previous commit is gone — it was a second implementation of the same thing, and
two of these would drift.

---

## D1 — open order

### The mechanism that makes most of this a non-problem

`GxAvdev_OpenModule()` is a **refcounted singleton per (type, id)** —
`gxavdev.c:62-86`:

```c
if (-1 == module_handle[type][id])
        module_handle[type][id] = GxAVOpenModule(dev, type, id);
if (module_handle[type][id] >= 0)
        module_opr_count[type][id]++;
return module_handle[type][id];
```

Every consumer that opens demux 0 by instance number gets **the same handle**.
That shared handle is the process-wide demux object the design assumes — it just
lives as an array slot in the avdev layer rather than as a named global. So
configuring demux 0 memory-fed configures it for all of them at once, and our
`GxMediaApi_ModuleClose()` only decrements a count the consumers already hold.

Two consequences, and they pull in opposite directions:

**Slot allocation is order-independent.** Slots and filters sit *downstream* of
the source selector. A consumer that allocated its slot while demux 0 was still
tuner-fed simply follows when the source changes — nothing to re-attach. This is
why "EPG works but decode doesn't" cannot arise from slot ordering.

**Source *writes* are not order-independent — and exactly one consumer writes.**
`app_epg_enable()` reaches `GxEpg_SubtChannelCreate()`, which does an
unconditional

```c
GxDmxSetSource(epg_cfg.dmx_id, epg_cfg.ts_src);   /* gxepg_table.c:123 */
```

on every EPG channel create, with `epg_cfg.dmx_id = TS_2_DMX`, which is **0** on
this build (`app_nim.h:28-32`; `CA_SUPPORT` is 0 and `SUBTTRANS_SUPPORT` is
undefined). Left alone it writes the **tuner** source and silently undoes the
tail. Every other consumer only allocates slots.

### The order, exactly

| # | when | what | why there |
|---|---|---|---|
| 1 | zap | `app_prepare_for_play()` tunes; lock poll runs | demux 0 is still **tuner-fed** — see below |
| 2 | zap | `app_rist_play_change()` spawns sender + receiver | receiver emits nothing yet; it holds a full buffer first |
| 3 | zap | `app_rist_screen_enabled()` → 1 → `app_player_close(PLAYER_FOR_NORMAL)` | releases the one video decoder |
| 4 | +50 ms…delay1 | lock reported → `_rist_capture_begin()` → dmx2 capture feeds 6300 | needs a **tuner-fed demux 0** to report lock |
| 5 | +delay3 (3000 ms) | `_rist_screen_cb` → `_rist_p8_tail_start(0)`: module open, config, start, reader | demux 0 becomes `DEMUX_SDRAM` here |
| 6 | immediately after | `_rist_p8_epg_repoint(3, …)` — `app_epg_disable()` then `app_epg_enable(3, TS_2_DMX)` | the only source-writing consumer, re-pointed at the moment the demux changes hands |
| 7 | ongoing | inject → decode; `app_sdt`/`app_pat`/TDT filters already on demux 0 follow the new source | |

**Why the tail opens at delay3 and not at chain start.** This is the one place
the brief's "born memory-fed at chain start" had to bend, and it is not a
preference:

`GxFrontend_QueryStatus()` (`gxfrontend.c:2352`) asks **the NIM's demux — demux
0** for `GxDemuxPropertyID_TSLockQuery`, and `dvbs_lock_state_get()` ANDs that
with the demod's `FE_READ_STATUS` (`app_nim_dvbs.c:285-301`). The RF tune and the
demod lock genuinely are demux-independent — that part of "the deadlock is
phantom" is right — but the **TS-sync half of the lock check is not**. A
memory-fed demux 0 never reports TS sync, so opening it before the capture
starts would make the lock poll that gates `_rist_capture_begin()` time out on
the `delay1` ceiling on **every** zap. Opening after delay3 leaves that check
running against a tuner-fed demux 0, exactly as today, and still puts the
memory-fed demux 0 in place well before the receiver's first output.

**Why teardown restores the source by hand.** `GxMediaApi_ModuleClose()` releases
our ES slots but leaves the instance on `DEMUX_SDRAM`. That would break the
*next* zap's lock poll on every channel, Part 8 or not.
`_rist_p8_dmx_restore_tuner()` writes the tuner source back, field-for-field in
the shape the NIM uses at `FRONTEND_OPEN` (`app_nim_dvbs.c:405-413`), and then
EPG is re-pointed at the tuner too. The next factory play would eventually
rewrite it anyway, but "eventually, if the right consumer happens to restart" is
not a fallback.

---

## D2 — decode contention

**No special case. All three tails answer the same way.** There is one hardware
video decoder (`vd_hw.c:550`), and on every Part 8 tail something else owns it:

| tail | owner of the decoder |
|---|---|
| `player_av` | the loopback player (Step 1) |
| `dmx3` | our `GxMediaApi` module on demux 3 |
| `dmx0` | our `GxMediaApi` module on demux 0 — same module, same video/audio mod id 0 |

So `app_rist_screen_enabled()` returns true on `chain_active` exactly as it does
for Part 7, `app_play_control.c:1155` calls `app_player_close(PLAYER_FOR_NORMAL)`,
and the memory-fed module takes the decoder that releases. The module's
video/audio/stc mod ids stay **0** on all tails — asking for a second decoder id
would fail, or worse half-succeed.

*(The previous commit inverted this suppression, on the reading that normal play
would carry the picture. That was wrong for this architecture and is reverted.)*

**Fallback is the same single mechanism.** Any chain or tail failure clears
`chain_active` and `p8_active` **before** `app_normal_play` reads the
suppression decision, so suppression lifts in the same zap and factory decode
runs. There is no path that suppresses the tuner decode for a tail that did not
come up.

Three layers, all ending in a picture:

1. chain will not spawn → flags cleared → factory decode, same zap.
2. tail will not open (any stage) → falls straight through to `player_av` on the
   loopback — the Step 1 path, kept selectable as the diagnostic fallback.
3. tail opens but no first frame in 12 s → torn down, source restored, EPG
   re-pointed, screen handed to `player_av` on a 100 ms timer.
4. `echo 0 > /tmp/ristp8` → next zap is factory, untouched.

`app_play_control.c` is back to stock for the `ts_src` decision. `play->url`
keeps the **tuner** source deliberately: normal play is suppressed here so the
URL is unused, and if the tail fails and suppression lifts, a `tsid:3` URL would
aim demux 0 at a buffer nobody writes. `normal_play.ts_src` is set to 3 only
*after* the chain hook, for the SI side — `app_time` reads it for the TDT
subtable (`app_time.c:352` → `gxextra.c:98`).

---

## PSI — the capture carries the broadcast tables, and it is not a mode

### P1, rebuilt — this is not "add SI on top of injected PSI"

The earlier answer was the wrong shape. It described adding SDT/EIT/TDT as extra
PIDs *alongside* the box's injected PAT/PMT. That is not what Part 8 needs.

**`user_pmt = false`, unconditionally, whenever the Part 8 path is on.**
`/tmp/ristp8psi` is retired — a knob there could only ever select "cut different
bytes from the headend", which is never a thing anyone wants.

**Why it cannot be optional.** The sequence numbering has to match the headend,
and the headend cuts the **broadcast** stream: `part8_recovery_server` is fed by
`tsp -P filter --pid …` over a fixed PSI set plus this service's PMT/PCR/ES. If
the box carried its own injected PAT/PMT the two ends would be packetising
*different bytes*, and no sequence number could ever line up — not a tuning
problem, an arithmetic impossibility. The broadcast PSI is part of what gets cut
and numbered.

This is the **opposite** of the Part 7 capture (`user_pmt = true`), which injects
because it has no byte-level counterpart to match. The injected-PSI shape is a
Part 7 artifact.

**And rebuilding the answer against that turned up a real mismatch in what was
already committed.** The previous four-PID set — PAT/SDT/EIT/TDT — was chosen for
*what the consumers need*, which is the wrong question. The headend filters
**seven** (`P8_PSI_PIDS = '0,1,16,17,18,19,20'`,
`rist-monitor/services/part8-service.php:57`):

| PID | table | was in the box's set? |
|---|---|---|
| 0x00 | PAT | yes |
| **0x01** | **CAT** | **no — would have diverged** |
| **0x10** | **NIT** | **no — would have diverged** |
| 0x11 | SDT | yes |
| 0x12 | EIT | yes |
| **0x13** | **RST** | **no — would have diverged** |
| 0x14 | TDT | yes |

The box now captures the same seven, plus the broadcast PMT (slotted by
`user_pmt = false` — `app_ts_record.c:1302-1310`) and the service's PCR/video/
audio. Four would have differed from the server at 0x01/0x10/0x13 wherever this
transponder carries them.

**One capture, both jobs.** The cutter gets bytes it can number identically to
the server; dmx0 gets the SDT/EIT/TDT that `app_epg` and `app_time` read off the
repaired stream. There is no second PSI path and no mode to get wrong.

The slots are real, not asserted: `app_ts_record.c:1326-1350` allocates a
`DEMUX_SLOT_PSI` slot per ext PID with `DMX_REPEAT_MODE|DMX_TSOUT_EN`, PID 0
explicitly allowed. `TS_REC_MAX_EXTPID_NUM` is 32, so seven is not near the limit.

### Two consequences I am naming rather than leaving to be discovered

**The ES half of the alignment is still open.** The PSI half now matches the
headend exactly. The ES half matches only for a service with one video and one
audio: the headend filters *every* stream the service declares, while this
capture slots video + **current** audio + PCR. A second audio track, subtitles or
teletext would be in the server's cut and not in ours. Harmless for Step 1 and
for the dmx0 tail's own decode — it must be closed before the sequence anchor is
trusted.

**The `player_av` tail can now show the wrong service.** It receives the
broadcast PAT, which lists the whole transponder while only this service's PMT
and ES are captured, so a player that picks `program[0]` can land on a service
that was never slotted. That is the one path this change can degrade, and it
logs a NOTE when that tail is selected. **dmx0 and dmx3 are immune** —
`GxMedia_DemuxConfig()` is handed `vidPid`/`audPid`/`pcrPid` explicitly and never
reads a PAT.

### P2 — full schedule EPG, not now/next only

**The earlier framing was wrong and is withdrawn.** The RIST buffer is a **delay
line, not a lossy cache**. Every packet that enters leaves 2000 ms later, in
order. Nothing is evicted for being large or low-rate, sections are never held or
reassembled inside the buffer, and `app_epg` assembles 0x50 off dmx0 exactly as
it always did off the tuner — it was already assembling sections spread over many
seconds. Buffer depth governs how much **recovery time** there is, never which
data survives.

So full schedule EPG is expected to work. This box filters **0x4E** (p/f actual)
and **0x50** (schedule actual) — `gxepg.c:66-76` defaults `epg_day = 3` and
`cur_tp_only = 1`, and `gxepg_table.c:169-192` walks `c_EpgTableId[]` keeping the
even indices.

The probe's per-table-id breakdown stays, but as **instrumentation, not a
predicted limitation** — counting 0x4E and 0x50 separately is what turns "EPG
works" into an answer rather than an impression:

```
p8sec:     EIT breakdown: now/next (0x4E,0x4F) 41 sections, SCHEDULE (0x50-0x5F) 7 sections
```

*(A bug fixed along the way: the probe's EIT filter was `mask 0xF0`, accepting
only 0x40–0x4F, so it would have reported schedule EIT as absent whether or not
it arrived. Widened to `0xE0`.)* If 0x50 is genuinely absent the log now says
plainly that it is **not** a buffer effect and points at the dmx2 slot or the
transponder itself.

### P3 — TDT for the clock

PID 0x0014, table id 0x70, an 8-byte section every few seconds, slotted by the
same seven-PID list. Filtered by `extra_sync_time()` (`gxextra.c:98-120`) with
`crc = CRC_OFF` on `demux_id` 0 (left zero by the `memset`) and
`ts_src = normal_play.ts_src` — which is why that variable is set to 3 after the
chain hook.

**Note:** `extra_sync_time()` filters TDT only, not TOT (0x73, which carries the
local-offset descriptor). The clock is UTC from TDT plus the box's configured
zone — unchanged from today, not a regression introduced here.

---

## Bring-up

### 1. Build and flash

The one command. `install.sh` needs no change for this step — both
`app_rist_capture.c` and `app_play_control.c` are already in `FILES` and in
`deploy.manifest` (regenerated and committed), and the build-stamp relocation
that blocked the last build is in from the previous run. Watch for:

```
  removed the legacy in-tree build stamp (it tripped the guard below)   [first run only]
  OK: all 12 changed file(s) are in FILES
=== cross-build: stb_part8_receiver (ARM) ===
  cutter linked: rist_pcr_cut_feed present in the symbol table
  post-strip check: 'PCR-boundary framing ON' present
=== verify: payloads reached the packed rootfs ===
  OK: stb_part8_receiver in the image carries the cutter
```

### 2. Knobs and zap

```sh
echo 1 > /tmp/ristp8
echo dmx0 > /tmp/ristp8tail
echo 1 > /tmp/ristp8sec      # optional: the per-table SDT/EIT/TDT counts off dmx0
# zap away from NCN and back -- the tail is read per zap
```

There is no PSI knob any more. Broadcast PSI is what the Part 8 capture
takes, always.

### 3. What serial should show, stage by stage

**Gate and chain**

```
[RIST] play_change: chain=ENABLED (default, no /tmp/ristchain)  part8=ON
[RIST] svc_id=2 -> PART 8 video path (tail=dmx0, SI consumers to ts_src=3 on demux 0)
[RIST] p8: tail=dmx0  (demux 0 fed from the repaired stream; consumers stay where they are)
[RIST] p8: ports cap=6300 local=6400 out=6500  buffer=2000ms  pcr_cut=0x0022 (34)  svc_id=2 prog=N
[RIST] chain: started stb_part8_receiver pid=...
[RIST] chain: started ristreceiver pid=...
```

`pcr_cut=0x0022` is the first thing to check. Anything else and the box's PMT
disagrees with the headend's analysis — stop there.

**Frontend lock and dmx2 capture** (unchanged from today — this is the check that
opening the tail late preserves)

```
[RIST] start: frontend LOCKED after Nms
[RIST] udp: dest = 127.0.0.1:6300  (RIST chain input (fixed))
[RIST] p8: BROADCAST PSI capture (user_pmt=false) -- PAT 0x0000 CAT 0x0001 NIT 0x0010 SDT 0x0011 EIT 0x0012 RST 0x0013 TDT 0x0014
[RIST] p8:   + broadcast PMT 0x008E, PCR 0x0022, video 0x0022, audio 0x0023  -- the same bytes the headend cuts
[DVB2IP] ... capture active
```

**Cutter framing**

```
[INFO] [START] PCR-boundary framing ON, pcr_pid=0x0022 (34)
[INFO] [RUN] dgrams=... bytes=... ts_pkts=... pcrs=... pre_pcr_dropped=30 resyncs=0 badsync=0 write_err=0
```

`ts_pkts` and `pcrs` both climbing is the chain working.

**dmx0 memory-fed open, then the consumers**

```
[RIST] screen: tail=dmx0 -- feeding demux 0 from the receiver instead of player_av
[RIST] p8tail: STAGE 1 -- module open on dmx0 (source=TS -> DEMUX_SDRAM)
[RIST] p8tail: STAGE 2 -- configured v=0x0022 a=0x0023 pcr=0x0022 vcodec=0x1 acodec=0x2
[RIST] p8tail: STAGE 3 -- module started
[RIST] p8tail: app_epg re-pointed at ts_src=3 on demux 0 (demux 0 is now memory-fed)
[RIST] p8tail: reading udp://@127.0.0.1:6500 -> dmx0  (first-frame deadline 12000ms)
[RIST] p8tail: STAGE 4 -- first inject ACCEPTED 1316 of 1316 bytes
[RIST] p8tail: T+500ms rx=... inj_ok=... busy=... err=0
[RIST] p8tail: STAGE 6 -- FIRST FRAME at T+...ms (vpts=... stc=...)
```

**Per-table SI, if `/tmp/ristp8sec=1`**

```
[RIST] p8sec: 4/4 filters armed on dmx0 (ts_src=DEMUX_SDRAM). Sections below or nothing.
[RIST] p8sec: *** PAT SECTION on dmx0, 61 bytes: 00 B0 95 00 05 C5 00 00 ***
[RIST] p8sec:   PAT pid 0x0000: 12 sections, 732 bytes  (positive control)
[RIST] p8sec:   SDT pid 0x0011: 4 sections, ...  (app_sdt / service names)
[RIST] p8sec:   EIT pid 0x0012: 48 sections, ... (app_epg (0x4E now/next + 0x50 schedule))
[RIST] p8sec:     EIT breakdown: now/next (0x4E,0x4F) 41 sections, SCHEDULE (0x50-0x5F) 7 sections
[RIST] p8sec:   TDT pid 0x0014: 6 sections, ...  (app_time)
```

**The three consumer answers, from the box not the log**

1. **Decode** — picture on screen, and `STAGE 6 -- FIRST FRAME`.
2. **app_epg** — open the EPG after a minute. Now/next populated is one answer;
   schedule populated is a second. The `EIT breakdown` line above says which
   should be possible.
3. **app_time** — the clock keeps ticking and survives a channel change.

### 4. Reverting

```sh
echo player_av > /tmp/ristp8tail   # Step 1 loopback picture, next zap
echo 0 > /tmp/ristp8               # factory, next zap
```

Either way the teardown logs the restore:

```
[RIST] p8tail: dmx0 module closed
[RIST] p8tail: demux 0 restored to the tuner (source=0)
[RIST] p8tail: app_epg re-pointed at ts_src=0 on demux 0 (tail down -- back on the tuner)
```

If `RESTORE FAILED` ever appears, the next zap's lock poll will time out on the
delay1 ceiling on every channel — that line is the one to grep for if the box
starts feeling slow to zap after a Part 8 session.

---

## Did decode, EPG and the clock all come up? — the honest answer

**I have not run any of it.** No box, and none of this can be exercised
off-hardware: the media API, the demux instance, the decoder and the SI stack are
all SDK and silicon. Nothing above is a claim that demux 0 decodes from memory,
that `app_epg` gets its tables off it, or that the clock follows.

Those are **three separate answers** and the build is instrumented to give three
separate answers rather than one blanket result — a picture with a frozen clock,
or a clock with now/next EPG and no schedule, are all distinguishable outcomes in
the log above and all worth as much as a clean pass.

What is established, and quoted from the tree rather than inferred: the
refcounted-singleton demux handle that makes slot ordering a non-issue
(`gxavdev.c:62-86`); that `app_epg` is the only consumer that writes the source
(`gxepg_table.c:123`) and where its ordering constraint comes from; that the
TS-sync half of the lock check reads the NIM's demux (`gxfrontend.c:2352`), which
is why the tail opens after the capture and why teardown restores by hand; and
that the PSI ext-pids really are slotted (`app_ts_record.c:1326-1350`).

Still not in this step: sync / PCR-sequence anchor, recovery peer, NACK, error
detection, CAS descramble. The box uses its own numbering.

Still open: **G2** — a real box-vs-headend TS diff, which needs an STB capture I
cannot produce.

# Part 8 box, Step 2 — reinjection into a second demux

Step 1 is done and confirmed on live NCN hardware: capture → cutter → local RIST
sender → local RIST receiver → `player_av` → screen. Step 2 replaces the tail
with the production decode path.

**This is an experiment.** Feeding a demux instance from memory and having it run
its own filter+decode chain has never been demonstrated on this box, so the code
is built to report which stage fails and to hand the screen back to `player_av`
rather than go black.

---

## Design

### Which demux, and how the inject is wired

**dmx3**, via `GxMediaApi_ModuleOpen3()` with `mod.demux.demux_modid = 3`.
dmx0/dmx1 are the AV path, dmx2 is our capture, and `TS_REC_DEMUX_MOD_MAX` is 4
— so 3 is the one free instance.

The memory-fed path is not something we assemble; **the SDK wires it**:

- `GxMedia_DemuxOpen()` with `source_type = GXMEDIA_SOURCE_TS` opens a DVR module
  configured `src = DVR_INPUT_MEM`, `dst = DVR_OUTPUT_DMX`
  (`gxmedia_demux.c`).
- `GxMedia_ModuleConfig()` forces `tsid = -1`, which `GxMedia_DemuxConfig()`
  turns into `dmx_config.source = DEMUX_SDRAM`.

So Q1's *mechanism* exists and is exactly the DEMUX_SDRAM path. What is unproven
is whether it decodes here.

`GxMediaApi_ModuleInjectData()` returns the accepted length, **0 when the ES
fifos are above 7/8** (a deliberate back-off in `gxmedia_demux.c`), and <0 on a
real failure. Zero is not an error — treating it as one would turn normal
back-pressure into a torn-down chain — but a run of zeros with nothing decoding
is the signature of "the demux accepted nothing", so it is counted separately.

### B1, corrected: this demux does not lock a service from PSI

`GxMedia_DemuxConfig(mdemux, tsid, AudPid, VidPid, PcrPid)` allocates slots for
exactly those three PIDs. **It never reads a PAT or a PMT.**

Two consequences:

1. **The "player picks program 0 of 34" risk does not exist on this path.** We
   name NCN's PIDs directly. They come from the box's own PMT read — the same
   `prog` record the capture's slot set and the cutter's PCR PID come from, so
   all three cannot disagree.
2. **Broadcast PSI passthrough is not required for dmx3 to decode.** PSI packets
   reaching dmx3 have no slot and are dropped inside the demux — and that is
   true whether they are the broadcast tables (`/tmp/ristp8psi=1`) or the box's
   own injected pair (the default). The tail behaves identically either way.

So `/tmp/ristp8psi` is **not** forced on for this tail. It stays a knob, still
default off. Broadcast PSI remains genuinely required later — for byte-level
parity with the headend, which is what a sequence number indexes — but it is not
what makes Step 2 decode, and turning it on by default would have added the
one-of-34-services risk to an experiment that does not need it.

If you want it anyway, it is one echo: `echo 1 > /tmp/ristp8psi`.

### B3, decoder contention: already correct, nothing new

`app_rist_screen_enabled()` is already true whenever `chain_active` is set, and
`app_play_control.c` already responds by calling
`app_player_close(PLAYER_FOR_NORMAL)` — which is exactly what releases the single
video decoder. The dmx3 module then opens video/audio module **0**, the same
hardware `player_av` would have taken.

That is also why the module's video/audio/stc mod ids stay 0 while only the
demux id changes: there is one hardware video decoder and one audio decoder on
this chip. Asking for a second id would fail, or worse, half-succeed.

The tail is torn down inside `_rist_screen_stop()`, so it is released at every
point `player_av` would have been — otherwise the next zap's owner would find the
decoder taken.

### The tail knob

| `/tmp/ristp8` | `/tmp/ristp8tail` | result |
|---|---|---|
| absent / 0 | — | factory decode, untouched |
| 1 | absent or `player_av` | **Step 1 path** (proven) |
| 1 | `dmx3` | **Step 2 experiment** |

Read at the moment the tail starts, not at chain start, so it takes effect on
the zap it is set for.

**Every dmx3 failure falls through to `player_av` in the same zap.** Open,
config, start, socket and thread failures fall through immediately; a start that
succeeds but produces no frame within 12 s is torn down and hands the screen to
`player_av` on a 100 ms timer.

---

## Stage instrumentation

The point of Step 2 is to learn, so each stage announces itself and a stall names
itself.

| stage | log line |
|---|---|
| 1 | `p8tail: STAGE 1 -- module open on dmx3 (source=TS -> DEMUX_SDRAM)` |
| 2 | `p8tail: STAGE 2 -- configured v=0x0022 a=0x0023 pcr=0x0022 vcodec=... acodec=...` |
| 3 | `p8tail: STAGE 3 -- module started` |
| 4 | `p8tail: STAGE 4 -- first inject ACCEPTED n of m bytes` |
| 5 | every 500 ms: `fifo ts_used=… vid_used=… aud_used=…` and `dec video state=… err=…` |
| 6 | `p8tail: STAGE 6 -- FIRST FRAME at T+…ms (vpts=… stc=…)` |

On the deadline it says which stage failed, from the counters rather than by
guessing:

```
p8tail: NO FIRST FRAME after 12000ms -- REINJECTION DID NOT DECODE.
p8tail:   rx=… bytes, injected=…, busy=…, err=…
p8tail:   STAGE 3 FAILED: nothing arrived from the receiver
       or STAGE 4 FAILED: the demux accepted no bytes
       or STAGE 5/6 FAILED: bytes went in, no frames came out
p8tail: falling back to player_av (Step 1 path) for this zap
```

A moving `vpts` is the only unambiguous "the decoder consumed a frame" signal
available here; the fifo levels say whether the bytes got that far.

---

## The build-stamp bug that blocked the last build

`install.sh` wrote its librist build stamp to
`$riststb_dir/librist/.build-arm.commit` — **inside the checkout**, and not
covered by `librist/.gitignore` (which ignores `build*`, so `build-arm/` is fine
and the dotfile is not). The next run saw `?? librist/.build-arm.commit` in
`git status --porcelain` and refused to build, blaming the operator for a file it
had created itself.

Reproduced and fixed:

```
OLD behaviour: stamp inside the checkout
  run 2 (stamp from run 1 present):
  GUARD DIED: ?? librist/.build-arm.commit

NEW behaviour: legacy stamp removed, new stamp outside
  removed the legacy in-tree stamp
  guard passed
  run 3 (stamp from run 2 present):
  guard passed
  build-arm/ still ignored: yes
```

The stamp now lives in `$RIST_TREES/.build-stamps/`, keyed by which checkout it
describes. A legacy in-tree stamp is deleted on sight so the first run after this
fix heals itself, and the guard's own error text now names the bug in case an old
one turns up.

---

## A latent bug found while adding state

`s_rist`'s positional initialiser had drifted out of step with the struct. Its
three `-1` values were landing on `chain_active`, `chain_running` and
`p8_active` rather than the descriptors they were written for — leaving
`udp_fd` initialised to **0**, so a teardown before any capture had opened would
have run `close(0)` on stdin.

Nothing had tripped it, because every path reaching the teardown opens the socket
first. Adding fourteen fields in the middle is how a latent one becomes live, so
it is now a designated initialiser.

---

## Bring-up

### 1. Build and flash

Same one command. New in the log:

```
  removed the legacy in-tree build stamp (it tripped the guard below)   [first run only]
=== cross-build: stb_part8_receiver (ARM) ===
  cutter linked: rist_pcr_cut_feed present in the symbol table
  post-strip check: 'PCR-boundary framing ON' present
=== verify: payloads reached the packed rootfs ===
  OK: stb_part8_receiver in the image carries the cutter
```

### 2. Run Step 1 first, to confirm nothing regressed

```sh
echo 1 > /tmp/ristp8
rm -f /tmp/ristp8tail          # or: echo player_av > /tmp/ristp8tail
```

Zap to NCN. Expect the Step 1 picture and:

```
[RIST] p8: tail=player_av  (STEP 1 path, proven on hardware; echo dmx3 > /tmp/ristp8tail)
```

### 3. Switch the tail and zap again

```sh
echo dmx3 > /tmp/ristp8tail
```

Zap away and back. Expect:

```
[RIST] p8: tail=dmx3  (STEP 2 experiment: reinject into a memory-fed demux)
[RIST] screen: tail=dmx3 -- reinjecting into demux 3 instead of player_av
[RIST] p8tail: STAGE 1 -- module open on dmx3 (source=TS -> DEMUX_SDRAM)
[RIST] p8tail: STAGE 2 -- configured v=0x0022 a=0x0023 pcr=0x0022 vcodec=0x1 acodec=0x2
[RIST] p8tail: STAGE 3 -- module started
[RIST] p8tail: reading udp://@127.0.0.1:6500 -> dmx3  (first-frame deadline 12000ms)
[RIST] p8tail: STAGE 4 -- first inject ACCEPTED 1316 of 1316 bytes
[RIST] p8tail: T+500ms rx=… inj_ok=… busy=… err=0
[RIST] p8tail:   fifo ts_used=… vid_used=… aud_used=…  apts=… vpts=…
[RIST] p8tail:   dec video state=… err=…
[RIST] p8tail: STAGE 6 -- FIRST FRAME at T+…ms
```

### 4. Reverting

```sh
echo player_av > /tmp/ristp8tail   # back to Step 1, next zap
echo 0 > /tmp/ristp8               # back to factory decode, next zap
```

---

## Did reinjection decode? — the honest answer

**Unknown. I have not run it.** I have no access to the box, and none of this
step can be exercised off-hardware: the media API, the demux instance and the
decoder are all SDK/hardware. Nothing here should be read as a claim that
reinjection works.

What I can state:

- **The mechanism is real and is the SDK's own**, not something inferred:
  `GXMEDIA_SOURCE_TS` → `DVR_INPUT_MEM`/`DVR_OUTPUT_DMX` → `DEMUX_SDRAM`, quoted
  above from `gxmedia_demux.c`. That answers "does the API exist and is it the
  memory-fed path" with yes.
- **The reference usage exists in the SDK**
  (`vod_source/vod_trans/hwts/vod_trans_hwts.c`) — open, StcConfig 45000/2,
  DemuxConfig with explicit PIDs, DemuxStart, then inject per buffer. This code
  follows that sequence through the `Module*` wrapper, which bundles the same
  calls plus the video/audio bind.
- **Codec and PID mapping is derived, not guessed**: `GXBUS_PM_PROG_*` →
  `VIDEO_CODEC_*` and `GXBUS_PM_AUDIO_*` → `AUDIO_CODEC_*`, the latter mirroring
  `app_audio.c`'s existing `app_audio_type()`.
- **The failure path is the proven path.** Whatever happens, the worst outcome
  is a log line saying which stage stalled and a `player_av` picture.

The build is a question, not an answer. The serial log at step 3 above is what
answers it.

---

## Not in this step

No sync / PCR→sequence anchor. No recovery peer, NACK or FSR. No error
detection. No CAS — NCN is FTA and dmx3 runs clear (`GXMEDIA_OUTPUT_NORMAL`).

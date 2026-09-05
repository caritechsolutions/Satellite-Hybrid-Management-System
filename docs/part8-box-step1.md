# Part 8 box receiver, Step 1 — the video path

Prove the box can capture NCN, cut it on PCR into RTP, run it through a local
sender→receiver pair and decode it on its own screen. No recovery, no NACK, no
sync. The Part 8 analogue of the Part 7 STEP C loopback.

---

## Design

### Where it sits

`stb_part8_receiver` is a new binary, spawned instead of the Part 7 pair, never
alongside it. One capture and one `player_av` cannot serve two chains, so the
Part 8 gate short-circuits the Part 7 arm in `app_rist_play_change()` rather
than sitting beside it.

```
dmx2 capture --UDP 6300--> stb_part8_receiver --RIST 6400--> ristreceiver
             --UDP 6500--> player_av
```

All three hops are `127.0.0.1`. Not a preference: the headend's recovery server
spent a session reading zero bytes off a multicast group it had bound but never
joined, on a bridge that was NO-CARRIER anyway. Every local hop on this box is
unicast loopback.

### Ports

| | Part 7 | Part 8 |
|---|---|---|
| capture → sender (UDP) | 6000 | **6300** |
| sender → receiver (RIST) | 6100 | **6400** |
| receiver → player_av (UDP) | 6200 | **6500** |

Separate hundreds. A shared capture port would mean two senders reading one
stream; a shared out port, two writers into `player_av`. Both present as "the
picture is wrong" rather than as a port collision. Two accessors
(`_rist_cap_port()`, `_rist_out_port()`) pick per mode so no call site can get
half of it right.

### The knob

`/tmp/ristp8`, **default OFF** — unlike `/tmp/ristchain`, which defaults ON. A
box that has never heard of Part 8 boots to exactly what it does today.

**Step 1 takes no API record.** There is no recovery peer to look up, so
requiring one would make a bench test depend on the headend for a step that does
not involve the headend. The PCR PID comes from the tuned service's own PMT
(`GxBus_PmProgGetById` → `prog.pcr_pid`) — the same record the capture's slot
set is built from, so the slot set and the cutter cannot disagree about it, and
the same rule the headend now uses after scanning handed an NCN channel
TLC-GUYANA's PCR PID.

Failure falls through to factory decode, never a black screen: the spawn is
synchronous, and a failure clears **both** `chain_active` and `p8_active` before
`app_normal_play` reads the suppression decision. Clearing only the first would
leave the capture aimed at 6300 and `player_av` pointed at a port nothing is
writing.

### The cut is not reimplemented

`librist/src/pcr_cut.c` — the same file the headend's `part8_recovery_server`
uses — is compiled into `stb_part8_receiver`. It is not in `librist.so`
(`RIST_PRIV` hides its symbols, which is why `ristsender` compiles it in too),
so it is listed as a second translation unit. The PID arrives as
`?pcr_cut=<pid>` on the input URL and is parsed by librist's own
`rist_parse_udp_address2()`, exactly as `ristsender` does it.

### Not watchdogged

The Part 7 sender runs under `rist_watchdog` because it can legitimately sit
waiting for a first marker that may never arrive. The Part 8 sender has no such
state — it binds, cuts, emits — so a restart loop would hide a real failure
rather than survive a transient.

### Shutdown

Same three-part guard as the headend's recovery server, for the same reason: a
zap restarts this process, and one that will not die holds udp/6300 so the next
start binds nothing and shows a black screen. `sigaction` without `SA_RESTART`;
a second signal `_exit()`s; the first arms `alarm(5)` whose handler `_exit()`s
whatever cleanup is doing.

---

## One deviation from the brief, and why

**B1 asked for `user_pmt` OFF so the broadcast PSI passes through. That is not
the default here. It is a knob, `/tmp/ristp8psi`, default 0.**

Broadcast PSI is genuinely required later — the headend filters it through
untouched, so the box must hold the same bytes or the two streams differ at
exactly the PIDs a sequence number indexes. But Step 1 has no server to match:
the brief itself says the box just uses its own numbering.

Two things argue for keeping it a knob in this step:

1. **Step 1's success condition is a picture**, and the self-contained PAT/PMT
   (`user_pmt = true`) is the shape already proven to decode on this box by the
   Part 7 loopback.
2. **The broadcast PAT lists every service on the transponder**, while only this
   service's PMT and ES are captured. A player that picks the first program in
   the PAT can land on one whose PMT was never slotted — a black screen for a
   reason that has nothing to do with the cutter, on the one step meant to
   isolate the cutter.

Both modes ship in the same flash, so testing the PSI path costs a re-zap rather
than a re-flash:

```sh
echo 1 > /tmp/ristp8psi     # then zap away and back
```

That mode needed one real change: `app_ts_record.c`'s ext-PID loop rejected PID
0, so the PAT could never be slotted. The blanket `VALID_PID()` rejection moved
out of the shared allocator (which every other caller — video, audio, PCR, PMT —
still applies at its own call site) into the loop that needs it. PID 0 *is*
slottable: the demux delivered 68 real broadcast PAT packets when an earlier
experiment left `.pid = 0` on a slot by accident.

---

## What is NOT wired — do not test for it

- **No recovery peer.** The receiver has ONE peer. No weight-1000, no
  `timing-mode=1` (that exists to reconcile two senders' clocks onto one flow;
  here there is one sender).
- **No NACK, no FSR, no retransmission.** A packet lost on loopback is lost.
- **No sync-to-server.** The anchor query (`?pcr=` on the recovery API) exists
  on the headend and the box does not call it. Box and headend sequence
  numbering are unrelated in Step 1 and are *supposed* to be.
- **No error detection.** No TEI, no CC checking.
- **No CAS/descramble.** NCN is FTA.
- **No `part8_rist_url` involvement.** The API's Part 8 fields are read by the
  existing Part 7 chain path, not by this one. `/tmp/ristp8` is the only switch.

---

## Bring-up

### 1. Build and flash

On the toolchain VM, the usual one command. `RISTSTB_REF` must point at the
riststb branch carrying `librist/src/pcr_cut.c` — it defaults to the same branch
name as the pacman `REF`, which is correct today.

Watch for, in order:

```
=== cross-build: stb_part8_receiver (ARM) ===
  cutter : /opt/stb/rist/riststb/librist/src/pcr_cut.c
  compiled OK
  cutter linked: rist_pcr_cut_feed present in the symbol table
  post-strip check: 'PCR-boundary framing ON' present
...
=== verify: payloads reached the packed rootfs ===
  OK: usr/bin/stb_part7_receiver (...)
  OK: usr/bin/stb_part8_receiver (...)
  OK: lib/librist.so.4.5.0 (...)
  OK: stb_part8_receiver in the image carries the cutter
```

The build **dies** rather than warns if `pcr_cut.c` is not in the riststb
checkout, if the symbol is missing, or if the string is absent from the packed
copy. A Part 8 binary that reached the image without its cut rule would bind,
forward and never cut — which reads as a decode problem three steps later.

On the box itself, the same check:

```sh
strings /usr/bin/stb_part8_receiver | grep 'PCR-boundary framing'
```

### 2. Turn it on and zap

```sh
echo 1 > /tmp/ristp8
# zap away from NCN and back (the gate is read per zap)
```

### 3. What the serial console should show

**The gate, at the zap:**

```
[RIST] play_change: chain=ENABLED (default, no /tmp/ristchain)  part8=ON
[RIST] svc_id=2 -> PART 8 video path (Step 1, no recovery peer)
```

**The chain, immediately after:**

```
[RIST] p8: ports cap=6300 local=6400 out=6500  buffer=4000ms  pcr_cut=0x0022 (34)  svc_id=2 prog=N
[RIST] chain: started stb_part8_receiver pid=...
[RIST] chain: started ristreceiver pid=...
```

`pcr_cut=0x0022` is the value to check. If it reads anything else, the box's PMT
says something different from what the headend's analysis says and the two ends
will never align — stop there.

**The sender, once the capture starts feeding:**

```
[INFO] [IN] bound 127.0.0.1:6300  rcvbuf asked 1048576 got <n>
[INFO] [START] PCR-boundary framing ON, pcr_pid=0x0022 (34)
[INFO] [START] in=udp://@127.0.0.1:6300?pcr_cut=34 out=rist://@127.0.0.1:6400?buffer=4000
```

and every 5 s:

```
[INFO] [RUN] dgrams=... bytes=... ts_pkts=... pcrs=... pre_pcr_dropped=30 resyncs=0 badsync=0 write_err=0
```

`ts_pkts` and `pcrs` both climbing is the chain working. `ts_pkts` climbing with
`pcrs` stuck at 0 means the capture is feeding but nothing carries a PCR on
0x0022 — check the slot set. `dgrams` stuck at 0 means the capture is not
reaching 6300.

**The capture:**

```
[RIST] udp: dest = 127.0.0.1:6300  (RIST chain input (fixed))
[DVB2IP] ... capture active
```

**First frame:** `player_av` is started `delay3` ms after the chain (default
3000, `/tmp/ristdelay3`), pointed at `udp://@:6500`.

```
[RIST] screen: player_av in 3000ms (delay3/chain) on udp://@:6500
```

Picture on screen is the pass condition.

### 4. Reverting

```sh
echo 0 > /tmp/ristp8      # or: rm /tmp/ristp8
```

and zap. Nothing else changes: the Part 7 chain, its binaries, its ports and its
API path are untouched by this step.

---

## What was verified here, and what was not

**Verified natively** (x86, the same sources, not the box):

```
capture (UDP 1316-byte chunks) -> stb_part8_receiver -> ristreceiver -> UDP out

  cutter ON  (?pcr_cut=34): 4692 packets out, BYTE-IDENTICAL to the source
                            from the first PCR onward
  cutter OFF              : 4725 packets out, BYTE-IDENTICAL
```

The 33-packet difference is the 30 packets before the first PCR, which the
cutter discards by design, plus the 3 still pending in its buffer at
end-of-stream. On a live stream that tail is latency, not loss.

That is B2 and B3 proven end to end: the cut-and-reassemble chain reproduces the
input exactly.

**Not verified here**, because it needs the hardware: the dmx2 capture and its
slot set (B1), the spawn path, `player_av` decode, and the broadcast-PSI mode.
The native test feeds a file where the box will feed its demux.

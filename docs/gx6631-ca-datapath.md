# GX6631 CA datapath — where descrambling happens, and what it means for repair

Code reading with citations. No design content. Answers C1–C6.

Method as before: the demux property-types header is absent, so enums come from
DWARF in `platform/gxbus/_build-arm-ecos/libgxdvb.a` and flag macro values from
disassembling `player/libgxplayer.a` against its DWARF line table.

---

## 0. The premise is live, and now measured

The downlink capture — what the recovery server holds — is scrambled:

```
packets with transport_scrambling_control != 0 : 109,849
  PID 0x08AD  sc=2  4546 of 4546      PID 0x0731  sc=2  3931 of 4588
  PID 0x0907  sc=2  4525 of 4525      PID 0x082B  sc=2  3885 of 4553
  PID 0x08FD  sc=2  4497 of 4497      ... 30+ more ES PIDs, all sc=2
```

`sc=2` is "scrambled, even key". Every elementary stream on the transponder is
scrambled, **including 0x0731**, the video PID used in the TEI work. The PIDs
showing a mix (3931 of 4588) are those whose remaining packets are
adaptation-field-only, which is never scrambled.

So the mismatch is not hypothetical: the server holds `sc=2` payload, and the
question is only what the box holds.

## C1 — Descrambling is applied on the way INTO the slot's output buffer

The strongest evidence is that the SDK implements "record encrypted vs record in
the clear" using exactly this flag on exactly this path.
`platform/gxbus/player/demuxer/hw_demux.c:944-951`:

```c
aslotflags = (DMX_REPEAT_MODE|DMX_PTS_TO_SDRAM|DMX_TSOUT_EN);
vslotflags = (DMX_REPEAT_MODE|DMX_PTS_TO_SDRAM|DMX_TSOUT_EN);

if (conf->ts_info.ext_info.is_encrypt == 0) {
    vslotflags |= (DMX_DES_EN);
    aslotflags |= (DMX_DES_EN);
}
```

and identically for extra PIDs at `:715`, `:795`, `:821`:

```c
encrypt |= (conf->ts_info.ext_info.is_encrypt) ? 0 : DMX_DES_EN;
```

Confirmed in the shipped object — `.text.GxHwDemux_Config`:

```
0x180  ldr    r7, [r5, #0x6c]     ; is_encrypt
0x184  cmp    r7, #0
0x188  movne  r4, #0x1a           ; encrypted recording  -> no DES_EN
0x194  moveq  r4, #0x5a           ; clear recording      -> DES_EN
0x19c  orrne  r4, r4, #0x2000     ; bind_descr -> DMX_DES_AUTO_BIND
```

**A recording is a TSOUT capture into memory — the same thing our capture does.**
If descrambling happened downstream of the slot write, on the way to the
decoder, then a TSOUT recording could never be clear and the `is_encrypt == 0`
branch would be dead code implementing a shipped feature. It is not dead.

Three corroborations:

1. **The decode path never sets `DES_EN` at all.** `hw_demux.c:533-538` uses
   `DMX_AVOUT_EN` and adds only `DMX_DES_AUTO_BIND`. `DES_EN` is specific to the
   TSOUT path; if descrambling were a common downstream stage there would be no
   reason for a TSOUT-only control.
2. **The API contract says so.** `app_descrambler_api.c:198-204`, on
   `GxDescrmb_SetStreamPID`: *"tell descrambler that which pid channel need to
   bind. the pid will used to find the id of slot, then descrambler can
   descramble the stream."* Descrambling is a property of a slot.
3. The debug line at `hw_demux.c:110-120` prints `TS_EN / AV_EN / DES_EN /
   AUTO_BIND` per slot at config time, i.e. all four are slot attributes.

**Answer: a slot with `DES_EN` set delivers CLEAR bytes to our capture.** With
keys loaded, today's capture would hold descrambled payload while the server
holds `sc=2` — which is the breakage you predicted.

**Honest limit.** This is proof of the API contract and of vendor intent, not a
read of the driver's datapath ordering; the demux driver is not in the tree and
`GxDemuxPropertyID_*` handling lives inside `libgxav`. For the question at hand
the contract governs, because the driver is what implements it. C6 is how to
close the remaining gap empirically.

## C2 — `DMX_DES_EN = 0x40`, and the complete flag map

Recovered by disassembly and cross-validated three ways (record path 0x1a/0x5a,
decode path 0x2a/0xaa, `dvbsource_tsbuff.c` 0x13/0x93):

| flag | value | derivation |
|---|---|---|
| `DMX_CRC_DISABLE` | `0x01` | only bit unique to tsbuff's 0x13 |
| `DMX_REPEAT_MODE` | `0x02` | present in decode 0x2a, which has no TSOUT |
| `DMX_PTS_TO_SDRAM` | `0x08` | only bit unique to record base 0x1a |
| `DMX_TSOUT_EN` | `0x10` | in 0x13 and 0x1a, absent from 0x2a |
| `DMX_AVOUT_EN` | `0x20` | decode base 0x2a minus `REPEAT\|PTS` |
| **`DMX_DES_EN`** | **`0x40`** | **0x5a − 0x1a at `GxHwDemux_Config+0x194`** |
| `DMX_ERR_DISCARD_EN` | `0x80` | 0x93 − 0x13, and decode 0xaa − 0x2a |
| `DMX_DES_AUTO_BIND` | `0x2000` | `orrne r4, r4, #0x2000` at `+0x19c` |

Every value reproduces all three observed constants exactly.

Our capture slots (`app_ts_record.c:1679-1680`) are therefore:

```
video  REPEAT|PTS_TO_SDRAM|TSOUT_EN|DES_EN            = 0x5a
audio  ... |ERR_DISCARD_EN                            = 0xda
```

**Clearing `DES_EN` gives `0x1a` / `0x9a`** — bit-identical to the SDK's own
"encrypted recording" flag set, which is reassuring: it is a configuration the
vendor ships, not one we are inventing.

**What clearing it affects.** Nothing outside our own slots, with one caveat:

- **Decode is unaffected.** The decode path (`dmx0`/`dmx1`, `AVOUT_EN`) is a
  different demux instance with its own slot table, and never sets `DES_EN`.
- **The CA client is unaffected.** Keys are delivered by
  `GxDescrmb_SetOddKey`/`SetEvenKey`/`SetCW` against a descrambler handle opened
  on a demux id; clearing a slot flag on a different instance does not touch it.
- **Caveat — other slots on the same PID within our instance.** Flags
  **accumulate**: `_ts_rec_demux_slot_alloc()` does `slot.flags = slot_flags` on
  first allocation (`app_ts_record.c:1017`) and `slot.flags |= slot_flags` on
  reuse (`:1026`). `hw_demux.c:316-321` does the same. So if any other allocation
  of the same PID in the same instance asks for `DES_EN`, it is OR-ed back in and
  clearing it at one site is silently undone. In the capture path today nothing
  else requests `DES_EN`, but this is the failure mode to watch for.

## C3 — The CA binding, end to end

| step | file : function | what happens |
|---|---|---|
| 1. ECM filtered | `platform/gxbus/si/si_filter.c` / CAS section filters | ECM sections delivered to the CA client |
| 2. CA client derives CW | `platform/gxcas/cas/…` (per-CAS), e.g. `cryptoguard_secure.c:61-71` | opens a descrambler: `GxDescOpen(GXDESC_MOD_TS, 0, GXDESC_ALG_CAS_CSA2)` |
| 3. descrambler opened | `app_descrambler_api.c:151-182` `GxDescrmb_Open(DemuxID)` | `GxDescOpen(GXDESC_MOD_TS, DemuxID, …)` then `GxDescConfig(…, GXDESC_KLM_NONE, 0)` |
| 4. bound to a PID | `app_descrambler_api.c:204-212` `GxDescrmb_SetStreamPID` | records the PID against the handle |
| 5. key set | `app_descrambler_api.c:223 / 254 / 289` `SetOddKey` / `SetEvenKey` / `SetCW` | CW handed to the descrambler for that PID |
| 6. **key becomes usable** | `platform/gxcas/module/gxcas_descrambler.c:222-231` | `SlotQueryByPid(pid) -> slot.slot_id`; `ca.slot_id = slot.slot_id`; `ca.flags = DMX_CA_KEY_ODD\|DMX_CA_KEY_EVEN`; `GxDemuxPropertyID_CAConfig` |
| 7. packet descrambled | inside the demux, on the slot | per C1, before the slot's buffer is written |

**The key becomes usable at step 6**, and only for the slot that
`SlotQueryByPid` resolves — so a key is scoped to (demux instance, PID).

### Can one PID be on two slots with different `DES_EN`?

**Within one demux instance: no.** Slots are keyed by PID and the lookup is
per-instance — `find_slot_in_module(index, slotpid)` scans
`hw_demux.module[index]->slot[i].slot.pid` (`hw_demux.c:144-156`). On a hit the
flags are OR-ed into the existing slot rather than a second slot being made
(`:316-321`). `SlotQueryByPid` has the same one-answer shape, which is why step 6
can assume a single slot id.

**Across instances: yes — and the box already does it.** `dmx0`/`dmx1` run the AV
path (`app_demux_api.c:202-203`) while `dmx2` runs our capture
(`app_ts_record.c:155`), and our instance is configured onto the *same* physical
frontend TS:

```c
demux.source    = ts_src;      /* APP_FRONTEND_DEMUX_TS_SRC[tuner] */
demux.ts_select = FRONTEND;
demux.stream_mode = DEMUX_PARALLEL;         /* _ts_rec_demux_module_config, :629-631 */
```

Multiple demux instances fan out from one TS input. **So "one slot clear for
decode, one scrambled for capture" is available today by construction** — the
two consumers are already separate instances, and only the `DES_EN` bit on ours
differs. That resolves the scrambled/clear mismatch with no splice-time
descrambling, which is the answer to the question you asked.

> **Incidental defect, our own code.** `_ts_rec_demux_module_config()` declares
> `GxDemuxProperty_ConfigDemux demux;` uninitialised and assigns only seven of
> its eight fields — **`demux.flags` is never set** before the struct is passed
> to `GxDemuxPropertyID_Config` (`app_ts_record.c:610-637`). Stack garbage
> reaches the driver. Every other site in the tree either uses a designated
> initialiser or `= {0}`. One-line fix; unrelated to CA but found while reading.

## C4 — `dmx1/dmx2` vs `dmx3/dmx4`: two different mechanisms, and the distinction matters

Your observation is real, but it is **not** about descrambling. There are two
independent things on this chip that both yield unreadable bytes:

**(i) CSA descrambling** — `DES_EN`, `CAConfig`, broadcast conditional access.
This is **usage, not configuration.** Nothing in `GxDemuxProperty_ConfigDemux`
carries a CA or clear/encrypted attribute; the full struct is
`{sync_lock_gate, sync_loss_gate, time_gate, byt_cnt_err_gate, stream_mode,
ts_select, source, flags}` and none of those is CA-related. An instance is
"descrambling" only because something bound a descrambler to a slot on it.
**Every descrambler open in the tree targets demux id 0**:
`cryptoguard_secure.c:69` hardcodes `GxDescOpen(GXDESC_MOD_TS, 0, …)`, and the
sample at `solution/demo/descramber/sample.c:116-117` uses `GxDescrmb_Open(0)`.

**(ii) TSW at-rest protection** — the chip encrypting its own TS-write buffer in
DDR so the CPU cannot read premium content. This **is** configured, is
OTP-fused, and is per-region: `GXMEM_FLAG_DEMUX_TSW` /
`GXMEM_FLAG_DEMUX_TSR` set from `gx_otp_query_flag(OTP_FLAG_TS_BUF_PROTECT)`
(`gxloader/common/meminfo.c:38-40`), applied by `firewall_config_filter()` over
`CMDLINE_TSWMEM_START` / `CMDLINE_TSRMEM_START`
(`gxloader/user/canopus_config.c:31-44`), and read back at runtime via
`GxAvdev_GetFirewallFlag()` (`memhole.c:259`).

**This second mechanism is what your observation is about**, and our own tree
says so: `app_ts_record.c:150-155` exists precisely "to test whether the TSW
at-rest protection is per-instance or global", and defaults
`s_ts_rec_modid = 2` with the comment *"default to the UNPROTECTED instance
(clear TS)"*. That is the dmx3/dmx4-are-clear finding, recorded at the time.

So: **refuted as stated, confirmed as a different thing.** dmx0/dmx1 are not
"the encrypted paths" because of how they are configured for CA — they are where
CA happens to be bound. dmx2/dmx3 are readable because of the TSW firewall, not
because of CSA.

### What each instance is doing today

| instance | role | CA bound? | citation |
|---|---|---|---|
| dmx0 | AV decode path | **yes** — every `GxDescOpen` in the tree uses id 0 | `app_demux_api.c:202`, `cryptoguard_secure.c:69` |
| dmx1 | AV path, second module | no open targets it | `app_demux_api.c:203` |
| dmx2 | **our RIST capture**, `source = frontend ts_src`, slots `0x5a`/`0xda` | no | `app_ts_record.c:155`, `:629-631`, `:1679-1680` |
| dmx3 | unused; `TS_REC_DEMUX_MOD_MAX = 4` admits it | no | `app_ts_record.c:14` |

The "DEMUX 3" in the `#if CA_SUPPORT` block at `app_top.c:614` is a TODO comment
about PSI/CAT PIDs, not a configured instance — the code under it calls
`app_exshift_pid_add()`, the timeshift extra-PID path.

## C5 — What the code supports, and what each costs

### (a) Clear `DES_EN` on the capture slot — box holds scrambled TS

**Supported, and it is a one-bit change**: `0x5a → 0x1a`, `0xda → 0x9a` at
`app_ts_record.c:1679-1680`. It produces exactly the SDK's own "encrypted
recording" flag set. Server and box then both hold `sc=2` bytes and a spliced
repair is byte-compatible.

**Cost — and this is the part that is not free.** It requires a descrambling
stage *after* our code, and **the current chain has none.** The chain is
`capture(dmx2) → udp:6000 → receiver → udp:6200 → player_av`
(`app_rist_capture.c:107-112`). `player_av` is a software player consuming clear
TS; hand it `sc=2` payload with CAS on and it decodes garbage. So (a) is
necessary but not sufficient on its own — it needs the repaired stream to reach
something that descrambles, which is (c). Watch the flag-accumulation caveat in
C2.

### (b) Descramble the repair on the box before splicing

**Partly supported, with a real caveat.** Whether CWs are reachable in plaintext
depends on the key-ladder mode, and on this chip the default is **no ladder**:
`_GetKLMType()` returns `GXDESC_KLM_NONE` for `CANOPUS` unless `SCPU_USED` or
`SECURE_ACPU_SUPPORT`/`SECURE_FULL_SUPPORT` is defined
(`app_descrambler_api.c:524-541`), and `GxDescrmb_Open` then calls
`GxDescConfig(handle, GXDESC_ALG_CAS_CSA2, GXDESC_KLM_NONE, 0)` (`:174`). With
no ladder the CAS passes control words in the clear through
`GxDescrmb_SetCW(handle, OddKey, EvenKey, len)` (`:289`), an app-layer call.

**Cost.** Two things, both significant:

1. **There is no "descramble this buffer" API.** Every descrambling primitive is
   bound to a slot (C3 step 6). Doing it in our process means implementing CSA2
   in software on the A7 for the repair bytes. Feasible at repair volumes, not at
   stream volumes.
2. **It puts plaintext control words inside our process.** That is a security
   posture change and an operator decision, not an engineering one. If the
   deployed CAS is ever built with `SCPU_USED` or a secure ACPU, the ladder turns
   on, `SetECW` replaces `SetCW`, and this option disappears without warning.

### (c) Re-inject and let the instance's CA binding descramble

**Supported in shape, but two things must be true and one of them is still
open.**

- The injection mechanism exists and is proven: `DVR_INPUT_MEM` →
  `DVR_OUTPUT_DMX` + `GxAVModuleWrite`, demux `source = DEMUX_SDRAM`
  (`gxfrontend_net.c:118-143`, `:334`), and the two-instance topology is how
  timeshift already works (`dvbsource_tscache.c:425-438`).
- **Open: does a `DEMUX_SDRAM`-sourced instance descramble?** Nothing in the tree
  binds a descrambler to one. Timeshift is the closest analogue and deliberately
  sets no `DES_EN`, because it stores content already descrambled on the way in.
  This is R4(c), still unanswered.
- **CA is bound to instance 0, which is the live AV path.** Descrambling our
  injected stream means either binding a descrambler to the injection instance
  (untested per the previous point) or making instance 0 the injection target —
  which requires switching its `source` to `DEMUX_SDRAM` and so removes the live
  frontend feed that everything else decodes from.

**Given C1, (c)'s prospects are better than they look**: if descrambling happens
on the way into a slot's buffer, it is a slot-level operation and there is no
structural reason it should care whether the instance's `source` is a frontend
or SDRAM. But "no structural reason" is not evidence, and this one needs the
test.

## C6 — The definitive test

### Does `CA_SUPPORT=0` invalidate it? Yes — for the core question.

With no keys loaded, no `CAConfig` is ever issued, so `DES_EN` has nothing to
act on and is inert. **Capturing with `DES_EN` on and off would produce
byte-identical output today**, which would look like "the flag does nothing" and
would be a false negative. The DES_EN comparison **cannot be run until CAS is
enabled and a service is authorised.**

That is worth stating plainly because it is the trap: the test appears to pass
trivially and the wrong conclusion is the comfortable one.

### What you CAN run today, and it is worth running

Everything except descrambling itself — that the capture path is otherwise
byte-transparent, so that when keys arrive the only variable is `DES_EN`.

1. **Capture the box's output** for a scrambled service, via the HTTP route
   already documented for the TEI work:
   `curl -s http://<stb-ip>:8999/stream=<prog_id>.ts > box.ts`
2. **Capture the same service from the downlink** at the headend over an
   overlapping interval.
3. **Check the scrambling bits on the box capture:**
   ```
   python3 - <<'EOF'
   import collections,sys
   sc=collections.Counter(); tot=collections.Counter()
   f=open('box.ts','rb')
   while True:
       b=f.read(188)
       if len(b)<188: break
       if b[0]!=0x47: continue
       pid=((b[1]&0x1f)<<8)|b[2]; tot[pid]+=1
       sc[(pid,(b[3]>>6)&3)]+=1
   for (pid,s),n in sorted(sc.items()):
       if s: print("PID 0x%04X sc=%d %d/%d" % (pid,s,n,tot[pid]))
   EOF
   ```
   **Expected today: `sc=2` on the ES PIDs, matching the downlink** — because
   nothing is descrambling. If you instead see `sc=0` everywhere, something is
   clearing the field and that is a finding in itself.
4. **Align and byte-compare.** Do **not** align on wall clock or file offset.
   Align on `(PID, continuity_counter)` with a PCR anchor: pick a PCR PID present
   in both, find the packet whose PCR value matches, and walk forward from there
   comparing packets of the same PID in order. The capture is PID-filtered and
   the downlink is not, so a raw offset comparison is meaningless. Compare bytes
   4–187; expect an exact match. Byte 3's lower nibble is the CC and will match;
   bits 6–7 are the scrambling control.

That establishes the capture path is byte-transparent, which is the half of the
question that does not need keys.

### The real test, once CAS is on

With keys loaded and a service authorised, capture the same PID twice — once
with `app_ts_record.c:1679-1680` unchanged (`0x5a`/`0xda`) and once with
`DES_EN` cleared (`0x1a`/`0x9a`) — and compare each against the downlink.

| result | meaning |
|---|---|
| `DES_EN` on → differs from downlink, `sc` cleared; off → matches downlink byte-for-byte | C1 confirmed. Option (a) is available and cheap. |
| both match the downlink | descrambling is downstream of the slot write; C1 wrong; the capture was always scrambled and there is no mismatch to fix |
| both differ from the downlink | something else in the path is rewriting bytes — investigate before drawing any CA conclusion |

One control worth having: run it on a **free-to-air** service on the same
transponder first, if one exists. Both captures must match the downlink
regardless of `DES_EN`, because there is nothing to descramble. If they do not,
the comparison method is at fault rather than the demux.

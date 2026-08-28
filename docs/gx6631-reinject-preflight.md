# Filter / repair / reinject — preflight answers Q1–Q7

Code reading plus measurement against the downlink capture. No design content.

Same method as before: enums from DWARF in the prebuilt archives, macro values by
disassembly. One correction to method this time — **check which side of an
`#ifdef` a finding is on before citing it.** I nearly reported a dead code path
as evidence; see Q1.

---

## The configuration that governs everything below

`platform/gxcas/include/autoconf.h`, generated, not hand-edited:

```
CONFIG_DMX_OLD is not set          CONFIG_ABV_PVR   absent
#define CONFIG_DMX_NEW             #define CONFIG_CGCS   (Cryptoguard CS)
CONFIG_SCPU is not set             #define CONFIG_ACPU
CONFIG_MTC  is not set             #define CONFIG_CARD
```

and `solution/app/include/app_config.h`: `CASCAM_SUPPORT 0`,
`QUICK_SWITCH_SUPPORT 0`.

`CONFIG_SCPU`/`CONFIG_MTC` unset corroborates the earlier `_GetKLMType()`
reading independently: **no key ladder, control words in plaintext.**

## Q1 — Can a descrambler bind to a memory-fed instance?

**Not closed. But the "demux 0" hardcoding is definitely usage, not a
limitation, and nothing anywhere ties descrambling to a frontend source.**

The instance id is a first-class parameter at every layer:

| layer | signature | citation |
|---|---|---|
| public app API | `int GxDesc_Open(unsigned int demux_id, DescAlgEnum stream_alg)` | `app_descrambler_api.c:711` |
| older app API | `GxDescrmb_Open(uint32_t DemuxID)` → `GxDescOpen(GXDESC_MOD_TS, DemuxID, …)` | `:151`, `:171` |
| CAS descrambler | `int32_t GxCas_Desc_Init(uint32_t dmxid)` — opens its own `dev`/`dmx` on that id | `gxcas_descrambler.c:141-157` |
| CAS demux/SI | `int32_t GxCas_Demux_Init(uint32_t tsid, uint32_t dmx_id)` → `GxDmxSetSource(dmx_id, tsid)` | `gxcas_demux.c:1154-1168` |

`GxDesc_Open` takes `demux_id` as an argument of the public API. The hardcoded
zeros (`cryptoguard_secure.c:69`, `sample.c:116-117`,
`app_cgcas_menu.c:389`) are callers choosing 0, not the API refusing anything.

Note `GxDescMod` — `GXDESC_MOD_TS=0, GP=1, M2M=2, CRYPTO=3, TSIO=4`. The
descrambler block is addressed by *module class*, and `MOD_TS` is selected
independently of which demux instance is named. Nothing in the enum, and nothing
in any signature, mentions a frontend.

**The honest gap.** The compiled binding goes through `GxDmxSetSource(dmx_id,
tsid)` into `dvbhal/gxdemux_hal.h`, which is not in the tree, and the descrambler
itself lives in `libgxav` under `GXLIB_PATH`, also absent. So "no evidence of a
frontend tie" is not the same as "proven to work".

**A retraction before it becomes load-bearing.** In the previous round I was
about to cite this, from `gxcas_demux.c:518-521`:

```c
config_demux.source = tsid;
if (tsid == 3) config_demux.ts_select = OTHER;   /* 3 == DEMUX_SDRAM */
else           config_demux.ts_select = FRONTEND;
```

That looks like proof the CAS anticipates a memory-fed instance. **It is inside
`#ifdef CONFIG_DMX_OLD` and is not compiled.** It shows the vendor wrote SDRAM
handling into the CAS demux layer at some point — real evidence of intent, and
worth knowing — but it is dead code and must not be quoted as behaviour.

**The test.** With `CASCAM_SUPPORT` on: `GxCas_Desc_Init(3)` /
`GxCas_Demux_Init(3, 3)`, feed dmx3 from memory with a known-scrambled service
and a known-good CW, and read the slot output. Either the bytes come out clear or
they do not; there is no ambiguous outcome. This is one of the two things worth
building a bench harness for.

## Q2 — Two concurrent CA contexts on one service

**The hardware plausibly allows it. The software stack is a singleton, so as
written: no.** But you do not need it — see below.

Every layer holds process-wide state:

| layer | state | citation |
|---|---|---|
| CAS ops | `static gxcas_control *ca_ops` | `gxcas.c:9` |
| CAS descrambler | `static uint32_t s_dmxid; static handle_t dev, dmx` | `gxcas_descrambler.c:6-8` |
| CAS demux (compiled path) | `static uint16_t s_demux_id` | `gxcas_demux.c:1153` |
| channel alloc | `GxChannelAllocate(s_demux_id, pid)` — always the one id | `gxcas_demux.c:1207-1216` |

`GxCas_Init()` calls `GxCas_Demux_Init(param.ts_id, param.dmx_id)` and
`GxCas_Desc_Init(param.dmx_id)` **once** (`gxcas.c:13-15`). A second context
means a second CAS instance, and there is one.

Two adjacent details worth having:

- On canopus, descrambler contexts are **not** demux CA slots.
  `CONSTEL_CHIP` is defined as `(TAURUS||SIRIUS||GEMINI||CANOPUS||CYGNUS||SCORPIO)`
  (`gxcas_chip_info.h:14`), and that branch uses
  `GxCas_Deschal_Open(GXDESC_MOD_TS, s_dmxid, …)` rather than
  `GxDemuxPropertyID_CAAlloc` (`gxcas_descrambler.c:160-180`). The pool is in the
  descrambler block.
- There is a dead constant `GXDES_MAX_DEMUX_SUPPORT (2)`
  (`app_descrambler_api.c:7`) — defined, never referenced. Someone believed two
  demuxes could descramble concurrently. It enforces nothing.

**Why this is not the blocker.** The topology needs **one** CA context, not two.
dmx0's decode is suppressed anyway — that is the whole point of the Part 7
suppression in Q6 — so dmx0 does not need to descramble. Move the single CA
context to dmx3 and the question dissolves.

## Q3 — Where the CA client reads ECMs from, and whether we can move it

**It reads from exactly one demux instance, named at init, and that name is one
editable line of app-layer C.**

```c
/* solution/app/ca/cas/cryptoguardcas/app_cgcas_menu.c:384-405 */
void app_cgcas_init(void)
{
    CascamInitParam init_param;
    memset(&init_param, 0, sizeof(CascamInitParam));
    init_param.dmx_id = 0;
    init_param.ts_id  = 0;
    ...
    cascam_init(&init_param);
}
```

`CascamInitParam` is `{dmx_id, ts_id, sci, flash, backup}`
(`ca/export/arm-ecos/cascam/cascam.h:55-61`). Everything downstream inherits
those two numbers: `GxCas_Demux_Init(ts_id, dmx_id)` sets `s_demux_id`, and
`GxCas_ChannelAllocate(pid)` allocates every ECM/EMM section channel on it.

The zap-time call carries **no** instance: `CascamSwitchChannel` is
`{pmt_pid, service_id, audio_pid, video_pid, sessionid}` (`cascam.h:63-69`). So
instance selection happens once, at init, and never again.

**Answer: yes, a CA client can bind to dmx3** — set `dmx_id = 3` and `ts_id = 3`
(`DEMUX_SDRAM`). App layer, no BOOT change, no library change. Recovered ECMs
reinjected into dmx3 then reach the CA client by the ordinary path, because the
CA client's section filters *are* dmx3's section filters.

The cost is that this is exclusive: with the CAS on dmx3, **dmx0 has no ECM
source and cannot descramble at all.** That is consistent with the topology — the
repaired stream is the only one that should reach the decoder — but it means the
CA path has no fallback if reinjection fails. Losing reinjection loses CAS, not
just the repair.

## Q4 — ECM loss: measured, and it reframes the problem

Measured on the downlink capture, Cryptoguard ECM PIDs (0x0736 sits in the same
0x073x block as our video 0x0731):

```
ECM 0x0736  13 packets in 4.00 s   spacing min 299  max 300  mean 300 ms
            CC increments on 12 of 12 transitions
            table_ids 0x80 (even) and 0x81 (odd), one parity flip at t=3.42 s
ECM 0x07E0  13 packets in 4.00 s   spacing min 299  max 301  mean 300 ms
            CC increments on 12 of 12 transitions   flip at t=2.21 s
ECM 0x0683  13 packets              CC increments on 12 of 12
```

Every ECM is one section in one TS packet, PUSI set on every packet.

**Three of your premises do not hold on this transponder, and the third one
changes the design.**

1. **CC is reliable here.** 12 of 12 increments on all three ECM PIDs. The
   general worry is sound; it does not apply to these PIDs.
2. **A gap of exactly 16 would be 4.8 s of ECM loss** at 300 ms spacing —
   far outside anything a packet-loss detector is for. It is not an invisible
   failure, it is a fade.
3. **The same ECM is retransmitted every 300 ms for the whole crypto period.**
   The parity flips are 3.42 s and 2.21 s into the window and only one flip
   appears per PID in 4 s, so the crypto period exceeds the capture — but the
   *repeat* interval is firmly 300 ms. At a typical 10 s period that is ~33
   identical copies of each ECM.

**So a lost ECM packet is self-healing.** The next identical copy is 300 ms
behind it, and dozens follow before the CW it carries is needed. To actually
miss a control word you must lose *every* copy across a crypto period — seconds
of consecutive loss, which is an outage, not packet loss, and is what the
full-stream fallback exists for.

**Consequences.**

- ECM loss needs **no dedicated detector and no ECM-specific repair path**. This
  is a real saving, and it removes the timing-budget problem rather than
  solving it: there is no round trip to beat, because waiting 300 ms costs
  nothing.
- Your PCR-addressing concern is moot for the same reason. You never need to
  request a *specific* ECM; you need the next one, and it is already coming.
- **EMM remains genuinely dangerous and is unchanged by any of this.** EMMs are
  card-addressed and not repeated. Four EMM streams are present — Beijing
  Compunicate (0x0583), Irdeto (0x0584), Verimatrix (0x0585), **Cryptoguard
  (0x0586)**. Cryptoguard's read 0 b/s across the whole 4 s window, so I cannot
  characterise its cadence from this capture; a longer capture on 0x0586 would
  settle it. Note the multiplex is simulcrypt with four CA systems.

All ECM and EMM PIDs are carried **clear** (`C` in tsanalyze), as expected — so
they are unaffected by the DES_EN question entirely.

## Q5 — Clock recovery on a memory-fed instance

**Consumer-paced, not clock-recovered. Padding is not required.**

Every memory-fed path in the tree sets the same flow-control mode:

```c
GxDvrProperty_TSRFlowControl ctrl = {0};
ctrl.flags = DVR_FLOW_CONTROL_ES;
GxAVSetProperty(dev, dvr, GxDvrPropertyID_TSRFlowControl, &ctrl, sizeof(ctrl));
```

`gxfrontend_net.c:124-137`, `dvbsource_tscache.c:319-322`,
`dvbsource_tsbuff.c:155-158`. `GxDvrProperty_TSRFlowControl` has exactly one
member, `flags` (DWARF). **`_ES` names the elementary-stream buffer as the
flow-control reference** — back-pressure from downstream occupancy.

The writers behave accordingly. No rate calculation, no PCR-scheduled release,
no timestamped emission anywhere:

```c
wsize = GxFifo_Write(tsc->fifo_w, pkt.data, size, 100000);
while (wsize < size) { wsize += GxFifo_Write(...); GxCore_ThreadDelay(10); }
```

(`dvbsource_tscache.c:491-499`; `gxfrontend_net.c:333-334` is the same shape with
`GxAVModuleWrite`.) Push until the consumer pushes back, sleep 10 ms, retry.

This is also what timeshift does, and timeshift plays back from a file where
original packet positions are long gone. **So the SDK already relies on this
model working without position restoration.**

**Therefore: pacing is sufficient; do not pad to the original slot count.** That
avoids the 13× bandwidth waste.

**One thing this does not settle.** Transport-level clock recovery (locking a
27 MHz oscillator to PCR arrival times) is what would demand original positions
and the ±500 ns budget, and it is meaningless on a memory-fed instance. But the
STC still has to be *loaded* from PCR values for A/V sync — `GxDemuxPropertyID_Pcr`
and `GxSTCPropertyID_TimeResolution`/`_Play` (`hw_demux.c:525-531`) — and how
tolerant that is of PCR values whose spacing no longer matches arrival is not
determinable from the tree. Timeshift is the existence proof that it works in
practice. Watch for drift or A/V sync creep on the first bench run rather than
designing padding in advance.

## Q6 — Decoder contention

**`app_rist_screen_enabled()` at `app_play_control.c:1165` is still the correct
suppression point.** Sketch only, as asked.

The ordering is already right for this topology. `app_rist_play_change()` runs at
`:1155`, deliberately *before* the decode block, with the existing comment:

> "Runs BEFORE the decode below so any previous program's `player_av` is torn
> down first, freeing the video decoder for whichever player owns the screen
> this zap."

and the suppression follows at `:1163-1170`. The single-video-decoder constraint
is identical; only the identity of the winning path changes — the decoder is fed
from dmx3 rather than from `player_av` on a loopback UDP socket. The predicate
`app_rist_screen_enabled()` (`app_rist_capture.c:667`,
`return on || s_rist.chain_active`) is the right shape and would need its
condition widened, not moved.

## Q7 — Zap

**`app_rist_play_change()` at `app_play_control.c:1155` is still the right
hook** — it already runs before decode, is non-blocking, and already tears down
the previous program's capture (`app_rist_capture.c:1124-1136`).

Also already present on this path and useful: **`app_ca_stop_descramble()` is
called at `app_play_control.c:1161`**, immediately after our hook, under
`#if CASCAM_SUPPORT`. So CA teardown on zap exists; it is the *rebuild* on dmx3
that is new.

What this topology must do that Part 7 did not:

1. **Tear down and rebuild dmx3's slots**, in addition to dmx2's. Part 7 had one
   demux instance to manage; this has two, and they must be sequenced — dmx3's
   slots cannot be built before the PMT for the new service is known.
2. **Re-establish the CA binding on dmx3 every zap**, i.e. new ECM PIDs, new
   `GxDescrmb_SetStreamPID` per ES, new CW delivery. `CascamSwitchChannel`
   carries `{pmt_pid, service_id, audio_pid, video_pid, sessionid}` and no
   instance, so the switch itself is instance-agnostic — but the descrambler
   handles are per-PID and must be reopened.
3. **Flush the injection FIFO before the new service's packets enter it.**
   Nothing in Part 7 had a downstream buffer holding the *previous* service's
   bytes. Stale packets from the old service arriving at dmx3 after the new
   PMT is installed will be descrambled with the wrong keys.
4. **Order the CA rebind against the PMT.** ECM PIDs come from the new PMT, so
   the CA rebind cannot start until the PMT is parsed — which in this topology
   arrives *through* dmx3, i.e. after reinjection is already running. That is a
   sequencing dependency Part 7 never had, and it is the one I would expect to
   cause trouble first.
5. **One-time, not per-zap, but easy to miss:** if `QUICK_SWITCH_SUPPORT` is ever
   enabled it claims `normal_play.dmx_id + 1` for tscache
   (`app_play_control.c:1145`). It is `0` today, so there is no contention — but
   it would collide with an instance allocation made on the assumption that
   dmx3 is free.

## Summary against your "if Q1, Q2 or Q3 is NO"

- **Q1 — open, leaning yes.** Nothing ties descrambling to a frontend source and
  the instance id is a parameter everywhere. Needs the bench test; the answer is
  binary and cheap to get.
- **Q2 — no as written, but you do not need it.** One CA context on dmx3 is the
  shape, not two.
- **Q3 — yes.** Two editable lines in `app_cgcas_init()`.

So the fallback is not triggered on Q2/Q3. It is triggered only if **Q1 comes
back negative**, and in that case the nearest achievable shape is option (b) —
descrambling the repair in our own process — which is worth stating properly
rather than as a footnote:

**Option (b), assessed.** It is real, and better-supported than it looked. It
rests on two independent facts, both now confirmed: `_GetKLMType()` returns
`GXDESC_KLM_NONE` for CANOPUS absent `SCPU_USED`, and `autoconf.h` shows
`CONFIG_SCPU`/`CONFIG_MTC` unset with `CONFIG_ACPU` set — so control words arrive
in plaintext through `GxDescrmb_SetCW(handle, OddKey, EvenKey, len)`
(`app_descrambler_api.c:289`) and our code can observe them.

Three things decide whether it is acceptable:

1. **Volume.** Software CSA2 over *repair* bytes only is tractable; over the
   stream it is not. This is viable precisely because repairs are bounded ranges.
2. **`GXDESC_MOD_M2M = 2` exists** — memory-to-memory descrambling in hardware,
   which would remove the CPU cost entirely. **But we have already tried M2M on
   this chip and it panicked the kernel on app-owned buffers** — that failure is
   why the userspace-decrypt approach was abandoned and dvb2ip adopted
   (`install.sh:4-7`). Treat M2M as known-hostile unless someone re-opens it
   deliberately.
3. **It puts plaintext control words in our process**, and it silently stops
   working if the CAS is ever rebuilt with a secure CPU. That is an operator
   decision, not an engineering one.

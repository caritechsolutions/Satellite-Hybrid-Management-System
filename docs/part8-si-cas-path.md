# Where SI/EPG/time actually come from — and why they survive Step 1

Investigation only. No code changed.

The puzzle: Step 1 injects a self-contained PAT/PMT with no SDT/EIT/TDT, yet EPG
and the clock keep working. The answer is that **SI never went through the Part 8
chain at all**, and the reason matters for what Step 3 has to be.

---

## Q1 — where the section filters are fed from

**Every SI section filter on this box is bound to a demux instance whose input is
the live tuner, and that demux is not ours.**

The whole SI stack funnels into one function:

`platform/gxbus/si/si_filter.c:213` — `ts_demux_connect(ts_src, demux_id)`

```c
s_Demux[demux_id].handle = GxAvdev_OpenModule(dev, GXAV_MOD_DEMUX, demux_id);
...
config_demux.source      = ts_src;          /* caller's choice */
config_demux.ts_select   = FRONTEND;        /* hardcoded */
config_demux.stream_mode = DEMUX_PARALLEL;
GxAVSetProperty(dev, s_Demux[demux_id].handle, GxDemuxPropertyID_Config, ...);
```

reached from `GxBus_SiFilterCreate(ts_src, demux_id, si_filter)`
(`si_filter.c:358`), itself reached from `gxsi.c:505`:

```c
si_filter_id = GxBus_SiFilterCreate(p_si_subtable->ts_src,
                                    p_si_subtable->demux_id,
                                    &(p_si_subtable->si_filter));
```

The parallel subsystem path is identical —
`sub_system/dmx_sub_system/dmx_sub_driver/dmx_sub_driver.c:37`:

```c
int32_t gx_dmx_sub_driver_config(handle_t device, handle_t demux, uint32_t ts)
{
    config_demux.source    = ts;
    config_demux.ts_select = FRONTEND;
    ...
}
```

### Which demux instance: 0

- `include/sub_system/si_sub_engine/si_sub_engine.h` on `dmx_id`:
  *"硬件dmx id，由驱动提供，目前分别是 0,1"* — hardware demux id, currently 0 and 1.
- `app_system_init.c:1155-1163` opens every frontend with
  `open.dmx_id = 0` (the `INDEPEND_RECORD` alternative is compiled out —
  `app_config.h:217` sets it to 0) and `open.ts_src = ts_src_list[i]`, an
  `enum dmx_input` naming the tuner's TS input.
- `app_play_control.c:1087`: `g_AppPlayOps.normal_play.dmx_id = config.dmx_id`.

### Each consumer, traced

| table | consumer | how it gets demux 0 |
|---|---|---|
| **SDT 0x11** | `app_sdt_start()` (`psi-si/app_sdt.c:35`) | called as `app_sdt_start(g_AppPlayOps.normal_play.dmx_id, …)` — `app_play_control.c:2055` and `:3083` |
| **EIT 0x12** | `GxEpg_SubtChannel*` (`epg/gxepg_table.c`) | `GxDmxSetSource(epg_cfg.dmx_id, epg_cfg.ts_src)` at `:123`, then `GxDmxStart(epg_cfg.dmx_id, params)` at `:193` |
| **TDT/TOT 0x14** | `extra_sync_time()` (`extra/gxextra.c:98`) | `memset(&subt_detail, 0, …)` then **only `ts_src` is set** — `demux_id` stays 0 |
| **ECM** | `app_ecm_start()` (`psi-si/app_ecm.c:113`) | `subt_detail.ts_src = ecm->ts_src; subt_detail.demux_id = ecm->DemuxId` (`:145-146`) — same subtable path |

### Why Step 1 didn't disturb any of it

Our capture is a **separate demux instance reading the same tuner input in
parallel**. `app_ts_record.c:752-754`:

```c
demux.source    = ts_src;        /* the same tuner TS input as demux 0 */
demux.ts_select = FRONTEND;
```

on module `s_ts_rec_modid` = 2 (`app_ts_record.c:162`, `:769`).

So the topology during Step 1 is:

```
tuner TS input ─┬─> demux 0 : AV slots + ALL SI section filters (SDT/EIT/TDT/ECM)
                └─> demux 2 : our capture (service PIDs) -> UDP -> cutter -> …
```

`app_player_close(PLAYER_FOR_NORMAL)` closes the *player*, not the frontend or
demux 0's filters. The tuner stays locked — it has to, because demux 2 is reading
it — so demux 0 keeps receiving live SI throughout. **EPG and time work in Step 1
for the same reason they work with the Part 8 chain switched off.**

---

## Q2 — what happens in a real fade

**Confirmed: SI has no recovery, and this is the gap.**

In a fade the tuner loses lock, so demux 0's input stops. Nothing else feeds it.
Precisely:

- **EIT** stops arriving. The EPG store (`gxepg.c:66-77`: 400 services, 2000
  events, 3 days) keeps what it already holds, so the grid does not empty — it
  *freezes*, and ages out as events pass.
- **SDT** stops. Service names/flags hold at their last value.
- **TDT/TOT** stops. **The wall clock does not stop** — Linux keeps ticking — but
  the DVB time correction and any TDT-driven timezone/summer-time handling stop.
  There is no second source to cover it: `app_rist_api.c:412-424` does exactly
  **one** SNTP step at network-up, deliberately (*"a clock that leaps while a
  chain is running is the hazard"*), and the platform's own net-time path is
  compiled out.
- **ECM** stops — which for a scrambled service is not degradation, it is the
  picture stopping a couple of crypto-periods later, whatever Part 8 does for
  the video.

So Part 8 today recovers **video and audio and nothing else**. A box on a
recovered stream through a long fade shows moving pictures with a frozen EPG,
and — once CAS is on — would lose the picture anyway when the ECM stops.

---

## Q3 — what it would take to feed SI from the repaired stream

### First, a correction to the obvious reading

`ts_select = FRONTEND` looks like the blocker. **It is not.** From the DWARF in
`_build-arm-ecos/libgxdvb.a`:

```
dmx_ts_select : FRONTEND = 0, OTHER = 1
dmx_input     : DEMUX_TS1 = 0, DEMUX_TS2 = 1, DEMUX_TS3 = 2, DEMUX_SDRAM = 3
```

and the memory-fed media demux that Step 2 uses sets **the same value**
(`gxmedia_demux.c`, `GxMedia_DemuxConfig`):

```c
dmx_config.ts_select = 0;                                  /* == FRONTEND */
dmx_config.source    = (Tsid == -1) ? DEMUX_SDRAM : Tsid;  /* the real switch */
```

**`source` is the tuner-vs-memory discriminator, and every SI entry point already
takes it as a parameter** — `ts_demux_connect(ts_src, …)`,
`gx_dmx_sub_driver_config(…, ts)`, `GxDmxSetSource(dmx_id, ts_src)`,
`subt_detail.ts_src`, `epg_cfg.ts_src`. Nothing is hardcoded to the tuner except
the value the app happens to pass.

(For the record, the one place that *does* branch on the source —
`gxcas/module/gxcas_demux.c:519`, `if (tsid == 3) ts_select = OTHER` — is inside
`#ifdef CONFIG_DMX_OLD` and is dead. The live CAS path at `:1162` is under
`CONFIG_DMX_NEW` and calls `GxDmxSetSource`.)

### So the answer is (a), with a correction to how it was framed

Not "reinject into a demux instance whose filters the platform already reads" —
the platform reads demux **0**, and repointing that at memory would take the live
tuner AV path down with it, removing the fallback we depend on.

**It is: reinject into demux 3 with `source = DEMUX_SDRAM`, then point the SI
consumers at demux 3.** Both halves are parameters that already exist:

| what | where it is set today | what it becomes |
|---|---|---|
| SDT | `app_sdt_start(normal_play.dmx_id, …)` | pass 3 |
| EIT | `epg_cfg.dmx_id`, `epg_cfg.ts_src` → `GxDmxSetSource()` | 3, `DEMUX_SDRAM` |
| TDT/TOT | `subt_detail.demux_id` (currently left 0 by `memset`) | set to 3 |
| ECM | `ecm->DemuxId`, `ecm->ts_src` | 3, `DEMUX_SDRAM` |

**This is what earns the second demux its place.** Step 1 proved `player_av`
decodes video fine, so dmx3 is not needed for pictures. It is needed because it
is the only way SI and CAS reach their normal consumers from a stream we
control.

### Three things that must hold, and are not yet established

1. **The repaired stream must actually carry SDT/EIT/TDT.** It does not today.
   The capture slot set is the service's PIDs (+ its PMT); `/tmp/ristp8psi=1`
   adds 0x0000/0x0011/0x0012/0x0014, which is exactly this. **So the PSI
   passthrough mode that Step 2 does not need is precisely what Step 3
   requires** — and it also has to be added to the *headend's* filter list, which
   already includes the §4.3 set (`P8_PSI_PIDS = 0,1,16,17,18,19,20`). Both ends
   already agree on the list; only the box's capture has it behind a knob.
2. **Two owners of demux 3.** Step 2's `GxMediaApi_ModuleOpen3` opens
   `GXAV_MOD_DEMUX` id 3 and configures it; the SI layer's `ts_demux_connect(3, …)`
   would open and configure the same module. Both want `source = DEMUX_SDRAM`,
   `ts_select = 0` and the same gates, so the configs are compatible and slot
   allocation is additive — but `si_filter.c`'s refcount
   (`s_Demux[demux_id].count`) knows nothing about the media module's handle, and
   whichever closes last wins. **Unverified.** It may need one owner opening the
   demux and the other attaching to it.
3. **EIT is a transponder-wide table.** EIT p/f for *other* services, and EIT
   schedule, live on the same PID 0x0012 as ours. Filtering one service's PIDs
   still carries the whole 0x0012 stream, so this works — but the recovered
   bandwidth is then the whole EIT, not our slice of it. Worth measuring before
   committing: on the earlier capture EIT was 627 packets in 4 s against 4,799
   total, i.e. **~13% of the stream**, which is not free.

### Why not (b)

Redirecting the *existing* filters means `GxDmxSetSource(0, DEMUX_SDRAM)` —
repointing demux 0. That is the demux the live AV path and every SI consumer
share. It would work only while the Part 8 chain is up, and it removes the
tuner fallback at exactly the moment a fade would need it. Rejected.

---

## Q4 — CAS/ECM

**Same path, same answer.** `psi-si/app_ecm.c:113` builds a `GxSubTableDetail`
and sets `subt_detail.ts_src` / `subt_detail.demux_id` (`:145-146`) — the same
`GxBus_SiSubtableCreate` → `GxBus_SiFilterCreate` → `ts_demux_connect` chain as
SDT and TDT. There is nothing special about ECM's source: it is a section filter
on a demux instance, and both the instance and the source are parameters.

So **yes, in principle ECM can be fed from the repaired stream by the same
change** — the ECM PIDs would have to be in the capture slot set and in the
headend's filter list, and `ecm->DemuxId`/`ecm->ts_src` pointed at 3/`DEMUX_SDRAM`.

Three caveats I will not paper over:

- `CA_SUPPORT` and `CASCAM_SUPPORT` are both **0** (`app_config.h:318`, `:272`),
  so none of this compiles today and none of it can be tested.
- The ECM PIDs are per-service and come from the PMT's CA descriptors. The
  capture's slot set is built from `GxBus_PmProgGetById()`, which gives
  video/audio/PCR/PMT — **not** CA PIDs. Adding them is a separate piece of work
  in `app_ts_record.c`, and the headend's filter derivation would need the same
  addition from its own PMT parse.
- Descrambling happens in the demux (`DMX_DES_EN` on the slot). A memory-fed
  demux 3 doing its own descrambling is a further unproven step beyond
  reinjection itself — the key ladder is bound to a demux instance, and which
  one is not something I have traced.

**Your Q4 is cut off** — it ends at *"Cannot test — CA_SUPPORT=0 — but"*. I have
answered where the CA client reads ECMs from and whether that source can be fed
from the repaired stream. If the rest of the question was about the key ladder,
or about whether to build it blind, say so and I will take it separately.

---

## What this means for the plan

Step 2's dmx3 tail was framed as the production video path. On this evidence its
real value is different and larger: **it is the only route by which SI and CAS
reach their normal consumers from a stream we control.** Video already works
through `player_av`.

That reframes the Step 3 shape: PSI passthrough stops being optional, dmx3 stops
being about pictures, and the ownership question in point 2 above becomes the
thing to settle first — before any SI consumer is repointed.

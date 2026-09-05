# U1 and U2 — can a memory-fed demux drive the consumers?

Investigation plus one probe. **The full path is not built** — U1 is not answered
and I will not build on an unanswered unknown.

---

## First: a premise I have to push back on

> "You proved dmx3 ACCEPTS injected TS and PARSES PSI (the shadow)."

**I did not.** I wrote the dmx3 tail last turn and said plainly it was unrun —
"I have no access to the box and none of this step can be exercised
off-hardware. Nothing above should be read as a claim that it works." I also
have no record of anything called "the shadow"; that is not a thing I built.

If that result came from your own flash-and-run, it is good news and it moves U1
a long way — but I need the serial log before I treat it as established. If it
did not, then dmx3 accepting injected TS is *also* still open, and the probe
below covers it (the `inj_ok`/`fifo ts_used` counters already in the tail).

---

## U2 — answered, and the answer removes the problem

**The consumers do not need to move to dmx3. Point demux 0 at memory instead.**

Three independent SDK subsystems do exactly this, and none of them relocates a
consumer:

| subsystem | what it does |
|---|---|
| `frontend/gxfrontend_net.c` | `#define DEMUX_ID (0)`; opens demux **0** with `source = DEMUX_SDRAM`, opens `GXAV_MOD_DVR` 0 with `src = DVR_INPUT_MEM`, and writes TS in with `GxAVModuleWrite()` (`:316`, `:334`). Registered as a **frontend** — `GxFrontend_RegisterNet(i, PLAYER_FOR_NORMAL)`, opened with `open.ts_src = 3; open.dmx_id = 0` (`app_system_init.c:1184-1188`). |
| `module/pdmx/app_pdmx.c:610-612` | `info.output_info.demux_id = dmx_id; info.output_info.demux_source = 3; info.input_info.demux_id = dmx_id==0?1:0;` — the consumers' demux becomes SDRAM-fed and **the tuner moves to the other instance**. |
| `player/access/dvbsource_tscache.c:427-438` | `dmx_r` on `cache_dmxid` = tuner, `dmx_w` on `dmxid` = `DEMUX_SDRAM`. `app_play_control.c:1144` passes `tscache_dmxid: normal_play.dmx_id + 1`, so demux 0 is the SDRAM one. Same shape in `dvbsource_tsbuff.c:263-273`. |

So the platform's own answer to "insert a stage between tuner and consumers" is
**flip demux 0's source**, not move the consumers. Every consumer keeps its
compiled-in `dmx_id` — `app_sdt_start(normal_play.dmx_id, …)`, `epg_cfg.dmx_id`,
the TDT subtable's `memset`-zeroed `demux_id`, `ecm->DemuxId` — and none of them
has to know.

### It is a runtime switch, and a cheap one

`source` is a field of `GxDemuxProperty_ConfigDemux`, set through
`GxDemuxPropertyID_Config` / `GxDmxSetSource(dmx_id, source)`. It is re-settable
on a live instance — `dvbsource_tscache.c:171-174` sets it again on teardown to
put the demux back. So the switch is one property set on demux 0:

- **factory / non-Part-8 channels:** `source = <tuner TS input>` — exactly today.
- **Part 8 active:** `source = DEMUX_SDRAM`, fed from the repaired stream.
- **any failure:** set it back. That is the whole revert.

`PDMX_SUPPORT` is **1** on this build (`app_config.h:175`) and the switch is a
runtime config key, `GXBUS_FRONTEND_PDMX` ("frontend>pdmx", OFF/ON/AUTO —
`gxfrontend_module.h:30-33`), default OFF.

### Which also means dmx3 is the wrong instance

If demux 0 is the memory-fed one, dmx3 is not needed at all. The topology
becomes:

```
tuner ──> demux 2 : capture (broadcast PSI + service) ──> cutter ──> RIST ──┐
                                                                            │
demux 0 : source = DEMUX_SDRAM  <───────────────────────────────────────────┘
          └─> video/audio decode, app_sdt, app_epg, app_time, CA client
```

One fewer demux instance, no consumer changes, and the two-owners problem I
flagged last turn disappears.

**Caveat I cannot close from here:** `app_pdmx.c` is committed but `pdmx.h` and
the library behind `pdmx_start()`/`pdmx_stop()` are **not in this repo** — I
searched the whole filesystem. The SDK tree here is partial. So I can read the
pdmx *architecture* but not its implementation, and I cannot tell you whether
pdmx itself works or is vestigial. What is fully readable and complete is
`gxfrontend_net.c`, and it does the same thing with plain `GxAVSetProperty` and
`GxAVModuleWrite` — which is the mechanism we would use directly anyway.

---

## U1 — NOT answered. Here is exactly what is and is not established.

### U1b, the decoder off a memory-fed demux: established by design, unrun here

`GxMedia_ModuleOpen()` binds the video and audio decoders to the demux
(`GxMedia_VideoBindDemux` / `GxMedia_AudioBindDemux`, `gxmedia_module.c:38-43`),
and `GXMEDIA_SOURCE_TS` is precisely the memory-fed configuration. The SDK's own
VOD path (`vod_trans_hwts.c`) and both access modules are built on it. I would be
surprised if this fails.

### U1a, section filters off a memory-fed demux: NOT established by anything

**No code in this SDK allocates a section filter on a demux whose source is
DEMUX_SDRAM and demonstrates it firing.** I looked:

- `tscache` and `tsbuff` put their slots on the **tuner** demux (`dmx_r`) — those
  slots select what to cache. Nothing is allocated on `dmx_w`.
- `gxfrontend_net.c` sets up the SDRAM demux and the DVR, and stops there. What
  section-filters against it is the generic search/SI stack — which would be the
  proof, except `NET_SEARCH_SUPPORT` is **0** (`app_config.h:145`), so it has
  never run on this box.
- `QUICK_SWITCH_SUPPORT` is **0** (`:185`), so tscache has never run either.
- `GXBUS_FRONTEND_PDMX` defaults OFF, so pdmx has never run either.

So three subsystems *assume* section filters are source-agnostic, and **not one
of them is exercised on this build**. That is an architecture betting on a
property, not a demonstration of it. Architecturally it should hold — `source`
selects what feeds the demux's packet processor, and slots and filters sit after
it — but "should" is not what this decision can rest on.

---

## The probe (built, not run)

`/tmp/ristp8sec=1` arms four section filters **on dmx3, with
`ts_src = DEMUX_SDRAM`**, using the platform's own SI API rather than a
hand-rolled slot+filter — because the question is not "can I get bytes out of the
hardware", it is "would `app_epg` and `app_time` get their tables", and those go
through `GxBus_SiFilterCreate()` / `GxBus_SiFilterRead()`.

| filter | PID | table id | who would consume it |
|---|---|---|---|
| PAT | 0x0000 | 0x00 | **positive control** |
| SDT | 0x0011 | 0x42 | `app_sdt` / service names |
| EIT | 0x0012 | 0x4E mask 0xF0 | `app_epg` |
| TDT | 0x0014 | 0x70 | `app_time` |

PAT is the control that makes a negative result meaningful: if PAT arrives and
SDT/EIT do not, the filters work and the *stream* is short of tables; if nothing
arrives at all, the filters do not fire on a memory-fed demux.

It **refuses to run** without `/tmp/ristp8psi=1`, because without PSI passthrough
the capture carries the box's own PAT/PMT and no SDT/EIT/TDT — a silent probe
would prove nothing and would read as a NO.

### Running it

```sh
echo 1 > /tmp/ristp8            # Part 8 path
echo 1 > /tmp/ristp8psi         # broadcast PSI into the capture -- REQUIRED
echo dmx3 > /tmp/ristp8tail     # the memory-fed demux tail
echo 1 > /tmp/ristp8sec         # the probe
# zap away from NCN and back
```

Expected shape:

```
[RIST] p8sec: PAT pid 0x0000 tid 0x00/0xFF -> filter 3 (positive control)
[RIST] p8sec: SDT pid 0x0011 tid 0x42/0xFF -> filter 4 (app_sdt / service names)
[RIST] p8sec: EIT pid 0x0012 tid 0x4E/0xF0 -> filter 5 (app_epg)
[RIST] p8sec: TDT pid 0x0014 tid 0x70/0xFF -> filter 6 (app_time)
[RIST] p8sec: 4/4 filters armed on dmx3 (ts_src=DEMUX_SDRAM). Sections below or nothing.
[RIST] p8sec: *** PAT SECTION on dmx3, 61 bytes: 00 B0 95 00 05 C5 00 00 ***
[RIST] p8sec:   PAT pid 0x0000: 12 sections, 732 bytes  (positive control)
[RIST] p8sec:   SDT pid 0x0011: 4 sections, ... (app_sdt / service names)
[RIST] p8sec: U1 VERDICT at T+2000ms: section filters on a memory-fed demux DO FIRE
```

or

```
[RIST] p8sec: U1 VERDICT at T+12000ms: section filters on a memory-fed demux produced NOTHING
[RIST] p8sec:   Before concluding they cannot: check ts_in/inj_ok above are non-zero ...
```

The three failure points are separated deliberately: `FilterCreate` failing means
the demux refuses the allocation; filters armed but silent with `inj_ok = 0`
means nothing reached dmx3; filters armed and silent with `inj_ok` climbing is
the real NO.

---

## What happens with each verdict

**If U1a is YES** — build the target, but on **demux 0**, not dmx3: flip demux 0
to `DEMUX_SDRAM` while Part 8 is active, feed it the repaired stream, leave every
consumer where it is, suppress the dmx0 tuner decode (already handled —
`app_rist_screen_enabled()` → `app_player_close(PLAYER_FOR_NORMAL)` at
`app_play_control.c:1155`), and set the source back on any failure. dmx3 is not
needed.

**If U1a is NO** — say so and stop. Video falls back to the proven Step 1
`player_av` tail, and SI-in-a-fade becomes a different problem: the remaining
options would be a software section parser fed from the repaired stream that
calls the consumers' parse entry points directly, which is a much larger and
uglier piece of work and should be costed before it is chosen.

---

## What I did not do

I did not build the demux-0 switch, the consumer repoint, or the SI feed. U1a is
the load-bearing unknown and it is one flash away from being answered — building
the architecture on top of it first would be exactly the thing you asked me not
to do.

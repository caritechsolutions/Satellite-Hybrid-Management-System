# GX6631 Part 8 repair path — preflight findings (R1–R6)

Whether a transparent shim between tuner and everything else is achievable on
this hardware. Factual record with citations; no receiver design.

Tree examined: `6631SDK/` at `4d66410`. The demux property-types header is
**absent** from the checkout, as before — but it is no longer needed. Every enum
below was recovered from DWARF in the prebuilt archives, and the `DMX_*` macro
values from disassembly of the shipped objects.

**Method note.** `platform/gxbus/_build-arm-ecos/libgxdvb.a` and
`player/libgxplayer.a` were built with `-g`. `readelf --debug-dump=info` yields
the complete demux type set. The `DMX_*` slot flags are macros so they are not
in DWARF, but they are baked into the compiled code and readable by
disassembling `.text.tsbuff_config` against the DWARF line table.

---

## R1 — There is no raw tap. Definitive, negative.

The full `GxDemuxProperty_ConfigDemux` type, recovered from DWARF:

```c
struct GxDemuxProperty_ConfigDemux {
    sync_lock_gate; sync_loss_gate; time_gate; byt_cnt_err_gate;
    stream_mode;    /* enum dmx_stream_mode */
    ts_select;      /* enum dmx_ts_select   */
    source;         /* enum dmx_input       */
    flags;
};

enum dmx_stream_mode { DEMUX_PARALLEL=0, DEMUX_SERIAL=1,
                       DEMUX_SERIAL_2BIT=2, DEMUX_SERIAL_4BIT=3 };
enum dmx_ts_select   { FRONTEND=0, OTHER=1 };
enum dmx_input       { DEMUX_TS1=0, DEMUX_TS2=1, DEMUX_TS3=2, DEMUX_SDRAM=3 };
```

**`stream_mode` is the TS input wire format — parallel or serial at three
widths. It is not a bypass and there is no passthrough value.** That was the
hypothesis and it is dead.

`ts_select` selects frontend versus other input. `source` selects which physical
TS input, or memory. `flags` is never set to anything but 0 anywhere in the
tree.

The property enum is also complete and contiguous, 1001–1023:

```
Config=1001  SlotAlloc=1002  SlotConfig=1003  SlotEnable=1004  SlotDisable=1005
SlotFree=1006  FilterAlloc=1007  FilterConfig=1008  FilterEnable=1009
FilterDisable=1010  FilterFIFOReset=1011  FilterFIFOQuery=1012  FilterFree=1013
CAAlloc=1014  CAConfig=1015  CAFree=1016  FilterRead=1017  Pcr=1018
TSLockQuery=1019  SlotQueryByPid=1020  ReadStc=1021  MTCCAConfig=1022
FilterFifoFreeSize=1023
```

There is no Raw, Bypass, Record or Passthrough property. **Answer: no mode
delivers the complete demodulator output to memory before PID filtering.**
Everything reaching memory does so through a slot.

## R2 — No wildcard PID, but there is an all-pass SLOT TYPE

`enum dmx_slot_type` from DWARF:

```
DEMUX_SLOT_PSI=0  PES=1  PES_AUDIO=2  PES_VIDEO=3
DEMUX_SLOT_TS=4   DEMUX_SLOT_MUXTS=5
DEMUX_SLOT_AUDIO=6  SPDIF=7  AUDIO_SPDIF=8  VIDEO=9
```

The vendor's own autotest allocates `DEMUX_SLOT_MUXTS` **with no `pid` field
assigned at all** — `solution/app/module/autotest/module/module_tsfilter.c:119-137`,
commented "sepical slot", behind `#if 0`:

```c
sg_MuxSlot.type  = DEMUX_SLOT_MUXTS;
sg_MuxSlot.flags = (DMX_TSOUT_EN | DMX_CRC_DISABLE | DMX_REPEAT_MODE);
GxAVGetProperty(..., GxDemuxPropertyID_SlotAlloc, &sg_MuxSlot, ...);
```

Every other slot allocation in the tree sets `.pid`. This one does not, and it
is the only slot type named for the multiplex rather than for an elementary
stream. **That is the shape of an all-PID slot**, and it is the all-pass
candidate — not a magic PID value.

No wildcard PID is special-cased anywhere reachable. The `0x1FFF` guards are
library-level validators, not the driver: `hw_demux.h:26-27`,
`dvbsource_tscache.c:21-22`, `dvbsource_tsbuff.c:7-8`,
`app_play_control.c:104`. None of them binds the capture path.

**Status: probable, not proven.** It is `#if 0` in the only place it appears, so
it is untested vendor code. Settling it is a small experiment on the box, not a
design question — see "What to run on the box" below.

## R3 — Counts: answered from source, and my earlier framing was wrong

`platform/gxbus/sub_system/dmx_sub_system/dmx_sub_system.c:178-184`:

```c
static uint32_t get_hard_num(void)
{
    DMX_SUB_NUM = 2;
    DMX_SUB_SLOT_NUM = 64;
    DMX_SUB_FILTER_NUM = 64;
    return GX_DMX_SUB_SUCCESS;
}
```

They are runtime variables assigned compile-time constants by a stub. **No flash
is needed to learn them — 2/64/64 is what the software will enforce.**

But they are the **SI subsystem's** view, not the demux instance count, and I
previously conflated the two. The GXAV demux module count is larger and is
already exercised:

| instance | user | citation |
|---|---|---|
| dmx0, dmx1 | normal AV path | `app_demux_api.c:202-203` |
| dmx2 | our RIST capture | `app_ts_record.c:155` |
| max | 4 | `TS_REC_DEMUX_MOD_MAX`, `app_ts_record.c:14` |

**Three instances run concurrently today and the code admits four.** The
two-demux-path fallback is not blocked on instance count. That corrects my
earlier "unmeasured".

## R4 — Memory-fed demux

### (a) Confirmed, and now named precisely

`platform/gxbus/frontend/gxfrontend_net.c` is the reference implementation:

```c
GxDvrProperty_Config config = { .src = DVR_INPUT_MEM, .dst = DVR_OUTPUT_DMX };
ctrl.flags = DVR_FLOW_CONTROL_ES;
GxAVSetProperty(dev, dvr, GxDvrPropertyID_TSRFlowControl, &ctrl, ...);   /* :137 */
GxAVSetProperty(dev, dvr, GxDvrPropertyID_Config, &config, ...);        /* :139 */
GxAVSetProperty(dev, dvr, GxDvrPropertyID_Run, NULL, 0);                /* :143 */
...
GxAVModuleWrite(dev, dvr, buffer + write_data, len - write_data, 0);    /* :334 */
```

with the demux configured `.source = DEMUX_SDRAM` (`:55`). `enum dvr_output`
from DWARF: `DVR0=0 DVR1=1 DVR2=2 DVR3=3 MEM=4 DSP=5 DMX=6 T2MI=7`.

The capture direction we already use is the mirror: `DVR_INPUT_DMX` /
`DVR_OUTPUT_MEM` + `GxAVModuleRead`.

**The two-path topology is not something we would invent — it is how timeshift
already works.** `dvbsource_tscache.c:425-438` opens two demux modules on one
device and configures them in opposite directions:

```c
tsc->dmx_r = GxAvdev_OpenModule(tsc->dev, GXAV_MOD_DEMUX, tsc->cache_dmxid);
tsc->dmx_w = GxAvdev_OpenModule(tsc->dev, GXAV_MOD_DEMUX, tsc->dmxid);
cfg_dmx.source = tsc->tsid;      GxAVSetProperty(... dmx_r ...);  /* live   */
cfg_dmx.source = DEMUX_SDRAM;    GxAVSetProperty(... dmx_w ...);  /* memory */
```

### (b) YES — section filters run against a memory-fed instance

`gxfrontend_net.c` is a whole **frontend** whose TS arrives from SDRAM, and the
app drives a full channel search over it:
`solution/app/netapps/server_search/app_net_search_setting.c:1012-1046` sets
`params_net.ts = &s_sp_ts_src` (`= DEMUX_SDRAM`, `:1012`) and fires
`GXMSG_SEARCH_SCAN_NET_START` with `time_out.pat / .pmt / .sdt / .nit` and a
`check_ca_fun` callback.

A channel search **is** hardware section filtering. This is a shipping feature
with its own UI, not an inference from architecture. **The platform SI/EPG
parser will see our repaired tables.**

### (c) NO ANSWER — and it is the one that decides the architecture

Nothing in the tree binds a descrambler to a `DEMUX_SDRAM`-sourced instance.
Timeshift comes closest and deliberately does not: `dvbsource_tscache.c` sets no
`DMX_DES_EN` and allocates no CA on either side, because timeshift stores
content that was already descrambled on the way in.

**But the binding mechanism makes the question sharper than "does it work".**
`platform/gxcas/module/gxcas_descrambler.c:222-231`:

```c
slot.pid = pid;
GxAVGetProperty(dev, dmx, GxDemuxPropertyID_SlotQueryByPid, &slot, ...);
ca.ca_id   = descid;
ca.slot_id = slot.slot_id;          /* <-- the descrambler attaches to a SLOT */
ca.flags   = DMX_CA_KEY_ODD | DMX_CA_KEY_EVEN;
GxAVSetProperty(dev, dmx, GxDemuxPropertyID_CAConfig, &ca, ...);
```

**The descrambler is bound per slot, and a slot is a PID.** Descrambling
therefore *requires* PID filtering by construction — the descrambler lives
inside the demux, downstream of the PID filter, not downstream of the demux.

This is the finding that matters most, and it is independent of R1, R2 and R5:

> "Raw unfiltered TS in, repaired raw TS out, and the descrambler behaves
> exactly as it does today" is **internally contradictory on this hardware.**
> An all-PID MUXTS slot, even if it works, yields a stream that cannot be
> descrambled as a unit, because CA keys are attached per slot.

So a shim carrying scrambled TS needs instance B to re-establish per-PID slots
and CA bindings — i.e. it is not transparent, it is a second full demux setup.
A shim carrying *descrambled* TS has already had CA handled in instance A, which
also is not transparent: CA state, ECM handling and the descrambler have all
moved upstream of the shim.

## R5 — Throughput: not viable as specified

The buffers in the current path (`app_ts_record.c:17-18`) are:

```c
#define SW_BUFFER_SIZE  (3*188*1024)   /*  577,536 B */
#define HW_BUFFER_SIZE  (1*188*1024)   /*  192,512 B */
```

At the measured 59.145 Mb/s = 7.393 MB/s:

| buffer | today @ ~2.5 Mb/s | @ 59.145 Mb/s |
|---|---|---|
| DVR SW | 1.85 s | **78 ms** |
| DVR HW | 616 ms | **26 ms** |
| `RIST_FIFO_SIZE` 963,584 B | 3.1 s | **130 ms** |

(Your 190 ms for the FIFO used the old 40 Mb/s figure: 963,584 B = 7,708,672
bits, ÷ 59,145,000 = 130 ms. It is worse than you assumed, not better.)

78 ms of DVR slack is below the stall duration that was losing whole datagrams
before the `SO_RCVBUF` fix. That alone is disqualifying, but it is not the
binding constraint. Four things compound:

1. **Memory.** Holding 4 s at 59.145 Mb/s is **29.6 MB** for the repair buffer
   alone, on a 128 MB box whose DDR is already carved into memhole regions for
   esv/esa/vfb/afb/pcm/vpu (`platform/gxbus/player/avutil/memhole.c:27-37`).
2. **Contiguity.** `SW_BUFFER_SIZE`/`HW_BUFFER_SIZE` are `GxCore_MemholeMalloc`
   (`app_ts_record.c:961-962`) — physically contiguous. Restoring today's 1.85 s
   of slack at 24× the rate needs ~13.9 MB + 4.6 MB **contiguous**, and the
   memhole sizes come from the kernel command line, so growing them is a **BOOT
   change** — outside the app-only constraint.
3. **Copy cost.** Every byte crosses demux → memhole → userspace → memhole →
   demux: ~15 MB/s of memcpy plus per-packet inspection of 39,325 packets/s, on
   a Cortex-A7 pair already running decode, two RIST endpoints and the capture.
4. **Waste.** The tuned service is ~3% of the multiplex. This transports,
   inspects and repairs 97% of bytes nobody is watching.

**Assessment: full-multiplex userspace handling at 59 Mb/s is not viable on this
hardware app-only.** It is not a tuning problem. The honest answer is the one
you asked for in advance: **this pushes back to a reduced PID set.**

## R6 — Where the code sits

The transparent full-multiplex shim is **not achievable**: R1 negative, R5
negative, and R4(c) shows the shape is self-contradictory where CA is concerned.

**Nearest achievable shape** — extend the existing capture rather than replace
the demux position:

| what | where |
|---|---|
| slot set is built | `_ts_rec_prog_config()`, `app_ts_record.c:1645` |
| extra PIDs added | `_ts_rec_demux_ext_slot_alloc()`, `app_ts_record.c:1127` |
| our binary reads | `_rist_reader()`, `app_rist_capture.c:857`, via `app_ts_record_read()` |
| re-injection would attach | second instance, `DVR_INPUT_MEM` → `DVR_OUTPUT_DMX`, pattern from `gxfrontend_net.c:118-143` |
| chain is spawned | `_rist_chain_start()`, `app_rist_capture.c:525` |

Widen the slot set at `_ts_rec_prog_config()` to PSI/SI, CAT, PMT, PCR and the
tuned service's ES — a reduced set, not the multiplex — and, if downstream
transparency is wanted, re-inject into a spare instance (dmx3; dmx0/1 are the AV
path and dmx2 is the capture).

**What it replaces:** on the box, `stb_part8_receiver` replaces
`stb_part7_receiver` in the argv built by `_rist_chain_start()`. The chain
topology and the headend are unchanged.

**R2 decides one thing only:** whether the wider slot set is "list the PIDs we
need" or "one `DEMUX_SLOT_MUXTS` slot". It does not revive the transparent shim,
because of R4(c) and R5.

---

## What to run on the box

**R2, the MUXTS test** — the only genuinely open question that code can settle.
In `_ts_rec_prog_config()`, before the existing video/audio allocations, alloc
one slot with `.type = DEMUX_SLOT_MUXTS`, `.flags = (DMX_TSOUT_EN |
DMX_CRC_DISABLE | DMX_REPEAT_MODE)`, no `.pid`, pointed at the same DVR handle.
Then read the existing `[DVB2IP] DIAG` classification line and the reader's
`B/s`. Full multiplex present → ~7.4 MB/s and PIDs from other services in the
capture. Rejected → `SlotAlloc` returns < 0, which is equally conclusive.

**R4(c), the descrambler test** — set `DMX_DES_EN` on a slot of a
`DEMUX_SDRAM`-sourced instance fed with scrambled TS and see whether output is
plaintext. Only worth doing if the reduced-PID shape still wants CA downstream.

**Sizes, no flash needed** — `cat /proc/cmdline` gives the memhole region sizes
(`tswmem`, `tsrmem`, `esvmem`, `esamem`, `vfbmem`, …) that R5's contiguity
argument turns on. Worth having before any decision to grow them.

## The header, for the record

`GXLIB_PATH` resolves via `solution/Makefile:304-308`: the in-tree
`$(GXSRC_PATH)/../library/goxceed/$(CROSS_PATH)` if `../library/goxceed` exists,
otherwise `$(OPT)/opt/goxceed/$(CROSS_PATH)`. The in-tree directory does not
exist, so on the toolchain server the headers are under
`/opt/goxceed/arm-linux/include/`.

```
find /opt/goxceed -name '*demux*'
ls /opt/goxceed/arm-linux/include/
```

**This is now a cross-check, not a dependency.** Every enum above came from
DWARF and every macro value from disassembly; the header would only confirm
them. Worth one command if it is cheap, not worth blocking on.

## Flag values recovered by disassembly

From `.text.tsbuff_config` in `libgxplayer.a`, against the DWARF line table for
`dvbsource_tsbuff.c`:

- line 175, `slot.flags = (DMX_TSOUT_EN|DMX_CRC_DISABLE|DMX_REPEAT_MODE)`
  → `mov r2, #0x13` … `str r2, [sp, #0x38]` → **0x13**
- line 182, `slot.flags |= DMX_ERR_DISCARD_EN`
  → `mov lr, #0x93` … `str lr, [sp, #0x38]` → **0x93**

Therefore **`DMX_ERR_DISCARD_EN = 0x80`**. The same disassembly confirms
`slot.type = 9` for video, matching `DEMUX_SLOT_VIDEO=9`, and a 20-byte
`GxDemuxProperty_Slot` with `flags` at offset 0x10 — consistent with
`{slot_id, type, pid, ts_out_pin, flags}` as recovered from DWARF.

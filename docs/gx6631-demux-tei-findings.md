# GX6631 demux: error-discard flag and TEI handling — source findings

Factual record of what the SDK tree does and does not say about whether
transport_error_indicator (TEI) packets reach a TS-output slot. No design
content: this exists so the next person does not have to re-derive it, and so
the empirical test has a written starting point.

Tree examined: `6631SDK/` as checked in. Line numbers are from that tree.

---

## 1. `DMX_ERR_DISCARD_EN` exists, is per-slot, and is opt-in

The flag is applied to individual slots with `|=` after a base flag set has been
assigned, which is only meaningful if a slot without it behaves differently:

| file | line | code |
|---|---|---|
| `platform/gxbus/player/access/dvbsource_tsbuff.c` | 175 | `slot.flags = (DMX_TSOUT_EN \| DMX_CRC_DISABLE \| DMX_REPEAT_MODE);` |
| | 182 | `slot.flags \|= DMX_ERR_DISCARD_EN;` (video) |
| | 195 | `slot.flags \|= DMX_ERR_DISCARD_EN;` (audio) |
| `platform/gxbus/player/access/dvbsource_tscache.c` | 339 | `slot.flags \|= DMX_ERR_DISCARD_EN;` (video) |
| | 352 | `slot.flags \|= DMX_ERR_DISCARD_EN;` (audio) |
| `platform/gxbus/player/demuxer/hw_demux.c` | 533, 1491 | set together with `DMX_AVOUT_EN` on decode paths |

Note the base TS-output flag set at `dvbsource_tsbuff.c:175` does **not** include
it. It is added per elementary stream afterwards.

## 2. The RIST capture path applies it ASYMMETRICALLY

`solution/app/dvb2ip_server/app_ts_record.c`:

| slot | flags set at | value | discard flag |
|---|---|---|---|
| **video** | 1680, used 1682 | `DMX_REPEAT_MODE\|DMX_PTS_TO_SDRAM\|DMX_TSOUT_EN\|DMX_DES_EN` | **absent** |
| **audio** | 1679, used 1688 | as above **+ `DMX_ERR_DISCARD_EN`** | **present** |
| PCR PID | 1079, used 1080 | `DMX_REPEAT_MODE\|DMX_TSOUT_EN` | absent |
| PMT PID | 1089, used 1090 | `DMX_REPEAT_MODE\|DMX_TSOUT_EN` | absent |
| ext PIDs | 1127, used 1128 | `DMX_REPEAT_MODE\|DMX_TSOUT_EN` | absent |

`_ts_rec_demux_slot_alloc()` (line 1001) does not modify the flags it is given:
`slot.flags = slot_flags` on first allocation (1017), `slot.flags |= slot_flags`
when an existing slot for the same PID is reused (1026). The asymmetry is
therefore real as written, not an artefact of the allocator.

Consequence for reuse: flags ACCUMULATE across allocations of the same PID. For a
service whose PCR PID equals its video PID, the video slot is allocated first
(1682) and the PCR allocation (1080) then ORs in only
`DMX_REPEAT_MODE|DMX_TSOUT_EN`, so no discard flag is acquired.

## 3. What is NOT in the tree

The flag's definition and its hardware semantics are not present:

- `DMX_ERR_DISCARD_EN`, `DMX_TSOUT_EN`, `DMX_REPEAT_MODE`, `DMX_PTS_TO_SDRAM`,
  `DMX_AVOUT_EN`, `DMX_DES_EN`, `DMX_CRC_DISABLE` appear **only in `.c` files**.
  No header in the tree defines any of them.
- `GxDemuxPropertyID_SlotConfig` / `_SlotAlloc` / `_SlotEnable` appear only inside
  prebuilt archives (`platform/*/\_build-arm-ecos/*.a`, `*.o`).
- `platform/gxloader/include/driver/` contains `gxav_vout_propertytypes.h` and
  `gxav_vpu_propertytypes.h` but **no `gxav_demux_propertytypes.h`**.
- `platform/gxbus/player/include/gx_demux.h` exists but defines demuxer types and
  pin names only — no slot flags.
- The register-level loader demux, `platform/gxloader/drivers/demux/demux_smp.c`,
  contains no TEI or error handling at all.

## 4. The only in-tree TEI parsing is the SOFTWARE demux

`platform/gxbus/si/soft_demux/soft_demux.c`:
- line 25: `uint32_t transport_error_indicator : 1;`
- line 110: `tTSHeadInfo.transport_error_indicator = (*(ucPayloadUnit+1))>>7;`

This is a separate userspace path and says nothing about hardware demux
behaviour.

## 5. Signal-quality counters available for an independent measurement

Taken before the demux, so usable as evidence that an impairment actually
degraded reception:

- `platform/gxbus/include/module/frontend/gxfrontend_module.h:108-111`
  ```c
  typedef struct {
      uint32_t snr;
      uint32_t error_rate;
  } GxFrontendSignalQuality;
  ```
- `gxfrontend_module.h:245` — `GxFrontend_GetQuality(int32_t tuner, GxFrontendSignalQuality *sq)`
- `gxfrontend_module.h:241` — `GxFrontend_QueryStatus(int32_t tuner)` returns 1 when locked
- `solution/app/include/module/app_ioctl.h:38-40` —
  `FRONTEND_STRENGTH_GET`, `FRONTEND_QUALITY_GET`, `FRONTEND_ERRORRATE_GET`
- `solution/app/module/nim/app_nim_dvbs.c:330-366` — the DVB-S path reading
  `sq.snr` and `sq.error_rate`

BER encoding, from `solution/app/module/app_utility.c:903-952`: the value returned
by `FRONTEND_ERRORRATE_GET` packs mantissa in bits 8-19 and exponent in bits 0-3,
displayed as `%d.%02dE-%02d` from `mantissa/100` and `exponent`.

## 6. Questions the source cannot answer

1. Does "error" in `DMX_ERR_DISCARD_EN` mean TEI specifically, or also CRC and
   descrambling failures?
2. Does a slot **without** the flag pass TEI-marked packets through, or does the
   demux drop them at input regardless, making the flag control something else?
3. Does the flag gate the slot's output or the demux input?

These require measurement on hardware. The asymmetry in section 2 is what makes
that measurement tractable: video and audio slots on the same service, under the
same impairment at the same instant, differ only in this flag.

## 7. Separate issue, not Part 8

The video slot in the capture path carries no error-discard flag while the audio
slot does. If the flag means what its name implies, corrupt video packets reach
the decoder. This appears unintentional rather than deliberate and is recorded
here for its own investigation; it is not a Part 8 matter.

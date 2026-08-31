# All-pass on the box — P1 experiment and P2 re-cost

Two prerequisites for the all-pass proposal. P1 is code to flash. P2 is a
re-costing, and its honest conclusion is that **P1's flash answers most of P2**
— so they are one trip, not two.

---

## P2 — Re-costing R5 for this topology

**Your reading is right: the ingest is the constraint, not the buffer, and I
bundled them before.** But I cannot give you (d) — a sustainable rate — from the
code, and I should say that plainly rather than produce a number with false
precision. What I *can* do is make P1 measure it directly, which is what the
instrumentation below is for.

### (a) Is 78 ms of contiguous capture buffer survivable?

**Probably, and it is the right question — but it is not answerable from source.**

The numbers stand: `SW_BUFFER_SIZE = 3*188*1024` = 577,536 B and
`HW_BUFFER_SIZE = 188*1024` = 192,512 B (`app_ts_record.c:17-18`), both
`GxCore_MemholeMalloc` (`:961-962`), so contiguous and boot-sized. At 7,393,125
B/s that is **78.1 ms** and **26.0 ms**.

What changed in your favour is what has to happen inside that window. In the
transparent-shim costing, userspace had to validate, hold and reconstruct before
the buffer drained. Here the reader does `GxAVModuleRead` → `ts_multi_fifo_write`
→ `sendto`, and librist owns everything after that. The window only has to cover
**scheduling jitter plus one memcpy plus one syscall**, not a processing stage.

That is a much weaker requirement, and 78 ms is a long time for it. But "78 ms of
slack on a non-RT Linux 4.9 with a decoder running" is a claim about scheduling
behaviour under load, and no amount of reading `app_ts_record.c` settles it.

**The good news: an overrun is not silent.** If the DVR buffer overflows, packets
are dropped, and continuity counters break. So a CC discontinuity count over an
all-pass capture *is* the overrun detector, needs no new driver API, and is
already what P1 has to measure anyway.

### (b) The CPU cost — affordable, and not the thing to worry about

Working it through at 59.145 Mb/s:

| item | rate | estimated cost |
|---|---|---|
| `GxAVModuleRead` → fifo memcpy | 7.4 MB/s | ~1–2% of one core |
| `sendto` of 1316-byte datagrams | 5,618/s | ~1–2% of one core |
| loopback `recvfrom` into librist | 5,618/s | ~1–2% of one core |
| CC inspection, 4 bytes + counter per packet | 39,325/s | well under 1% |

Call it **5–10% of one A7 core** for the capture-and-packetise path. That is
affordable. The transparent-shim figure was dominated by the hold buffer and
reconstruction, which are not in this design.

**One thing to note rather than assume away:** the chain today crosses a UDP
loopback socket between the capture and librist (`udp://127.0.0.1:6000`,
`app_rist_capture.c:107-112`). At 59 Mb/s that is ~11,200 syscalls/s and two
extra copies of every byte. Still affordable, but it is pure overhead that an
in-process packetiser would not pay, and it is the first thing to remove if the
measurement comes back tight.

### (c) `RIST_FIFO_SIZE` — raise it freely, it is heap

**Heap, not memhole.** `ts_multi_fifo_init()` (`app_ts_record.c:328-353`) does:

```c
#if DVB2IP_SERVER_USE_STATIC_SCREEN
    GxCore_HwMalloc(&fifo->hwobj, MALLOC_WITH_CACHE);
#else
    if ((fifo->ptr = calloc(1, size)) == NULL)
#endif
```

and `DVB2IP_SERVER_USE_STATIC_SCREEN` is **0** (`app_config.h:127`). So the
963,584-byte FIFO is an ordinary `calloc`, and `RIST_FIFO_SIZE`
(`app_rist_capture.c:64`) can be raised to whatever heap allows.

At 59.145 Mb/s the current size is **130 ms**. I would raise it to at least 4 MB
(~540 ms) for the experiment — it costs nothing structurally and it decouples the
FIFO from the question you actually care about, which is the memhole-backed DVR
buffer you cannot grow.

**This matters more than it looks.** It means the only boot-sized constraint in
the whole path is the 78 ms DVR buffer. Everything else — the FIFO, librist's
~29.6 MB receive buffer — is heap, on a 128 MB box. Tight, not blocked, exactly
as you read it.

### (d) The sustainable rate — I will not guess

The chain is: demux TS-out → DVR → memhole buffer → `GxAVModuleRead` → heap FIFO
→ `sendto`. Only the first three are opaque, and they are the ones that decide
the answer. The DVR was designed for single-service recording at a few Mb/s; the
capture path has only ever been run at ~2.5 Mb/s.

**Refusing to give a number here is the honest answer, and P1 turns it into a
measurement:** the same flash that settles whether MUXTS works also runs the
capture at whatever rate all-pass actually delivers, and reports achieved rate
plus CC discontinuities. If it reports 59 Mb/s with no CC breaks, the topology is
viable. If it reports 30 Mb/s, or 59 Mb/s with continuous CC breaks, a reduced
PID set is forced and we go back to the filtered design.

**So P1 and P2(d) are one experiment.** That is the reason to instrument it
properly rather than just checking the return code.

---

## P1 — The all-pass experiment

Runtime-gated so one flash covers both arms. All of it is inside
`#if DVB2IP_SERVER_SUPPORT` in `app_ts_record.c`.

### Gate and state — near the other `/tmp` knobs (~line 155)

```c
/* All-pass (DEMUX_SLOT_MUXTS) experiment. `echo 1 > /tmp/ristallpass` then start
 * ONE stream. Default 0 = today's per-PID slots, so a bad result is one file
 * away from the working box. */
#define TS_REC_ALLPASS_FILE   "/tmp/ristallpass"
static int      s_ts_rec_allpass    = 0;
static int32_t  s_ts_rec_mux_slotid = -1;

/* Measurement window over the all-pass capture. */
static int      s_ap_active   = 0;
static uint64_t s_ap_t0_ms    = 0;
static uint64_t s_ap_bytes    = 0;
static uint32_t s_ap_pkts     = 0;
static uint32_t s_ap_badsync  = 0;
static uint32_t s_ap_ccerr    = 0;
static uint8_t  s_ap_seen[8192];      /* PID present bitmap  */
static uint8_t  s_ap_cc[8192];        /* last CC seen, 0xFF = none yet */
```

`_ts_rec_modid_refresh()` already reads the other knobs; add:

```c
fp = fopen(TS_REC_ALLPASS_FILE, "r");
if(fp) { int v = 0; if(fscanf(fp, "%d", &v) == 1) s_ts_rec_allpass = (v != 0); fclose(fp); }
```

### The allocation — in `_ts_rec_prog_config()`, before the video/audio slots

`_ts_rec_demux_slot_alloc()` cannot be reused: it rejects on `!VALID_PID(pid)`
(`:1008`) and a MUXTS slot carries no PID. This allocates directly, exactly as
the vendor's autotest does (`module_tsfilter.c:119-137`) — `.type` and `.flags`
only, **`.pid` left at zero by the memset and deliberately not assigned**.

```c
if(s_ts_rec_allpass)
{
    GxDemuxProperty_Slot mslot;
    int32_t ret;

    memset(&mslot, 0, sizeof(GxDemuxProperty_Slot));
    mslot.type  = DEMUX_SLOT_MUXTS;
    /* Same flags as the per-PID capture minus DES_EN: we want SCRAMBLED bytes.
       0x1a = REPEAT_MODE|PTS_TO_SDRAM|TSOUT_EN. No .pid -- that is the test. */
    mslot.flags = (DMX_REPEAT_MODE | DMX_PTS_TO_SDRAM | DMX_TSOUT_EN);

    ret = GxAVGetProperty(ctrl->dev, ctrl->dmx_handle,
                          GxDemuxPropertyID_SlotAlloc,
                          &mslot, sizeof(GxDemuxProperty_Slot));
    if(ret < 0)
    {
        printf("[ALLPASS] SlotAlloc(DEMUX_SLOT_MUXTS, no pid) REFUSED, ret=%d "
               "-> all-pass NOT available on dmx%d\n", ret, s_ts_rec_modid);
        s_ts_rec_allpass = 0;          /* fall back to per-PID slots below */
    }
    else
    {
        s_ts_rec_mux_slotid = mslot.slot_id;
        printf("[ALLPASS] SlotAlloc OK: slot_id=%d flags=0x%x type=%d on dmx%d\n",
               mslot.slot_id, mslot.flags, mslot.type, s_ts_rec_modid);

        ret = GxAVSetProperty(ctrl->dev, ctrl->dmx_handle,
                              GxDemuxPropertyID_SlotConfig,
                              &mslot, sizeof(GxDemuxProperty_Slot));
        printf("[ALLPASS] SlotConfig ret=%d\n", ret);

        ret = GxAVSetProperty(ctrl->dev, ctrl->dmx_handle,
                              GxDemuxPropertyID_SlotEnable,
                              &mslot, sizeof(GxDemuxProperty_Slot));
        printf("[ALLPASS] SlotEnable ret=%d -> measuring 10s\n", ret);

        memset(s_ap_seen, 0, sizeof(s_ap_seen));
        memset(s_ap_cc, 0xFF, sizeof(s_ap_cc));
        s_ap_bytes = 0; s_ap_pkts = 0; s_ap_badsync = 0; s_ap_ccerr = 0;
        s_ap_t0_ms = 0;                /* set on first read */
        s_ap_active = 1;
    }
}
```

Then guard the existing per-PID allocations with `if(!s_ts_rec_allpass)` so the
two arms are exclusive. **A refusal falls back to today's behaviour**, so a
failed experiment still leaves a working box.

### The measurement — in the DVR read loop, beside the existing DIAG

Insert immediately after the `s_ts_rec_diag_rearm` block (`~:817`), inside the
`read_len > 0` branch:

```c
if(s_ap_active)
{
    int i;
    if(s_ap_t0_ms == 0) s_ap_t0_ms = _ts_rec_now_ms();   /* or GxCore tick */

    s_ap_bytes += read_len;
    for(i = 0; i + 187 < read_len; i += 188)
    {
        uint8_t *p = buffer + i;
        uint16_t pid;
        uint8_t  cc;

        s_ap_pkts++;
        if(p[0] != 0x47) { s_ap_badsync++; continue; }

        pid = ((p[1] & 0x1F) << 8) | p[2];
        s_ap_seen[pid] = 1;

        if(pid == 0x1FFF) continue;              /* nulls do not carry CC */
        if(p[1] & 0x80)   continue;              /* TEI: CC untrustworthy */
        if(!(p[3] & 0x10)) continue;             /* no payload: CC does not advance */

        cc = p[3] & 0x0F;
        if(s_ap_cc[pid] != 0xFF && cc != ((s_ap_cc[pid] + 1) & 0x0F))
            s_ap_ccerr++;
        s_ap_cc[pid] = cc;
    }

    if(_ts_rec_now_ms() - s_ap_t0_ms >= 10000)
    {
        uint64_t ms = _ts_rec_now_ms() - s_ap_t0_ms;
        int npid = 0;
        for(i = 0; i < 8192; i++) if(s_ap_seen[i]) npid++;

        printf("[ALLPASS] %llu bytes / %llu ms = %llu kbit/s  packets=%u\n",
               (unsigned long long)s_ap_bytes, (unsigned long long)ms,
               (unsigned long long)(s_ap_bytes * 8ULL / (ms ? ms : 1)),
               s_ap_pkts);
        printf("[ALLPASS] distinct PIDs=%d  badsync=%u  CC discontinuities=%u\n",
               npid, s_ap_badsync, s_ap_ccerr);
        printf("[ALLPASS] PIDs:");
        for(i = 0; i < 8192; i++) if(s_ap_seen[i]) printf(" %04x", i);
        printf("\n");
        s_ap_active = 0;
    }
}
```

`_ts_rec_now_ms()` — reuse whatever monotonic millisecond helper the file already
has; `app_rist_capture.c` has `_rist_now_ms()` if none is handy locally.

### Also bundle, since a flash cycle is expensive

**The demux hardware counts, still never printed.** One call, at
`app_ts_record_init()`:

```c
#include "sub_system/dmx_sub_system/dmx_sub_system.h"
{
    uint32_t dmx_num = 0, chan_num = 0;
    GxSubsystem_DmxGetHardwareNum(&dmx_num, &chan_num);
    printf("[DVB2IP] DMX_SUB_NUM=%u  channel_num(max slot/filter)=%u\n",
           dmx_num, chan_num);
}
```

Source says this is a stub returning 2 and max(64,64) (`dmx_sub_system.c:178-189`),
so it should print `2` and `64`. **If it prints anything else, the stub is not
what runs** and several conclusions need revisiting — which is exactly why it is
worth one printf.

**The TSW firewall state**, deferred since the at-rest protection work:

```c
printf("[DVB2IP] firewall_flag=0x%x\n", GxAvdev_GetFirewallFlag());
```

That settles per-instance versus global TSW protection directly, instead of
inferring it from whether the capture reads as ciphertext.

**And from the shell, no flash needed:** `cat /proc/cmdline` for the memhole
region sizes (`tswmem`, `tsrmem`, `esvmem`, …). Worth capturing in the same
session since P2's contiguity argument turns on them.

### How to read the result

| observation | conclusion |
|---|---|
| `SlotAlloc ... REFUSED, ret=<0` | **All-pass is not available.** Conclusive; go back to the filtered design. |
| Alloc OK, but rate ≈ 0 / `distinct PIDs` ≤ 2 | Slot allocated but delivers nothing — "silently empty", the case you were right to guard against. Treat as a negative. |
| Alloc OK, ~59,000 kbit/s, ~190 distinct PIDs, CC ≈ 0 | **All-pass works and the rate is sustainable.** P1 and P2(d) both answered; proceed. |
| Alloc OK, ~59,000 kbit/s, CC discontinuities climbing | All-pass works, **the pipeline cannot keep up** — the 78 ms DVR buffer is overrunning. Reduced PID set forced. |
| Alloc OK, rate well below 59,000 kbit/s | Something upstream is rate-limiting. Report the number; that is P2(d). |

The reference capture had **195 distinct PIDs**, so ~190 is the number to expect.
Anything in the tens means it is not really all-pass.

Two cautions:

- **Run it with the recovery chain off** (`echo 0 > /tmp/ristchain`, then zap) so
  nothing else is competing for the capture, and so a failure cannot take the
  picture down with it.
- **CC will not be zero even when healthy.** PID `0x03E8` carries two interleaved
  continuity counters and produced 164 CC errors in four clean seconds on this
  transponder. Expect a few hundred per 10 s window at baseline; what matters is
  whether the count is stable or climbing.

---

## D4 under all-pass — unchanged, and slightly better

The periodic, phase-carrying, self-validating anchor is still needed, and it
works unchanged. Two things do improve:

- **The phase component gets easier.** With no filtering, phase is a property of
  where each side started grouping the full multiplex, not of a filter's output.
  A PCR-bearing packet identifies a position in the multiplex unambiguously and
  both sides can compute the same offset from it.
- **Re-anchoring gets cheaper to validate.** Under all-pass both sides hold the
  same bytes, so an anchor reply carrying (sequence, phase, PCR) can be checked
  against the box's own buffer directly. Divergence becomes detectable rather
  than merely bounded — which is what turns D4 from silent corruption into a
  re-sync event.

D4 itself does not go away: two receptions, one loss at the headend, permanent
offset. Periodic re-anchoring is still the answer.

## What P1 does not settle

Worth stating so nothing is assumed later:

- **Q1** — whether a descrambler binds to a memory-fed instance — is untouched by
  this experiment and still open.
- **Whether an all-pass slot is still all-pass with `DES_EN` set** is a separate
  question. The experiment above deliberately clears it (flags `0x1a`), which is
  what the topology wants, but if MUXTS and per-slot descrambling turn out to
  interact, that is found later.
- **The 78 ms buffer under a loaded box.** The measurement runs while the box is
  otherwise idle-ish. A quiet 10 s window passing is necessary, not sufficient;
  re-run it with the decoder and UI busy before trusting it.

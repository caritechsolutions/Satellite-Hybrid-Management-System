# Part 8 box — demux 0 fed from the RIST receiver, full time

The target architecture. `tuner → dmx2 capture → cutter → local RIST sender →
local RIST receiver → demux 0 → every consumer already bound to demux 0.` No
runtime source switching: demux 0 is on the memory source for the whole life of
a Part 8 channel and back on the tuner the moment the flag comes off.

Two things were to be confirmed before building. Both are answered below, and
one of them changed the shape of the build.

---

## First: two corrections to the brief

### `g_hDemux` does not exist

> "the instance the consumers are ALREADY bound to via the `g_hDemux` global"

There is no such symbol anywhere in the SDK tree — `grep -rn g_hDemux
--include=*.c --include=*.h 6631SDK/` returns nothing. The consumers are not
bound through a shared handle. They are bound through a **pair of numbers**,
`g_AppPlayOps.normal_play.{ts_src, dmx_id}`, which each consumer reads and
passes down at start:

| consumer | how it binds |
|---|---|
| `app_sdt` / `app_pat` | `app_sdt_start(g_AppPlayOps.normal_play.dmx_id, …)` (`app_play_control.c:2055,2058`) |
| `app_epg` | `epg_cfg.dmx_id` / `epg_cfg.ts_src` → `GxDmxSetSource(epg_cfg.dmx_id, epg_cfg.ts_src)` (`gxepg_table.c:123`) |
| `app_time` | `time.ts_src = g_AppPlayOps.normal_play.ts_src` (`app_time.c:352`) |
| TDT (`extra_sync_time`) | `subt_detail.ts_src = ts_src`, `demux_id` left 0 by the `memset` (`gxextra.c:98,108`) |
| ECM / CAT / EMM | `app_demux_param_adjust(&ts_src, &demux_id, 0)` (`app_pmt.c:1019`, `app_cat.c:751`, `app_emm.c:177`) |

That matters, because a handle you can repoint is a different problem from a
number every consumer copies. It also means the lever is smaller than the brief
assumed, not larger.

### The claim that U1 is already confirmed by file playback

> "U1 already confirmed a memory-fed demux runs the full consumer chain
> (file-playback path, GXMEDIA_SOURCE_TS)."

I flagged last turn that file playback might section-filter in **software**,
which would make it evidence for decode only. **I withdraw that objection.**
`si/soft_demux/soft_demux.c` is dead code: `SOFT_DEMUX` is `#define`d to `(0)`
at `si_filter.c:31` and defined nowhere else, so the `#else` branch that uses
it (`si_filter.c:810-989`) never compiles. There is no software section demux
in this build; every SI consumer goes through the hardware slot+filter path at
`si_filter.c:358`.

That removes the counter-evidence. It does **not** turn into proof: nothing in
this SDK demonstrates a hardware section filter firing on a demux whose source
is `DEMUX_SDRAM`. What is now true is that the file-playback path and the SI
path use the same hardware filters, so if file playback really did run SI
tables here, U1a follows. Architecturally it should hold — `source` selects
what feeds the demux's packet processor, and slots and filters sit after it —
and this build is the way to find out.

---

## The two questions, answered

### Q1 — can demux 0 be put on the memory source without disturbing the consumers?

**Not by setting the source behind their backs. Yes, by setting the one number
they all read.**

Why the direct approach is wrong, from the code:

- **`dmx_sub_system.c:249-262`** keys a demux instance on `(dmx_id, ts)`. A
  second opener asking for the same `dmx_id` with a **different** `ts` gets a
  hard `return -1` and `"dmx sub already open!"`. Two owners disagreeing about
  the source is not a race that settles — it is an error return that kills
  whoever asked second.
- **`si_filter.c`'s `ts_demux_connect()` (:213)** writes `config_demux.source =
  ts_src` **only on the first open**. `s_Demux[id].handle` is never zeroed,
  because the close inside `ts_demux_disconnect()` is `#if 0`'d out
  (`:201-209`). So after boot the SI layer's opinion of the source is frozen
  and its `ts_src` argument is ignored — good news in one direction (SI can
  never slam demux 0 back to the tuner behind the player) and a trap in the
  other (a source we set unilaterally is invisible to it).

What **does** write the source, unconditionally, on every single play:

```c
/* platform/gxbus/player/access/dvbsource_normal.c:33-46 */
GxUrl_GetItem(url, GX_URL_KEY_TSID,  &tsid);
GxUrl_GetItem(url, GX_URL_KEY_DMXID, &dmxid);
dn->dmx = GxAvdev_OpenModule(dn->dev, GXAV_MOD_DEMUX, dmxid);
cfg_dmx.source = tsid;
GxAVSetProperty(dn->dev, dn->dmx, GxDemuxPropertyID_Config, &cfg_dmx, ...);
```

and the URL is built from that same pair:

```c
/* app_play_control.c:962 */
sprintf(url_tmp, "&tsid:%d&dmxid:%d&progid:%d", ts_src, dmx_id, prog->id);
```

**The SDK already has this exact branch, and it is live on this build.**
`app_play_control.c:1089-1098`, under `PDMX_SUPPORT` (which is `1`):

```c
g_AppPlayOps.normal_play.dmx_id = config.dmx_id;      /* unchanged */
if (app_program_need_predemux(prog_node) && !app_frontend_hard_cap(...))
    g_AppPlayOps.normal_play.ts_src = 3;              /* SDRAM */
else
    g_AppPlayOps.normal_play.ts_src = config.ts_src;  /* tuner */
```

`app_demux_param_adjust()` (`app_extend.c:227`) is the same idea for timeshift:
`if (exshift_flag || cmshift_flag) *ts_source = 3;` — currently inert
(`EX_SHIFT_SUPPORT 0`, `CMM_SUPPORT 0`), but it is the platform saying, in its
own code, that "insert a stage between tuner and consumers" means **change
ts_src, keep dmx_id**.

So the answer is yes, the bindings survive — because nothing about them
changes. Every consumer keeps `dmx_id` 0. Only where demux 0 gets its bytes
changes, and it changes through the one write the player already performs.

### Q2 — does reverting `/tmp/ristp8` cleanly put demux 0 back on the tuner?

**Yes, on the next zap, with no sticky state and no reboot.**

`ts_src` is recomputed from scratch in `app_normal_play()` on every zap, and
`dvbnormal_config()` rewrites `cfg_dmx.source` on every play. `echo 0 >
/tmp/ristp8` and change channel: the URL carries `&tsid:<tuner>` and demux 0 is
back on the tuner.

The one layer that *is* sticky — `si_filter.c`'s frozen `s_Demux[0].source` —
is harmless in both directions, because it is only ever written on the first
open at boot and the player's write is what the hardware actually ends up with.

---

## What was built

### The tail selector gains a third value

| `/tmp/ristp8` | `/tmp/ristp8tail` | result |
|---|---|---|
| absent / 0 | — | factory decode, untouched |
| 1 | absent or `player_av` | **Step 1** — `player_av` on the loopback (proven) |
| 1 | `dmx3` | **Step 2** — reinjection into a spare demux (experiment, unrun) |
| 1 | `dmx0` | **this** — demux 0 fed from the repaired stream |

Anything unrecognised falls back to `player_av`, so a typo degrades to the path
that works.

### The division of labour, which is the whole design

**`app_play_control.c` sets one variable.** A new branch above the pdmx one,
guarded by `DVB2IP_SERVER_SUPPORT`:

```c
int p8_dmx0 = app_rist_p8_dmx0_wanted(&prog_node->prog_data);
if (p8_dmx0)                       g_AppPlayOps.normal_play.ts_src = 3;
else if (app_program_need_predemux(...)) g_AppPlayOps.normal_play.ts_src = 3;
else                               g_AppPlayOps.normal_play.ts_src = config.ts_src;
```

`dmx_id` is not touched. The pdmx branch is not touched — it is now an `else
if`, reached identically whenever Part 8's dmx0 tail is off, which is always
unless someone has echoed `dmx0` into the knob.

**`app_rist_capture.c` owns only the memory side.** It never opens or
configures demux 0. It opens `GXAV_MOD_DVR 0`, configures `src = DVR_INPUT_MEM,
dst = DVR_OUTPUT_DMX`, runs it, and writes the receiver's output in — the
sequence in `_GxFrontendNet_TSRConfig()` (`gxfrontend_net.c:118-143`) and its
writer at `:316/:334`, which is the one complete and readable implementation of
this direction in the tree.

DVR 0 is free while we run: the dmx2 capture opens DVR **2**
(`app_ts_record.c:1149`). The only other claimant of DVR 0 is
`GxMedia_DemuxOpen()`, which hardcodes it (`gxmedia_demux.c:138,150`) — that is
the dmx3 tail, and the two tails are alternatives, never concurrent.

### The screen suppression is INVERTED on this tail

Step 1 and Step 2 both take the video decoder away from normal play, so
`app_rist_screen_enabled()` returns true and `app_normal_play` calls
`app_player_close(PLAYER_FOR_NORMAL)`.

Here normal play **is** the decode path — just sourced from SDRAM. Suppressing
it would close the very player whose `&tsid:3` URL points demux 0 at memory,
and nothing would decode at all. So `app_rist_screen_enabled()` returns 0 for
the dmx0 tail.

### Broadcast PSI defaults ON for this tail

`/tmp/ristp8psi` stays default-off for `player_av` and `dmx3`; on `dmx0` it
defaults **on**. That is not a preference. `app_sdt`, `app_epg` and `app_time`
section-filter demux 0, and once demux 0 is fed from memory the only tables
they can ever see are the ones we captured. Without broadcast PSI the capture
carries the box's own generated PAT/PMT and no SDT/EIT/TDT at all, so service
names, EPG and the clock would go stale the moment the tail was selected —
silently, and looking exactly like a fade.

The "player picks program 0 of 34" risk that kept it off for Step 1 does not
apply here: the player is handed `vpid`/`apid`/`pcrpid` explicitly on the URL
and never picks a program out of the PAT.

### Buffer: 2000 ms

Part 8's local pair gets its own default (`RIST_P8_BUFFER_MS`), separate from
Part 7's 4000. The 4000 is sized for the recovery peer over the public internet
— multi-NACK depth against a ~985 ms RTT. Part 8's hop is 127.0.0.1 to
127.0.0.1: no recovery peer, no NACK round trip worth budgeting for, and the
buffer is paid for twice over in zap-to-picture. `/tmp/ristbuffer` still
overrides.

### Fallback

Three layers, all of which end in a normal tuner picture rather than a black
screen:

1. **Chain failed to spawn** — `chain_active`, `p8_active` and `p8_dmx0_want`
   are all cleared before `app_normal_play` reads the suppression decision.
2. **DVR feed failed to start** — `_rist_p8_dmx0_start()` returns <0 with
   everything released. `app_play_control` then sees `ts_src == 3` with
   `app_rist_p8_dmx0_asked() && !app_rist_p8_dmx0_active()` and **rebuilds the
   URL on the tuner source before `GxPlayer_MediaPlay()` is issued**. The
   failure costs nothing.
3. **`echo 0 > /tmp/ristp8`** — next zap is factory, per Q2 above.

The existing `screen_failed` / `failed_svc_id` latch still applies, so a
service that gave up this visit does not immediately restart into the same
failure. `app_rist_p8_dmx0_wanted()` honours the same latch, so a latched
service never gets `tsid:3` in its URL either.

Deliberately **no** first-frame watchdog on this tail, unlike dmx3. The decode
it feeds is normal play, and `screen_failed` already covers "the chain never
produced a picture" — a second watchdog racing that one would be two mechanisms
fighting over the same zap.

### Two details taken from the reference rather than assumed

- **Write alignment.** `gxfrontend_net.c:330-331` rounds every write down to a
  multiple of `TS_PACKET_SIZE*4` (752) and carries the remainder. Our datagrams
  are 1316 bytes (7×188), which is **not** a multiple of 752, so writing them
  straight through would hand the hardware a length shape the one working
  reference deliberately avoids. The reader carries a remainder and writes
  aligned runs.
- **No `DVR_FLAG_MEM_NOT_PROTECTED`.** `app_ts_record.c:1163` needs that flag
  because it runs DMX → MEM and the driver otherwise encrypts the captured
  buffer at rest — the bug that made the dvb2ip TS body come out as ciphertext.
  This is the opposite direction and `gxfrontend_net.c` sets no flags at all.
  Noted in the code because the symptom ("bytes go in, garbage comes out") is
  identical and it cost a session to diagnose the first time.

### `app_first_play()` is deliberately untouched

`app_first_play()` (`app_play_control.c:2900`) has the same `ts_src` decision
but **no** `app_rist_play_change()` call — the chain is not started on the boot
play at all. So a box boots into its last channel on the tuner and the dmx0
path engages on the first real zap. Adding the hook there would have set
`tsid:3` with nothing feeding it.

---

## Bring-up

```sh
echo 1 > /tmp/ristp8
echo dmx0 > /tmp/ristp8tail
# zap away from NCN and back -- the tail is read per zap
```

Expected, in order:

```
[RIST] play_change: chain=ENABLED (default, no /tmp/ristchain)  part8=ON
[RIST] svc_id=2 -> PART 8 video path (tail=dmx0, normal play stays on demux 0)
[RIST] p8: tail=dmx0  (demux 0 fed from the repaired stream; consumers stay where they are)
[RIST] p8: ports cap=6300 local=6400 out=6500  buffer=2000ms  pcr_cut=0x0022 (34)  svc_id=2 prog=N
[RIST] chain: started stb_part8_receiver pid=...
[RIST] chain: started ristreceiver pid=...
[RIST] p8dmx0: STAGE 1 -- DVR 0 open (feeds demux 0)
[RIST] p8dmx0: STAGE 2 -- DVR 0 running, src=MEM dst=DMX
[RIST] p8dmx0: reading udp://@127.0.0.1:6500 -> DVR 0 -> demux 0
[RIST] p8dmx0: demux 0 source is set by the PLAYER from &tsid:3 -- nothing here writes it
[RIST] p8: broadcast PSI passthrough ON -- user_pmt=false, ext pids 0x0000 0x0011 0x0012 0x0014 ...
[RIST] p8dmx0: STAGE 3 -- first write ACCEPTED 1504 of 1504 bytes into DVR 0
[RIST] p8dmx0: T+1000ms rx=... wrote=... werr=0
```

`pcr_cut=0x0022` is still the value to check first — if it reads anything else
the box's PMT disagrees with the headend's analysis and nothing downstream can
align.

What to look at, in this order:

1. **`rx=` climbing** — the receiver is producing. If it is 0 after 15 s the
   problem is upstream of the demux entirely; check the sender's `[RUN]
   ts_pkts/pcrs`.
2. **`wrote=` climbing with `werr=0`** — the memory feed is accepting. `rx`
   climbing with `wrote=0` means DVR 0 configured but is not running.
3. **A picture** — demux 0 decoded from SDRAM. This is U1b.
4. **Service name, EPG, clock still updating after a minute** — the section
   filters are firing on the memory-fed demux. **This is U1a**, and it is the
   thing the whole SI/CAS-off-the-repaired-stream architecture rests on. If the
   picture is fine but the clock freezes and EPG stops filling, U1a is NO and
   the answer is worth as much as the picture.

Revert:

```sh
echo player_av > /tmp/ristp8tail   # back to Step 1, next zap
echo 0 > /tmp/ristp8               # back to factory decode, next zap
```

---

## What I have not done

**I have not run any of this.** No box, no way to exercise the media API, the
demux or the decoder off-hardware. Nothing above should be read as a claim that
demux 0 decodes from memory, that the DVR accepts our writes, or that section
filters fire on it.

What is established is the reasoning, and it is quoted from the tree rather
than inferred: the `(dmx_id, ts)` keying that rules out the direct approach, the
unconditional source write in `dvbsource_normal.c` that makes the URL the right
lever, the pdmx branch that already does exactly this, and the DVR
MEM → DMX sequence copied from the one working reference.

The serial log above is what turns that into an answer.

Still open and unchanged: **U1a**, and **G2** (a real box-vs-headend TS diff,
which needs an STB capture I cannot produce).

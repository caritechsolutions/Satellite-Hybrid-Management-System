# Field test: does the GX6631 demux pass TEI packets?

Written to be followed at the dish with a laptop. Every command is one you can
copy. Everything you need to record by hand is on the sheet at the end.

**The question.** The Part 8 loss detector on the box needs to know how damage
arrives. Two possibilities, and they are mutually exclusive:

| | what you see in the capture |
|---|---|
| demux **passes** the errored packet, flagged | TEI count rises, continuity counters stay unbroken |
| demux **discards** it at input | TEI count stays zero, continuity counters break |

That mutual exclusivity is the whole design of the test, and it is also the
positive control: **either** signature appearing proves the water actually
damaged the stream. If neither moves, the run says nothing and you repeat it.

**The lever.** In `app_ts_record.c` the audio slot is allocated with
`DMX_ERR_DISCARD_EN` and the video slot without it — same service, same
impairment, same instant, one flag different (`_ts_rec_prog_config()`, lines
1679–1680). If video shows TEI and audio does not, both questions are answered
at once. This asymmetry is why the test is tractable at all.

---

## P0 — Before you leave the bench

Do this indoors, with a good signal, at least a day before. It is the same
work as P2, so nothing is wasted: **the baseline you take here is the baseline
the analysis needs.** If the capture path does not work indoors it will not
work in the rain.

### P0.1 Pick the service and write down its PIDs

Pick a service on the transponder you will be testing, and note its **video
PID** and **audio PID**. You need both, and you need to know which is which —
the entire verdict is "video behaved differently from audio".

The analyser can read them from the PMT, but do not rely on that at the dish.
Write them on the sheet.

### P0.2 Choose a capture route

**Your assumption that the box's satellite-path output reaches the headend is
wrong, and it matters.** In the chain configuration the demux capture goes to
`udp://127.0.0.1:6000` and is consumed on the box by `stb_part7_receiver`
(`app_rist_capture.c:107-112`, `_rist_chain_start()` at :525). Nothing of it
leaves the box. There is no headend capture point for this stream. You must
pull it off the box directly, by one of these two routes.

**Route A — HTTP pull (preferred: nothing to configure on the box).**
`dvb2ip_server` serves the same demux capture over HTTP. It reaches
`_ts_rec_prog_config()` with the identical slot flags, so it is the same bytes
the RIST path would send.

```
# on the laptop, box on the same network
curl -s http://<stb-ip>:8998/play_file            # program list -> find prog_id
curl -s http://<stb-ip>:8999/stream=<prog_id>.ts > baseline.ts
```

Ctrl-C to stop. **Verify on the bench that this actually starts and that the
file grows** — if it does, you need no shell on the box at all, which given
that you cannot scp is the difference between a workable day and a wasted one.

**Route B — redirect the UDP capture to the laptop.** Requires writing two
files in `/tmp` on the box. Use this only if Route A fails.

```
# on the box
echo 0 > /tmp/ristchain          # chain OFF -- see the warning below
echo <laptop-ip>:6000 > /tmp/ristcap
# then ZAP to the service (change channel away and back)
```

Two things about Route B that will cost you the run if you miss them:

- **`/tmp/ristcap` is ignored while the chain is active.** The destination is
  forced to `127.0.0.1:6000` (`_rist_resolve_dest()`, :323-330). The chain flag
  is only read at zap time (`app_rist_play_change()` at :1124, flag read at :1168), so writing
  `/tmp/ristchain` and *not* zapping changes nothing.
- With the chain off there is no recovery stream. That is correct for this
  test — we want the raw satellite path damaged — but the picture will break up
  and stay broken. Expect that; it is not a fault.

Once the chain is off, `/tmp/ristcap` is re-read every 2 seconds
(`RIST_RESOLVE_SECS`, :63), so you can retarget the laptop without another zap.

Capture on the laptop:

```
tsp -I ip 0.0.0.0:6000 --buffer-size 8388608 --receive-timeout 30000 \
    -O file baseline.ts
```

The datagrams are 1316-byte raw TS (7 × 188), no RTP, so `-I ip` needs no
extra options. When the stream stops for 30 s, tsp exits printing
`Error: ip: error receiving from UDP socket: Resource temporarily unavailable`.
**That is the intended stop, not a failure** — the file is complete.

### P0.3 Confirm the capture is what you think it is

```
ls -l baseline.ts                      # must be a multiple of 188
python3 tests/part8/tei_analyse.py baseline.ts --video 0x____ --audio 0x____
```

You want to see: your video and audio PIDs present with sensible packet counts,
`TEI packets: 0`, and a verdict of **NO DAMAGE DETECTED**. That last line is
the analyser confirming the baseline is clean, which is exactly what a baseline
must be.

If `TEI packets` is **not** zero indoors, stop and tell me before going to the
dish — the box is already seeing errors and the test needs rethinking.

### P0.4 Take the baseline and keep it

Capture **at least 60 seconds** of clean stream and keep it as `baseline.ts`.
Longer is better; two minutes costs you nothing.

> **Do not skip this and do not substitute a baseline from another box, another
> service or another day.** Continuity discontinuities are *not* zero on a
> healthy stream — on this transponder PID `0x03E8` carries two interleaved
> continuity counters and produces 164 CC errors in four clean seconds with
> nothing wrong. Without a baseline the analyser judges CC against zero and
> calls a perfect capture damaged. It did exactly that to me before the
> baseline differencing existed.

---

## P1 — The positive control: reading damage independently of the demux

The capture alone cannot tell you the water worked, because "no TEI and no CC
breaks" is indistinguishable from "nothing happened". You need a measurement
taken **before** the demux. The frontend gives you one.

### Where to read it

Press **INFO** to the channel-info signal page (`app_channel_info.c`, the
signal window). It refreshes every 100 ms and does **not** time out, so you can
leave it on screen for the whole session. That is the practical answer to
recording by hand: **stand the phone in front of the TV recording video, and
the signal page logs itself.**

Three fields, in the order they matter:

**1. BER — the number that moves first.** Displayed as `d.ddE-ee`
(`app_snr_progbar_update()`, `app_utility.c:903-952`). The exponent is the
digit that matters: the frontend packs mantissa in bits 8-19 and exponent in
bits 0-3 of `FRONTEND_ERRORRATE_GET`. **A larger exponent is a better link.**
Clean is typically `E-06`/`E-07`; a rain fade walks it down through `E-05`,
`E-04`, `E-03`.

Two readings are not measurements and you must not record them as such:

- **`1.00E-00` means NOT LOCKED.** When `GxFrontend_QueryStatus()` returns
  anything but 1, the code substitutes mantissa 100, exponent 0 and bar value
  10 (:927-931). It is a placeholder, not a BER.
- **`0.00E-00` means locked with zero errors** — a genuine clean reading.

**2. The SNR bar colour.** Red below a bar value of 48, green above (:935).
The bar value is `12 × exponent`, so the bar turns red at roughly `E-03` or on
loss of lock. Red is your "you are in or past the band" cue at a glance.

**3. Quality % and Strength %.** Coarse and gradient-mapped
(`dvbs_quality_get()`, `app_nim_dvbs.c:330`). Record them, but do not
steer by them — they move late.

### The band you are trying to hold

> **Locked, BER between about `E-05` and `E-03`, picture visibly blocking.**

That is where the demodulator is still tracking but the FEC is failing, which
is the only condition that produces errored packets for the demux to have an
opinion about.

**Loss of lock is not the impairment you want.** Unlocked, the demux receives
nothing at all: no TEI, one enormous CC gap, and no information about the
question. If the BER reads `1.00E-00`, back the water off.

### A fourth signal, free, if you are on Route B

The reader logs its throughput once per second to the console you already read
`[RIST]` lines on (`_rist_reader()`, :921):

```
[RIST] reader: 254000 B/s  total=... sent=... err=... -> 192.168.1.50:6000
```

If the demux **discards** errored packets, fewer bytes come out and this number
sags during the fade. If it **passes** them flagged, the byte rate is
unaffected. It is a coarse signal — the service is VBR, so the rate moves on
its own — but a large sustained drop that tracks your water is a live
preliminary answer before you analyse anything.

---

## P2 — Baseline at the dish

You already have `baseline.ts` from P0.4. Take **one more, on site, before any
water touches anything**, so the baseline shares the day, the weather, the
service and the dish alignment with the impaired capture.

```
curl -s http://<stb-ip>:8999/stream=<prog_id>.ts > baseline_site.ts     # Route A
# or
tsp -I ip 0.0.0.0:6000 --buffer-size 8388608 --receive-timeout 30000 \
    -O file baseline_site.ts                                            # Route B
```

Sixty seconds minimum. Record the clean BER, quality and strength on the sheet
before you stop it — those numbers are what "clean" means for this session.

---

## P3 — The capture under impairment

**Start the capture before you go near the LNB, and leave it running for the
entire session.** One continuous file. Do not start and stop it around each
water application.

```
curl -s http://<stb-ip>:8999/stream=<prog_id>.ts > impaired.ts          # Route A
# or
tsp -I ip 0.0.0.0:6000 --buffer-size 8388608 --receive-timeout 60000 \
    -O file impaired.ts                                                 # Route B
```

At roughly 2–3 Mb/s this is about 20 MB per minute. A half-hour session is
~600 MB. Capture too much; you said it yourself, and it is right — you cannot
re-run a wetting.

One continuous file is safe because **the analysis is cumulative counts, not a
time series**: clean stretches inside the impaired file only inflate the packet
total, which makes the baseline-rate expectation larger and the test slightly
*more* conservative. Nothing is lost by over-capturing.

**Do not point a second socket consumer at the same port.** Two readers on one
UDP port compete for datagrams and the kernel gives each one to whichever reads
first, starving both. One `tsp -I ip`, or one `curl`, and nothing else. (A
passive `tcpdump -i any -w field.pcap 'udp port 6000'` does *not* steal
datagrams — it taps the interface — so it is safe to run alongside as insurance
if you want it. Extracting a `.ts` from the pcap is a step I can do afterwards.)

### Applying the water

Drape a wet cloth over the **feed horn / LNB aperture**, not the dish face, and
re-wet it to deepen the fade. A cloth is far more controllable than pouring: it
holds the impairment steady, and squeezing it gives you a repeatable step. Pour
if you must, but expect to overshoot into loss of lock.

Watch the signal page. Aim for the band in P1. **Every time you touch the LNB,
note the laptop's wall-clock time and the BER on the sheet** — the laptop clock,
not the box's, because the `[RIST]` log carries only a `T+ms` offset from the
last zap and nothing you can correlate against.

Hold the marginal band for **60 seconds if you can**, then let it dry back to
clean, and repeat three or four times. If you only ever get brief dips, that is
fine — see P5.

---

## P4 — The analysis

```
python3 tests/part8/tei_analyse.py impaired.ts \
        --baseline baseline_site.ts \
        --video 0x____ --audio 0x____
```

Runnable on the laptop at the dish; it needs only Python 3. TSDuck is used only
to infer PIDs if you omit `--video`/`--audio`, which you should not.

### Reading the per-PID table

```
  PID          packets        TEI    CC breaks
  0x0731          4538         40            0  <- video, NO discard flag
  0x0732           196          0            9  <- audio, HAS discard flag
```

The two marked rows are the entire experiment. Read them as a pair:

| video (no flag) | audio (flag set) | what it means |
|---|---|---|
| TEI > 0 | TEI = 0, CC breaks | **Best case.** The demux passes TEI, and `DMX_ERR_DISCARD_EN` is what suppresses it. TEI is available *and* per-slot selectable. Detector branch (a). |
| TEI > 0 | TEI > 0 | The demux passes TEI regardless of the flag, so the flag governs something else (CRC or descrambling). TEI is available, without per-slot control. Still branch (a). |
| TEI = 0, CC breaks | TEI = 0, CC breaks | The demux drops errored packets at input regardless of the flag. Detector branch (b) — loss is detected by continuity, not by TEI. |
| TEI = 0 | TEI > 0 | Contradicts the flag's name. Do not design against it; bring me the table. |

The header block also prints, from the baseline, **which PIDs have CC errors
when healthy**, e.g.:

```
  PIDs with CC errors when HEALTHY (their CC cannot be trusted):
    0x03E8  164 CC breaks in 3250 packets (50.46 per 1000)
```

Those PIDs are discounted by their own measured rate rather than believed, so
they cannot manufacture a false "damage confirmed". If your **video or audio
PID** appears in that list, say so — it weakens the CC half of the evidence for
exactly the PID that matters, and I need to know before drawing a conclusion.

### The line that decides whether the run counts

```
  CC breaks above baseline rate: 49  (raw 213)
```

`raw` is everything; the first number is what survived subtracting the
baseline rate per PID. **The verdict is computed from the first number.** If it
is below 1 and TEI is zero, the analyser reports `NO DAMAGE DETECTED. This run
establishes nothing about TEI.` — that is not a bug, it is the positive control
correctly refusing to conclude.

### Independent cross-check

The analyser deliberately parses the bytes itself rather than using TSDuck, so
that a second opinion has no shared failure mode. Take it:

```
tsp -I file impaired.ts -P filter --valid --negate -O file tei_only.ts
ls -l tei_only.ts        # 0 bytes = no TEI found; non-zero = TEI packets exist
```

`--valid` selects packets with a good sync byte and no transport error;
`--negate` inverts it. Verified: 0 bytes on a clean capture, and it finds
injected TEI packets exactly. If this disagrees with the analyser, trust
neither and send me both files.

---

## P5 — If you cannot hold the marginal band

**Repeated brief dips are as good as one sustained soak.** The analysis is
cumulative counts over the whole file, so ten five-second excursions and one
fifty-second hold are the same evidence. Do not chase a steady state you cannot
hold; get in and out of the band as many times as you can and let the counts
accumulate.

What actually threatens the run, in order:

1. **You never leave clean.** Nothing fires, verdict is `NO DAMAGE DETECTED`,
   and the day is wasted. More water, or wet the feed horn directly rather than
   the dish face. Check the BER exponent is falling before you invest more time.
2. **You jump straight to loss of lock.** `1.00E-00`, no picture, a huge CC gap
   and no TEI. This looks superficially like branch (b) and is **not** — the
   demux saw no packets, not damaged ones. If most of your file is unlocked,
   the run is inconclusive and the analyser's verdict must not be believed. Note
   the unlocked periods on the sheet so I can discount them. Recover by
   backing the water off and approaching the band from the clean side more
   slowly.
3. **The water dries too fast to be useful.** Re-wet the cloth rather than
   re-pouring; a cloth holds the fade for minutes where a pour lasts seconds.

If after all that you still cannot land in the band, **stop and keep what you
have.** Send me `baseline_site.ts` and `impaired.ts` with the sheet. A partial
run with honest numbers is worth more than a forced one, and there is a
decisive fallback that needs no dish at all:

> **The confirming rebuild.** Clear `DMX_ERR_DISCARD_EN` from the audio slot
> (`app_ts_record.c:1679`), reflash, and repeat. If TEI then appears where it
> did not before, the demux does pass it and the flag is the suppressor —
> which is the same answer, obtained on the bench. The analyser already tells
> you to do this whenever it reports branch (b), precisely because branch (b)
> should not be accepted on one field run.

---

## Record by hand

Laptop wall clock throughout — it is the only clock that correlates with the
capture files.

| time | event | lock | BER (`d.ddE-ee`) | quality % | strength % | picture |
|---|---|---|---|---|---|---|
| | capture started | | | | | |
| | baseline_site.ts started / stopped | | | | | |
| | **clean reference reading** | | | | | |
| | water applied #1 | | | | | |
| | | | | | | |
| | dried back to clean | | | | | |
| | water applied #2 | | | | | |
| | | | | | | |
| | capture stopped | | | | | |

Also note once, at the top: box IP, service name, **video PID**, **audio PID**,
`prog_id` (Route A) or that the chain was disabled (Route B), and the weather.

Take a reading every time you touch the LNB and at least once a minute while
impaired. Mark clearly any period where BER read `1.00E-00` — those are
unlocked stretches and they must be discounted, not analysed.

## Bring back

- `baseline.ts` (bench, from P0.4)
- `baseline_site.ts` (on site, pre-water)
- `impaired.ts` (one continuous file)
- the sheet
- the phone video of the signal page, if you took it
- the console log, if you were on Route B

---

## Not part of this test

The video slot in the capture path carries no error-discard flag while the
audio slot does. If the flag means what its name implies, corrupt video packets
are reaching the decoder in normal operation. That looks unintentional and is
worth its own investigation — but it is what makes *this* test possible, so it
must not be changed before the test runs. See
`docs/gx6631-demux-tei-findings.md` §7.

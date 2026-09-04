# `pcr_cut` — PCR-boundary packetisation in librist. Design.

Design only; no code yet. One correction to the premise up front, because it
makes P5 easier than stated.

## Correction: the box's sender is not in-process

The brief says the flag must reach "the box's in-process sender". It is not
in-process. `_rist_chain_start()` spawns it as a separate binary
(`app_rist_capture.c:605-611`):

```c
wd_argv[0] = "/usr/bin/rist_watchdog";
wd_argv[1] = "/usr/bin/stb_part7_receiver";   /* RIST_BIN_SENDER */
wd_argv[2] = "-i";  wd_argv[3] = in_url;      /* udp://@127.0.0.1:6000 */
wd_argv[4] = "-u";  wd_argv[5] = out_url;     /* rist://@127.0.0.1:6100?... */
```

`stb_part7_receiver` is, despite the name, the **sender** — UDP in, RIST out,
invoked exactly like `ristsender`. So **both sides already run the same tool
shape with the same argument form**, which is what makes "one implementation"
work without any special casing.

---

## P1 — Where the cut attaches

**One site.** `rist_sender_data_write()` is called from three places in
`ristsender.c`, and only one is on our path:

| line | path | relevant |
|---|---|---|
| **288** | `input_udp_recv()` — UDP input | **yes, both sides** |
| 594 | TUN input | no |
| 642 | rist2rist relay | no |

At `:284-289` the callback has finished populating `data_block.payload` and
`.payload_len` and makes a single call:

```c
if (peer_connected_count) {
    if (rist_sender_data_write(callback_object->sender_ctx->ctx, &data_block) < 0)
        rist_log(...);
}
```

**This is a clean wrapper point — nothing reaches into input handling.** The
shape:

```c
if (peer_connected_count) {
    if (cut->enabled)
        pcr_cut_feed(cut, data_block.payload, data_block.payload_len, ctx);
    else
        rist_sender_data_write(ctx, &data_block);   /* untouched */
}
```

`pcr_cut_feed()` accumulates, re-frames, cuts, and calls
`rist_sender_data_write()` **once per emitted payload** — so sequence advances
per payload, which is the corrected numbering rule from G3.

### Keeping the flag-off path byte-identical

Recommendation: **do not change `rist_sender_data_write()` at all.** Add the
cutter as a separate entry point and let the caller choose. Then the flag-off
path is literally the same code that runs today — the strongest available
guarantee that Part 7 and the deployed Part 8 server are untouched, and it needs
no argument to defend.

The alternative — a flag on the sender context that makes
`rist_sender_data_write()` sometimes 1:1 and sometimes N:1 — is less API surface
but makes a documented 1:1 call surprising, and it puts the branch inside the
path we promised not to disturb. Not worth it.

**Where it lives:** in the library (`src/`), not in `tools/`. Both sides happen
to run a tool today, but the cut rule is protocol-adjacent — it defines what a
sequence number *means* on this flow — and it belongs versioned with the
protocol code, not with a CLI. The tool only parses the option and passes it in.

---

## P2 — How the sender learns the PCR PID

**CLI/URL argument, not PMT parsing.** This one is not close.

Parsing the PMT makes the cut rule depend on *when* the PMT was seen:

- Before the first PMT arrives the sender does not know the PCR PID and must
  either buffer indefinitely or fall back to plain 7-packing. The two sides
  reach that point at different times — the server has been running for hours,
  the box just started — so **the fallback window produces different boundaries
  on the two sides**. That is precisely the misalignment the whole design exists
  to remove, reintroduced at startup.
- A mid-stream PMT change (PCR PID moves) would re-cut at different instants on
  each side.

An argument is deterministic from byte zero. Both sides already have the value:
the box from `GxBus_PmProgGetById()` → `prog.pcr_pid` (`app_ts_record.c:1230`,
the same record the slot set comes from), the server from the channel record.

If the PCR PID ever changes, restart the sender — which is what a zap does
anyway.

---

## P3 — Edge cases, pinned in the cutter

All of these live in `pcr_cut_feed()`, not in the caller.

| case | rule |
|---|---|
| PCR packet seen | **flush pending first, then start the new payload with the PCR packet as packet 0** |
| adjacent PCRs (pending empty at the cut) | emit nothing; do not call `data_write`; sequence does not advance |
| 1-packet interval | one 188-byte payload |
| interval > 7 packets | greedy 7 from the interval start, short remainder flushed at the next PCR |
| bytes before the first PCR | discard; do not emit |

**The greedy-7 reset happens AT the PCR packet — confirmed as the rule.** Flush
happens *before* appending the PCR packet, so the PCR packet is always offset 0
of a payload and the 7-counter restarts there. Given the same ordered packet
list, both sides therefore produce identical splits within every interval.

On the last row: the two sides will generally start at *different* first PCRs
(the server has been running longer). That is fine and expected — boundaries
coincide from each side's first PCR onward, and absolute alignment is the
anchor's job, not the cutter's.

Measured shape, for sizing (from the G3 work): NCN-Guyana 1–49 packets per
interval, mean 22.1 → 1–7 payloads, mean 3.59. BET 24–79 → 4–12, mean 7.28.

---

## P4 — Input framing. Confirmed, and it is cheaper than it looks.

**Yes, the sender can and must ignore input datagram boundaries.** The cutter
accumulates across callbacks and re-frames on 188-byte alignment, exactly as
`_rist_reader()` already does with its `sbuf`/`sfill` remainder carry
(`app_rist_capture.c:902-910`). Requirements:

- carry a partial TS packet across `input_udp_recv()` calls;
- resync on `0x47` if alignment is ever lost, rather than trusting the input.

**The accumulator never needs to hold a whole interval.** It flushes at 7 packets
or at a PCR, whichever comes first, so it needs at most 7 packets plus one
partial — about **1.5 KB**. Intervals of 79 packets do not imply a 79-packet
buffer.

This is the property that makes "same binary both sides" real: it does not
matter that `tsp -O ip` packs 7 and the box's reader packs 1316-byte chunks with
its own phase. Both framings are discarded and the stream is re-cut from the
same rule.

---

## P5 — Flag plumbing: a UDP URL parameter

**Use a URL parameter on the input URL, not a new CLI switch:**

```
udp://@127.0.0.1:6000?pcr_cut=1841
```

Absent or `0` means off. One parameter carries both the enable and the PID —
there is no state where the flag is on and the PID unknown.

Why this over a CLI flag:

1. **The extension point already exists.** `parse_url_udp_options()`
   (`rist-common.c:88-118`) already parses `?key=value` into `udp_config` fields
   — `miface`, `stream_id`, `rtp_timestamp`, `rtp_sequence`, `rtp_ptype`,
   `multiplex_mode`. Adding `pcr_cut` is one more case in the same chain.
2. **It works unchanged on both sides.** Server: append to `ristsender -i`. Box:
   `in_url` is already built with `snprintf()` (`app_rist_capture.c:543`) — only
   the format string changes.
3. **It avoids a real bug.** `wd_argv` is declared `char *wd_argv[8]`
   (`app_rist_capture.c:529`) and currently uses indices 0–5 with `NULL` at 6.
   Adding `-pcr_cut` and its value would need indices 6 and 7 with `NULL` at 8 —
   **one past the end of an 8-element array.** A CLI flag here means also growing
   that array; the URL parameter needs neither.

Follow the existing `version == 1` gating so old URLs behave exactly as before.

---

## Summary of the change

| piece | where | size |
|---|---|---|
| `pcr_cut` URL param parsing | `rist-common.c` `parse_url_udp_options()` | one `else if` |
| `udp_config.pcr_cut` field | `librist/udp.h` config struct | one field |
| the cutter | new file in `src/`, public entry point | ~120 lines |
| call-site branch | `ristsender.c:288` | 4 lines |
| box: pass the param | `app_rist_capture.c:543` format string | one line |
| server: pass the param | `ristsender` invocation | none, config |

The flag-off path is untouched code. Nothing in Part 7, the deployed Part 8
server, or the existing box chain changes behaviour until the parameter is set.

## What this design does not solve

Stated so it is not assumed away later: **`pcr_cut` guarantees the two sides cut
identically given the same packet list. It does not guarantee the same packet
list.** That is still G2 — the box-vs-TSDuck diff, which needs the box capture I
could not produce. If the ES packet sets or ordering diverge, identical cutting
produces identically-wrong-in-different-places payloads, and the anchor will
thrash. G2 remains the gate on this being useful, not merely correct.

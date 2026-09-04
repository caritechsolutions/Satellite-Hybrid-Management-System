# Part 8 per-channel recovery sender + GUI — design

Scope B1–B4 before the build. Two answers differ from the brief, both because
something already exists that the brief assumed had to be written.

---

## The correction that moves B1 and B3

**The brief's chain says "librist sender". It should be `part8_recovery_server`,
and that changes the unit name answer and most of the anchor answer.**

`part8_recovery_server` already is a UDP-in → RIST-out sender with:

- `require_selection` **on by default** (`-S` disables). Stock `ristsender` has
  no CLI or URL route to it at all — it is API-only and off by default — so
  "require_selection stays ON" cannot be met by `ristsender` without adding a
  flag to librist.
- weight-1000 output semantics, the retry path and the `last_rtt` guard.
- **a PCR → sequence catalogue, built at packetisation time**, and a `resolve`
  query over a Unix socket that is already chowned to `www-data`.

That last one is B3. `emit_payload()` (`part8_recovery_server.c:1697`) says it
outright:

> The catalogue is built HERE, at packetization time, because this is the only
> point where the mapping (PCR -> sequence) is known for free.

`resolve <pid> <pcr> <duration>` already returns `start_ext` / `start_wire` and
`end_ext` / `end_wire` — the RTP sequence range for a PCR interval — plus a
serviceability status. **The anchor does not need building. It needs exposing.**

So the chain is:

```
shared transponder ingest (multicast, existing, untouched)
  -> tsp -r -I ip <ingest> -P filter --pid <service ES + PMT + PSI>
                           -O ip 238.0.0.<n>:<port> --ttl 1
  -> part8_recovery_server -i udp://@238.0.0.<n>:<port>
                           -o rist://@<ip>:<9800..9899>?weight=1000&buffer=N
                           -C <pcr_pid>            <- new, off by default
  -> box connects to rist://<ip>:<port>
```

---

## B1 — the per-channel unit

**Two units. Neither needs a new name pattern, and one of them needs no new
anything at all.**

| unit | what | helper that accepts it |
|---|---|---|
| `part8-recovery@<id>.service` | the sender | **existing** `part8-unit`, existing template, existing sudoers |
| `ristsender-<id>-p8src.service` | the tsp filter stage | **existing** `rist-unit` (`ristsender-*` glob), existing sudoers |

### Why the sender needs nothing new

`part8-unit` constructs `part8-recovery@<token>.service` from a validated token
and refuses to take a unit name — so a per-channel instance is just a token. The
channel id is already `^[a-z0-9][a-z0-9-]*$` (`slug()`), which is exactly
`p8_valid_name()`'s pattern. `ncn-guyana` is a legal token as it stands.

The template's `ExecStart` ends in `$P8_EXTRA`, so `-C <pcr_pid>` reaches the
binary **through the env file, with no unit-file edit**. `part8-unit create`
takes that env on stdin, and `www-data` is already granted `part8-unit` — the
grant is not scoped per vhost, so `rist-monitor`'s PHP can call it today.

### Why the tsp stage uses the Part 7 helper

`rist-unit` accepts only `ristsender-*` and `ristmarker-*`. Every name it
accepts therefore contains `rist`. `ristsender-<id>-p8src` follows the existing
`ristmarker-<id>-tsp` suffix precedent, so `putUnit()`, `systemctl` and the
sudoers globs all work unchanged.

Chain control, so start/stop of the sender drives the whole thing:

```
[Unit]
After=part8-recovery@<id>.service rist-mcast-bridge.service
Requires=rist-mcast-bridge.service
BindsTo=part8-recovery@<id>.service
PartOf=part8-recovery@<id>.service
[Install]
WantedBy=part8-recovery@<id>.service
```

Same shape as Part 7's marker → sender binding.

### The consequence, flagged

`BindsTo` means starting the Part 8 instance starts a unit whose name contains
`rist`. `part8-unit`'s `report_diff()` buckets any changed line matching `rist`
as **PART7-CHANGED**. So every Part 8 start would raise the alarm the file
exists to make meaningful — the cry-wolf its own comment warns about. **This
needs a one-line classification fix in `part8-unit`** (`-p8src` is Part 8, not
Part 7). Flagged as live-helper touch #1.

### Ports and the internal group

`p8_alloc_port()` from `part8-monitor/lib.php` — 9800–9899, four independent
conflict sources, refuses rather than guesses. Reused rather than
`channel-service.php`'s `nextFree()`, which only looks at `channels.json`.
`p8_ports_in_use()` already picks up any numeric channel key whose name contains
`port`, so `p8_port` and `p8_tsp_port` are seen automatically.

The tsp → sender hop gets its own group in the existing 238.0.0.0/8 fabric,
offset clear of the Part 7 marker hop.

---

## B2 — GUI

One checkbox: **Part 8 recovery**. Everything else is derived.

| value | derived from |
|---|---|
| filter PID set | analysis: PMT PID + PCR PID + the service's ES PIDs + the fixed §4.3 PSI set |
| `?pcr_cut` / `-C` PID | analysis `pcr_pid` |
| listen port | `p8_alloc_port()` |
| internal group / port | allocator |

Shown read-only next to the checkbox so it can be eyeballed; **never typed**.

Two refusals rather than guesses, both in house style:

1. **No analysis → refuse.** Same as `declare_marker` does today.
2. **A non-empty `remap`, or an `out_service_id` that differs from the source →
   refuse.** If the uplink renumbers PIDs, the box's filtered stream and the
   server's differ byte-for-byte at exactly the PIDs the sequence is supposed to
   index, and identical cutting then produces identically-wrong payloads. This
   is the G2 failure mode made certain rather than possible, so it is a gate, not
   a warning. NCN-Guyana has neither set.

The PSI set is fixed (PAT 0x0000, CAT 0x0001, NIT 0x0010, SDT/BAT 0x0011,
EIT 0x0012, RST 0x0013, TDT/TOT 0x0014) rather than derived from what the
analysis happened to see. V1 found NIT and TDT/TOT entirely absent on this
transponder; a list derived from presence would change the moment a table
appeared, and only on one side.

---

## B3 — the anchor: HTTP, over the existing per-service lookup

**`GET /api/recovery.php?service_id=N&pcr=<value>` → `resolve` on the
instance's debug socket.**

Chosen over an RTCP exchange, and the reason is not convenience:

- The RTCP path would be served by the sender's protocol thread — the same
  thread that runs `evsocket_loop_single()` for **every** peer while holding
  `peerlist_lock`. We already documented that blocking it takes reception down
  for all peers (it is why the recv-error log had to be quiesced). Putting
  fleet-wide query load there is the failure we have already had once.
- HTTP needs no new protocol on either end. The box has an HTTP client, retry
  and cache in `app_rist_api.c`, and the failure-silent contract that a miss
  means "stay on the factory path".
- It needs no second connection: it is the same GET the box already makes, on
  the same host name, with one more query parameter.
- The answer comes from the same catalogue the retransmit path serves from, so
  it cannot disagree with what a NACK would get.

Serviceability comes back verbatim — `OK`, `BEFORE_EPOCH`, `OUTSIDE_BUFFER`,
`TOO_OLD` — so the box is told *why* an anchor is unusable rather than being
handed a number it cannot act on. `bounds` is exposed the same way for
diagnostics.

PHP talks to `/run/part8-recovery/<id>/debug.sock`, which the template already
creates with `-g www-data` and mode 0660.

---

## B4 — what the recovery API advertises

Additive only. Every Part 7 field keeps its name, type and meaning; the box's
`_api_parse()` uses cJSON and ignores unknown keys, so a Part 7 box in the field
sees no change whatsoever.

```json
{
  "service_id": 2, "ts_id": 1, "name": "NCN-Guyana",
  "marker_pid": 8176,
  "rist_url": "rist://1.2.3.4:5700?buffer=8000",

  "part8": 1,
  "part8_rist_url": "rist://1.2.3.4:9800?buffer=8000",
  "part8_pcr_cut": 1,
  "part8_server_pcr_pid": 34,
  "part8_filter_pids": [0,1,16,17,18,19,20,34,35,142]
}
```

**`part8_server_pcr_pid` is diagnostic and the box must not use it for its own
`?pcr_cut`.** The box's PCR PID is what it sees after the uplink mux
(`s_rist.prog.pcr_pid`, from its own PMT); the server's is pre-uplink. They are
equal only when nothing is remapped — which the B2 gate now enforces — but the
box should still use its own, because that is the PID present in the bytes it is
cutting. `part8_filter_pids` is there so the box can assert its slot set matches
rather than assume it.

---

## Part 7 / Part 8 coexistence, and the revert path

**One field: `part8` on the channel.**

| `part8` | Part 7 units | Part 8 units |
|---|---|---|
| absent / false (every existing channel) | byte-identical to today | none |
| true | **byte-identical to today** | `part8-recovery@<id>` + `ristsender-<id>-p8src` |

They coexist per channel rather than replacing each other. The Part 7 chain
(`ristsender-<id>`, weight-0 + weight-1000 marker path) is not read, not
rewritten and not restarted when Part 8 is enabled. A field box on Part 7 keeps
its `rist_url`; a Part 8 box uses `part8_rist_url`. Both peers exist, on
different ports, fed from the same ingest.

Revert is the flag off — which stops, disables and removes only the two Part 8
units, the same code path `tspNeeded()` already uses for the Part 7 tsp stage.

---

## Live touches, flagged

Both off-by-default, both one-line reverts.

1. **`part8-unit` `report_diff()`** — classify `-p8src` as Part 8 so a Part 8
   start does not report PART7-CHANGED. Pure classification; no behaviour change
   to any unit.
2. **`part8_recovery_server.c` — `-C <pcr_pid>`**, PCR-boundary framing instead
   of fixed 7. **Without `-C` the packetiser is unchanged**, so the deployed
   `part8-recovery@` instances keep their exact current behaviour, and revert is
   deleting `-C <pid>` from `P8_EXTRA` in one env file.

   Note for later, not a blocker: the bandwidth estimators divide by
   `TS_PER_RTP` to convert TS rate to payload rate. Measured on the NCN-Guyana
   capture, `-C 34` gives **767 payloads where fixed-7 gives 675** — mean 6.12
   packets per payload, so about **14% more payloads** and the estimates read
   ~12% low. They feed headroom reporting, not the buffer itself.

---

## What was tested

**The load-bearing claim is that the two implementations cut identically**, and
they are separate code in separate repos, so it was measured rather than
asserted. `ncn_server.ts`, 4,725 packets, 208 PCRs on 0x0022:

```
server stage_packet()  ->  767 payloads
box librist pcr_cut.c  ->  767 payloads, IDENTICAL lengths in the same order
  at input chunk sizes 1, 3, 188, 189, 1316, 4096, 65536 bytes
```

The box cutter reaches the same split from every input framing, which is what
makes "the box packs 1316-byte reads and the headend packs 7" irrelevant.

Against the real binary over UDP, both arms:

| | payloads | `pcr_cut_pid` | `pcr_cuts` | `pcr_dropped_pre` | `ts_in` |
|---|---|---|---|---|---|
| no `-C` | 675 = ceil(4725/7) | 0 | 0 | 0 | 4725 |
| `-C 34` | 767 | 34 | 208 = every PCR | 30 = packets before the first | 4725 |

Derivation and gates, 20 assertions, all passing: the PID set for a
single-service report and for an MPTS report with per-PID association; the
refusal when an MPTS report carries no association; the refusals for no
analysis, for a remap and for a service rename; the env file.

Not tested here, and still open: **G2 — the real box-vs-headend TS diff.** This
proves both sides cut the same packet list identically. It does not prove they
receive the same packet list, which needs a capture off the box.

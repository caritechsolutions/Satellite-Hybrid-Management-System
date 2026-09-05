# Part 8 as a standalone channel type

The first build made Part 8 a checkbox on the Part 7 "Add channel" form. That
was wrong: it forced an operator to fill in a marker PID, an uplink-to-mux
output and a satellite peer port for a channel that has none of them. The
services being recovered are not Part 7 channels — they have no marker and no
uplink chain — so configuring a Part 7 channel nobody wants was the price of
reaching one checkbox.

This is a re-shaping of the data model and the form. The unit generation, the
derivation, the anchor, the API fields and the isolation guarantee are the ones
already built and tested.

---

## Q1 — where the Part 8 list lives: its own store

`rist-monitor/config/part8-channels.json`. Not a `type` field in
`channels.json`, for two reasons that are not stylistic:

1. **"No Part 7 record changed" becomes a property of the code, not a claim
   about it.** Nothing in `Part8Service` opens, writes or locks
   `channels.json`. With a shared file every Part 8 create would rewrite it —
   `write()` re-adds `settings.server_ip`, and a `json_encode` round trip is
   free to reformat — so the guarantee would rest on an argument instead of a
   fact. The test hashes `channels.json` around every create, update, refusal
   and delete.
2. **`createChannel()` enforces service-id uniqueness across the whole file.** A
   Part 8 channel for service 2 would collide with a Part 7 channel for service
   2 — exactly the pairing that is supposed to be allowed.

Two consequences handled rather than discovered later:

- `install.sh`'s `STATE_FILES` gains the new file, so a re-install preserves it.
- `part8-monitor/lib.php`'s port scan gains it too. A created-but-not-started
  Part 8 channel owns `p8_port` and `p8_tsp_port` while holding no socket, so
  neither the unit scan nor `/proc/net` can see it, and the allocator would
  hand the same port out twice. (Since the hop became loopback, the service
  also excludes its siblings' ports directly — see below.)

## Q2 — same page, separate section

`channels.php` now carries two independent cards: **Add Part 8 recovery
channel** and **Part 8 recovery channels**, below the Part 7 pair. One place to
manage recovery, no shared form state, no shared JavaScript, its own API
(`api/part8.php`).

The Part 7 half of that page is **byte-identical to before Part 8 existed** —
`diff` against `58d6dbf~1` is empty. Same for `api/channels.php`.

## Q3 — what the record needs

Confirmed, and nothing else. The test asserts the stored key list exactly:

```
id, name, input_url, service_id, buffer, created_at, p8_port, p8_tsp_port
```

plus `analysis` once the ingest has been read. The same test asserts that none
of `marker_pid, uplink_url, sat_port, recovery_port, metrics_port, tsp_port,
declare_marker, out_service_id, remap, part8, ts_id` is present.

Two notes on that list:

- **`buffer` is not a Part 7 leftover** — it is the RIST retransmit window, and
  the sender needs one. It defaults to 8000 ms and is not on the create form.
- **`ts_id` is derived, not asked.** The analysis reports it, so the recovery
  record takes it from there.
- **`service_id` is one field doing one job here.** In Part 7 the channel's
  `service_id` (what boxes look up, post-mux) is deliberately *not* the id in
  the source stream. A Part 8 channel is not remuxed — the transponder goes out
  as it came in — so the id in our ingest and the id the box sees are the same
  number, and asking twice would only create a way to get it wrong.

---

## Coexistence, corrected

Part 7 channels and Part 8 channels are separate records. Neither references the
other; they simply both read the same shared multicast ingest. No field on one
type gates the other.

`api/recovery.php` merges the two lists by `service_id`:

- Part 7 only → the record it has always produced, unchanged.
- Part 8 only → a record with `part8_rist_url` and **no `rist_url`**.
- Both → one record carrying both sets of keys, so the box still does a single
  lookup. The merge writes Part 8 keys *into* the Part 7 record and never over
  `name`, `ts_id` or `service_id`, because those are what field boxes have been
  seeing.

**One box change was required by this.** `_api_parse()` used to require
`service_id` **and** `rist_url`, so a Part-8-only record would have been
silently dropped — the exact channels the box is being pointed at. The minimum
is now `service_id` plus either `rist_url` or (`part8` and `part8_rist_url`),
and `_rist_chain_start()` refuses rather than starting a chain aimed at an empty
URL.

### Migration out of the boolean

There is no automatic migration, deliberately: migrating would mean writing
`channels.json`, which is the one thing this change promises not to do. The
`part8` boolean is simply no longer read by anything. A Part 7 record that still
carries it is reported in the API as `stale_part8_flag` and otherwise ignored;
recreate the channel under **Add Part 8 recovery channel** and, if it had ever
been started, remove the old units by hand:

```sh
sudo part8-unit remove <old-id>
sudo rist-unit remove ristsender-<old-id>-p8src
```

The flag shipped in `58d6dbf` and was never enabled on a live channel, so in
practice there is nothing to migrate.

---

## What is shared, and why that is the safe direction

`services/ts-analysis.php` now holds the tsp invocation and the report parsing,
extracted verbatim from `ChannelService`. Both channel types call it.

This is the one place that probes TSDuck's key spellings and decides which
service a PID belongs to. Two copies of that would be two chances for the
headend and the box to disagree about which PIDs a service owns — which is the
failure the whole feature exists to prevent. `ChannelService::analyseInput()`
keeps its signature, its storage behaviour and its log line; only the two moved
blocks changed hands.

---

## The filter → cutter hop is loopback unicast

It was a `238.0.0.x` group on `br-rist`, mirroring the Part 7 marker hop. That
does not work, for two independent reasons, both observed on the live headend:

- **`part8_recovery_server` never joins the group.** There is no
  `IP_ADD_MEMBERSHIP` anywhere in it — it binds the port and waits.
  `ip maddr show br-rist` showed the Part 7 group joined and the Part 8 group
  absent, and the cutter read `ts_in=0` for the entire run.
- **`br-rist` is member-less and comes up NO-CARRIER**, so multicast to it
  black-holes until something forces a dummy member up.

Part 7 needs the fabric because its internal hops are consumed by `tsp -I ip`,
which rejects a unicast address. **Part 8's consumer is
`part8_recovery_server`, which binds whatever it is given**, so that reason
never applied here.

So the hop is `-O ip 127.0.0.1:<port>` into `-i udp://@127.0.0.1:<port>`, with
no `--ttl` and no `Requires=rist-mcast-bridge.service` on the filter unit.

**Does anything still need br-rist for Part 8? No.** The channel's *ingest* is
still a multicast group, but tsp joins that itself and it has real members
already. Part 8 has no dependency on the 238.0.0.0/8 fabric or on the bridge.
Part 7 still does, and is untouched.

Both ports now come from the Part 8 allocator in one pass (`p8_alloc_ports`),
because a loopback port is a real host-wide bind target where a per-channel
multicast group's port could safely repeat. Ports held by other Part 8 channels
are excluded from the candidates directly as well, so uniqueness does not depend
on the allocator's hard-coded config path being right.

## Isolation

Unchanged from the previous build and re-verified:

- The sender is `part8-recovery@<id>` under the **existing** template, through
  the **existing** `part8-unit` helper, with the **existing** sudoers grant.
  `-C <pcr_pid>` reaches it through `P8_EXTRA`; no unit file is edited.
- The tsp stage is `ristsender-<id>-p8src`, which `rist-unit` already accepts.
  It has to wear a `rist*` name because that helper accepts no other kind, so
  both isolation checks classify `-p8src` as Part 8 rather than as a Part 7 unit
  moving.
- Creating, editing and deleting Part 8 channels touches no Part 7 unit and no
  Part 7 record.

## Revert

| how far back | what to do |
|---|---|
| stop one channel | **Stop** in the Part 8 table |
| remove one channel | **Delete** — stops, disables and removes both units and the env file |
| fixed-7 framing without touching anything else | delete `-C <pid>` from `P8_EXTRA` in `/etc/part8/instances/<id>.env`, then `sudo part8-unit restart <id>` |
| take a box back to Part 7 | stop the Part 8 channel; the API stops advertising `part8` and the box falls back on its next fetch — provided that service also has a Part 7 channel |

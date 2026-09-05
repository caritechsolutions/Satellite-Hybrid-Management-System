# Bringing NCN-Guyana up on Part 8 — end to end

Service 2, PMT 0x008E, PCR/video 0x0022, audio 0x0023. All-clear FTA.

A Part 8 channel is its own record. **There are no Part 7 fields anywhere in
this procedure** — no marker PID, no uplink output, no satellite peer port, no
"declare marker in the PMT". Nothing here stops, restarts or rewrites a Part 7
unit or a Part 7 channel record.

---

## 0. Install

```sh
curl -fsSL "https://raw.githubusercontent.com/caritechsolutions/Satellite-Hybrid-Management-System/main/install.sh?$(date +%s)" | sudo bash
```

Two lines to watch:

```
==> Checking librist against riststb
    librist matches riststb <sha>          <- must not warn
==> Isolation check: what changed in unit state
    no rist* unit changed state, enablement or main PID
```

The install rebuilds `part8_recovery_server` with `-C`. Deployed
`part8-recovery@` instances keep running the old binary until restarted, and
their env files carry no `-C`, so restarting them is a no-op either way.
Nothing needs restarting for this.

---

## 1. Create the channel — three fields

Channels page → scroll to **Add Part 8 recovery channel**:

| field | value |
|---|---|
| Channel name | `NCN-Guyana` |
| Transponder ingest (UDP) | the shared feed, e.g. `239.5.5.5:5000` |
| Service ID | `2` |

**Create Part 8 channel.**

The ingest is read, never consumed — the Part 7 chains on the same group keep
working. Creating allocates two ports in 9800–9899 (the RIST listen port and
the loopback port the filter hands the cutter) and writes nothing but
`config/part8-channels.json`.

## 2. Analyse the ingest

The form stays open on the new channel. **Analyse ingest.**

The derived panel then reads:

```
sender    part8-recovery@ncn-guyana -> rist://<recovery-ip>:9800?buffer=8000
filter    ristsender-ncn-guyana-p8src -> 127.0.0.1:9801 (loopback)
service   2 - PMT 142 (0x008E)
pcr_cut   34 (0x0022)
pids      10 - 0x0000 0x0001 0x0010 0x0011 0x0012 0x0013 0x0014 0x0022 0x0023 0x008E
```

**Check `pcr_cut` against NCN's own PMT, not the transponder's first service.**
It must read 34 (0x0022). 33 (0x0021) is TLC-GUYANA's, and a cutter given it
would never see a PCR and never cut.

That is the whole configuration and none of it was typed. If it refuses, it
says why — the two it can say are "Analyse the input first" and, on an ingest
whose report lists several services without saying which PIDs belong to which,
that the elementary streams cannot be identified. The second is correct: feed it
a single-service ingest rather than working around it.

## 3. Start

**Start** in the **Part 8 recovery channels** row. That writes the two units and
starts `part8-recovery@ncn-guyana`; the tsp filter follows it (`BindsTo`), so
one button brings the chain up.

```sh
systemctl is-active part8-recovery@ncn-guyana ristsender-ncn-guyana-p8src
journalctl -u part8-recovery@ncn-guyana -n 20 --no-pager
```

The start log must show the framing, or the box will be cutting and the headend
will not:

```
[START] Part 8 recovery server: in=udp://@127.0.0.1:9801 out=rist://@0.0.0.0:9800 ...
[START] PCR-boundary framing ON, pcr_pid=0x0022 (34).
```

Then confirm it is producing:

```sh
echo stats | socat - UNIX-CONNECT:/run/part8-recovery/ncn-guyana/debug.sock
```

`ts_in` climbing and `pcr_cuts` climbing at roughly the PCR rate means the chain
is alive.

Two failure shapes worth naming, because both were seen by hand:

- **`ts_in` stuck at 0.** The cutter is receiving nothing. This is what the
  multicast hop did — the sender binds the port but never joins the group, and
  br-rist comes up NO-CARRIER anyway. The hop is loopback unicast now, so this
  should not recur; if it does, check the filter unit is running and that its
  `-O ip` and the sender's `P8_INPUT` name the same port.
- **`pcr_cuts` stuck at 0 while `ts_in` climbs.** Packets are arriving but none
  carry a PCR on the configured PID — the classic symptom of a `-C` from the
  wrong service.

The catalogue should also be filling:

```sh
echo 'bounds' | socat - UNIX-CONNECT:/run/part8-recovery/ncn-guyana/debug.sock
```

`pid 0x0022` with a rising entry count, and `resolve 34 <newest PCR> 0` should
answer `OK ... slot=0` — slot 0 meaning the PCR sits at the head of its payload,
which is the whole point of the PCR-boundary framing.

## 4. Check what the box will be told

```sh
curl -s 'http://localhost/api/recovery.php?service_id=2' | python3 -m json.tool
```

For a service with **no** Part 7 channel — which is NCN's case — the record has
no `rist_url` and no `marker_pid` at all:

```json
{
  "service_id": 2,
  "ts_id": 1,
  "name": "NCN-Guyana",
  "part8": 1,
  "part8_rist_url": "rist://<ip>:9800?buffer=8000",
  "part8_pcr_cut": 1,
  "part8_server_pcr_pid": 34,
  "part8_filter_pids": [0,1,16,17,18,19,20,34,35,142]
}
```

**Also check a Part 7 service is unchanged** — that is the isolation test that
matters to boxes in the field:

```sh
curl -s 'http://localhost/api/recovery.php?service_id=<a Part 7 service>'
```

It must still carry `rist_url` and `marker_pid` exactly as before, with no Part
8 keys added.

Then the anchor, which is the query the box makes at zap:

```sh
echo bounds | socat - UNIX-CONNECT:/run/part8-recovery/ncn-guyana/debug.sock
curl -s 'http://localhost/api/recovery.php?service_id=2&pcr=<a PCR from bounds>'
```

The record comes back with an `anchor` object; `start_wire` is the RTP sequence
carrying that PCR. `OUTSIDE_BUFFER` or `TOO_OLD` is a correct answer to a stale
PCR, not a fault.

---

## 5. Flash the box

No local config. The box finds service 2 in the API, sees `part8_rist_url`,
connects there instead of a Part 7 peer, and turns its own cutter on using
**its own** PCR PID from the tuned PMT.

After the zap:

```
chain: recovery peer = rist://<ip>:9800?buffer=8000 (Part 8 per-channel sender)
```

and no warning. If you see

```
chain: WARNING headend PCR PID 0x.... != ours 0x0022
```

the path between our ingest and the dish is renumbering PIDs, so the two
filtered streams cannot match and Part 8 is not usable on it.

`/tmp/ristpcrcut` still forces the box's cutter on for bench work against a
headend that has not been switched over. It is ORed with the API, so leaving it
set cannot turn Part 8 *off* on a channel the headend has enabled.

---

## Reverting

| how far back | what to do |
|---|---|
| stop the channel | **Stop** in the Part 8 table |
| remove it entirely | **Delete** — stops, disables and removes both units and the env file |
| fixed-7 framing, nothing else changed | delete `-C 34` from `P8_EXTRA` in `/etc/part8/instances/ncn-guyana.env`, then `sudo part8-unit restart ncn-guyana` |

None of these touch a Part 7 unit or `channels.json`.

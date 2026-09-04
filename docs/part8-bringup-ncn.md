# Bringing one channel up on Part 8 — NCN-Guyana, end to end

Service 0x0002, PMT 0x008E, PCR/video 0x0022, audio 0x0023. All-clear FTA, no
remap, no service rename — which is what makes it eligible.

Nothing here stops or restarts a Part 7 unit. The channel keeps serving Part 7
boxes throughout.

---

## 0. Install

One command on the headend, as always:

```sh
curl -fsSL "https://raw.githubusercontent.com/caritechsolutions/Satellite-Hybrid-Management-System/main/install.sh?$(date +%s)" | sudo bash
```

Watch two things in the output:

```
==> Checking librist against riststb
    librist matches riststb <sha>          <- the sync check; it must not warn
==> Isolation check: what changed in unit state
    no rist* unit changed state, enablement or main PID
```

The install rebuilds `part8_recovery_server` with `-C`. **Deployed
`part8-recovery@` instances keep running the old binary until they are
restarted**, and they have no `-C` in their env files, so restarting them is
also a no-op behaviourally. Nothing needs restarting for this.

---

## 1. Analyse the input

Channels page → NCN-Guyana → **Edit** → **Analyse input**.

This is what the whole feature is derived from, so check it before going on:

```
service_id 2   pmt_pid 142 (0x008E)   pcr_pid 34 (0x0022)
```

If the report lists more than one service and does not say which service each
PID belongs to, saving with Part 8 on will refuse and tell you so. That refusal
is correct — feed the channel a single-service ingest rather than working around
it.

---

## 2. Turn Part 8 on

Same edit form, **Part 8 recovery** section → tick **Run the Part 8 per-channel
recovery sender** → **Save changes**.

The panel then shows exactly what was derived. For NCN it should read:

```
sender    part8-recovery@ncn-guyana on port 9800 (inactive)
filter    ristsender-ncn-guyana-p8src -> 238.0.0.128:6400
pcr_cut   34
pids      10 - 0x0000 0x0001 0x0010 0x0011 0x0012 0x0013 0x0014 0x0022 0x0023 0x008E
```

If it refuses, it will say why. The three refusals are: no analysis, a PID
remap, a service rename. All three mean the box would filter different PID
numbers than the headend sends.

---

## 3. Start it

**P8 start** in the channel row. That starts `part8-recovery@ncn-guyana`, and
the tsp filter stage follows it (`BindsTo`), so one button brings the chain up.

Confirm from the shell:

```sh
systemctl is-active part8-recovery@ncn-guyana ristsender-ncn-guyana-p8src
journalctl -u part8-recovery@ncn-guyana -n 20 --no-pager
```

The start log must show the framing, or the box will be cutting and the headend
will not:

```
[START] Part 8 recovery server: in=udp://@238.0.0.128:6400 out=rist://@0.0.0.0:9800 ...
[START] PCR-boundary framing ON, pcr_pid=0x0022 (34).
```

And that the filter is producing:

```sh
part8-observe --socket /run/part8-recovery/ncn-guyana/debug.sock \
              --unit part8-recovery@ncn-guyana --interval 30 &
```

or once, by hand:

```sh
echo stats | socat - UNIX-CONNECT:/run/part8-recovery/ncn-guyana/debug.sock
```

`ts_in` climbing and `pcr_cuts` climbing at roughly the PCR rate means the chain
is alive. `pcr_cuts` stuck at 0 with `ts_in` climbing means the filter is passing
packets but none carry a PCR on 34 — check the PID list.

---

## 4. Check what the box will be told

```sh
curl -s 'http://localhost/api/recovery.php?service_id=2' | python3 -m json.tool
```

Expected — Part 7 fields unchanged, Part 8 fields added:

```json
{
  "service_id": 2,
  "name": "NCN-Guyana",
  "marker_pid": 8176,
  "rist_url": "rist://<ip>:5700?buffer=8000",
  "part8": 1,
  "part8_rist_url": "rist://<ip>:9800?buffer=8000",
  "part8_pcr_cut": 1,
  "part8_server_pcr_pid": 34,
  "part8_filter_pids": [0,1,16,17,18,19,20,34,35,142]
}
```

**`rist_url` must still be there and still point at 5700.** That is what the
Part 7 boxes in the field are using; if it has moved, stop and do not flash a
box.

Then the anchor, which is the query the box makes at zap. Take a current PCR
from the catalogue first:

```sh
echo bounds | socat - UNIX-CONNECT:/run/part8-recovery/ncn-guyana/debug.sock
curl -s 'http://localhost/api/recovery.php?service_id=2&pcr=<a PCR from bounds>'
```

The record comes back with an `anchor` object:

```json
"anchor": { "status": "OK", "pcr_pid": 34, "actual_pcr": ..., "start_ext": ..., "start_wire": ... }
```

`start_wire` is the RTP sequence number carrying that PCR — the box aligns on
it. A status of `OUTSIDE_BUFFER` or `TOO_OLD` is a correct answer to a stale
PCR, not a fault.

---

## 5. Flash the box

The box picks Part 8 up from the API with no local config: `part8_rist_url`
replaces `rist_url` for this service, and `part8_pcr_cut` turns its own cutter
on using **its own** PCR PID from the tuned PMT.

After the zap, in the box log:

```
chain: recovery peer = rist://<ip>:9800?buffer=8000 (Part 8 per-channel sender)
```

and no warning line. If you see

```
chain: WARNING headend PCR PID 0x.... != ours 0x0022
```

the uplink is renumbering and the two filtered streams cannot match — Part 8 is
not usable on that path, whatever the GUI let you enable.

`/tmp/ristpcrcut` still forces the cutter on for a bench box talking to a
headend that has not been switched over. It is an override, not the production
route, and it is ORed with the API — so leaving it set will not turn Part 8
*off* on a channel the headend has enabled.

---

## Reverting

| how far back | what to do |
|---|---|
| stop Part 8 for this channel | **P8 stop** in the GUI |
| remove Part 8 from this channel | untick the box and save — stops, disables and deletes both units and the env file |
| put the sender back to fixed-7 without touching anything else | delete `-C 34` from `P8_EXTRA` in `/etc/part8/instances/ncn-guyana.env`, then `sudo part8-unit restart ncn-guyana` |
| take the box back to Part 7 | the API stops advertising `part8` as soon as the unit is inactive, so a **P8 stop** is enough — the box falls back to `rist_url` on its next fetch |

None of these touch `ristsender-ncn-guyana` or `ristmarker-ncn-guyana`.

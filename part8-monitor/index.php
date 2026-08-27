<?php require __DIR__ . '/lib.php'; ?>
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Part 8 Recovery Servers</title>
<link rel="stylesheet" href="assets/app.css">
</head>
<body>

<header class="bar">
  <h1>Part 8 Recovery Servers</h1>
  <div class="bar-right">
    <span id="summary" class="summary"></span>
    <button id="addBtn" class="btn primary">Add instance</button>
  </div>
</header>

<p class="scope">
  Manages <code>part8-recovery@*</code> only. Part 7 units
  (<code>ristsender-*</code>, <code>ristmarker-*</code>) are not touched here &mdash;
  every change reports whether anything outside Part 8 moved.
</p>

<div id="isolation" class="isolation hidden"></div>
<div id="instances"></div>

<template id="tpl-card">
  <section class="card">
    <div class="card-head">
      <span class="dot"></span>
      <h2 class="name"></h2>
      <span class="pill state"></span>
      <span class="pill enabled"></span>
      <span class="grow"></span>
      <div class="controls">
        <button data-act="start"   class="btn">Start</button>
        <button data-act="stop"    class="btn">Stop</button>
        <button data-act="enable"  class="btn">Enable</button>
        <button data-act="disable" class="btn">Disable</button>
        <button data-act="edit"    class="btn">Edit</button>
        <button data-act="journal" class="btn">Log</button>
        <button data-act="remove"  class="btn danger">Remove</button>
      </div>
    </div>

    <div class="cfg"></div>

    <div class="metrics">
      <div class="metric" data-k="input">
        <label>Input bitrate</label><b class="v">&mdash;</b><small class="s"></small>
      </div>
      <div class="metric" data-k="buffer">
        <label>Buffer vs ceiling</label><b class="v">&mdash;</b><small class="s"></small>
        <div class="bar-track"><div class="bar-fill"></div></div>
      </div>
      <div class="metric" data-k="peers">
        <label>Peers</label><b class="v">&mdash;</b><small class="s"></small>
      </div>
      <div class="metric" data-k="cat">
        <label>Catalogue</label><b class="v">&mdash;</b><small class="s"></small>
      </div>
      <div class="metric" data-k="trip">
        <label>Tripwires</label><b class="v">&mdash;</b><small class="s"></small>
      </div>
      <div class="metric" data-k="disc">
        <label>Discontinuities</label><b class="v">&mdash;</b><small class="s"></small>
      </div>
    </div>

    <div class="peers hidden"></div>
    <div class="events hidden"></div>
    <pre class="journal hidden"></pre>
  </section>
</template>

<div id="modal" class="modal hidden">
  <form class="modal-box" id="frm">
    <h3 id="frmTitle">Add instance</h3>
    <label>Name
      <input name="name" id="fName" required pattern="[a-z0-9][a-z0-9-]{0,30}"
             placeholder="tp-astra-1">
      <small>lowercase letters, digits and hyphens; becomes <code>part8-recovery@&lt;name&gt;</code></small>
    </label>
    <label>Input address
      <input name="input" id="fInput" placeholder="udp://@239.10.0.1:5000">
      <small>the live transponder feed, raw TS over UDP. Left as
             <code>UNCONFIGURED</code> the instance installs but refuses to start.</small>
    </label>
    <label>Listen port
      <input name="listen_port" id="fPort" type="number" min="1" max="65535">
      <small>allocated from <?= P8_PORT_MIN ?>&ndash;<?= P8_PORT_MAX ?>; refused if it
             collides with another instance, a Part 7 unit, or anything already bound</small>
    </label>
    <label>Buffer target (ms)
      <input name="buffer_ms" id="fBuffer" type="number" min="250" max="60000"
             value="<?= P8_DEFAULT_BUFFER_MS ?>">
      <small>4000 is set on the RTT budget. It is not a switchover-gap lever &mdash;
             FSR resumes at the live edge with no backfill.</small>
    </label>
    <label class="check">
      <input name="require_selection" id="fSel" type="checkbox" checked>
      Require a registered content selection
      <small>on: a peer with no Part 6 selection gets no retransmissions. Off passes
             <code>-S</code>, and one request can pull the whole multiplex.</small>
    </label>
    <div id="frmErr" class="err hidden"></div>
    <div class="modal-actions">
      <button type="button" class="btn" id="frmCancel">Cancel</button>
      <button type="submit" class="btn primary" id="frmSave">Save</button>
    </div>
  </form>
</div>

<script src="assets/app.js"></script>
</body>
</html>

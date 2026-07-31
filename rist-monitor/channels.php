<?php
// channels.php - Channel management UI
require_once __DIR__ . '/config/config.php';
?>
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Channels &middot; RIST Monitor</title>
<style>
  :root{
    --bg:#0b1220; --panel:#131c2e; --panel2:#1a2438; --line:#243049;
    --text:#e6edf7; --dim:#8fa0bd; --accent:#3ddc97; --accent2:#4d9fff;
    --warn:#ffb454; --bad:#ff6b6b;
  }
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--text);
       font:14px/1.5 -apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif}
  header{background:var(--panel);border-bottom:1px solid var(--line);
         padding:14px 22px;display:flex;align-items:center;gap:18px}
  header h1{margin:0;font-size:17px;color:var(--accent);font-weight:600}
  header nav a{color:var(--dim);text-decoration:none;margin-right:14px;font-size:13px}
  header nav a:hover,header nav a.on{color:var(--text)}
  .server{margin-left:auto;font-size:12px;color:var(--dim)}
  .server b{color:var(--text);font-family:ui-monospace,Consolas,monospace}
  main{max-width:1180px;margin:22px auto;padding:0 22px}
  .card{background:var(--panel);border:1px solid var(--line);border-radius:10px;
        padding:18px 20px;margin-bottom:20px}
  .card h2{margin:0 0 14px;font-size:15px;font-weight:600}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:14px}
  label{display:block;font-size:12px;color:var(--dim);margin-bottom:5px}
  input{width:100%;background:var(--panel2);border:1px solid var(--line);border-radius:6px;
        color:var(--text);padding:9px 11px;font-size:13px;font-family:inherit}
  input:focus{outline:none;border-color:var(--accent2)}
  input[readonly]{color:var(--dim);background:#101828}
  .hint{font-size:11px;color:var(--dim);margin-top:4px}
  .row{display:flex;gap:10px;align-items:center;margin-top:16px;flex-wrap:wrap}
  button{border:0;border-radius:6px;padding:9px 16px;font-size:13px;font-weight:600;
         cursor:pointer;font-family:inherit}
  .primary{background:var(--accent);color:#062015}
  .ghost{background:transparent;color:var(--dim);border:1px solid var(--line)}
  .mini{padding:5px 11px;font-size:12px;font-weight:500}
  .start{background:rgba(61,220,151,.15);color:var(--accent)}
  .stop{background:rgba(255,180,84,.15);color:var(--warn)}
  .del{background:rgba(255,107,107,.13);color:var(--bad)}
  table{width:100%;border-collapse:collapse;font-size:13px}
  th{text-align:left;color:var(--dim);font-weight:500;font-size:11px;
     text-transform:uppercase;letter-spacing:.04em;padding:0 10px 9px;
     border-bottom:1px solid var(--line)}
  td{padding:11px 10px;border-bottom:1px solid rgba(36,48,73,.6);vertical-align:middle}
  tr:last-child td{border-bottom:0}
  .mono{font-family:ui-monospace,Consolas,monospace;font-size:12px}
  .pill{display:inline-block;padding:2px 9px;border-radius:20px;font-size:11px;font-weight:600}
  .active{background:rgba(61,220,151,.15);color:var(--accent)}
  .inactive{background:rgba(143,160,189,.13);color:var(--dim)}
  .failed{background:rgba(255,107,107,.15);color:var(--bad)}
  .empty{text-align:center;color:var(--dim);padding:26px}
  .statrow td{background:#0e1626;padding:0}
  .statwrap{padding:14px 16px}
  .stath{font-size:11px;text-transform:uppercase;letter-spacing:.04em;color:var(--dim);
         margin:0 0 8px;font-weight:600}
  .kpis{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:14px}
  .kpi{background:var(--panel2);border:1px solid var(--line);border-radius:8px;
       padding:9px 14px;min-width:104px}
  .kpi .v{font-size:16px;font-weight:600;font-family:ui-monospace,Consolas,monospace}
  .kpi .l{font-size:10px;color:var(--dim);text-transform:uppercase;letter-spacing:.04em}
  .peers{width:100%;border-collapse:collapse;font-size:12px;margin-bottom:12px}
  .peers th{font-size:10px;padding:0 8px 6px}
  .peers td{padding:6px 8px;border-bottom:1px solid rgba(36,48,73,.5)}
  .none{color:var(--dim);font-size:12px;padding:6px 8px}
  .good{color:var(--accent)} .mid{color:var(--warn)} .bad{color:var(--bad)}
  #toast{position:fixed;right:20px;bottom:20px;padding:11px 17px;border-radius:8px;
         font-size:13px;display:none;max-width:400px}
  .ok{background:rgba(61,220,151,.16);border:1px solid var(--accent);color:var(--accent)}
  .err{background:rgba(255,107,107,.16);border:1px solid var(--bad);color:var(--bad)}
</style>
</head>
<body>

<header>
  <h1>RIST Monitor</h1>
  <nav><a href="index.php">Dashboard</a><a href="channels.php" class="on">Channels</a></nav>
  <div class="server">server ip <b id="srvip">-</b></div>
</header>

<main>
  <div class="card">
    <h2 id="formTitle">Add channel</h2>
    <div class="grid">
      <div>
        <label>Channel name</label>
        <input id="f_name" placeholder="BBC One">
        <div class="hint">Must be unique - used for the service names</div>
      </div>
      <div>
        <label>Input UDP (from headend)</label>
        <input id="f_input" class="mono" placeholder="239.5.5.5:5000">
      </div>
      <div>
        <label>Output UDP (uplink to mux)</label>
        <input id="f_uplink" class="mono" placeholder="239.6.6.6:6000">
      </div>
      <div>
        <label>Marker PID</label>
        <input id="f_pid" class="mono" value="8176">
        <div class="hint">8176 = 0x1FF0 (current tool default)</div>
      </div>
    </div>

    <div class="grid" style="margin-top:14px">
      <div>
        <label>Satellite peer port (weight 0)</label>
        <input id="f_sat" class="mono" readonly value="auto">
      </div>
      <div>
        <label>Recovery peer port (weight 1000)</label>
        <input id="f_rec" class="mono" readonly value="auto">
      </div>
      <div>
        <label>Metrics port</label>
        <input id="f_met" class="mono" readonly value="auto">
      </div>
    </div>

    <div class="row">
      <button class="primary" id="saveBtn" onclick="save()">Create channel</button>
      <button class="ghost" id="cancelBtn" onclick="resetForm()" style="display:none">Cancel</button>
    </div>
  </div>

  <div class="card">
    <h2>Channels</h2>
    <table>
      <thead>
        <tr>
          <th>Name</th><th>Input</th><th>Uplink</th><th>Marker PID</th>
          <th>Ports (sat / rec / metrics)</th><th>Sender</th><th>Marker</th><th></th>
        </tr>
      </thead>
      <tbody id="rows"><tr><td colspan="8" class="empty">Loading&hellip;</td></tr></tbody>
    </table>
  </div>
</main>

<div id="toast"></div>

<script>
const API = 'api/channels.php';
let editing = null;
let openStats = new Set();

function toast(msg, ok = true) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.className = ok ? 'ok' : 'err';
  t.style.display = 'block';
  clearTimeout(t._h);
  t._h = setTimeout(() => t.style.display = 'none', 4200);
}

async function api(method, qs = '', body = null) {
  const opt = { method, headers: { 'Content-Type': 'application/json' } };
  if (body) opt.body = JSON.stringify(body);
  const r = await fetch(API + qs, opt);
  const j = await r.json();
  if (j.error) throw new Error(j.message || 'Request failed');
  return j.data;
}

function pill(state) {
  const cls = state === 'active' ? 'active' : (state === 'failed' ? 'failed' : 'inactive');
  return `<span class="pill ${cls}">${state}</span>`;
}

async function load() {
  try {
    const d = await api('GET');
    document.getElementById('srvip').textContent = d.settings.server_ip || '-';
    const rows = document.getElementById('rows');

    if (!d.channels.length) {
      rows.innerHTML = '<tr><td colspan="8" class="empty">No channels yet - add one above</td></tr>';
      return;
    }

    rows.innerHTML = d.channels.map(c => `
      <tr id="row-${c.id}">
        <td><b>${esc(c.name)}</b><div class="hint mono">${esc(c.id)}</div></td>
        <td class="mono">${esc(c.input_url)}</td>
        <td class="mono">${esc(c.uplink_url)}</td>
        <td class="mono">${c.marker_pid} <span class="hint">0x${(+c.marker_pid).toString(16).toUpperCase()}</span></td>
        <td class="mono">${c.sat_port} / ${c.recovery_port} / ${c.metrics_port}</td>
        <td>${pill(c.status)}</td>
        <td>${pill(c.marker_status)}</td>
        <td style="white-space:nowrap">
          ${c.status === 'active'
            ? `<button class="mini stop" onclick="ctl('${c.id}','stop')">Stop</button>`
            : `<button class="mini start" onclick="ctl('${c.id}','start')">Start</button>`}
          <button class="mini ghost" onclick="toggleStats('${c.id}')">${openStats.has(c.id) ? 'Hide' : 'Stats'}</button>
          <button class="mini ghost" onclick='edit(${JSON.stringify(c)})'>Edit</button>
          <button class="mini del" onclick="del('${c.id}','${esc(c.name)}')">Delete</button>
        </td>
      </tr>
      ${openStats.has(c.id) ? `<tr class="statrow" id="st-${c.id}"><td colspan="8">
           <div class="statwrap" id="sw-${c.id}">Loading stats&hellip;</div></td></tr>` : ''}`).join('');

    openStats.forEach(id => loadStats(id));
  } catch (e) {
    toast(e.message, false);
    document.getElementById('rows').innerHTML =
      `<tr><td colspan="8" class="empty">${esc(e.message)}</td></tr>`;
  }
}

function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, m =>
    ({ '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;' }[m]));
}

async function save() {
  const payload = {
    name:       document.getElementById('f_name').value,
    input_url:  document.getElementById('f_input').value,
    uplink_url: document.getElementById('f_uplink').value,
    marker_pid: document.getElementById('f_pid').value,
  };
  try {
    if (editing) {
      await api('PUT', '?id=' + encodeURIComponent(editing), payload);
      toast('Channel updated');
    } else {
      await api('POST', '', payload);
      toast('Channel created - press Start to bring it up');
    }
    resetForm();
    load();
  } catch (e) { toast(e.message, false); }
}

function edit(c) {
  editing = c.id;
  document.getElementById('formTitle').textContent = 'Edit channel';
  document.getElementById('f_name').value   = c.name;
  document.getElementById('f_input').value  = c.input_url;
  document.getElementById('f_uplink').value = c.uplink_url;
  document.getElementById('f_pid').value    = c.marker_pid;
  document.getElementById('f_sat').value    = c.sat_port;
  document.getElementById('f_rec').value    = c.recovery_port;
  document.getElementById('f_met').value    = c.metrics_port;
  document.getElementById('saveBtn').textContent = 'Save changes';
  document.getElementById('cancelBtn').style.display = '';
  window.scrollTo({ top: 0, behavior: 'smooth' });
}

function resetForm() {
  editing = null;
  ['f_name','f_input','f_uplink'].forEach(i => document.getElementById(i).value = '');
  document.getElementById('f_pid').value = '8176';
  ['f_sat','f_rec','f_met'].forEach(i => document.getElementById(i).value = 'auto');
  document.getElementById('formTitle').textContent = 'Add channel';
  document.getElementById('saveBtn').textContent = 'Create channel';
  document.getElementById('cancelBtn').style.display = 'none';
}

async function ctl(id, action) {
  try {
    await api('POST', `?action=${action}&id=${encodeURIComponent(id)}`);
    toast(`${action} ok`);
    setTimeout(load, 700);
  } catch (e) { toast(e.message, false); }
}

async function del(id, name) {
  if (!confirm(`Delete channel "${name}"? This stops and removes both services.`)) return;
  try {
    await api('DELETE', '?id=' + encodeURIComponent(id));
    toast('Channel deleted');
    load();
  } catch (e) { toast(e.message, false); }
}

function toggleStats(id) {
  if (openStats.has(id)) openStats.delete(id); else openStats.add(id);
  load();
}

function fmtBps(v) {
  v = +v || 0;
  if (v >= 1e6) return (v / 1e6).toFixed(2) + ' Mbps';
  if (v >= 1e3) return (v / 1e3).toFixed(0) + ' kbps';
  return v.toFixed(0) + ' bps';
}

function qcls(q) { return q >= 99 ? 'good' : (q >= 90 ? 'mid' : 'bad'); }

function peerTable(list, emptyMsg) {
  if (!list.length) return `<div class="none">${emptyMsg}</div>`;
  return `<table class="peers">
    <thead><tr><th>Remote</th><th>CNAME</th><th>Bandwidth</th><th>RTT</th>
               <th>Quality</th><th>Sent</th><th>Retrans</th></tr></thead>
    <tbody>${list.map(p => `<tr>
      <td class="mono">${esc(p.remote)}</td>
      <td class="mono">${esc(p.cname)}</td>
      <td class="mono">${fmtBps(p.bandwidth_bps)}</td>
      <td class="mono">${p.rtt_ms} ms</td>
      <td class="mono ${qcls(p.quality)}">${p.quality}%</td>
      <td class="mono">${p.sent}</td>
      <td class="mono">${p.retransmitted}</td>
    </tr>`).join('')}</tbody></table>`;
}

async function loadStats(id) {
  const box = document.getElementById('sw-' + id);
  if (!box) return;
  try {
    const s = await api('GET', '?stats=1&id=' + encodeURIComponent(id));
    if (!s.available) {
      box.innerHTML = '<div class="none">No metrics - the sender is not running, '
                    + 'or nothing has connected to port ' + s.metrics_port + ' yet.</div>';
      return;
    }
    box.innerHTML = `
      <div class="kpis">
        <div class="kpi"><div class="v">${s.totals.peers}</div><div class="l">Peers</div></div>
        <div class="kpi"><div class="v">${fmtBps(s.totals.bandwidth_bps)}</div><div class="l">Bandwidth</div></div>
        <div class="kpi"><div class="v">${s.recovery.length}</div><div class="l">In recovery</div></div>
        <div class="kpi"><div class="v">${s.totals.retransmitted}</div><div class="l">Retransmits</div></div>
      </div>
      <p class="stath">Satellite path &mdash; weight 0 (feeds the marker &rarr; uplink)</p>
      ${peerTable(s.satellite, 'No satellite peer connected - is ristmarker running?')}
      <p class="stath">Recovery path &mdash; weight 1000 (receivers pulling over IP)</p>
      ${peerTable(s.recovery, 'No receivers connected yet')}`;
  } catch (e) {
    box.innerHTML = `<div class="none">${esc(e.message)}</div>`;
  }
}

load();
setInterval(load, 10000);
</script>
</body>
</html>

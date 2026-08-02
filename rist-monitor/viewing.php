<?php
// viewing.php - Viewing statistics reported by the set-top boxes
require_once __DIR__ . '/config/config.php';
?>
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Viewing &middot; RIST Monitor</title>
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
  .right{margin-left:auto;display:flex;align-items:center;gap:8px}
  main{max-width:1240px;margin:22px auto;padding:0 22px}
  .card{background:var(--panel);border:1px solid var(--line);border-radius:10px;
        padding:18px 20px;margin-bottom:20px}
  .card h2{margin:0 0 14px;font-size:15px;font-weight:600}
  .kpis{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px}
  .kpi{background:var(--panel2);border:1px solid var(--line);border-radius:8px;padding:12px 14px}
  .kpi .v{font-size:22px;font-weight:600;font-family:ui-monospace,Consolas,monospace}
  .kpi .l{font-size:10px;color:var(--dim);text-transform:uppercase;letter-spacing:.05em;margin-top:2px}
  table{width:100%;border-collapse:collapse;font-size:13px}
  th{text-align:left;color:var(--dim);font-weight:500;font-size:11px;
     text-transform:uppercase;letter-spacing:.04em;padding:0 10px 9px;
     border-bottom:1px solid var(--line);white-space:nowrap}
  td{padding:9px 10px;border-bottom:1px solid rgba(36,48,73,.6);vertical-align:middle}
  tr:last-child td{border-bottom:0}
  .mono{font-family:ui-monospace,Consolas,monospace;font-size:12px}
  .num{text-align:right}
  .pill{display:inline-block;padding:2px 9px;border-radius:20px;font-size:11px;font-weight:600}
  .rist{background:rgba(77,159,255,.15);color:var(--accent2)}
  .tuner{background:rgba(143,160,189,.14);color:var(--dim)}
  .fail{background:rgba(255,107,107,.15);color:var(--bad)}
  .rec{background:rgba(255,180,84,.15);color:var(--warn)}
  .empty{text-align:center;color:var(--dim);padding:26px}
  .hint{font-size:11px;color:var(--dim)}
  select,button{background:var(--panel2);border:1px solid var(--line);border-radius:6px;
                color:var(--text);padding:7px 11px;font-size:13px;font-family:inherit;cursor:pointer}
  .bar{height:5px;background:var(--panel2);border-radius:3px;overflow:hidden;min-width:70px;margin-top:4px}
  .bar i{display:block;height:100%;background:var(--accent2)}
</style>
</head>
<body>

<header>
  <h1>RIST Monitor</h1>
  <nav>
    <a href="index.php">Dashboard</a>
    <a href="channels.php">Channels</a>
    <a href="viewing.php" class="on">Viewing</a>
  </nav>
  <div class="right">
    <select id="range" onchange="load()">
      <option value="1h">Last hour</option>
      <option value="24h" selected>Last 24 hours</option>
      <option value="7d">Last 7 days</option>
      <option value="all">All time</option>
    </select>
    <button onclick="load()">Refresh</button>
  </div>
</header>

<main>
  <div class="card">
    <h2>Summary</h2>
    <div class="kpis" id="kpis"><div class="empty">Loading&hellip;</div></div>
    <div class="hint" id="note" style="margin-top:12px"></div>
  </div>

  <div class="card">
    <h2>By channel</h2>
    <table>
      <thead><tr>
        <th>Channel</th><th>Service ID</th>
        <th class="num">Views</th><th class="num">Watch time</th><th class="num">Avg view</th>
        <th>Path split</th><th class="num">Recovery-only</th>
        <th class="num">Avg to picture</th><th class="num">Failed</th>
      </tr></thead>
      <tbody id="chans"><tr><td colspan="9" class="empty">Loading&hellip;</td></tr></tbody>
    </table>
  </div>

  <div class="card">
    <h2>By box</h2>
    <table>
      <thead><tr>
        <th>Box</th><th class="num">Views</th><th class="num">Watch time</th>
        <th class="num">Via RIST</th><th>Clock</th><th>Last seen</th>
      </tr></thead>
      <tbody id="boxes"><tr><td colspan="6" class="empty">Loading&hellip;</td></tr></tbody>
    </table>
  </div>

  <div class="card">
    <h2>Recent views</h2>
    <table>
      <thead><tr>
        <th>Received</th><th>Box</th><th>Channel</th><th>Path</th>
        <th>Source</th><th class="num">Duration</th><th class="num">To picture</th>
      </tr></thead>
      <tbody id="recent"><tr><td colspan="7" class="empty">Loading&hellip;</td></tr></tbody>
    </table>
  </div>
</main>

<script>
const esc = s => String(s ?? '').replace(/[&<>"']/g, m =>
  ({ '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;' }[m]));

function kpi(v, l) { return `<div class="kpi"><div class="v">${v}</div><div class="l">${l}</div></div>`; }

function ffTxt(ms) { return ms < 0 ? '<span class="pill fail">never</span>'
                                   : `<span class="mono">${(ms/1000).toFixed(1)}s</span>`; }

function shortBox(id) {
  // MACs are long; show the last three octets, full value on hover.
  const p = String(id).split(':');
  return p.length >= 3 ? `<span class="mono" title="${esc(id)}">…${p.slice(-3).join(':')}</span>`
                       : `<span class="mono">${esc(id)}</span>`;
}

function ago(iso) {
  const s = Math.max(0, (Date.now() - new Date(iso).getTime()) / 1000);
  if (s < 60)    return Math.round(s) + 's ago';
  if (s < 3600)  return Math.round(s/60) + 'm ago';
  if (s < 86400) return Math.round(s/3600) + 'h ago';
  return Math.round(s/86400) + 'd ago';
}

async function load() {
  const range = document.getElementById('range').value;
  try {
    const r = await fetch('api/viewing.php?range=' + range);
    const d = await r.json();
    if (d.error) throw new Error(d.message || 'Request failed');

    const s = d.summary;
    const pctRist = s.views ? Math.round(100 * s.rist_views / s.views) : 0;
    document.getElementById('kpis').innerHTML =
      kpi(s.views, 'Views') +
      kpi(s.watch, 'Watch time') +
      kpi(s.boxes, 'Boxes') +
      kpi(pctRist + '%', 'Via RIST') +
      kpi(s.recovery_only_views, 'Recovery-only') +
      kpi(s.failed_views, 'Failed views');

    document.getElementById('note').textContent = d.log_present
      ? 'Times are server-side (received_at) — the box clock is not trusted.'
      : 'No stats log yet. Boxes write one after their first POST.';

    // ---- channels
    const ch = document.getElementById('chans');
    if (!d.channels.length) {
      ch.innerHTML = '<tr><td colspan="9" class="empty">Nothing reported in this period</td></tr>';
    } else {
      ch.innerHTML = d.channels.map(c => {
        const tot = c.rist + c.tuner || 1;
        const pct = Math.round(100 * c.rist / tot);
        return `<tr>
          <td><b>${esc(c.name)}</b></td>
          <td class="mono">${c.service_id}</td>
          <td class="num mono">${c.views}</td>
          <td class="num mono">${esc(c.watch)}</td>
          <td class="num mono">${esc(c.avg)}</td>
          <td>
            <span class="mono">${pct}% RIST</span>
            <div class="bar"><i style="width:${pct}%"></i></div>
          </td>
          <td class="num mono">${c.recovery_only || '-'}</td>
          <td class="num">${ffTxt(c.avg_ff)}</td>
          <td class="num mono">${c.failed ? `<span class="pill fail">${c.failed}</span>` : '-'}</td>
        </tr>`;
      }).join('');
    }

    // ---- boxes
    const bx = document.getElementById('boxes');
    bx.innerHTML = d.boxes.length ? d.boxes.map(b => `<tr>
        <td>${shortBox(b.box_id)}</td>
        <td class="num mono">${b.views}</td>
        <td class="num mono">${esc(b.watch)}</td>
        <td class="num mono">${b.rist}</td>
        <td>${b.clock_synced ? '<span class="pill rist">synced</span>'
                             : '<span class="pill rec">unsynced</span>'}</td>
        <td class="mono">${b.last_seen ? ago(b.last_seen) : '-'}</td>
      </tr>`).join('')
      : '<tr><td colspan="6" class="empty">No boxes reporting</td></tr>';

    // ---- recent
    const rc = document.getElementById('recent');
    rc.innerHTML = d.recent.length ? d.recent.map(v => `<tr>
        <td class="mono">${ago(v.received_at)}</td>
        <td>${shortBox(v.box_id)}</td>
        <td>${esc(v.name)} <span class="hint mono">${v.service_id}</span></td>
        <td><span class="pill ${v.path === 'rist' ? 'rist' : 'tuner'}">${esc(v.path || '?')}</span></td>
        <td>${v.sat_source === 'none' ? '<span class="pill rec">recovery only</span>'
                                      : `<span class="hint">${esc(v.sat_source || '-')}</span>`}</td>
        <td class="num mono">${esc(v.duration)}</td>
        <td class="num">${ffTxt(v.first_frame_ms)}</td>
      </tr>`).join('')
      : '<tr><td colspan="7" class="empty">No views yet</td></tr>';

  } catch (e) {
    document.getElementById('kpis').innerHTML =
      `<div class="empty">${esc(e.message)}</div>`;
  }
}

load();
setInterval(load, 15000);
</script>
</body>
</html>

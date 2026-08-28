/* Part 8 management UI.
 *
 * Polls api.php?action=list, which walks every instance's debug socket. Slow
 * enough that a wedged instance costs a tick rather than a hang; the server
 * bounds its own socket writes, so a stuck instance cannot stall the others.
 */
'use strict';

const POLL_MS = 4000;
const $  = (s, r) => (r || document).querySelector(s);
const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));

let editing = null;
let openPanels = {};      // name -> 'peers' | 'events' | 'journal'

function mbps(bps) {
  if (!bps || bps <= 0) return '0';
  return (bps / 1e6).toFixed(2);
}
function ago(sec) {
  const d = Math.max(0, Math.floor(Date.now() / 1000 - sec));
  if (d < 60) return d + 's ago';
  if (d < 3600) return Math.floor(d / 60) + 'm ago';
  if (d < 86400) return Math.floor(d / 3600) + 'h ago';
  return Math.floor(d / 86400) + 'd ago';
}

/*
 * Never let a non-JSON response throw. A 500 with an empty body used to surface
 * as "Unexpected end of JSON input" in the console and nothing in the UI, which
 * meant reading the PHP-FPM log to learn anything. Whatever comes back, this
 * returns an object the caller can render.
 */
async function api(params) {
  const q = new URLSearchParams(params);
  let r, text;
  try {
    r = await fetch('api.php?' + q.toString(), { cache: 'no-store' });
    text = await r.text();
  } catch (e) {
    return { ok: false, error: 'Could not reach api.php: ' + e.message };
  }
  if (!text.trim()) {
    return { ok: false,
             error: `api.php returned HTTP ${r.status} with an EMPTY body.\n` +
                    `That is a PHP fatal before any output, or php-fpm refusing ` +
                    `the request. Check: journalctl -u php*-fpm -n 50` };
  }
  try {
    return JSON.parse(text);
  } catch (e) {
    return { ok: false,
             error: `api.php returned HTTP ${r.status} with a non-JSON body:\n` +
                    text.slice(0, 600) };
  }
}

function showError(msg) {
  const el = $('#isolation');
  el.classList.remove('hidden');
  el.classList.remove('ok');
  el.textContent = msg;
}

function showIsolation(text, name) {
  const el = $('#isolation');
  if (!text) { el.classList.add('hidden'); return; }
  const clean = text.indexOf('PART7-UNCHANGED') !== -1;
  el.classList.remove('hidden');
  el.classList.toggle('ok', clean);
  el.textContent = clean
    ? `Isolation check after ${name}: no rist* unit changed state, was restarted, or changed enablement.`
    : `WARNING - a unit outside Part 8 changed while running "${name}":\n` + text;
  if (clean) setTimeout(() => el.classList.add('hidden'), 6000);
}

/* --------------------------------------------------------------- rendering */

function render(rows) {
  const host = $('#instances');
  const seen = new Set();
  let active = 0, tripped = 0, failed = 0;

  rows.forEach(row => {
    seen.add(row.name);
    let card = $(`[data-name="${CSS.escape(row.name)}"]`, host);
    if (!card) {
      card = $('#tpl-card').content.firstElementChild.cloneNode(true);
      card.dataset.name = row.name;
      card.addEventListener('click', onCardClick);
      host.appendChild(card);
    }
    const s = row.stats;
    const st = row.state || {};
    const trip = s && s.tripwires && (s.tripwires.a || s.tripwires.b);

    if (st.active === 'failed') failed++;
    else if (st.active === 'active') { active++; if (trip) tripped++; }

    card.className = 'card ' + (
      st.active === 'failed' ? 'is-failed' :
      st.active !== 'active' ? 'is-stopped' :
      trip ? 'is-tripped' : 'is-active');

    $('.name', card).textContent = row.name;

    const pState = $('.state', card);
    pState.textContent = st.active || '?';
    pState.className = 'pill state ' + (st.active === 'active' ? 'ok'
                     : st.active === 'failed' ? 'bad' : '');

    const pEn = $('.enabled', card);
    pEn.textContent = st.enabled || '?';
    pEn.className = 'pill enabled ' + (st.enabled === 'enabled' ? 'ok' : '');

    const c = row.config || {};
    const unconf = c.input === 'UNCONFIGURED';
    $('.cfg', card).innerHTML =
      `in <b>${unconf ? '<span style="color:var(--warn)">UNCONFIGURED</span>' : esc(c.input)}</b>` +
      ` &nbsp;·&nbsp; listen <b>:${c.listen_port}</b>` +
      ` &nbsp;·&nbsp; buffer <b>${c.buffer_ms} ms</b>` +
      ` &nbsp;·&nbsp; selection <b>${c.require_selection ? 'required' : 'NOT required (-S)'}</b>`;

    $('[data-act="start"]', card).disabled  = st.active === 'active' || unconf;
    $('[data-act="stop"]', card).disabled   = st.active !== 'active';
    $('[data-act="enable"]', card).disabled  = st.enabled === 'enabled';
    $('[data-act="disable"]', card).disabled = st.enabled !== 'enabled';

    renderMetrics(card, row, s);
    renderPanels(card, row, s);
  });

  $$('.card', host).forEach(c => { if (!seen.has(c.dataset.name)) c.remove(); });

  if (!rows.length && !$('.empty', host))
    host.innerHTML = '<p class="empty">No instances yet. "Add instance" creates one per transponder.</p>';

  const bits = [`${rows.length} instance${rows.length === 1 ? '' : 's'}`, `${active} running`];
  if (tripped) bits.push(`${tripped} TRIPWIRE`);
  if (failed)  bits.push(`${failed} FAILED`);
  $('#summary').textContent = bits.join(' · ');
}

function setMetric(card, key, value, sub, cls) {
  const m = $(`.metric[data-k="${key}"]`, card);
  m.className = 'metric' + (cls ? ' ' + cls : '');
  $('.v', m).textContent = value;
  $('.s', m).textContent = sub || '';
  return m;
}

function renderMetrics(card, row, s) {
  if (!s) {
    ['input','buffer','peers','cat','trip','disc']
      .forEach(k => setMetric(card, k, '—', row.state.active === 'active' ? 'no socket' : 'stopped'));
    return;
  }

  // (1) input bitrate: RIST reports what it sends, never what arrives.
  const win = s.input.rate_bps_window, life = s.input.rate_bps_lifetime;
  setMetric(card, 'input', mbps(win) + ' Mb/s',
    `lifetime ${mbps(life)} · ${s.input.rtp_pps.toFixed(0)} payload/s` +
    (s.input.sync_errors ? ` · ${s.input.sync_errors} SYNC ERR` : ''),
    win <= 0 ? 'bad' : s.input.sync_errors ? 'warn' : '');

  // Buffer against THIS instance's own ceiling, derived from its own rate.
  const b = s.buffer, pct = b.pct_of_ceiling;
  const bcls = pct >= 75 ? 'bad' : pct >= 50 ? 'warn' : 'ok';
  const m = setMetric(card, 'buffer', pct.toFixed(1) + '%',
    `${b.payloads} payloads · ${b.ms} ms · ceiling at ${b.safe_buffer_s.toFixed(1)} s`,
    b.have_stats ? bcls : '');
  const fill = $('.bar-fill', m);
  fill.style.width = Math.min(100, pct) + '%';
  fill.className = 'bar-fill ' + (pct >= 75 ? 'bad' : pct >= 50 ? 'warn' : '');

  const p = s.peers;
  const q = p.list.length ? (p.list.reduce((a, x) => a + x.quality, 0) / p.list.length) : 0;
  setMetric(card, 'peers', String(p.count),
    p.count ? `mean quality ${q.toFixed(1)}%` : 'none attached',
    p.count ? (q < 95 ? 'warn' : 'ok') : '');

  setMetric(card, 'cat', String(s.catalogue.pid_count),
    `${s.catalogue.entries} entries`, s.catalogue.pid_count ? '' : 'warn');

  const t = s.tripwires;
  setMetric(card, 'trip', (t.a || t.b) ? [t.a && 'A', t.b && 'B'].filter(Boolean).join('+') : 'clear',
    t.outside_buffer ? `${t.outside_buffer} out-of-range` : 'none fired',
    (t.a || t.b) ? 'bad' : 'ok');

  // (3) discontinuities: section 7 is open, so this must not be buried.
  const d = s.discontinuities;
  setMetric(card, 'disc', String(d.total),
    d.total ? 'click to inspect' : 'none seen', d.total ? 'warn' : '');
}

function renderPanels(card, row, s) {
  const which = openPanels[row.name];
  const peers = $('.peers', card), events = $('.events', card);
  peers.classList.toggle('hidden', which !== 'peers');
  events.classList.toggle('hidden', which !== 'events');
  $('.journal', card).classList.toggle('hidden', which !== 'journal');
  if (!s) return;

  if (which === 'peers') {
    peers.innerHTML = s.peers.list.length
      ? '<table><tr><th>peer</th><th>quality</th><th>RTT ms</th><th>retransmitted</th><th>sent</th></tr>'
        + s.peers.list.map(p => `<tr><td>${p.id}</td><td class="num">${p.quality.toFixed(2)}</td>`
          + `<td class="num">${p.rtt_ms}</td><td class="num">${p.retransmitted}</td>`
          + `<td class="num">${p.sent}</td></tr>`).join('') + '</table>'
      : '<p class="empty">No peers attached. With selection required, a receiver '
        + 'must register a Part 6 content selection or it gets nothing.</p>';
  }

  if (which === 'events') {
    const d = s.discontinuities;
    events.innerHTML = d.recent.length
      ? `<table><tr><th>when</th><th>PID</th><th>kind</th><th>epoch</th>`
        + `<th>delta ms</th><th>ext_seq</th></tr>`
        + d.recent.slice().reverse().map(e =>
            `<tr class="${e.kind === 'backward-jump' ? 'evt-back' : ''}">`
            + `<td>${ago(e.at)}</td><td>0x${e.pid.toString(16).toUpperCase().padStart(4,'0')}</td>`
            + `<td>${esc(e.kind)}</td><td class="num">${e.epoch}</td>`
            + `<td class="num">${e.delta_ms.toFixed(1)}</td>`
            + `<td class="num">${e.ext_seq}</td></tr>`).join('')
        + `</table><p class="cfg">${d.total} total, ${d.indicator_nopcr} were an indicator `
        + `on a packet carrying no PCR.</p>`
      : '<p class="empty">No discontinuities recorded on this instance.</p>';
  }
}

function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g,
    c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

/* ------------------------------------------------------------------ events */

async function onCardClick(ev) {
  const btn = ev.target.closest('button[data-act]');
  if (!btn) return;
  const card = ev.currentTarget, name = card.dataset.name, act = btn.dataset.act;

  if (act === 'edit')    return openForm(name);
  if (act === 'journal') {
    openPanels[name] = openPanels[name] === 'journal' ? null : 'journal';
    if (openPanels[name] === 'journal') {
      const r = await api({ action: 'journal', name });
      $('.journal', card).textContent = r.text || '(no journal output)';
    }
    return tick();
  }
  if (act === 'remove') {
    if (!confirm(`Remove instance "${name}"?\n\nStops and disables it and deletes its config. `
               + `Part 7 units are not touched.`)) return;
    const r = await api({ action: 'remove', name });
    if (!r.ok) return alert('Remove failed: ' + r.error);
    showIsolation(r.isolation, 'remove ' + name);
    return tick();
  }

  btn.disabled = true;
  const r = await api({ action: act, name });
  if (!r.ok) alert(`${act} failed:\n${r.error || r.output || 'unknown error'}`);
  else showIsolation(r.isolation, `${act} ${name}`);
  tick();
}

// The metric tiles double as toggles for the detail tables.
document.addEventListener('click', ev => {
  const m = ev.target.closest('.metric');
  if (!m) return;
  const card = ev.target.closest('.card');
  if (!card) return;
  const k = m.dataset.k;
  const want = k === 'peers' ? 'peers' : k === 'disc' ? 'events' : null;
  if (!want) return;
  openPanels[card.dataset.name] = openPanels[card.dataset.name] === want ? null : want;
  tick();
});

/* -------------------------------------------------------------------- form */

async function openForm(name) {
  editing = name || null;
  $('#frmTitle').textContent = name ? `Edit ${name}` : 'Add instance';
  $('#frmErr').classList.add('hidden');
  $('#fName').disabled = !!name;

  if (name) {
    const r = await api({ action: 'list' });
    const row = (r.instances || []).find(i => i.name === name);
    const c = row ? row.config : {};
    $('#fName').value   = name;
    $('#fInput').value  = c.input === 'UNCONFIGURED' ? '' : (c.input || '');
    $('#fPort').value   = c.listen_port || '';
    $('#fBuffer').value = c.buffer_ms || 4000;
    $('#fSel').checked  = !!c.require_selection;
  } else {
    $('#frm').reset();
    $('#fSel').checked = true;
    const p = await api({ action: 'suggest_port' });
    $('#fPort').value = p.ok ? p.port : '';
    if (!p.ok) showFormErr(p.error + (p.where ? '\n  at ' + p.where : ''));
  }
  $('#modal').classList.remove('hidden');
}

function showFormErr(msg) {
  const e = $('#frmErr');
  e.textContent = msg;
  e.classList.remove('hidden');
}

$('#addBtn').addEventListener('click', () => openForm(null));
$('#frmCancel').addEventListener('click', () => $('#modal').classList.add('hidden'));
$('#modal').addEventListener('click', ev => {
  if (ev.target.id === 'modal') $('#modal').classList.add('hidden');
});

$('#frm').addEventListener('submit', async ev => {
  ev.preventDefault();
  $('#frmSave').disabled = true;
  const r = await api({
    action: editing ? 'update' : 'create',
    name: $('#fName').value.trim(),
    input: $('#fInput').value.trim() || 'UNCONFIGURED',
    listen_port: $('#fPort').value,
    buffer_ms: $('#fBuffer').value,
    require_selection: $('#fSel').checked ? 1 : ''
  });
  $('#frmSave').disabled = false;
  if (!r.ok) return showFormErr(r.error + (r.where ? '\n  at ' + r.where : ''));
  showIsolation(r.isolation, (editing ? 'update ' : 'create ') + $('#fName').value.trim());
  $('#modal').classList.add('hidden');
  tick();
});

/* -------------------------------------------------------------------- poll */

let busy = false;
async function tick() {
  if (busy) return;
  busy = true;
  try {
    const r = await api({ action: 'list' });
    if (r.ok) render(r.instances || []);
    else if (r && r.error) {
      showError('list failed: ' + r.error + (r.where ? '\n  at ' + r.where : ''));
      $('#summary').textContent = 'API error';
    }
  } catch (e) {
    $('#summary').textContent = 'API unreachable';
    showError('Unexpected UI error: ' + e.message);
  } finally {
    busy = false;
  }
}
tick();
setInterval(tick, POLL_MS);

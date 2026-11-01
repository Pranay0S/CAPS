'use strict';
/* CAPS IV pump testbench - browser client. No dependencies. */

const WINDOW_MS = 60_000;
const MAX_TLM = 3000;      // ~5 min at 10 Hz kept client-side for charts
const MAX_EVENTS = 5000;
const MAX_LOG_ROWS = 1000;
const LEVELS = { DEBUG: 0, INFO: 1, WARN: 2, ERROR: 3 };

const $ = (id) => document.getElementById(id);

const state = {
  tlm: [],
  events: [],
  latest: null,
  hoverTick: null,
};

// ---- formatting ------------------------------------------------------------------

const fmt = (v, d = 1) => (v == null || Number.isNaN(v) ? '—' : Number(v).toFixed(d));
const secs = (ms) => (ms == null ? '' : (ms / 1000).toFixed(1));

function setBadge(el, text, tone) {
  el.textContent = text;
  if (tone) el.dataset.tone = tone;
  else delete el.dataset.tone;
}

const STATE_TONE = { RUNNING: 'good', COMPLETE: 'good', PAUSED: 'warning', ALARM: 'critical' };

function renderStatus(t) {
  if (!t) return;
  setBadge($('st-state'), t.state, STATE_TONE[t.state]);
  setBadge($('st-alarm'), t.alarm, t.alarm === 'NONE' ? null : 'critical');
  setBadge($('st-attack'), t.attack, t.attack === 'NONE' ? null : 'serious');
  setBadge($('st-anomaly'), t.anomaly.replaceAll('|', ' · '), t.anomaly === 'NONE' ? null : 'warning');
  $('st-time').textContent = `${secs(t.tick_ms)} s`;

  $('t-target').textContent = fmt(t.target_rate, 1);
  $('t-flow').textContent = fmt(t.flow_meas, 1);
  $('t-flow-true').textContent = fmt(t.flow_true, 1);
  $('t-press').textContent = fmt(t.pressure_meas, 1);
  $('t-vol').textContent = `${fmt(t.volume_est, 3)} / ${fmt(t.volume_true, 3)}`;
  $('t-duty').textContent = fmt(t.duty * 100, 1);
}

// ---- charts --------------------------------------------------------------------------

const charts = [];
let drawPending = false;
function requestDraw() {
  if (drawPending) return;
  drawPending = true;
  requestAnimationFrame(() => {
    drawPending = false;
    for (const c of charts) c.draw(state.tlm);
  });
}

function cssVar(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

function niceStep(span, target = 5) {
  const raw = span / target;
  const mag = 10 ** Math.floor(Math.log10(raw));
  const norm = raw / mag;
  return (norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10) * mag;
}

class LineChart {
  constructor(canvas, legendEl, { series, thresholds = [], minMax = 10, decimals = 1 }) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.series = series;
    this.thresholds = thresholds;
    this.minMax = minMax;
    this.decimals = decimals;
    this.pad = { l: 44, r: 12, t: 10, b: 22 };
    this.renderLegend(legendEl);
    new ResizeObserver(() => this.resize()).observe(canvas);
    this.resize();
  }

  renderLegend(el) {
    const items = this.series.map((s) =>
      `<li><span class="swatch${s.dash ? ' dashed' : ''}" style="border-color:var(${s.color})"></span>${s.label}</li>`);
    for (const th of this.thresholds) {
      items.push(`<li><span class="swatch dashed" style="border-color:var(${th.color})"></span>${th.label}</li>`);
    }
    items.push('<li><span class="swatch band"></span>attack active</li>');
    el.innerHTML = items.join('');
  }

  resize() {
    const dpr = window.devicePixelRatio || 1;
    const { clientWidth: w, clientHeight: h } = this.canvas;
    this.w = w;
    this.h = h;
    this.canvas.width = Math.round(w * dpr);
    this.canvas.height = Math.round(h * dpr);
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    requestDraw();
  }

  xOf(tick) {
    return this.pad.l + ((tick - this.t0) / WINDOW_MS) * (this.w - this.pad.l - this.pad.r);
  }

  tickAt(x) {
    return this.t0 + ((x - this.pad.l) / (this.w - this.pad.l - this.pad.r)) * WINDOW_MS;
  }

  yOf(v) {
    return this.pad.t + (1 - v / this.yMax) * (this.h - this.pad.t - this.pad.b);
  }

  draw(data) {
    const { ctx, w, h, pad } = this;
    if (!w || !h) return;
    ctx.clearRect(0, 0, w, h);

    const last = data.length ? data[data.length - 1].tick_ms : 0;
    this.t0 = Math.max(0, last - WINDOW_MS);
    const visible = data.filter((d) => d.tick_ms >= this.t0);

    let max = this.minMax;
    for (const d of visible) for (const s of this.series) if (d[s.key] > max) max = d[s.key];
    for (const th of this.thresholds) max = Math.max(max, th.value);
    const step = niceStep(max * 1.08);
    this.yMax = Math.ceil((max * 1.08) / step) * step;

    const colors = {
      grid: cssVar('--grid'), text: cssVar('--text-3'), band: cssVar('--attack-band'), surface: cssVar('--surface'),
    };

    // attack bands (ground truth)
    ctx.font = '11px ' + cssVar('--mono');
    let runStart = null;
    let runName = null;
    const flush = (endTick) => {
      const x0 = Math.max(pad.l, this.xOf(runStart));
      const x1 = Math.min(w - pad.r, this.xOf(endTick));
      ctx.fillStyle = colors.band;
      ctx.fillRect(x0, pad.t, Math.max(2, x1 - x0), h - pad.t - pad.b);
      if (x1 - x0 > 40) {
        ctx.fillStyle = colors.text;
        ctx.fillText(runName, x0 + 4, pad.t + 12);
      }
    };
    for (const d of visible) {
      if (d.attack !== 'NONE' && runStart === null) {
        runStart = d.tick_ms;
        runName = d.attack;
      } else if (d.attack === 'NONE' && runStart !== null) {
        flush(d.tick_ms);
        runStart = null;
      }
    }
    if (runStart !== null) flush(last);

    // grid + y axis
    ctx.strokeStyle = colors.grid;
    ctx.fillStyle = colors.text;
    ctx.lineWidth = 1;
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (let v = 0; v <= this.yMax + 1e-9; v += step) {
      const y = Math.round(this.yOf(v)) + 0.5;
      ctx.beginPath();
      ctx.moveTo(pad.l, y);
      ctx.lineTo(w - pad.r, y);
      ctx.stroke();
      ctx.fillText(String(+v.toFixed(2)), pad.l - 6, y);
    }
    // x axis labels: seconds of sim time
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    const firstSec = Math.ceil(this.t0 / 10_000) * 10;
    for (let s = firstSec; s * 1000 <= this.t0 + WINDOW_MS; s += 10) {
      ctx.fillText(`${s}s`, this.xOf(s * 1000), h - pad.b + 6);
    }

    // thresholds
    for (const th of this.thresholds) {
      const y = Math.round(this.yOf(th.value)) + 0.5;
      ctx.strokeStyle = cssVar(th.color);
      ctx.setLineDash([4, 4]);
      ctx.beginPath();
      ctx.moveTo(pad.l, y);
      ctx.lineTo(w - pad.r, y);
      ctx.stroke();
      ctx.setLineDash([]);
    }

    // series
    ctx.lineWidth = 2;
    ctx.lineJoin = 'round';
    ctx.lineCap = 'round';
    for (const s of this.series) {
      ctx.strokeStyle = cssVar(s.color);
      ctx.setLineDash(s.dash ? [6, 4] : []);
      ctx.beginPath();
      let started = false;
      for (const d of visible) {
        const v = d[s.key];
        if (v == null) {
          started = false;
          continue;
        }
        const x = this.xOf(d.tick_ms);
        const y = this.yOf(Math.min(v, this.yMax));
        if (started) ctx.lineTo(x, y);
        else ctx.moveTo(x, y);
        started = true;
      }
      ctx.stroke();
    }
    ctx.setLineDash([]);

    // crosshair
    if (state.hoverTick != null && state.hoverTick >= this.t0) {
      const rec = nearest(visible, state.hoverTick);
      if (rec) {
        const x = Math.round(this.xOf(rec.tick_ms)) + 0.5;
        ctx.strokeStyle = colors.text;
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(x, pad.t);
        ctx.lineTo(x, h - pad.b);
        ctx.stroke();
        for (const s of this.series) {
          if (rec[s.key] == null) continue;
          ctx.fillStyle = cssVar(s.color);
          ctx.strokeStyle = colors.surface;
          ctx.lineWidth = 2;
          ctx.beginPath();
          ctx.arc(x, this.yOf(Math.min(rec[s.key], this.yMax)), 4, 0, Math.PI * 2);
          ctx.fill();
          ctx.stroke();
        }
      }
    }
  }
}

function nearest(arr, tick) {
  if (!arr.length) return null;
  let lo = 0;
  let hi = arr.length - 1;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (arr[mid].tick_ms < tick) lo = mid + 1;
    else hi = mid;
  }
  if (lo > 0 && Math.abs(arr[lo - 1].tick_ms - tick) < Math.abs(arr[lo].tick_ms - tick)) lo--;
  return arr[lo];
}

const flowChart = new LineChart($('chart-flow'), $('legend-flow'), {
  series: [
    { key: 'target_rate', label: 'target', color: '--series-ref', dash: true },
    { key: 'flow_meas', label: 'measured (firmware view)', color: '--series-1' },
    { key: 'flow_true', label: 'true (plant)', color: '--series-2' },
  ],
  minMax: 50,
});
const pressChart = new LineChart($('chart-press'), $('legend-press'), {
  series: [
    { key: 'pressure_meas', label: 'measured', color: '--series-1' },
    { key: 'pressure_true', label: 'true', color: '--series-2' },
  ],
  thresholds: [{ value: 40, label: 'occlusion limit 40 kPa', color: '--critical' }],
  minMax: 20,
});
charts.push(flowChart, pressChart);

// hover: shared crosshair, per-chart tooltip
const tooltip = $('tooltip');
for (const chart of charts) {
  chart.canvas.addEventListener('mousemove', (ev) => {
    const r = chart.canvas.getBoundingClientRect();
    const x = ev.clientX - r.left;
    state.hoverTick = chart.tickAt(x);
    const rec = nearest(state.tlm, state.hoverTick);
    if (!rec || x < chart.pad.l) {
      tooltip.hidden = true;
      return;
    }
    const rows = chart.series.map((s) =>
      `<div class="tt-row"><span><i class="tt-key" style="background:var(${s.color})"></i>${s.label}</span><b>${fmt(rec[s.key], 2)}</b></div>`).join('');
    tooltip.innerHTML = `<div class="tt-time">t = ${secs(rec.tick_ms)} s · ${rec.state}</div>${rows}` +
      (rec.attack !== 'NONE' || rec.anomaly !== 'NONE'
        ? `<div class="tt-attack">attack ${rec.attack} · flags ${rec.anomaly}</div>` : '');
    tooltip.hidden = false;
    const host = chart.canvas.closest('.charts').getBoundingClientRect();
    const left = ev.clientX - host.left + 14;
    const maxLeft = host.width - tooltip.offsetWidth - 8;
    tooltip.style.left = `${left > maxLeft ? ev.clientX - host.left - tooltip.offsetWidth - 14 : left}px`;
    tooltip.style.top = `${ev.clientY - host.top + 12}px`;
    requestDraw();
  });
  chart.canvas.addEventListener('mouseleave', () => {
    state.hoverTick = null;
    tooltip.hidden = true;
    requestDraw();
  });
}

// ---- event log ------------------------------------------------------------------------

const logBody = $('log-body');
const logWrap = $('log-wrap');
const levelSel = $('log-level');
const follow = $('log-follow');

function logRow(e) {
  const tr = document.createElement('tr');
  tr.dataset.level = e.level;
  tr.dataset.type = e.type;
  for (const v of [secs(e.tick_ms), e.type, e.level, e.module, e.message]) {
    const td = document.createElement('td');
    td.textContent = v ?? '';
    tr.appendChild(td);
  }
  return tr;
}

function passesFilter(e) {
  return (LEVELS[e.level] ?? 1) >= LEVELS[levelSel.value];
}

function appendLog(e) {
  if (!passesFilter(e)) return;
  logBody.appendChild(logRow(e));
  while (logBody.childElementCount > MAX_LOG_ROWS) logBody.firstElementChild.remove();
  if (follow.checked) logWrap.scrollTop = logWrap.scrollHeight;
}

function rebuildLog() {
  logBody.replaceChildren(...state.events.filter(passesFilter).slice(-MAX_LOG_ROWS).map(logRow));
  logWrap.scrollTop = logWrap.scrollHeight;
}
levelSel.addEventListener('change', rebuildLog);

// ---- terminal ------------------------------------------------------------------------------

const termOut = $('term-out');
const termInput = $('term-input');
const cmdHistory = [];
let histIdx = 0;

const COMPLETIONS = [
  'attack spike', 'attack drift', 'attack replay', 'attack freeze', 'attack dropout', 'attack noise',
  'attack tamper', 'attack clear', 'fault occlusion on', 'fault occlusion off',
  'start', 'pause', 'stop', 'clear', 'status', 'rate', 'vtbi', 'ids on', 'ids off',
  'loglevel debug', 'loglevel info', 'loglevel warn', 'help', 'cls',
];

const HELP = [
  ['attack spike   [mag=60] [samples=6]', 'transient on flow & pressure'],
  ['attack drift   [rate=0.4] [samples]', 'growing bias, ml/h per sample'],
  ['attack replay  [window=50] [samples]', 'loop recorded frames (old seq)'],
  ['attack freeze  [-] [samples]', 'hold last reading'],
  ['attack dropout [-] [samples]', 'swallow frames (jamming)'],
  ['attack noise   [sigma=25] [samples]', 'heavy Gaussian noise'],
  ['attack tamper  <rate>', 'overwrite setpoint RAM'],
  ['attack clear', 'stop active attack'],
  ['fault occlusion on|off', 'physical occlusion (not an attack)'],
  ['start pause stop clear status', 'pump console'],
  ['rate <ml/h>  vtbi <ml>  ids on|off', ''],
  ['loglevel debug|info|warn|error', ''],
  ['cls', 'clear this terminal'],
];

function tprint(text, cls) {
  const div = document.createElement('div');
  if (cls) div.className = cls;
  div.textContent = text;
  const atBottom = termOut.scrollHeight - termOut.scrollTop - termOut.clientHeight < 30;
  termOut.appendChild(div);
  while (termOut.childElementCount > 800) termOut.firstElementChild.remove();
  if (atBottom) termOut.scrollTop = termOut.scrollHeight;
}

function termEvent(e) {
  const t = e.tick_ms != null ? `${secs(e.tick_ms).padStart(6)}s ` : '        ';
  if (e.type === 'CMD') tprint(`${t}${e.module === 'cli' ? '[cli] ' : '› '}${e.message}`, 'cmd');
  else if (e.type === 'ACK') tprint(`${t}✓ ${e.module}: ${e.message}`, 'ok');
  else if (e.type === 'ERR') tprint(`${t}✗ ${e.module}: ${e.message}`, 'err');
  else if (e.level === 'ERROR') tprint(`${t}■ ${e.module}: ${e.message}`, 'err');
  else if (e.level === 'WARN') tprint(`${t}▲ ${e.module}: ${e.message}`, 'warn');
  else if (e.module === 'SIM' || e.module === 'SERVER') tprint(`${t}  ${e.module}: ${e.message}`, 'dim');
}

async function send(line) {
  try {
    const r = await fetch('/api/cmd', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ line, source: 'gui' }),
    });
    const body = await r.json();
    if (!body.ok) tprint(`✗ ${body.error}`, 'err');
    return body.ok;
  } catch (err) {
    tprint(`✗ server unreachable (${err.message})`, 'err');
    return false;
  }
}

$('term-form').addEventListener('submit', (ev) => {
  ev.preventDefault();
  const line = termInput.value.trim();
  termInput.value = '';
  if (!line) return;
  if (cmdHistory[cmdHistory.length - 1] !== line) cmdHistory.push(line);
  histIdx = cmdHistory.length;

  if (line === 'help' || line === '?') {
    tprint('commands:', 'dim');
    for (const [c, d] of HELP) tprint(`  ${c.padEnd(38)}${d}`, 'dim');
  } else if (line === 'cls') {
    termOut.replaceChildren();
  } else {
    send(line);
  }
});

termInput.addEventListener('keydown', (ev) => {
  if (ev.key === 'ArrowUp' && histIdx > 0) {
    termInput.value = cmdHistory[--histIdx];
    ev.preventDefault();
  } else if (ev.key === 'ArrowDown') {
    histIdx = Math.min(cmdHistory.length, histIdx + 1);
    termInput.value = cmdHistory[histIdx] ?? '';
    ev.preventDefault();
  } else if (ev.key === 'Tab') {
    const v = termInput.value;
    const hits = COMPLETIONS.filter((c) => c.startsWith(v));
    if (hits.length === 1) termInput.value = hits[0] + ' ';
    else if (hits.length > 1) tprint(hits.join('   '), 'dim');
    ev.preventDefault();
  }
});

$('quick').addEventListener('click', (ev) => {
  const btn = ev.target.closest('[data-line]');
  if (btn) send(btn.dataset.line);
});

// ---- controls -------------------------------------------------------------------------------

document.querySelectorAll('[data-cmd]').forEach((b) => b.addEventListener('click', () => send(b.dataset.cmd)));
document.querySelectorAll('[data-cmd-form]').forEach((f) => f.addEventListener('submit', (ev) => {
  ev.preventDefault();
  send(`${f.dataset.cmdForm} ${f.querySelector('input').value}`);
}));
$('tg-ids').addEventListener('change', (ev) => send(`ids ${ev.target.checked ? 'on' : 'off'}`));
$('tg-occl').addEventListener('change', (ev) => send(`fault occlusion ${ev.target.checked ? 'on' : 'off'}`));

$('restart').addEventListener('click', async () => {
  $('restart').disabled = true;
  try {
    const r = await fetch('/api/restart', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' });
    const body = await r.json();
    if (!body.ok) tprint(`✗ restart failed: ${body.error}`, 'err');
  } finally {
    $('restart').disabled = false;
  }
});

// ---- live stream ------------------------------------------------------------------------------

function resetSession(info) {
  state.tlm = [];
  state.events = [];
  state.latest = null;
  logBody.replaceChildren();
  $('tg-ids').checked = false;
  $('tg-occl').checked = false;
  $('session-id').textContent = info ? `· session ${info.id} · seed ${info.seed}` : '';
  requestDraw();
}

function pushTlm(t) {
  state.tlm.push(t);
  if (state.tlm.length > MAX_TLM) state.tlm.splice(0, state.tlm.length - MAX_TLM);
  state.latest = t;
}

function pushEvent(e) {
  state.events.push(e);
  if (state.events.length > MAX_EVENTS) state.events.splice(0, state.events.length - MAX_EVENTS);
  appendLog(e);
  termEvent(e);
}

function setConn(text, tone) {
  const el = $('conn');
  el.className = `pill ${tone}`;
  el.lastElementChild.textContent = text;
}

let renderPending = false;
function scheduleStatus() {
  if (renderPending) return;
  renderPending = true;
  requestAnimationFrame(() => {
    renderPending = false;
    renderStatus(state.latest);
  });
}

function connect() {
  const es = new EventSource('/events');
  es.addEventListener('hello', (m) => {
    const d = JSON.parse(m.data);
    resetSession(d.session);
    d.history.forEach(pushTlm);
    termOut.replaceChildren();
    tprint('connected · type help for commands · Tab completes · ↑↓ history', 'dim');
    d.events.forEach(pushEvent);
    setConn(d.running ? 'sim running' : 'sim stopped', d.running ? 'pill-good' : 'pill-bad');
    scheduleStatus();
    requestDraw();
  });
  es.addEventListener('session', (m) => {
    resetSession(JSON.parse(m.data));
    tprint('— new session —', 'dim');
  });
  es.addEventListener('status', (m) => {
    const d = JSON.parse(m.data);
    setConn(d.running ? 'sim running' : 'sim stopped', d.running ? 'pill-good' : 'pill-bad');
  });
  es.addEventListener('tlm', (m) => {
    pushTlm(JSON.parse(m.data));
    scheduleStatus();
    requestDraw();
  });
  es.addEventListener('event', (m) => pushEvent(JSON.parse(m.data)));
  es.onerror = () => setConn('server offline', 'pill-bad');
}

connect();

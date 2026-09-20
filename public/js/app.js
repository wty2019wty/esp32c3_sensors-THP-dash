import { api, setCsrf, syncCsrfFromCookie, exportCsv, isDemo, setDemo } from './api.js';
import {
  drawAll,
  bindChartCursor,
  pointAt,
  dataBounds,
  clampWin,
  isZoomed,
  minSpanForPoints,
} from './charts.js';

const $ = (sel, el = document) => el.querySelector(sel);
const $$ = (sel, el = document) => [...el.querySelectorAll(sel)];

/** Escape for HTML text / attribute contexts (device names, token labels, etc.) */
function escapeHtml(value) {
  return String(value ?? '')
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

/** Product display timezone — must match Worker default (REQUIREMENTS / Asia/Shanghai). */
const DISPLAY_TZ = 'Asia/Shanghai';

/** Local calendar-day start in a named IANA timezone (UTC instant). */
function dayStartInTz(timeZone, now = new Date()) {
  const fmt = new Intl.DateTimeFormat('en-CA', {
    timeZone,
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
  });
  const parts = Object.fromEntries(fmt.formatToParts(now).map((p) => [p.type, p.value]));
  const hour = parts.hour === '24' ? '0' : parts.hour;
  const probe = Date.UTC(
    Number(parts.year),
    Number(parts.month) - 1,
    Number(parts.day),
    Number(hour),
    Number(parts.minute),
    Number(parts.second)
  );
  const trueNow = now.getTime();
  const offsetMs = probe - trueNow;
  const midnightWall = Date.UTC(
    Number(parts.year),
    Number(parts.month) - 1,
    Number(parts.day),
    0,
    0,
    0,
    0
  );
  return new Date(midnightWall - offsetMs);
}

function isCoarsePointer() {
  return (
    matchMedia('(hover: none) and (pointer: coarse)').matches ||
    window.innerWidth <= 640
  );
}

function isNarrowViewport() {
  return window.innerWidth <= 640;
}

function updateChartHelp() {
  const el = $('#chart-help');
  if (!el) return;
  if (chartFs.open) {
    el.textContent = isCoarsePointer()
      ? '双指缩放时间轴；横向拖动平移。点选查看数据；点「退出全屏」或 Esc 返回。'
      : '滚轮/触控板缩放时间轴；放大后拖动平移。悬停查看数据；Esc 或点「退出全屏」返回。';
    return;
  }
  el.textContent = isCoarsePointer()
    ? '双指缩放时间轴；横向拖动平移。点选曲线查看该时刻数据；需要时点「重置缩放」恢复全程。点「全屏」放大查看图表。'
    : '滚轮/触控板缩放时间轴（可放大到采样点附近）；放大后拖动平移。悬停查看数据，点击固定；点「重置缩放」恢复全程。点「全屏」放大查看图表。';
}

const state = {
  user: null,
  devices: [],
  deviceId: null,
  range: 'today',
  from: null,
  to: null,
  view: 'combined',
  series: new Set(['temperature', 'humidity', 'pressure']),
  points: [],
  granularity: null,
};

/** Fullscreen chart (PC + mobile) */
const chartFs = {
  open: false,
  nativeFs: false,
};

/** Time-axis zoom window (ms); null = full range */
const chartZoom = {
  win: null,
};

/** chart cursor inspection (hover / pin) */
const cursor = {
  ctrl: null,
  index: null,
  pinned: false,
  lastPointer: { x: 0, y: 0 },
};

function currentXWindow() {
  const full = dataBounds(state.points);
  if (!full) return null;
  return clampWin(chartZoom.win, full.min, full.max, minSpanForPoints(state.points));
}

function chartIsZoomed() {
  const full = dataBounds(state.points);
  if (!full) return false;
  return isZoomed(currentXWindow(), full.min, full.max);
}

function setXWindow(win) {
  chartZoom.win = win;
  // Keep selected point if still meaningful; redraw charts + zoom chip
  if (cursor.index != null && cursor.index >= state.points.length) {
    cursor.index = state.points.length - 1;
  }
  drawChartsNow();
  updateChartSub();
  updateZoomUI();
}

function resetZoom() {
  chartZoom.win = null;
  drawChartsNow();
  updateChartSub();
  updateZoomUI();
}

function updateZoomUI() {
  const chip = $('#chart-zoom-chip');
  const btn = $('#btn-reset-zoom');
  const zoomed = chartIsZoomed();
  if (chip) {
    chip.hidden = !zoomed;
    if (zoomed) chip.textContent = '已放大';
  }
  if (btn) {
    btn.hidden = !zoomed;
  }
}

function chartPanel() {
  return $('.chart-panel');
}

/** Measure canvas CSS heights for normal vs fullscreen chart layouts. */
function chartCssHeights() {
  if (!chartFs.open) return null;
  const combinedHost = $('#chart-combined');
  const splitHost = $('#chart-split');
  const view = state.view;

  if (view === 'split') {
    const host = splitHost;
    if (!host || host.hidden || host.classList.contains('hidden')) {
      return { combined: null, split: 140 };
    }
    const styles = getComputedStyle(host);
    const padY =
      (parseFloat(styles.paddingTop) || 0) + (parseFloat(styles.paddingBottom) || 0);
    const hostH = host.clientHeight || window.innerHeight;
    const items = $$('.split-item', host);
    const labels = items.reduce((sum, item) => {
      const h3 = item.querySelector('h3');
      return sum + (h3 ? h3.offsetHeight + 4 : 18);
    }, 0);
    const gaps = Math.max(0, (items.length - 1)) * 6;
    const avail = Math.max(60, hostH - padY - labels - gaps);
    const per = Math.floor(avail / Math.max(1, items.length || 3));
    return { combined: null, split: Math.max(80, per) };
  }

  const host = combinedHost;
  if (!host || host.hidden || host.classList.contains('hidden')) {
    return { combined: 360, split: null };
  }
  const styles = getComputedStyle(host);
  const padY = (parseFloat(styles.paddingTop) || 0) + (parseFloat(styles.paddingBottom) || 0);
  const hostH = host.clientHeight || window.innerHeight;
  return { combined: Math.max(160, hostH - padY), split: null };
}

function paintCharts() {
  if (!state.points.length) return;
  drawAll(
    $('#canvas-combined'),
    { t: $('#canvas-t'), h: $('#canvas-h'), p: $('#canvas-p') },
    state.points,
    state.view,
    state.series,
    cursor.index,
    currentXWindow(),
    chartCssHeights()
  );
  updateCursorUI();
}

function drawChartsNow() {
  paintCharts();
}

function show(view) {
  const map = {
    bootstrap: $('#view-bootstrap'),
    login: $('#view-login'),
    app: $('#view-app'),
  };
  for (const [k, el] of Object.entries(map)) {
    const on = k === view;
    el.hidden = !on;
    el.classList.toggle('hidden', !on);
  }
}

function toast(msg, isError = false) {
  const el = $('#toast');
  el.hidden = false;
  el.textContent = msg;
  el.classList.toggle('error', isError);
  clearTimeout(toast._t);
  toast._t = setTimeout(() => {
    el.hidden = true;
  }, 3200);
}

function rangeToFromTo() {
  const now = new Date();
  if (state.range === 'custom' && state.from && state.to) {
    return { from: state.from, to: state.to };
  }
  if (state.range === 'today') {
    // Product timezone (same as Worker default), not browser local
    const start = dayStartInTz(DISPLAY_TZ, now);
    return { from: start.toISOString(), to: now.toISOString() };
  }
  const hours = {
    '24h': 24,
    '7d': 24 * 7,
    '30d': 24 * 30,
    '90d': 24 * 90,
    '365d': 24 * 365,
  }[state.range];
  if (!hours) return { from: now.toISOString(), to: now.toISOString() };
  const from = new Date(now.getTime() - hours * 3600000);
  return { from: from.toISOString(), to: now.toISOString() };
}

function rangeLabel() {
  const map = {
    today: '当天',
    '24h': '近 24 小时',
    '7d': '近 7 天',
    '30d': '近 30 天',
    '90d': '近 90 天',
    '365d': '近 1 年',
    custom: '自定义范围',
  };
  return map[state.range] || state.range;
}

function fmtNum(n, digits = 1) {
  if (n == null || !Number.isFinite(Number(n))) return '—';
  return Number(n).toFixed(digits);
}

function fmtTime(iso) {
  if (!iso) return '—';
  try {
    return new Date(iso).toLocaleString();
  } catch {
    return iso;
  }
}

function updateCursorUI() {
  const readout = $('#chart-readout');
  const tip = $('#chart-tooltip');
  const empty = state.points.length === 0;
  const info = !empty && cursor.index != null ? pointAt(state.points, cursor.index) : null;

  // Keep the readout bar mounted at all times so the chart never jumps.
  readout.hidden = false;
  readout.classList.toggle('is-empty', !info);

  if (!info) {
    tip.hidden = true;
    $('#readout-ts').textContent = empty ? '—' : '未选中';
    $('#readout-hint').textContent = empty
      ? '当前范围内没有数据'
      : isCoarsePointer()
        ? '点选曲线可查看该时刻数据'
        : '在曲线上悬停或点击，可查看该时刻数据';
    $('#readout-t').textContent = '—';
    $('#readout-h').textContent = '—';
    $('#readout-p').textContent = '—';
    return;
  }

  $('#readout-ts').textContent = info.label;
  $('#readout-hint').textContent = cursor.pinned
    ? isCoarsePointer()
      ? '已选中 · 再次点选取消'
      : '已固定 · 点击曲线其它点可改 · Esc 取消'
    : isCoarsePointer()
      ? '点选可查看 · 再次点选取消'
      : '悬停预览 · 点击固定 · ←/→ 微调';

  const map = {
    temperature: '#readout-t',
    humidity: '#readout-h',
    pressure: '#readout-p',
  };
  for (const v of info.values) {
    const el = $(map[v.key]);
    if (el) el.textContent = fmtNum(v.value, 1);
  }

  // Floating tooltip is for mouse hover; on touch/narrow use the readout bar.
  const coarse = isCoarsePointer();
  const narrow = isNarrowViewport();
  if (!coarse && !narrow && (cursor.lastPointer.x || cursor.lastPointer.y)) {
    tip.hidden = false;
    tip.innerHTML = `
      <div class="tt-time">${info.label}</div>
      <div class="tt-row temp"><span>温度</span><i>${fmtNum(info.values[0].value)} °C</i></div>
      <div class="tt-row humid"><span>湿度</span><i>${fmtNum(info.values[1].value)} %RH</i></div>
      <div class="tt-row press"><span>气压</span><i>${fmtNum(info.values[2].value, 1)} hPa</i></div>
      <div class="tt-pin">${cursor.pinned ? '已固定' : '点击固定'}</div>
    `;
    const pad = 14;
    const tw = tip.offsetWidth || 180;
    const th = tip.offsetHeight || 100;
    let left = cursor.lastPointer.x + pad;
    let top = cursor.lastPointer.y + pad;
    if (left + tw > window.innerWidth - 8) left = cursor.lastPointer.x - tw - pad;
    if (top + th > window.innerHeight - 8) top = cursor.lastPointer.y - th - pad;
    tip.style.left = `${Math.max(8, left)}px`;
    tip.style.top = `${Math.max(8, top)}px`;
  } else {
    tip.hidden = true;
  }
}

function ensureCursorBinding() {
  if (cursor.ctrl) return cursor.ctrl;
  cursor.ctrl = bindChartCursor({
    combinedCanvas: $('#canvas-combined'),
    splitCanvases: { t: $('#canvas-t'), h: $('#canvas-h'), p: $('#canvas-p') },
    getPoints: () => state.points,
    getView: () => state.view,
    getXWindow: () => currentXWindow(),
    setXWindow: (win) => setXWindow(win),
    onChange: ({ index, pinned }) => {
      cursor.index = index;
      cursor.pinned = pinned;
      const empty = state.points.length === 0;
      drawAll(
        $('#canvas-combined'),
        { t: $('#canvas-t'), h: $('#canvas-h'), p: $('#canvas-p') },
        state.points,
        state.view,
        state.series,
        empty ? null : index,
        currentXWindow(),
        chartCssHeights()
      );
      updateCursorUI();
      updateZoomUI();
    },
  });

  // track pointer for tooltip position
  for (const canvas of ['#canvas-combined', '#canvas-t', '#canvas-h', '#canvas-p']) {
    $(canvas)?.addEventListener('pointermove', (e) => {
      cursor.lastPointer = { x: e.clientX, y: e.clientY };
    });
  }

  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && cursor.index != null && !chartFs.open) {
      cursor.ctrl?.clear();
    }
  });

  return cursor.ctrl;
}

function setStats(device) {
  const latest = device?.latest;
  $('#stat-t').textContent = latest ? fmtNum(latest.temperature, 1) : '—';
  $('#stat-h').textContent = latest ? fmtNum(latest.humidity, 1) : '—';
  $('#stat-p').textContent = latest ? fmtNum(latest.pressure, 1) : '—';
  $('#stat-device').textContent = device?.name || device?.id || '—';
  const online = device?.online;
  const seen = device?.lastSeenAt || latest?.ts;
  $('#stat-seen').innerHTML = seen
    ? `最近上报 <time title="${escapeHtml(seen)}">${escapeHtml(fmtTime(seen))}</time> · <span class="${online ? 'ok' : 'bad'}">${online ? '在线' : '离线/未知'}</span>`
    : '尚无上报';
}

async function enterNativeFullscreen(el) {
  if (!el) return false;
  try {
    if (el.requestFullscreen) {
      await el.requestFullscreen({ navigationUI: 'hide' });
      return true;
    }
    if (el.webkitRequestFullscreen) {
      el.webkitRequestFullscreen();
      return true;
    }
    if (el.msRequestFullscreen) {
      el.msRequestFullscreen();
      return true;
    }
  } catch {
    /* denied / unsupported — CSS fullscreen still works */
  }
  return false;
}

async function exitNativeFullscreen() {
  try {
    if (document.fullscreenElement || document.webkitFullscreenElement || document.msFullscreenElement) {
      if (document.exitFullscreen) await document.exitFullscreen();
      else if (document.webkitExitFullscreen) document.webkitExitFullscreen();
      else if (document.msExitFullscreen) document.msExitFullscreen();
    }
  } catch {
    /* ignore */
  }
}

function updateChartFsButton() {
  const btn = $('#btn-fs-chart');
  if (!btn) return;
  btn.textContent = chartFs.open ? '退出全屏' : '全屏';
  btn.title = chartFs.open ? '退出图表全屏' : '全屏查看温湿度气压图表';
  btn.setAttribute('aria-pressed', chartFs.open ? 'true' : 'false');
}

function setChartCanvasTouchAction(mode) {
  for (const sel of ['#canvas-combined', '#canvas-t', '#canvas-h', '#canvas-p']) {
    const el = $(sel);
    if (el) el.style.touchAction = mode;
  }
}

async function openChartFullscreen() {
  const panel = chartPanel();
  if (!panel || chartFs.open) return;
  chartFs.open = true;
  panel.classList.add('chart-fullscreen');
  document.body.classList.add('chart-fs-open');
  updateChartFsButton();
  updateChartHelp();
  // Fullscreen: keep gestures on the chart (pinch/pan), not page scroll
  setChartCanvasTouchAction('none');

  // Prefer Fullscreen API (hides browser chrome). CSS fixed layout is fallback.
  chartFs.nativeFs = await enterNativeFullscreen(panel);

  // Layout after class + fullscreen so clientHeight is correct
  requestAnimationFrame(() => {
    requestAnimationFrame(() => {
      renderCharts();
    });
  });
}

async function closeChartFullscreen() {
  const panel = chartPanel();
  if (!chartFs.open) return;
  chartFs.open = false;
  panel?.classList.remove('chart-fullscreen');
  document.body.classList.remove('chart-fs-open');
  updateChartFsButton();
  updateChartHelp();
  setChartCanvasTouchAction('pan-y');
  if (chartFs.nativeFs) {
    chartFs.nativeFs = false;
    await exitNativeFullscreen();
  }
  requestAnimationFrame(() => {
    renderCharts();
  });
}

function toggleChartFullscreen() {
  if (chartFs.open) return closeChartFullscreen();
  return openChartFullscreen();
}

function bindChartFullscreen() {
  $('#btn-fs-chart')?.addEventListener('click', () => {
    toggleChartFullscreen();
  });

  const onFsChange = () => {
    const active =
      document.fullscreenElement || document.webkitFullscreenElement || document.msFullscreenElement;
    // User left native fullscreen via Esc / browser chrome
    if (!active && chartFs.open && chartFs.nativeFs) {
      closeChartFullscreen();
    }
  };
  document.addEventListener('fullscreenchange', onFsChange);
  document.addEventListener('webkitfullscreenchange', onFsChange);

  // Esc exits chart fullscreen first
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && chartFs.open) {
      // Native fullscreen often exits itself; sync UI either way
      closeChartFullscreen();
      e.preventDefault();
    }
  });
}

function renderCharts() {
  const empty = state.points.length === 0;
  $('#chart-empty').hidden = !empty;
  $('#chart-help').hidden = empty;
  $('#chart-combined').hidden = state.view !== 'combined' || empty;
  $('#chart-split').hidden = state.view !== 'split' || empty;
  $('#chart-combined').classList.toggle('hidden', state.view !== 'combined' || empty);
  $('#chart-split').classList.toggle('hidden', state.view !== 'split' || empty);

  updateChartHelp();
  ensureCursorBinding();
  // reset selection when data set changes length / reload
  if (empty) {
    cursor.index = null;
    cursor.pinned = false;
  } else if (cursor.index != null && cursor.index >= state.points.length) {
    cursor.index = state.points.length - 1;
  }

  // Keep zoom only if it still fits the new data range
  if (!empty && chartZoom.win) {
    const full = dataBounds(state.points);
    chartZoom.win = full
      ? clampWin(chartZoom.win, full.min, full.max, minSpanForPoints(state.points))
      : null;
  }
  if (empty) chartZoom.win = null;

  if (empty) {
    $('#chart-tooltip').hidden = true;
    updateCursorUI();
    updateZoomUI();
    return;
  }

  paintCharts();
  updateZoomUI();
}

function updateChartSub() {
  const g = state.granularity;
  const gran = g ? (g.id === 'raw' ? '原始 5 分钟' : g.label || g.id) : '自动粒度';
  let text = `${rangeLabel()} · ${gran} · ${state.points.length} 点`;
  const win = currentXWindow();
  if (win && chartIsZoomed()) {
    const from = new Date(win.min).toLocaleString();
    const to = new Date(win.max).toLocaleString();
    text += ` · 放大 ${from} → ${to}`;
  }
  $('#chart-sub').textContent = text;
}

function fillDeviceSelect() {
  const sel = $('#sel-device');
  sel.innerHTML = '';
  for (const d of state.devices) {
    const opt = document.createElement('option');
    opt.value = d.id;
    opt.textContent = d.name ? `${d.name} (${d.id})` : d.id;
    sel.appendChild(opt);
  }
  if (state.deviceId) sel.value = state.deviceId;
  const tokSel = $('#token-device');
  if (tokSel) {
    tokSel.innerHTML = '';
    for (const d of state.devices) {
      const opt = document.createElement('option');
      opt.value = d.id;
      opt.textContent = d.name ? `${d.name} (${d.id})` : d.id;
      tokSel.appendChild(opt);
    }
    if (state.deviceId) tokSel.value = state.deviceId;
  }
}

function renderDeviceTable() {
  const tbody = $('#table-devices tbody');
  tbody.innerHTML = '';
  for (const d of state.devices) {
    const tr = document.createElement('tr');
    const id = escapeHtml(d.id);
    const name = escapeHtml(d.name || '');
    const status = escapeHtml(d.status || '');
    const seen = escapeHtml(fmtTime(d.lastSeenAt));
    const badgeClass = d.status === 'active' ? 'ok' : 'mute';
    tr.innerHTML = `
      <td>${id}</td>
      <td>${name}</td>
      <td><span class="badge ${badgeClass}">${status}</span></td>
      <td>${seen}</td>
      <td><button type="button" class="btn danger" data-del-device="${id}">删除</button></td>
    `;
    tbody.appendChild(tr);
  }
}

async function refreshTokens() {
  const data = await api.tokens();
  const tbody = $('#table-tokens tbody');
  tbody.innerHTML = '';
  for (const t of data.tokens || []) {
    const tr = document.createElement('tr');
    const badge = t.status === 'active' ? 'ok' : 'mute';
    const id = escapeHtml(t.id);
    const actions =
      t.status === 'active'
        ? `<button type="button" class="btn danger" data-revoke-token="${id}">吊销</button>`
        : `<button type="button" class="btn danger" data-purge-token="${id}">删除</button>`;
    tr.innerHTML = `
      <td>${id}</td>
      <td>${escapeHtml(t.deviceName || t.deviceId || '')}</td>
      <td>${escapeHtml(t.name || '—')}</td>
      <td><span class="badge ${badge}">${escapeHtml(t.status || '')}</span></td>
      <td>${escapeHtml(fmtTime(t.lastUsedAt))}</td>
      <td>${actions}</td>
    `;
    tbody.appendChild(tr);
  }
}

async function refreshAdmin() {
  const data = await api.devices();
  state.devices = data.devices || [];
  fillDeviceSelect();
  renderDeviceTable();
  if (state.user?.role === 'admin') {
    try {
      await refreshTokens();
    } catch {
      /* token list optional for non-admin views */
    }
  }
}

async function refreshSeries() {
  if (!state.deviceId) {
    state.points = [];
    renderCharts();
    return;
  }
  const { from, to } = rangeToFromTo();
  state.from = from;
  state.to = to;
  const data = await api.readings({ device_id: state.deviceId, from, to });
  state.points = data.points || [];
  state.granularity = data.granularity;
  updateChartSub();
  if (data.latest || data.device) {
    // merge latest into stats via latest endpoint if needed
  }
  const latest = await api.latest(state.deviceId);
  const dev = (latest.devices || []).find((d) => d.id === state.deviceId);
  if (dev) setStats(dev);
  else if (data.latest) {
    setStats({
      id: data.device?.id,
      name: data.device?.name,
      latest: data.latest,
      lastSeenAt: data.device?.lastSeenAt,
      online: true,
    });
  }
  renderCharts();
}

async function afterLogin(user, csrf) {
  state.user = user;
  if (csrf) setCsrf(csrf);
  syncCsrfFromCookie();
  $('#user-label').textContent = `${user.username} · ${user.role}`;
  $('#btn-admin').hidden = user.role !== 'admin';
  $('#admin-panel').hidden = user.role !== 'admin';
  $('#admin-panel').classList.toggle('hidden', user.role !== 'admin');
  show('app');
  await refreshAdmin();
  if (!state.deviceId && state.devices.length) state.deviceId = state.devices[0].id;
  fillDeviceSelect();
  await refreshSeries();
}

async function boot() {
  const params = new URLSearchParams(location.search);
  if (params.get('demo') === '1') {
    setDemo(true);
  }

  try {
    if (!isDemo()) {
      const meRes = await api.me().catch((e) => {
        if (e.status === 401) return null;
        throw e;
      });
      if (meRes?.user) {
        setCsrf(meRes.csrf);
        await afterLogin(meRes.user, meRes.csrf);
        return;
      }
      const bs = await api.bootstrapStatus();
      if (bs.needsBootstrap) {
        show('bootstrap');
        return;
      }
      show('login');
      $('#demo-hint').hidden = false;
      return;
    }
  } catch (err) {
    // Network / not deployed — offer demo
    console.warn('API unavailable', err);
    show('login');
    $('#demo-hint').hidden = false;
    $('#login-error').hidden = false;
    $('#login-error').textContent = '无法连接 API。可部署 Worker 后刷新，或点击「演示模式」预览界面。';
    return;
  }

  if (isDemo()) {
    await enterDemo();
  }
}

async function enterDemo() {
  setDemo(true);
  const demo = await import('./demo.js');
  const user = demo.demoUser();
  await afterLogin(user, 'demo-csrf');
  toast('已进入演示模式（本地模拟数据）');
}

// ----- Events -----
$('#form-bootstrap')?.addEventListener('submit', async (e) => {
  e.preventDefault();
  const fd = new FormData(e.target);
  const errEl = $('#bootstrap-error');
  errEl.hidden = true;
  if (fd.get('password') !== fd.get('password2')) {
    errEl.hidden = false;
    errEl.textContent = '两次输入的密码不一致';
    return;
  }
  try {
    await api.bootstrapCreate(String(fd.get('username')), String(fd.get('password')));
    const loginRes = await api.login(String(fd.get('username')), String(fd.get('password')));
    setCsrf(loginRes.csrf);
    await afterLogin(loginRes.user, loginRes.csrf);
  } catch (err) {
    errEl.hidden = false;
    errEl.textContent = err.message || '创建失败';
  }
});

$('#form-login')?.addEventListener('submit', async (e) => {
  e.preventDefault();
  const fd = new FormData(e.target);
  const errEl = $('#login-error');
  errEl.hidden = true;
  try {
    const res = await api.login(String(fd.get('username')), String(fd.get('password')));
    setCsrf(res.csrf);
    await afterLogin(res.user, res.csrf);
  } catch (err) {
    errEl.hidden = false;
    errEl.textContent = err.message || '登录失败';
  }
});

$('#btn-demo')?.addEventListener('click', () => enterDemo());

$('#btn-logout')?.addEventListener('click', async () => {
  try {
    if (!isDemo()) await api.logout();
  } catch {
    /* ignore */
  }
  setDemo(false);
  state.user = null;
  show('login');
  $('#demo-hint').hidden = false;
});

$('#sel-device')?.addEventListener('change', async (e) => {
  state.deviceId = e.target.value;
  chartZoom.win = null;
  try {
    await refreshSeries();
  } catch (err) {
    toast(err.message, true);
  }
});

$('#sel-range')?.addEventListener('change', async (e) => {
  state.range = e.target.value;
  const custom = state.range === 'custom';
  $('#custom-range').hidden = !custom;
  $('#custom-range').classList.toggle('hidden', !custom);
  chartZoom.win = null;
  if (!custom) {
    try {
      await refreshSeries();
    } catch (err) {
      toast(err.message, true);
    }
  }
});

$('#btn-apply-range')?.addEventListener('click', async () => {
  const from = $('#inp-from').value;
  const to = $('#inp-to').value;
  if (!from || !to) {
    toast('请选择起止时间', true);
    return;
  }
  state.range = 'custom';
  state.from = new Date(from).toISOString();
  state.to = new Date(to).toISOString();
  chartZoom.win = null;
  try {
    await refreshSeries();
  } catch (err) {
    toast(err.message, true);
  }
});

$('#btn-reset-zoom')?.addEventListener('click', () => resetZoom());

function setChartView(view) {
  if (view !== 'combined' && view !== 'split') return;
  state.view = view;
  $$('[data-view]').forEach((b) => {
    b.classList.toggle('active', b.dataset.view === view);
  });
  renderCharts();
  updateCursorUI();
}

$$('.seg').forEach((btn) => {
  btn.addEventListener('click', () => setChartView(btn.dataset.view));
});

$$('.legend-item').forEach((btn) => {
  btn.addEventListener('click', () => {
    const key = btn.dataset.series;
    if (state.series.has(key)) state.series.delete(key);
    else state.series.add(key);
    if (state.series.size === 0) state.series.add(key);
    btn.classList.toggle('active', state.series.has(key));
    renderCharts();
  });
});

$('#btn-export')?.addEventListener('click', async () => {
  if (!state.deviceId) {
    toast('请先选择设备', true);
    return;
  }
  const { from, to } = rangeToFromTo();
  try {
    const result = await exportCsv({ device_id: state.deviceId, from, to });
    if (result && typeof result === 'object') {
      const name = result.filename || 'CSV';
      if (result.coarsened) {
        const g = result.granularityLabel || result.granularity || '更粗粒度';
        toast(`已导出 ${name}（长范围/数据量大，已降采样为 ${g}）`);
      } else {
        toast(`已导出 ${name}`);
      }
    } else {
      toast(`已导出 ${result || 'CSV'}`);
    }
  } catch (err) {
    toast(err.message || '导出失败', true);
  }
});

$('#btn-admin')?.addEventListener('click', () => {
  const panel = $('#admin-panel');
  panel.hidden = false;
  panel.classList.remove('hidden');
  panel.scrollIntoView({ behavior: 'smooth', block: 'start' });
});

$('#btn-new-device')?.addEventListener('click', () => {
  $('#device-error').hidden = true;
  $('#dlg-device').showModal();
});

$('#form-device')?.addEventListener('submit', async (e) => {
  e.preventDefault();
  const fd = new FormData(e.target);
  const errEl = $('#device-error');
  errEl.hidden = true;
  try {
    await api.createDevice({
      id: String(fd.get('id') || '').trim() || undefined,
      name: String(fd.get('name') || '').trim(),
    });
    $('#dlg-device').close();
    e.target.reset();
    await refreshAdmin();
    toast('设备已创建');
  } catch (err) {
    errEl.hidden = false;
    errEl.textContent = err.message || '创建失败';
  }
});

$('#btn-new-token')?.addEventListener('click', () => {
  fillDeviceSelect();
  $('#token-error').hidden = true;
  $('#dlg-token').showModal();
});

$('#form-token')?.addEventListener('submit', async (e) => {
  e.preventDefault();
  const fd = new FormData(e.target);
  const errEl = $('#token-error');
  errEl.hidden = true;
  try {
    const res = await api.createToken({
      deviceId: String(fd.get('deviceId')),
      name: String(fd.get('name') || '').trim(),
    });
    $('#dlg-token').close();
    e.target.reset();
    $('#secret-value').textContent = res.secret || '';
    $('#dlg-secret').showModal();
    await refreshAdmin();
  } catch (err) {
    errEl.hidden = false;
    errEl.textContent = err.message || '生成失败';
  }
});

$('#btn-copy-secret')?.addEventListener('click', async () => {
  const text = $('#secret-value').textContent;
  try {
    await navigator.clipboard.writeText(text);
    toast('已复制');
  } catch {
    toast('复制失败，请手动选中', true);
  }
});

$$('[data-close]').forEach((btn) => {
  btn.addEventListener('click', () => btn.closest('dialog')?.close());
});

$('#table-devices')?.addEventListener('click', async (e) => {
  const id = e.target?.dataset?.delDevice;
  if (!id) return;
  if (!confirm(`删除设备 ${id}？其 Token 与读数将一并删除。`)) return;
  try {
    await api.deleteDevice(id);
    if (state.deviceId === id) state.deviceId = state.devices.find((d) => d.id !== id)?.id || null;
    await refreshAdmin();
    await refreshSeries().catch(() => {});
    toast('设备已删除');
  } catch (err) {
    toast(err.message, true);
  }
});

$('#table-tokens')?.addEventListener('click', async (e) => {
  const revokeId = e.target?.dataset?.revokeToken;
  const purgeId = e.target?.dataset?.purgeToken;
  if (revokeId) {
    if (!confirm('吊销该 Token？关联设备将无法继续上报（记录仍保留，可再删除）。')) return;
    try {
      await api.revokeToken(revokeId);
      await refreshTokens();
      toast('Token 已吊销');
    } catch (err) {
      toast(err.message, true);
    }
    return;
  }
  if (purgeId) {
    if (!confirm('从数据库删除该已吊销 Token 记录？此操作不可恢复。')) return;
    try {
      await api.purgeToken(purgeId);
      await refreshTokens();
      toast('Token 记录已删除');
    } catch (err) {
      toast(err.message, true);
    }
  }
});

let resizeTimer = null;
function onViewportChange() {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => {
    updateChartHelp();
    if (state.points.length) renderCharts();
  }, 120);
}
window.addEventListener('resize', onViewportChange);
window.addEventListener('orientationchange', onViewportChange);

updateChartHelp();
updateChartFsButton();
bindChartFullscreen();
boot();

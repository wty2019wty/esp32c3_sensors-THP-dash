import { api, setCsrf, syncCsrfFromCookie, exportCsv, isDemo, setDemo } from './api.js';
import { drawAll, bindChartCursor, pointAt } from './charts.js';

const $ = (sel, el = document) => el.querySelector(sel);
const $$ = (sel, el = document) => [...el.querySelectorAll(sel)];

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
  el.textContent = isCoarsePointer()
    ? '在曲线上点选可查看该时刻数据；再点其它点可切换，再次点选取消。'
    : '在曲线上移动光标可查看该时刻数据；点击固定，再次点击或 Esc 取消；键盘 ←/→ 可逐点查看。';
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

/** chart cursor inspection (hover / pin) */
const cursor = {
  ctrl: null,
  index: null,
  pinned: false,
  lastPointer: { x: 0, y: 0 },
};

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
  const tz = Intl.DateTimeFormat().resolvedOptions().timeZone || 'Asia/Shanghai';
  if (state.range === 'custom' && state.from && state.to) {
    return { from: state.from, to: state.to };
  }
  if (state.range === 'today') {
    // local calendar day start
    const start = new Date(now);
    start.setHours(0, 0, 0, 0);
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

  if (!info) {
    readout.hidden = true;
    tip.hidden = true;
    return;
  }

  readout.hidden = false;
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
    if (el) el.textContent = fmtNum(v.value, v.key === 'pressure' ? 1 : 1);
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
        empty ? null : index
      );
      updateCursorUI();
    },
  });

  // track pointer for tooltip position
  for (const canvas of ['#canvas-combined', '#canvas-t', '#canvas-h', '#canvas-p']) {
    $(canvas)?.addEventListener('pointermove', (e) => {
      cursor.lastPointer = { x: e.clientX, y: e.clientY };
    });
  }

  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && cursor.index != null) {
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
    ? `最近上报 <time title="${seen}">${fmtTime(seen)}</time> · <span class="${online ? 'ok' : 'bad'}">${online ? '在线' : '离线/未知'}</span>`
    : '尚无上报';
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

  if (empty) {
    $('#chart-readout').hidden = true;
    $('#chart-tooltip').hidden = true;
    return;
  }

  drawAll(
    $('#canvas-combined'),
    { t: $('#canvas-t'), h: $('#canvas-h'), p: $('#canvas-p') },
    state.points,
    state.view,
    state.series,
    cursor.index
  );
  updateCursorUI();
}

function updateChartSub() {
  const g = state.granularity;
  const gran = g ? (g.id === 'raw' ? '原始 5 分钟' : g.label || g.id) : '自动粒度';
  $('#chart-sub').textContent = `${rangeLabel()} · ${gran} · ${state.points.length} 点`;
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
    tr.innerHTML = `
      <td>${d.id}</td>
      <td>${d.name || ''}</td>
      <td><span class="badge ${d.status === 'active' ? 'ok' : 'mute'}">${d.status}</span></td>
      <td>${fmtTime(d.lastSeenAt)}</td>
      <td><button type="button" class="btn danger" data-del-device="${d.id}">删除</button></td>
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
    const actions =
      t.status === 'active'
        ? `<button type="button" class="btn danger" data-revoke-token="${t.id}">吊销</button>`
        : `<button type="button" class="btn danger" data-purge-token="${t.id}">删除</button>`;
    tr.innerHTML = `
      <td>${t.id}</td>
      <td>${t.deviceName || t.deviceId}</td>
      <td>${t.name || '—'}</td>
      <td><span class="badge ${badge}">${t.status}</span></td>
      <td>${fmtTime(t.lastUsedAt)}</td>
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
  if (state.user?.role === 'admin' || state.user) {
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
  try {
    await refreshSeries();
  } catch (err) {
    toast(err.message, true);
  }
});

$$('.seg').forEach((btn) => {
  btn.addEventListener('click', () => {
    state.view = btn.dataset.view;
    $$('.seg').forEach((b) => b.classList.toggle('active', b === btn));
    renderCharts();
    updateCursorUI();
  });
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
    const name = await exportCsv({ device_id: state.deviceId, from, to });
    toast(`已导出 ${name || 'CSV'}`);
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
  const fd = new FormData(e.target);
  const errEl = $('#device-error');
  errEl.hidden = true;
  try {
    await api.createDevice({
      id: String(fd.get('id') || '').trim() || undefined,
      name: String(fd.get('name') || '').trim(),
    });
    $('#dlg-device').close();
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
  const fd = new FormData(e.target);
  const errEl = $('#token-error');
  errEl.hidden = true;
  try {
    const res = await api.createToken({
      deviceId: String(fd.get('deviceId')),
      name: String(fd.get('name') || '').trim(),
    });
    $('#dlg-token').close();
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
boot();

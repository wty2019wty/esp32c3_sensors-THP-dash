import { api, setCsrf, syncCsrfFromCookie, exportCsv, isDemo, setDemo } from './api.js';
import { drawAll } from './charts.js';

const $ = (sel, el = document) => el.querySelector(sel);
const $$ = (sel, el = document) => [...el.querySelectorAll(sel)];

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
  $('#chart-combined').hidden = state.view !== 'combined' || empty;
  $('#chart-split').hidden = state.view !== 'split' || empty;
  $('#chart-combined').classList.toggle('hidden', state.view !== 'combined' || empty);
  $('#chart-split').classList.toggle('hidden', state.view !== 'split' || empty);
  if (empty) return;

  drawAll(
    $('#canvas-combined'),
    { t: $('#canvas-t'), h: $('#canvas-h'), p: $('#canvas-p') },
    state.points,
    state.view,
    state.series
  );
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
    tr.innerHTML = `
      <td>${t.id}</td>
      <td>${t.deviceName || t.deviceId}</td>
      <td>${t.name || '—'}</td>
      <td><span class="badge ${badge}">${t.status}</span></td>
      <td>${fmtTime(t.lastUsedAt)}</td>
      <td>${
        t.status === 'active'
          ? `<button type="button" class="btn danger" data-revoke-token="${t.id}">吊销</button>`
          : '—'
      }</td>
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
  const id = e.target?.dataset?.revokeToken;
  if (!id) return;
  if (!confirm('吊销该 Token？关联设备将无法继续上报。')) return;
  try {
    await api.revokeToken(id);
    await refreshTokens();
    toast('Token 已吊销');
  } catch (err) {
    toast(err.message, true);
  }
});

window.addEventListener('resize', () => {
  if (state.points.length) renderCharts();
});

boot();

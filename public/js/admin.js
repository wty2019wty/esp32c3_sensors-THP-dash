import { api, setCsrf, syncCsrfFromCookie, isDemo, setDemo, restoreDemo } from './api.js';

const $ = (sel, el = document) => el.querySelector(sel);
const $$ = (sel, el = document) => [...el.querySelectorAll(sel)];

function escapeHtml(value) {
  return String(value ?? '')
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

function fmtTime(iso) {
  if (!iso) return '—';
  try {
    return new Date(iso).toLocaleString();
  } catch {
    return iso;
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

const state = {
  user: null,
  devices: [],
};

function show(view) {
  const map = {
    login: $('#view-login'),
    admin: $('#view-admin'),
  };
  for (const [k, el] of Object.entries(map)) {
    if (!el) continue;
    const on = k === view;
    el.hidden = !on;
    el.classList.toggle('hidden', !on);
  }
  const denied = $('#view-denied');
  if (denied) {
    const showDenied = view === 'admin' && state.user && state.user.role !== 'admin';
    denied.hidden = !showDenied;
  }
  const panel = $('#admin-panel');
  if (panel) {
    const showPanel = view === 'admin' && state.user?.role === 'admin';
    panel.hidden = !showPanel;
    panel.classList.toggle('hidden', !showPanel);
  }
}

function fillTokenDeviceSelect() {
  const sel = $('#token-device');
  if (!sel) return;
  sel.innerHTML = '';
  for (const d of state.devices) {
    const opt = document.createElement('option');
    opt.value = d.id;
    opt.textContent = d.name ? `${d.name} (${d.id})` : d.id;
    sel.appendChild(opt);
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
  fillTokenDeviceSelect();
  renderDeviceTable();
  if (state.user?.role === 'admin') {
    try {
      await refreshTokens();
    } catch {
      /* token list optional for non-admin views */
    }
  }
}

async function afterLogin(user, csrf) {
  state.user = user;
  if (csrf) setCsrf(csrf);
  syncCsrfFromCookie();
  $('#user-label').textContent = `${user.username} · ${user.role}`;
  show('admin');
  if (user.role === 'admin') {
    await refreshAdmin();
  }
}

async function enterDemo() {
  setDemo(true);
  const demo = await import('./demo.js');
  const user = demo.demoUser();
  await afterLogin(user, 'demo-csrf');
  toast('已进入演示模式（本地模拟数据）');
}

/** Leave mock mode and clear any real session cookies. */
async function exitDemo() {
  const wasDemo = isDemo();
  setDemo(false);
  try {
    await api.logout();
  } catch {
    /* ignore network errors */
  }
  state.user = null;
  show('login');
  const hint = $('#demo-hint');
  if (hint) hint.hidden = false;
  if (wasDemo) toast('已退出演示，真实会话已注销');
}

async function boot() {
  const params = new URLSearchParams(location.search);
  if (params.get('demo') === '1') {
    setDemo(true);
  } else {
    restoreDemo();
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
      show('login');
      $('#demo-hint').hidden = false;
      return;
    }
  } catch (err) {
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

// ----- Events -----
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
$('#btn-exit-demo')?.addEventListener('click', () => exitDemo());

$('#btn-logout')?.addEventListener('click', async () => {
  if (isDemo()) {
    await exitDemo();
    return;
  }
  try {
    await api.logout();
  } catch {
    /* ignore */
  }
  state.user = null;
  show('login');
  $('#demo-hint').hidden = false;
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
  fillTokenDeviceSelect();
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
    await refreshAdmin();
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

boot();

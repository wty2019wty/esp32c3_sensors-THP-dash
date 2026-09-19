/** API client for THP Dash. Same-origin Worker by default. */

let csrf = '';
let demoMode = false;

export function isDemo() {
  return demoMode;
}

export function setDemo(on) {
  demoMode = !!on;
}

export function setCsrf(token) {
  csrf = token || '';
}

export function getCsrf() {
  return csrf;
}

function readCookie(name) {
  const m = document.cookie.match(new RegExp('(?:^|; )' + name + '=([^;]*)'));
  return m ? decodeURIComponent(m[1]) : '';
}

export function syncCsrfFromCookie() {
  csrf = readCookie('thp_csrf') || csrf;
  return csrf;
}

async function request(path, { method = 'GET', body, auth = true, raw = false } = {}) {
  if (demoMode && !path.startsWith('/demo')) {
    const demo = await import('./demo.js');
    return demo.handle(path, method, body);
  }

  const headers = {};
  if (body !== undefined) headers['content-type'] = 'application/json';
  if (auth && method !== 'GET' && method !== 'HEAD') {
    syncCsrfFromCookie();
    if (csrf) headers['x-csrf-token'] = csrf;
  }

  const res = await fetch(path, {
    method,
    headers,
    credentials: 'same-origin',
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });

  if (raw) return res;

  let data = null;
  const text = await res.text();
  if (text) {
    try {
      data = JSON.parse(text);
    } catch {
      data = { error: text };
    }
  }
  if (!res.ok) {
    const err = new Error(data?.error || res.statusText || '请求失败');
    err.status = res.status;
    err.data = data;
    throw err;
  }
  return data;
}

export const api = {
  bootstrapStatus: () => request('/api/auth/bootstrap', { auth: false }),
  bootstrapCreate: (username, password) =>
    request('/api/auth/bootstrap', { method: 'POST', body: { username, password }, auth: false }),
  login: (username, password) => request('/api/auth/login', { method: 'POST', body: { username, password }, auth: false }),
  logout: () => request('/api/auth/logout', { method: 'POST', body: {} }),
  me: () => request('/api/auth/me'),
  devices: () => request('/api/v1/devices'),
  createDevice: (payload) => request('/api/v1/devices', { method: 'POST', body: payload }),
  deleteDevice: (id) => request(`/api/v1/devices/${encodeURIComponent(id)}`, { method: 'DELETE', body: {} }),
  tokens: () => request('/api/v1/tokens'),
  createToken: (payload) => request('/api/v1/tokens', { method: 'POST', body: payload }),
  revokeToken: (id) => request(`/api/v1/tokens/${encodeURIComponent(id)}`, { method: 'DELETE', body: {} }),
  /** 硬删除已吊销的 Token 记录 */
  purgeToken: (id) =>
    request(`/api/v1/tokens/${encodeURIComponent(id)}?purge=1`, { method: 'DELETE', body: {} }),
  readings: (params) => {
    const q = new URLSearchParams();
    if (params.device_id) q.set('device_id', params.device_id);
    if (params.from) q.set('from', params.from);
    if (params.to) q.set('to', params.to);
    return request(`/api/v1/readings?${q}`);
  },
  latest: (deviceId) => {
    const q = deviceId ? `?device_id=${encodeURIComponent(deviceId)}` : '';
    return request(`/api/v1/latest${q}`);
  },
  exportUrl: (params) => {
    const q = new URLSearchParams();
    q.set('device_id', params.device_id);
    if (params.from) q.set('from', params.from);
    if (params.to) q.set('to', params.to);
    return `/api/v1/export?${q}`;
  },
};

export async function exportCsv(params) {
  if (demoMode) {
    const demo = await import('./demo.js');
    return demo.exportCsv(params);
  }
  const res = await fetch(api.exportUrl(params), { credentials: 'same-origin' });
  if (!res.ok) {
    let msg = '导出失败';
    try {
      const j = await res.json();
      msg = j.error || msg;
    } catch {
      /* ignore */
    }
    throw new Error(msg);
  }
  const blob = await res.blob();
  const disp = res.headers.get('content-disposition') || '';
  const m = /filename="?([^"]+)"?/.exec(disp);
  const filename = m ? m[1] : `thp_export_${Date.now()}.csv`;
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
  return filename;
}

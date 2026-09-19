/** Local demo data when Worker API is not reachable (preview only). */

const KEY = 'thp_demo_v1';

function pad(n) {
  return String(n).padStart(2, '0');
}

function iso(d) {
  return d.toISOString();
}

function seed() {
  const devices = [
    { id: 'dev_demo1', name: '演示设备一号', status: 'active', createdAt: iso(new Date(Date.now() - 86400000)), lastSeenAt: iso(new Date()), activeTokens: 1 },
  ];
  const points = [];
  const now = Date.now();
  for (let i = 288; i >= 0; i--) {
    const ts = new Date(now - i * 5 * 60 * 1000);
    const t = 22 + Math.sin(i / 18) * 2.5 + (Math.random() - 0.5) * 0.3;
    const h = 55 + Math.cos(i / 22) * 8 + (Math.random() - 0.5) * 1.2;
    const p = 1012 + Math.sin(i / 40) * 4 + (Math.random() - 0.5) * 0.4;
    points.push({
      ts: iso(ts),
      temperature: Math.round(t * 10) / 10,
      humidity: Math.round(h * 10) / 10,
      pressure: Math.round(p * 10) / 10,
    });
  }
  return {
    user: { id: 'usr_demo', username: 'demo', role: 'admin' },
    devices,
    tokens: [
      {
        id: 'tok_demo',
        deviceId: 'dev_demo1',
        deviceName: '演示设备一号',
        name: 'demo-token',
        createdAt: iso(new Date(Date.now() - 3600000)),
        revokedAt: null,
        lastUsedAt: iso(new Date()),
        status: 'active',
      },
    ],
    points,
  };
}

function load() {
  try {
    const raw = localStorage.getItem(KEY);
    if (raw) return JSON.parse(raw);
  } catch {
    /* ignore */
  }
  const s = seed();
  try {
    localStorage.setItem(KEY, JSON.stringify(s));
  } catch {
    /* ignore */
  }
  return s;
}

export function demoUser() {
  return load().user;
}

function filterPoints(points, from, to) {
  const f = from ? Date.parse(from) : -Infinity;
  const t = to ? Date.parse(to) : Infinity;
  return points.filter((p) => {
    const ms = Date.parse(p.ts);
    return ms >= f && ms <= t;
  });
}

function pickGran(points) {
  if (points.length <= 400) return { id: 'raw', label: '原始 5 分钟', seconds: 300 };
  return { id: 'h2', label: '2 小时', seconds: 7200 };
}

export async function handle(path, method, body) {
  const db = load();
  if (path.startsWith('/api/auth/bootstrap') && method === 'GET') {
    return { needsBootstrap: false };
  }
  if (path.startsWith('/api/auth/me')) {
    return { ok: true, user: db.user, csrf: 'demo-csrf' };
  }
  if (path === '/api/auth/login' && method === 'POST') {
    return { ok: true, user: db.user, csrf: 'demo-csrf' };
  }
  if (path === '/api/auth/logout') return { ok: true };
  if (path.startsWith('/api/v1/devices') && method === 'GET') {
    return { ok: true, devices: db.devices };
  }
  if (path.startsWith('/api/v1/devices') && method === 'POST') {
    const id = body?.id || `dev_${Date.now()}`;
    db.devices.push({
      id,
      name: body?.name || id,
      status: 'active',
      createdAt: new Date().toISOString(),
      lastSeenAt: null,
      activeTokens: 0,
    });
    localStorage.setItem(KEY, JSON.stringify(db));
    return { ok: true };
  }
  if (method === 'DELETE' && /\/api\/v1\/devices\//.test(path)) {
    return { ok: true };
  }
  if (path.startsWith('/api/v1/tokens') && method === 'GET') {
    return { ok: true, tokens: db.tokens };
  }
  if (path.startsWith('/api/v1/tokens') && method === 'POST') {
    return {
      ok: true,
      token: { id: `tok_${Date.now()}`, deviceId: body?.deviceId, name: body?.name, status: 'active' },
      secret: `thp_demo_${Date.now().toString(16)}`,
      warning: '演示模式 Token（仅界面预览）',
    };
  }
  if (method === 'DELETE' && /\/api\/v1\/tokens\//.test(path)) {
    const purge = path.includes('purge=1');
    const id = path.split('?')[0].split('/').pop();
    const t = db.tokens.find((x) => x.id === id);
    if (!t) return { error: 'Token 不存在', status: 404 };
    if (!purge) {
      t.status = 'revoked';
      t.revokedAt = t.revokedAt || new Date().toISOString();
      localStorage.setItem(KEY, JSON.stringify(db));
      return { ok: true, id, status: t.revokedAt ? 'revoked' : 'revoked' };
    }
    if (t.status === 'active') {
      return { error: '请先吊销 Token，再删除记录', status: 400 };
    }
    db.tokens = db.tokens.filter((x) => x.id !== id);
    localStorage.setItem(KEY, JSON.stringify(db));
    return { ok: true, id, status: 'deleted' };
  }
  if (path.startsWith('/api/v1/readings') && method === 'GET') {
    const url = new URL(path, 'https://demo.local');
    const deviceId = url.searchParams.get('device_id') || db.devices[0]?.id;
    const from = url.searchParams.get('from');
    const to = url.searchParams.get('to');
    const points = filterPoints(db.points, from, to);
    const gran = pickGran(points);
    return {
      ok: true,
      device: db.devices.find((d) => d.id === deviceId) || db.devices[0],
      range: { from, to },
      granularity: gran,
      isDownsampled: gran.id !== 'raw',
      pointCount: points.length,
      rawCount: points.length,
      latest: points[points.length - 1] || null,
      points,
    };
  }
  if (path.startsWith('/api/v1/latest')) {
    return {
      ok: true,
      devices: db.devices.map((d) => ({
        ...d,
        online: true,
        latest: db.points[db.points.length - 1],
      })),
    };
  }
  throw new Error('演示模式：该接口未模拟');
}

export async function exportCsv(params) {
  const db = load();
  const points = filterPoints(db.points, params.from, params.to);
  const lines = ['timestamp,temperature,humidity,pressure'];
  for (const p of points) {
    lines.push(`${p.ts},${p.temperature},${p.humidity},${p.pressure}`);
  }
  const csv = lines.join('\r\n') + '\r\n';
  const filename = `thp_demo_${params.device_id || 'device'}_${Date.now()}.csv`;
  const blob = new Blob([csv], { type: 'text/csv;charset=utf-8' });
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

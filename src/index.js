/**
 * THP Dash — Cloudflare Worker entry
 * All business APIs require auth (device token or user session). See REQUIREMENTS.md
 */
import { json, jsonError, withCors } from './lib/http.js';
import * as authRoutes from './routes/auth.js';
import * as deviceRoutes from './routes/devices.js';
import * as tokenRoutes from './routes/tokens.js';
import * as readingRoutes from './routes/readings.js';
import * as exportRoutes from './routes/export.js';

function route(path) {
  return path.replace(/\/+$/, '') || '/';
}

export default {
  async fetch(req, env, ctx) {
    const url = new URL(req.url);
    const path = route(url.pathname);
    const method = req.method.toUpperCase();

    if (method === 'OPTIONS') {
      return withCors(req, new Response(null, { status: 204 }), env);
    }

    let res;
    try {
      res = await handle(req, env, path, method, url);
    } catch (err) {
      console.error('worker_error', err);
      const detail = err?.message || err?.cause?.message || String(err);
      res = jsonError('服务器内部错误', 500, { detail });
    }

    // Static assets under non-/api paths handled when res is null
    if (!res) {
      if (env.ASSETS && !path.startsWith('/api/')) {
        res = await env.ASSETS.fetch(req);
        if (res) return res;
      }
      res = jsonError('Not Found', 404);
    }

    if (path.startsWith('/api/')) {
      return withCors(req, res, env);
    }
    return res;
  },
};

async function handle(req, env, path, method, url) {
  // ---- Auth ----
  if (path === '/api/auth/bootstrap' && method === 'GET') {
    return authRoutes.bootstrapStatus(env);
  }
  if (path === '/api/auth/bootstrap' && method === 'POST') {
    return authRoutes.bootstrapCreate(env, req);
  }
  if (path === '/api/auth/migrate' && method === 'POST') {
    return authRoutes.migrate(env, req);
  }
  if (path === '/api/auth/login' && method === 'POST') {
    return authRoutes.login(env, req);
  }
  if (path === '/api/auth/logout' && method === 'POST') {
    return authRoutes.logout(env, req);
  }
  if (path === '/api/auth/me' && method === 'GET') {
    return authRoutes.meWithCsrf(env, req);
  }

  // ---- Devices ----
  if (path === '/api/v1/devices' && method === 'GET') {
    return deviceRoutes.listDevices(env, req);
  }
  if (path === '/api/v1/devices' && method === 'POST') {
    return deviceRoutes.createDevice(env, req);
  }
  const deviceMatch = /^\/api\/v1\/devices\/([A-Za-z0-9_-]+)$/.exec(path);
  if (deviceMatch && method === 'DELETE') {
    return deviceRoutes.deleteDevice(env, req, deviceMatch[1]);
  }

  // ---- Tokens ----
  if (path === '/api/v1/tokens' && method === 'GET') {
    return tokenRoutes.listTokens(env, req);
  }
  if (path === '/api/v1/tokens' && method === 'POST') {
    return tokenRoutes.createToken(env, req);
  }
  const tokenMatch = /^\/api\/v1\/tokens\/([A-Za-z0-9_-]+)$/.exec(path);
  if (tokenMatch && method === 'DELETE') {
    return tokenRoutes.revokeToken(env, req, tokenMatch[1]);
  }

  // ---- Readings ----
  if (path === '/api/v1/readings' && method === 'POST') {
    return readingRoutes.ingestReading(env, req);
  }
  if (path === '/api/v1/readings' && method === 'GET') {
    return readingRoutes.queryReadings(env, req);
  }
  if (path === '/api/v1/latest' && method === 'GET') {
    return readingRoutes.queryLatest(env, req);
  }

  // ---- Export ----
  if (path === '/api/v1/export' && method === 'GET') {
    return exportRoutes.exportCsv(env, req);
  }

  if (path === '/api/health' && method === 'GET') {
    return json({ ok: true, service: 'thp-dash', time: new Date().toISOString() });
  }

  if (path.startsWith('/api/')) {
    return jsonError('Not Found', 404);
  }

  // Non-API: try static assets (Pages / workers assets)
  if (env.ASSETS) {
    return env.ASSETS.fetch(req);
  }
  return null;
}

import {
  hashSessionSecret,
  hashCsrf,
  hashPassword,
  verifyPassword,
  issueSessionCredentials,
  randomId,
  nowIso,
  addDaysIso,
  timingSafeEqualHex,
} from './crypto.js';
import { parseCookies, serializeCookie, getQuery } from './http.js';
import { ensureSchemaAndCountUsers, isMissingTableError } from './schema.js';

export const SESSION_COOKIE = 'thp_session';
export const CSRF_COOKIE = 'thp_csrf';
export const SESSION_DAYS = 1;
const SESSION_MAX_AGE = SESSION_DAYS * 24 * 3600;

export async function createSession(env, userId) {
  const { secret, csrf } = issueSessionCredentials();
  const id = randomId('sess');
  const now = nowIso();
  const expires = addDaysIso(SESSION_DAYS);
  await env.DB.prepare(
    `INSERT INTO sessions (id, user_id, secret_hash, created_at, expires_at, last_used_at, csrf_hash)
     VALUES (?, ?, ?, ?, ?, ?, ?)`
  )
    .bind(id, userId, await hashSessionSecret(secret), now, expires, now, await hashCsrf(csrf))
    .run();
  return {
    sessionId: id,
    secret,
    csrf,
    expiresAt: expires,
    headers: [
      ['set-cookie', serializeCookie(SESSION_COOKIE, secret, {
        maxAge: SESSION_MAX_AGE,
        httpOnly: true,
        secure: true,
        sameSite: 'Lax',
      })],
      ['set-cookie', serializeCookie(CSRF_COOKIE, csrf, {
        maxAge: SESSION_MAX_AGE,
        httpOnly: false,
        secure: true,
        sameSite: 'Lax',
      })],
    ],
  };
}

export async function destroySession(env, req) {
  const cookies = parseCookies(req);
  const secret = cookies[SESSION_COOKIE];
  if (secret) {
    const secretHash = await hashSessionSecret(secret);
    await env.DB.prepare(`DELETE FROM sessions WHERE secret_hash = ?`).bind(secretHash).run();
  }
  return [
    ['set-cookie', serializeCookie(SESSION_COOKIE, '', { maxAge: 0, httpOnly: true, secure: true, sameSite: 'Lax' })],
    ['set-cookie', serializeCookie(CSRF_COOKIE, '', { maxAge: 0, httpOnly: false, secure: true, sameSite: 'Lax' })],
  ];
}

export async function resolveUserSession(env, req) {
  const cookies = parseCookies(req);
  const secret = cookies[SESSION_COOKIE];
  if (!secret) return null;
  const secretHash = await hashSessionSecret(secret);
  const row = await env.DB.prepare(
    `SELECT s.id AS session_id, s.expires_at, s.csrf_hash, s.user_id,
            u.username, u.role, u.disabled_at
     FROM sessions s
     JOIN users u ON u.id = s.user_id
     WHERE s.secret_hash = ?`
  )
    .bind(secretHash)
    .first();
  if (!row) return null;
  if (row.disabled_at) return null;
  if (Date.parse(row.expires_at) <= Date.now()) {
    await env.DB.prepare(`DELETE FROM sessions WHERE id = ?`).bind(row.session_id).run();
    return null;
  }
  // Sliding session: every successful auth extends expires_at AND re-issues cookies
  // so browser Max-Age tracks the same window (otherwise cookie dies at login+SESSION_DAYS).
  const now = new Date();
  const newExpires = addDaysIso(SESSION_DAYS, now);
  await env.DB.prepare(`UPDATE sessions SET last_used_at = ?, expires_at = ? WHERE id = ?`)
    .bind(now.toISOString(), newExpires, row.session_id)
    .run();
  const cookieHeaders = slidingCookieHeaders({ secret, csrf: cookies[CSRF_COOKIE] || '' });
  return {
    sessionId: row.session_id,
    userId: row.user_id,
    username: row.username,
    role: row.role,
    csrfHash: row.csrf_hash,
    expiresAt: newExpires,
    cookieHeaders,
  };
}

/** Set-Cookie pairs that refresh session + CSRF Max-Age to SESSION_MAX_AGE. */
export function slidingCookieHeaders({ secret, csrf }) {
  if (!secret) return [];
  const headers = [
    ['set-cookie', serializeCookie(SESSION_COOKIE, secret, {
      maxAge: SESSION_MAX_AGE,
      httpOnly: true,
      secure: true,
      sameSite: 'Lax',
    })],
  ];
  if (csrf) {
    headers.push([
      'set-cookie',
      serializeCookie(CSRF_COOKIE, csrf, {
        maxAge: SESSION_MAX_AGE,
        httpOnly: false,
        secure: true,
        sameSite: 'Lax',
      }),
    ]);
  }
  return headers;
}

export async function requireUser(env, req) {
  return resolveUserSession(env, req);
}

export function requireAdmin(session) {
  return !!session && session.role === 'admin';
}

/**
 * CSRF: non-GET requests must send X-CSRF-Token matching the csrf cookie / session.
 */
export async function assertCsrf(env, req, session) {
  if (!session) return false;
  const method = req.method.toUpperCase();
  if (method === 'GET' || method === 'HEAD' || method === 'OPTIONS') return true;
  const header = req.headers.get('x-csrf-token') || '';
  if (!header) return false;
  const cookies = parseCookies(req);
  const cookieCsrf = cookies[CSRF_COOKIE] || '';
  const headerHash = await hashCsrf(header);
  const cookieHash = cookieCsrf ? await hashCsrf(cookieCsrf) : null;
  const okHeader = timingSafeEqualHex(headerHash, session.csrfHash);
  const okCookie = cookieHash ? timingSafeEqualHex(cookieHash, session.csrfHash) : true;
  return okHeader && okCookie;
}

export async function countUsers(env) {
  try {
    const row = await env.DB.prepare(`SELECT COUNT(*) AS c FROM users`).first();
    return Number(row?.c || 0);
  } catch (err) {
    if (isMissingTableError(err)) return ensureSchemaAndCountUsers(env);
    throw err;
  }
}

export async function createUser(env, { username, password, role = 'admin' }) {
  const passwordHash = await hashPassword(password);
  const id = randomId('usr');
  await env.DB.prepare(
    `INSERT INTO users (id, username, password_hash, role, created_at)
     VALUES (?, ?, ?, ?, ?)`
  )
    .bind(id, username, passwordHash, role, nowIso())
    .run();
  return { id, username, role };
}

export async function findUserByUsername(env, username) {
  return env.DB.prepare(
    `SELECT id, username, password_hash, role, disabled_at FROM users WHERE username = ?`
  )
    .bind(username)
    .first();
}

export { verifyPassword, hashPassword };

/** Login failure window (seconds). Cache API needs max-age to actually store. */
export const LOGIN_RATE_LIMIT_WINDOW_S = 900;
export const LOGIN_RATE_LIMIT_MAX = 8;

function loginRateKey(req) {
  const ip = req.headers.get('cf-connecting-ip') || 'unknown';
  return new Request(`https://thp-ratelimit.local/login/${ip}`);
}

/**
 * Simple in-Workers login throttle via Cache API (best-effort).
 * Response MUST carry Cache-Control max-age or Workers Cache will not persist it.
 */
export async function loginRateLimited(req) {
  const key = loginRateKey(req);
  const cache = caches.default;
  try {
    const hit = await cache.match(key);
    if (!hit) return false;
    const expiresAt = Number(hit.headers.get('x-expires-at') || 0);
    if (expiresAt && Date.now() > expiresAt) {
      await cache.delete(key).catch(() => {});
      return false;
    }
    const count = Number(hit.headers.get('x-count') || 0);
    return count >= LOGIN_RATE_LIMIT_MAX;
  } catch {
    return false;
  }
}

export async function recordLoginFailure(req) {
  const key = loginRateKey(req);
  const cache = caches.default;
  try {
    const hit = await cache.match(key);
    const now = Date.now();
    const expiresAt = Number(hit?.headers.get('x-expires-at') || 0);
    const expired = !hit || (expiresAt && now > expiresAt);
    const count = expired ? 1 : Number(hit.headers.get('x-count') || 0) + 1;
    const res = new Response(null, { status: 200 });
    res.headers.set('x-count', String(count));
    const ttl = LOGIN_RATE_LIMIT_WINDOW_S;
    res.headers.set('x-expires-at', String(now + ttl * 1000));
    // Workers Cache API requires max-age/s-maxage to store the entry
    res.headers.set('Cache-Control', `public, max-age=${ttl}`);
    await cache.put(key, res.clone());
  } catch {
    // rate limit is best-effort
  }
}

export async function clearLoginFailures(req) {
  const ip = req.headers.get('cf-connecting-ip') || 'unknown';
  const key = new Request(`https://thp-ratelimit.local/login/${ip}`);
  try {
    await caches.default.delete(key);
  } catch {
    // ignore
  }
}

export function publicUser(session) {
  return {
    id: session.userId,
    username: session.username,
    role: session.role,
  };
}

export function csrfFromQueryOrHeader(req) {
  return req.headers.get('x-csrf-token') || getQuery(req).get('csrf') || '';
}

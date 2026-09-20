import {
  createSession,
  destroySession,
  resolveUserSession,
  createUser,
  findUserByUsername,
  verifyPassword,
  hashPassword,
  loginRateLimited,
  recordLoginFailure,
  clearLoginFailures,
  publicUser,
  assertCsrf,
  requireUser,
  CSRF_COOKIE,
} from '../lib/auth.js';
import { json, jsonError, readJson, requireString, parseCookies } from '../lib/http.js';
import { ensureSchemaAndCountUsers, ensureSchema } from '../lib/schema.js';

/** Dummy PBKDF2 hash — equalizes login timing when the username does not exist. */
let _dummyHash = null;
async function dummyPasswordHash() {
  if (!_dummyHash) {
    _dummyHash = await hashPassword('thp-dummy-timing-equalizer-not-a-real-secret');
  }
  return _dummyHash;
}

function applyHeaders(res, headerPairs) {
  const headers = new Headers(res.headers);
  for (const [k, v] of headerPairs || []) headers.append(k, v);
  return new Response(res.body, { status: res.status, headers });
}

export async function bootstrapStatus(env) {
  const n = await ensureSchemaAndCountUsers(env);
  return json({ needsBootstrap: n === 0 });
}

/**
 * POST /api/auth/migrate — create tables if missing.
 * Unauthenticated only when the DB has zero users (bootstrap path).
 * Once any user exists, requires admin session + CSRF.
 */
export async function migrate(env, req) {
  const n = await ensureSchemaAndCountUsers(env);
  let session = null;
  if (n > 0) {
    session = await requireUser(env, req);
    if (!session || session.role !== 'admin') {
      return jsonError('需要管理员权限', 403);
    }
    const ok = await assertCsrf(env, req, session);
    if (!ok) return jsonError('CSRF 校验失败', 403);
  }
  await ensureSchema(env);
  return applyHeaders(json({ ok: true, migrated: true }), session?.cookieHeaders || []);
}

export async function bootstrapCreate(env, req) {
  const existing = await ensureSchemaAndCountUsers(env);
  if (existing > 0) return jsonError('系统已初始化，无法再次引导创建', 403);
  const body = await readJson(req);
  const usernameError = requireString(body?.username, 'username', { min: 2, max: 64 });
  if (usernameError) return jsonError(usernameError);
  const passwordError = requireString(body?.password, 'password', { min: 8, max: 128 });
  if (passwordError) return jsonError(passwordError);
  const username = String(body.username).trim();
  if (!/^[a-zA-Z0-9._-]+$/.test(username)) {
    return jsonError('用户名仅允许字母、数字、点、下划线与短横线');
  }
  const user = await createUser(env, {
    username,
    password: String(body.password),
    role: 'admin',
  });
  return json({ ok: true, user: { id: user.id, username: user.username, role: user.role } }, 201);
}

export async function login(env, req) {
  if (await loginRateLimited(req)) {
    return jsonError('尝试过于频繁，请稍后再试', 429);
  }
  const body = await readJson(req);
  const username = String(body?.username || '').trim();
  const password = String(body?.password || '');
  if (!username || !password) {
    await recordLoginFailure(req);
    return jsonError('用户名或密码错误', 401);
  }
  const user = await findUserByUsername(env, username);
  // Always run a slow PBKDF2 verify so missing-user vs wrong-password timing is similar
  let ok = false;
  if (user && !user.disabled_at) {
    ok = await verifyPassword(password, user.password_hash);
  } else {
    await verifyPassword(password, await dummyPasswordHash());
  }
  if (!ok) {
    await recordLoginFailure(req);
    return jsonError('用户名或密码错误', 401);
  }
  await clearLoginFailures(req);
  const session = await createSession(env, user.id);
  return applyHeaders(
    json({
      ok: true,
      user: { id: user.id, username: user.username, role: user.role },
      csrf: session.csrf,
      expiresAt: session.expiresAt,
    }),
    session.headers
  );
}

export async function logout(env, req) {
  const headers = await destroySession(env, req);
  return applyHeaders(json({ ok: true }), headers);
}

/** Attach sliding-session Set-Cookie headers (if any) to a Response. */
function withSlidingCookies(res, session) {
  const headers = session?.cookieHeaders;
  if (!headers?.length) return res;
  return applyHeaders(res, headers);
}

export async function meWithCsrf(env, req) {
  const session = await resolveUserSession(env, req);
  if (!session) return jsonError('未登录', 401);
  const cookies = parseCookies(req);
  return withSlidingCookies(
    json({
      ok: true,
      user: publicUser(session),
      csrf: cookies[CSRF_COOKIE] || '',
    }),
    session
  );
}

export async function requireSessionAction(env, req, handler) {
  const session = await requireUser(env, req);
  if (!session) return jsonError('未登录', 401);
  const ok = await assertCsrf(env, req, session);
  if (!ok) return jsonError('CSRF 校验失败', 403);
  const result = await handler(session);
  return withSlidingCookies(result, session);
}

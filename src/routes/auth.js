import {
  createSession,
  destroySession,
  resolveUserSession,
  countUsers,
  createUser,
  findUserByUsername,
  verifyPassword,
  loginRateLimited,
  recordLoginFailure,
  clearLoginFailures,
  publicUser,
  assertCsrf,
  requireUser,
} from '../lib/auth.js';
import { json, jsonError, readJson, requireString } from '../lib/http.js';

function applyHeaders(res, headerPairs) {
  const headers = new Headers(res.headers);
  for (const [k, v] of headerPairs || []) headers.append(k, v);
  return new Response(res.body, { status: res.status, headers });
}

export async function bootstrapStatus(env) {
  const n = await countUsers(env);
  return json({ needsBootstrap: n === 0 });
}

export async function bootstrapCreate(env, req) {
  const existing = await countUsers(env);
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
  const ok = user && !user.disabled_at && (await verifyPassword(password, user.password_hash));
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

export async function me(env, req) {
  const session = await resolveUserSession(env, req);
  if (!session) return jsonError('未登录', 401);
  return json({ ok: true, user: publicUser(session), csrf: session.csrfHash ? undefined : undefined });
}

/** Return me with raw csrf from cookie for SPA convenience after reload — csrf still in cookie. */
export async function meWithCsrf(env, req) {
  const session = await resolveUserSession(env, req);
  if (!session) return jsonError('未登录', 401);
  const cookies = Object.fromEntries(
    (req.headers.get('cookie') || '').split(';').map((p) => {
      const i = p.indexOf('=');
      return i === -1 ? [p.trim(), ''] : [p.slice(0, i).trim(), decodeURIComponent(p.slice(i + 1).trim())];
    })
  );
  return json({
    ok: true,
    user: publicUser(session),
    csrf: cookies['thp_csrf'] || '',
  });
}

export async function requireSessionAction(env, req, handler) {
  const session = await requireUser(env, req);
  if (!session) return jsonError('未登录', 401);
  const ok = await assertCsrf(env, req, session);
  if (!ok) return jsonError('CSRF 校验失败', 403);
  return handler(session);
}

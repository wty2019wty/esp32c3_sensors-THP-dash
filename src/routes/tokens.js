import { issueDeviceToken, hashToken, randomId, nowIso } from '../lib/crypto.js';
import { json, jsonError, readJson, requireString } from '../lib/http.js';
import { requireSessionAction } from './auth.js';

export async function listTokens(env, req) {
  return requireSessionAction(env, req, async () => {
    const { results } = await env.DB.prepare(
      `SELECT t.id, t.device_id, t.name, t.created_at, t.revoked_at, t.last_used_at, d.name AS device_name
       FROM api_tokens t
       JOIN devices d ON d.id = t.device_id
       ORDER BY t.created_at DESC`
    ).all();
    return json({
      ok: true,
      tokens: results.map((r) => ({
        id: r.id,
        deviceId: r.device_id,
        deviceName: r.device_name,
        name: r.name,
        createdAt: r.created_at,
        revokedAt: r.revoked_at,
        lastUsedAt: r.last_used_at,
        status: r.revoked_at ? 'revoked' : 'active',
      })),
    });
  });
}

/** Create token — plaintext returned only once */
export async function createToken(env, req) {
  return requireSessionAction(env, req, async (session) => {
    if (session.role !== 'admin') return jsonError('需要管理员权限', 403);
    const body = await readJson(req);
    const deviceIdError = requireString(body?.deviceId, 'deviceId');
    if (deviceIdError) return jsonError(deviceIdError);
    const deviceId = String(body.deviceId).trim();
    const name = body?.name ? String(body.name).trim().slice(0, 64) : '';

    const device = await env.DB.prepare(`SELECT id FROM devices WHERE id = ?`).bind(deviceId).first();
    if (!device) return jsonError('设备不存在', 404);

    const issued = issueDeviceToken();
    const id = randomId('tok');
    const tokenHash = await hashToken(issued.plain);
    await env.DB.prepare(
      `INSERT INTO api_tokens (id, device_id, name, token_hash, created_at)
       VALUES (?, ?, ?, ?, ?)`
    )
      .bind(id, deviceId, name, tokenHash, nowIso())
      .run();

    return json(
      {
        ok: true,
        token: {
          id,
          deviceId,
          name,
          createdAt: nowIso(),
          status: 'active',
        },
        // SHOWN ONLY ONCE
        secret: issued.plain,
        warning: '请立即保存该 Token，关闭后将无法再次查看。',
      },
      201
    );
  });
}

export async function revokeToken(env, req, tokenId) {
  return requireSessionAction(env, req, async (session) => {
    if (session.role !== 'admin') return jsonError('需要管理员权限', 403);
    const row = await env.DB.prepare(`SELECT id, revoked_at FROM api_tokens WHERE id = ?`)
      .bind(tokenId)
      .first();
    if (!row) return jsonError('Token 不存在', 404);
    if (row.revoked_at) return json({ ok: true, id: tokenId, status: 'already_revoked' });
    await env.DB.prepare(`UPDATE api_tokens SET revoked_at = ? WHERE id = ?`)
      .bind(nowIso(), tokenId)
      .run();
    return json({ ok: true, id: tokenId, status: 'revoked' });
  });
}

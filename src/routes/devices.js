import { randomId, nowIso } from '../lib/crypto.js';
import { json, jsonError, readJson, requireString } from '../lib/http.js';
import { requireSessionAction } from './auth.js';

export async function listDevices(env, req) {
  return requireSessionAction(env, req, async () => {
    const { results } = await env.DB.prepare(
      `SELECT id, name, status, created_at, last_seen_at,
              (SELECT COUNT(*) FROM api_tokens t WHERE t.device_id = devices.id AND t.revoked_at IS NULL) AS active_tokens
       FROM devices
       ORDER BY created_at ASC`
    ).all();
    return json({
      ok: true,
      devices: results.map((r) => ({
        id: r.id,
        name: r.name,
        status: r.status,
        createdAt: r.created_at,
        lastSeenAt: r.last_seen_at,
        activeTokens: r.active_tokens,
      })),
    });
  });
}

export async function createDevice(env, req) {
  return requireSessionAction(env, req, async (session) => {
    if (session.role !== 'admin') return jsonError('需要管理员权限', 403);
    const body = await readJson(req);
    const nameError = requireString(body?.name, 'name', { min: 1, max: 64 });
    if (nameError) return jsonError(nameError);
    const name = String(body.name).trim();
    const id = body?.id && String(body.id).trim() ? String(body.id).trim() : randomId('dev');
    if (!/^[a-zA-Z0-9_-]{2,64}$/.test(id)) {
      return jsonError('设备 ID 仅允许字母、数字、下划线与短横线（2–64）');
    }
    const exists = await env.DB.prepare(`SELECT id FROM devices WHERE id = ?`).bind(id).first();
    if (exists) return jsonError('设备 ID 已存在', 409);
    await env.DB.prepare(
      `INSERT INTO devices (id, name, status, created_at) VALUES (?, ?, 'active', ?)`
    )
      .bind(id, name, nowIso())
      .run();
    return json({ ok: true, device: { id, name, status: 'active', createdAt: nowIso() } }, 201);
  });
}

export async function deleteDevice(env, req, deviceId) {
  return requireSessionAction(env, req, async (session) => {
    if (session.role !== 'admin') return jsonError('需要管理员权限', 403);
    const row = await env.DB.prepare(`SELECT id FROM devices WHERE id = ?`).bind(deviceId).first();
    if (!row) return jsonError('设备不存在', 404);
    // Cascade deletes tokens + readings via FK
    await env.DB.prepare(`DELETE FROM devices WHERE id = ?`).bind(deviceId).run();
    return json({ ok: true, deleted: deviceId });
  });
}

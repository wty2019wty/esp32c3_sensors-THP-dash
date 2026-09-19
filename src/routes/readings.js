import { hashToken, nowIso, timingSafeEqualHex } from '../lib/crypto.js';
import {
  json,
  jsonError,
  readJson,
  getBearer,
  getQuery,
  isValidIso,
  normalizeIso,
  localDayStartIso,
} from '../lib/http.js';
import { requireSessionAction } from './auth.js';
import { pickGranularity, aggregateRows, round3 } from '../lib/downsample.js';

function resolveDisplayTz(env) {
  return env.TZ_DISPLAY || env.DISPLAY_TZ || 'Asia/Shanghai';
}

async function resolveDeviceFromToken(env, plain) {
  if (!plain) return null;
  const tokenHash = await hashToken(plain);
  const row = await env.DB.prepare(
    `SELECT t.id AS token_id, t.device_id, t.revoked_at, d.status, d.name
     FROM api_tokens t
     JOIN devices d ON d.id = t.device_id
     WHERE t.token_hash = ?`
  )
    .bind(tokenHash)
    .first();
  if (!row) return null;
  if (row.revoked_at) return null;
  if (row.status !== 'active') return null;
  return row;
}

function validateReadingPayload(body) {
  const t = Number(body?.temperature);
  const h = Number(body?.humidity);
  const p = Number(body?.pressure);
  if (!Number.isFinite(t) || t < -40 || t > 85) return { error: 'temperature 不合法' };
  if (!Number.isFinite(h) || h < 0 || h > 100) return { error: 'humidity 不合法' };
  if (!Number.isFinite(p) || p < 300 || p > 1200) return { error: 'pressure 不合法' };
  if (body?.device_id != null && body.device_id !== '' && body.device_id !== undefined) {
    // checked by caller against token binding
  }
  let measuredAt = null;
  if (body?.measured_at) {
    if (!isValidIso(body.measured_at)) return { error: 'measured_at 必须是 ISO-8601 时间' };
    measuredAt = normalizeIso(body.measured_at);
  }
  let rssi = null;
  if (body?.rssi != null) {
    const r = Number(body.rssi);
    if (Number.isFinite(r)) rssi = Math.trunc(r);
  }
  return {
    values: {
      temperature: round3(t),
      humidity: round3(h),
      pressure: round3(p),
      measuredAt,
      rssi,
    },
  };
}

/** Device API: POST /api/v1/readings — Bearer device token only */
export async function ingestReading(env, req) {
  const bearer = getBearer(req);
  if (!bearer) return jsonError('缺少设备 Token', 401);
  const device = await resolveDeviceFromToken(env, bearer);
  if (!device) return jsonError('设备 Token 无效或已吊销', 401);

  const body = await readJson(req);
  const checked = validateReadingPayload(body);
  if (checked.error) return jsonError(checked.error);

  if (body?.device_id && String(body.device_id) !== device.device_id) {
    return jsonError('device_id 与 Token 绑定设备不一致', 403);
  }

  const ts = nowIso();
  const v = checked.values;
  await env.DB.prepare(
    `INSERT INTO readings (device_id, ts, temperature, humidity, pressure, measured_at, rssi)
     VALUES (?, ?, ?, ?, ?, ?, ?)`
  )
    .bind(device.device_id, ts, v.temperature, v.humidity, v.pressure, v.measuredAt, v.rssi)
    .run();

  await env.DB.prepare(
    `UPDATE devices SET last_seen_at = ? WHERE id = ?`
  )
    .bind(ts, device.device_id)
    .run();

  await env.DB.prepare(`UPDATE api_tokens SET last_used_at = ? WHERE id = ?`)
    .bind(ts, device.token_id)
    .run();

  return json({ ok: true, deviceId: device.device_id, ts }, 201);
}

async function loadSeries(env, deviceId, fromIso, toIso) {
  const fromMs = Date.parse(fromIso);
  const toMs = Date.parse(toIso);
  const gran = pickGranularity(fromMs, toMs);

  const { results } = await env.DB.prepare(
    `SELECT ts, temperature, humidity, pressure
     FROM readings
     WHERE device_id = ? AND ts >= ? AND ts <= ?
     ORDER BY ts ASC`
  )
    .bind(deviceId, fromIso, toIso)
    .all();

  const rows = (results || []).map((r) => ({
    ts: r.ts,
    temperature: r.temperature,
    humidity: r.humidity,
    pressure: r.pressure,
  }));

  const points = gran.sql ? aggregateRows(rows, gran.seconds) : rows;
  return { gran, points, rawCount: rows.length };
}

export async function queryReadings(env, req) {
  return requireSessionAction(env, req, async () => {
    const q = getQuery(req);
    const deviceId = (q.get('device_id') || '').trim();
    if (!deviceId) return jsonError('缺少 device_id');

    const tz = resolveDisplayTz(env);
    const now = Date.now();
    let from = q.get('from');
    let to = q.get('to');
    if (!from) from = localDayStartIso(tz);
    if (!to) to = new Date(now).toISOString();
    if (!isValidIso(from) || !isValidIso(to)) {
      return jsonError('from/to 必须是 ISO-8601 时间');
    }
    from = normalizeIso(from);
    to = normalizeIso(to);
    if (Date.parse(from) > Date.parse(to)) return jsonError('from 不能晚于 to');

    const device = await env.DB.prepare(`SELECT id, name, last_seen_at FROM devices WHERE id = ?`)
      .bind(deviceId)
      .first();
    if (!device) return jsonError('设备不存在', 404);

    const { gran, points, rawCount } = await loadSeries(env, deviceId, from, to);

    let latest = null;
    const last = await env.DB.prepare(
      `SELECT ts, temperature, humidity, pressure FROM readings
       WHERE device_id = ? ORDER BY ts DESC LIMIT 1`
    )
      .bind(deviceId)
      .first();
    if (last) {
      latest = {
        ts: last.ts,
        temperature: last.temperature,
        humidity: last.humidity,
        pressure: last.pressure,
      };
    }

    return json({
      ok: true,
      device: {
        id: device.id,
        name: device.name,
        lastSeenAt: device.last_seen_at,
      },
      range: { from, to, timezone: tz },
      granularity: { id: gran.id, label: gran.label, seconds: gran.seconds },
      isDownsampled: gran.id !== 'raw',
      pointCount: points.length,
      rawCount,
      latest,
      points,
    });
  });
}

export async function queryLatest(env, req) {
  return requireSessionAction(env, req, async () => {
    const q = getQuery(req);
    const deviceId = (q.get('device_id') || '').trim();
    const tz = resolveDisplayTz(env);
    const OFFLINE_MS = 10 * 60 * 1000;

    const devices = deviceId
      ? await env.DB.prepare(
          `SELECT id, name, status, last_seen_at FROM devices WHERE id = ?`
        )
        .bind(deviceId)
        .all()
      : await env.DB.prepare(
          `SELECT id, name, status, last_seen_at FROM devices ORDER BY created_at ASC`
        ).all();

    const out = [];
    for (const d of devices.results || []) {
      const reading = await env.DB.prepare(
        `SELECT ts, temperature, humidity, pressure FROM readings
         WHERE device_id = ? ORDER BY ts DESC LIMIT 1`
      )
        .bind(d.id)
        .first();
      const lastSeen = d.last_seen_at || reading?.ts || null;
      const offline =
        !lastSeen || Date.now() - Date.parse(lastSeen) > OFFLINE_MS;
      out.push({
        id: d.id,
        name: d.name,
        status: d.status,
        lastSeenAt: lastSeen,
        online: d.status === 'active' && !offline,
        latest: reading
          ? {
              ts: reading.ts,
              temperature: reading.temperature,
              humidity: reading.humidity,
              pressure: reading.pressure,
            }
          : null,
      });
    }

    return json({ ok: true, timezone: tz, devices: out });
  });
}

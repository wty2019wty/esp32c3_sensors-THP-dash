import { round3 } from './downsample.js';
import { isValidIso, normalizeIso } from './http.js';

/**
 * 读数字段可部分上报：仅温湿度（SHT40）或仅气压（BMP280）。
 * 缺省字段必须为 undefined/null；拒绝空串、布尔、数组等会被 Number 静默转成数字的输入。
 */
export function parseOptionalMetric(body, key, min, max) {
  if (body == null || body[key] === undefined || body[key] === null) {
    return { present: false, value: null };
  }
  const raw = body[key];
  if (typeof raw === 'number') {
    if (!Number.isFinite(raw) || raw < min || raw > max) {
      return { error: `${key} 不合法` };
    }
    return { present: true, value: round3(raw) };
  }
  if (typeof raw === 'string') {
    const s = raw.trim();
    if (!s) return { error: `${key} 不合法` };
    const n = Number(s);
    if (!Number.isFinite(n) || n < min || n > max) {
      return { error: `${key} 不合法` };
    }
    return { present: true, value: round3(n) };
  }
  return { error: `${key} 不合法` };
}

/**
 * 与设备上报协议对齐：T/H 同源必须同时出现；至少一组有效字段。
 * @returns {{error: string}|{values: {temperature: number|null, humidity: number|null, pressure: number|null, measuredAt: string|null, rssi: number|null}}}
 */
export function validateReadingPayload(body) {
  const t = parseOptionalMetric(body, 'temperature', -40, 85);
  if (t.error) return { error: t.error };
  const h = parseOptionalMetric(body, 'humidity', 0, 100);
  if (h.error) return { error: h.error };
  const p = parseOptionalMetric(body, 'pressure', 300, 1200);
  if (p.error) return { error: p.error };

  if (t.present !== h.present) {
    return { error: 'temperature 与 humidity 必须同时上报（均来自 SHT40）' };
  }
  if (!t.present && !p.present) {
    return { error: '至少上报 temperature/humidity 或 pressure' };
  }

  let measuredAt = null;
  if (body?.measured_at) {
    if (!isValidIso(body.measured_at)) return { error: 'measured_at 必须是 ISO-8601 时间' };
    measuredAt = normalizeIso(body.measured_at);
  }
  let rssi = null;
  if (body?.rssi != null) {
    if (typeof body.rssi === 'boolean' || Array.isArray(body.rssi) || (typeof body.rssi === 'object' && body.rssi !== null)) {
      return { error: 'rssi 不合法' };
    }
    if (typeof body.rssi === 'string' && !body.rssi.trim()) {
      return { error: 'rssi 不合法' };
    }
    const r = Number(body.rssi);
    if (Number.isFinite(r)) rssi = Math.trunc(r);
  }
  return {
    values: {
      temperature: t.present ? t.value : null,
      humidity: h.present ? h.value : null,
      pressure: p.present ? p.value : null,
      measuredAt,
      rssi,
    },
  };
}

import {
  getQuery,
  jsonError,
  isValidIso,
  normalizeIso,
  localDayStartIso,
} from '../lib/http.js';
import { requireSessionAction } from './auth.js';
import { MAX_EXPORT_ROWS } from '../lib/downsample.js';
import { readingsToCsv, exportFilename, csvResponse } from '../lib/csv.js';
import { loadSeries } from './readings.js';

function resolveDisplayTz(env) {
  return env.TZ_DISPLAY || env.DISPLAY_TZ || 'Asia/Shanghai';
}

/** GET /api/v1/export?device_id&from&to — user session; same auto-downsample as charts */
export async function exportCsv(env, req) {
  return requireSessionAction(env, req, async () => {
    const q = getQuery(req);
    const deviceId = (q.get('device_id') || '').trim();
    if (!deviceId) return jsonError('缺少 device_id');

    const tz = resolveDisplayTz(env);
    let from = q.get('from');
    let to = q.get('to');
    if (!from) from = localDayStartIso(tz);
    if (!to) to = new Date().toISOString();
    if (!isValidIso(from) || !isValidIso(to)) {
      return jsonError('from/to 必须是 ISO-8601 时间');
    }
    from = normalizeIso(from);
    to = normalizeIso(to);
    if (Date.parse(from) > Date.parse(to)) return jsonError('from 不能晚于 to');

    const device = await env.DB.prepare(`SELECT id, name FROM devices WHERE id = ?`)
      .bind(deviceId)
      .first();
    if (!device) return jsonError('设备不存在', 404);

    const { gran, points, rawCount } = await loadSeries(env, deviceId, from, to);

    if (points.length > MAX_EXPORT_ROWS) {
      return jsonError(
        `导出行数 ${points.length} 超过上限 ${MAX_EXPORT_ROWS}，请缩短时间范围`,
        413,
        { limit: MAX_EXPORT_ROWS, pointCount: points.length }
      );
    }
    if (rawCount > 0 && points.length === 0) {
      return jsonError('所选范围内没有数据', 404);
    }

    const csv = readingsToCsv(points, { utf8Bom: true });
    const filename = exportFilename(device.name || device.id, from, to);
    const res = csvResponse(csv, filename);
    res.headers.set('x-thp-granularity', gran.id);
    res.headers.set('x-thp-granularity-label', encodeURIComponent(gran.label));
    res.headers.set('x-thp-point-count', String(points.length));
    res.headers.set('x-thp-raw-count', String(rawCount));
    return res;
  });
}

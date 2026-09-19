import { round2 } from './downsample.js';

const CSV_COLUMNS = ['timestamp', 'temperature', 'humidity', 'pressure'];

function csvEscape(value) {
  if (value == null) return '';
  const s = String(value);
  if (/[",\r\n]/.test(s)) return `"${s.replace(/"/g, '""')}"`;
  return s;
}

/**
 * @param {Array<{ts:string, temperature:number, humidity:number, pressure:number}>} points
 * @param {{ utf8Bom?: boolean }} [opts]
 */
export function readingsToCsv(points, opts = {}) {
  const bom = opts.utf8Bom ? '﻿' : '';
  const lines = [CSV_COLUMNS.join(',')];
  for (const p of points) {
    lines.push(
      [
        csvEscape(p.ts),
        p.temperature == null ? '' : round2(p.temperature),
        p.humidity == null ? '' : round2(p.humidity),
        p.pressure == null ? '' : round2(p.pressure),
      ].join(',')
    );
  }
  return bom + lines.join('\r\n') + '\r\n';
}

export function exportFilename(deviceNameOrId, fromIso, toIso) {
  const safe = (s) => String(s).replace(/[^a-zA-Z0-9._-]+/g, '_').slice(0, 64);
  return `thp_${safe(deviceNameOrId)}_${safe(fromIso)}_${safe(toIso)}.csv`;
}

export function csvResponse(body, filename) {
  return new Response(body, {
    status: 200,
    headers: {
      'content-type': 'text/csv; charset=utf-8',
      'content-disposition': `attachment; filename="${filename}"`,
      'cache-control': 'no-store',
    },
  });
}

export { CSV_COLUMNS };

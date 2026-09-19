/**
 * Automatic downsampling ladder (REQUIREMENTS.md §7.2)
 * Human APIs pick granularity from range length; raw 5-min data stays in D1 forever.
 */

export const GRANULARITIES = [
  { id: 'raw', label: '原始 5 分钟', maxHours: 24, seconds: 300, sql: null },
  { id: 'm15', label: '15 分钟', maxHours: 24 * 7, seconds: 15 * 60, sql: '15 minutes' },
  { id: 'm30', label: '30 分钟', maxHours: 24 * 7, seconds: 30 * 60, sql: '30 minutes' },
  { id: 'h1', label: '1 小时', maxHours: 24 * 30, seconds: 3600, sql: '1 hour' },
  { id: 'h2', label: '2 小时', maxHours: 24 * 30, seconds: 2 * 3600, sql: '2 hours' },
  { id: 'h6', label: '6 小时', maxHours: 24 * 90, seconds: 6 * 3600, sql: '6 hours' },
  { id: 'd1', label: '1 天', maxHours: 24 * 365, seconds: 86400, sql: '1 day' },
  { id: 'w1', label: '1 周', maxHours: Infinity, seconds: 7 * 86400, sql: '7 days' },
];

/**
 * @param {number} fromMs
 * @param {number} toMs
 * @returns {{ id: string, label: string, seconds: number, sql: string|null, hours: number }}
 */
export function pickGranularity(fromMs, toMs) {
  const hours = Math.max((toMs - fromMs) / 3600000, 0);
  let chosen = GRANULARITIES[0];
  if (hours > 24) chosen = { id: 'm30', label: '30 分钟', seconds: 30 * 60, sql: '30 minutes' };
  if (hours > 24 * 7) chosen = { id: 'h2', label: '2 小时', seconds: 2 * 3600, sql: '2 hours' };
  if (hours > 24 * 30) chosen = { id: 'h6', label: '6 小时', seconds: 6 * 3600, sql: '6 hours' };
  if (hours > 24 * 90) chosen = { id: 'd1', label: '1 天', seconds: 86400, sql: '1 day' };
  if (hours > 24 * 365) chosen = { id: 'w1', label: '1 周', seconds: 7 * 86400, sql: '7 days' };
  return { ...chosen, hours };
}

export const MAX_EXPORT_ROWS = 50000;

/**
 * Bucket ISO timestamp down to granularity start (UTC).
 * @param {string} ts ISO string
 * @param {number} bucketSeconds
 */
export function bucketStartIso(ts, bucketSeconds) {
  if (!bucketSeconds || bucketSeconds <= 0) return ts;
  const ms = Date.parse(ts);
  if (!Number.isFinite(ms)) return ts;
  const bucketMs = bucketSeconds * 1000;
  return new Date(Math.floor(ms / bucketMs) * bucketMs).toISOString();
}

/**
 * Fallback JS aggregation when SQL dialect helpers are unavailable.
 * @param {Array<{ts:string, temperature:number, humidity:number, pressure:number}>} rows
 * @param {number} bucketSeconds
 */
export function aggregateRows(rows, bucketSeconds) {
  if (!bucketSeconds || bucketSeconds <= 0) return rows.slice();
  const map = new Map();
  for (const r of rows) {
    const key = bucketStartIso(r.ts, bucketSeconds);
    let acc = map.get(key);
    if (!acc) {
      acc = { ts: key, temperature: 0, humidity: 0, pressure: 0, n: 0 };
      map.set(key, acc);
    }
    acc.temperature += r.temperature;
    acc.humidity += r.humidity;
    acc.pressure += r.pressure;
    acc.n += 1;
  }
  return [...map.values()]
    .sort((a, b) => a.ts.localeCompare(b.ts))
    .map((a) => ({
      ts: a.ts,
      temperature: round3(a.temperature / a.n),
      humidity: round3(a.humidity / a.n),
      pressure: round3(a.pressure / a.n),
    }));
}

export function round3(n) {
  return Math.round(n * 1000) / 1000;
}

export function round2(n) {
  return Math.round(n * 100) / 100;
}

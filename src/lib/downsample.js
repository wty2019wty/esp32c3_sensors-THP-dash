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

/** Max raw rows safe to pull into Worker memory for JS aggregation fallback. */
export const SAFE_RAW_LOAD_ROWS = 20000;

const BY_SECONDS = [...GRANULARITIES].sort((a, b) => a.seconds - b.seconds);

export function findGranularity(id) {
  return GRANULARITIES.find((g) => g.id === id) || null;
}

/** Next coarser step on the ladder (higher bucket seconds), or null if already coarsest. */
export function nextCoarserGranularity(current) {
  const curSec = Number(current?.seconds) || 0;
  return BY_SECONDS.find((g) => g.seconds > curSec) || null;
}

/** Rough upper bound of buckets for a time span (inclusive + edge slack). */
export function estimateBucketCount(fromMs, toMs, granSeconds) {
  if (!granSeconds || granSeconds <= 0) return Infinity;
  const span = Math.max(toMs - fromMs, 0);
  return Math.floor(span / (granSeconds * 1000)) + 3;
}

/**
 * Pick a granularity whose estimated output rows fit maxRows.
 * Walks the ladder coarser when needed; returns the coarsest step if still over.
 */
export function pickGranularityForLimit(fromMs, toMs, rawCount, maxRows = MAX_EXPORT_ROWS) {
  let gran = pickGranularity(fromMs, toMs);
  const limit = maxRows == null ? Infinity : maxRows;
  for (;;) {
    const est = gran.sql
      ? Math.min(rawCount, estimateBucketCount(fromMs, toMs, gran.seconds))
      : rawCount;
    if (limit === Infinity || est <= limit) return gran;
    const coarser = nextCoarserGranularity(gran);
    if (!coarser) return gran;
    gran = coarser;
  }
}

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
 * JS 降采样；字段可为 null（部分传感器上报时缺省）。
 * @param {Array<{ts:string, temperature?:number|null, humidity?:number|null, pressure?:number|null}>} rows
 * @param {number} bucketSeconds
 */
export function aggregateRows(rows, bucketSeconds) {
  if (!bucketSeconds || bucketSeconds <= 0) return rows.slice();
  const map = new Map();
  for (const r of rows) {
    const key = bucketStartIso(r.ts, bucketSeconds);
    let acc = map.get(key);
    if (!acc) {
      acc = {
        ts: key,
        temperature: 0,
        humidity: 0,
        pressure: 0,
        nT: 0,
        nH: 0,
        nP: 0,
      };
      map.set(key, acc);
    }
    if (r.temperature != null && Number.isFinite(Number(r.temperature))) {
      acc.temperature += Number(r.temperature);
      acc.nT += 1;
    }
    if (r.humidity != null && Number.isFinite(Number(r.humidity))) {
      acc.humidity += Number(r.humidity);
      acc.nH += 1;
    }
    if (r.pressure != null && Number.isFinite(Number(r.pressure))) {
      acc.pressure += Number(r.pressure);
      acc.nP += 1;
    }
  }
  return [...map.values()]
    .sort((a, b) => a.ts.localeCompare(b.ts))
    .map((a) => ({
      ts: a.ts,
      temperature: a.nT ? round3(a.temperature / a.nT) : null,
      humidity: a.nH ? round3(a.humidity / a.nH) : null,
      pressure: a.nP ? round3(a.pressure / a.nP) : null,
    }));
}

export function round3(n) {
  return Math.round(n * 1000) / 1000;
}

export function round2(n) {
  return Math.round(n * 100) / 100;
}

/**
 * SQL expression that floors an ISO timestamp column to bucket start (UTC).
 * Assumes stored ts format: YYYY-MM-DDTHH:MM:SS[.sss]Z
 * @param {number} seconds bucket size in seconds
 * @param {string} col column name
 */
export function sqlBucketStartExpr(seconds, col = 'ts') {
  const s = Number(seconds) || 0;
  if (s <= 0) return col;
  const c = col;
  const hourExpr = `CAST(substr(${c}, 12, 2) AS INTEGER)`;
  const minuteExpr = `CAST(substr(${c}, 15, 2) AS INTEGER)`;
  // substr(ts,1,11) = 'YYYY-MM-DDT'  — hour follows, then ':00:00.000Z'
  // substr(ts,1,14) = 'YYYY-MM-DDTHH:' — minute follows, then ':00.000Z'
  const dateT = `substr(${c}, 1, 11)`;
  const dateHourColon = `substr(${c}, 1, 14)`;

  if (s === 30 * 60) {
    return `printf('%s%02d:00.000Z', ${dateHourColon}, CASE WHEN ${minuteExpr} < 30 THEN 0 ELSE 30 END)`;
  }
  if (s === 3600) {
    return `printf('%s%02d:00:00.000Z', ${dateT}, ${hourExpr})`;
  }
  if (s === 2 * 3600) {
    return `printf('%s%02d:00:00.000Z', ${dateT}, ${hourExpr} / 2 * 2)`;
  }
  if (s === 6 * 3600) {
    return `printf('%s%02d:00:00.000Z', ${dateT}, ${hourExpr} / 6 * 6)`;
  }
  if (s === 86400) {
    return `substr(${c}, 1, 10) || 'T00:00:00.000Z'`;
  }
  // Week / generic: floor epoch seconds to bucket boundary
  const epoch = `CAST(strftime('%s', substr(${c}, 1, 19)) AS INTEGER)`;
  return `strftime('%Y-%m-%dT%H:%M:%S.000Z', ${epoch} / ${s} * ${s}, 'unixepoch')`;
}

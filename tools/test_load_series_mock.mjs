/**
 * Mock-D1 checks for loadSeries export safety (no wrangler required).
 * Run: node tools/test_load_series_mock.mjs
 */
import { loadSeries } from '../src/routes/readings.js';
import { SAFE_RAW_LOAD_ROWS, MAX_EXPORT_ROWS } from '../src/lib/downsample.js';

let failed = 0;
function assert(cond, msg) {
  if (cond) console.log(`  ok  ${msg}`);
  else {
    failed += 1;
    console.error(`  FAIL ${msg}`);
  }
}

function makeDb({ rawCount, sqlFails = false, aggPoints = [], rawRows = [] }) {
  const calls = { count: 0, rawSelect: 0, agg: 0 };
  return {
    calls,
    prepare(sql) {
      const s = String(sql);
      if (/COUNT\(\*\)/i.test(s)) {
        return {
          bind() {
            return {
              async first() {
                calls.count += 1;
                return { c: rawCount };
              },
            };
          },
        };
      }
      if (/AVG\(temperature\)/i.test(s)) {
        return {
          bind() {
            return {
              async all() {
                calls.agg += 1;
                if (sqlFails) throw new Error('sql dialect unsupported');
                return { results: aggPoints };
              },
            };
          },
        };
      }
      if (/SELECT ts, temperature, humidity, pressure/i.test(s)) {
        return {
          bind() {
            return {
              async all() {
                calls.rawSelect += 1;
                return { results: rawRows };
              },
            };
          },
        };
      }
      throw new Error('unexpected sql: ' + s.slice(0, 80));
    },
  };
}

const hour = 3600 * 1000;
const now = Date.parse('2026-01-15T12:00:00.000Z');
const iso = (ms) => new Date(ms).toISOString();

console.log('export path: SQL ok, long range');
{
  const db = makeDb({
    rawCount: 105120,
    aggPoints: Array.from({ length: 365 }, (_, i) => ({
      ts: iso(now - i * 24 * hour),
      temperature: 20,
      humidity: 50,
      pressure: 1013,
    })),
  });
  const r = await loadSeries({ DB: db }, 'dev', iso(now - 365 * 24 * hour), iso(now), {
    maxOutputRows: MAX_EXPORT_ROWS,
    autoCoarsen: true,
  });
  assert(!r.loadError, 'no loadError');
  assert(r.points.length === 365, '365 downsampled points');
  assert(db.calls.rawSelect === 0, 'did not pull raw rows');
  assert(db.calls.count === 1, 'counted first');
}

console.log('export path: SQL fails + huge raw → coarsen SQL, no raw pull');
{
  // First SQL attempts fail until we succeed on a later gran — simulate always fail then...
  // Actually all SQL fails → should try coarser SQL, never rawSelect when rawCount huge.
  const db = makeDb({ rawCount: SAFE_RAW_LOAD_ROWS + 5000, sqlFails: true, aggPoints: [] });
  const r = await loadSeries({ DB: db }, 'dev', iso(now - 30 * 24 * hour), iso(now), {
    maxOutputRows: MAX_EXPORT_ROWS,
    autoCoarsen: true,
  });
  assert(!!r.loadError, 'returns loadError when all SQL fail and raw too big');
  assert(r.loadError.status === 503, 'status 503');
  assert(db.calls.rawSelect === 0, 'never .all() raw when over SAFE_RAW_LOAD_ROWS');
  assert(db.calls.agg >= 2, 'tried multiple SQL granularities');
}

console.log('export path: SQL fails + small raw → JS aggregate fallback');
{
  const rawRows = Array.from({ length: 48 }, (_, i) => ({
    ts: iso(now - (47 - i) * 30 * 60 * 1000),
    temperature: 21,
    humidity: 40,
    pressure: 1000,
  }));
  const db = makeDb({ rawCount: 48, sqlFails: true, rawRows });
  const r = await loadSeries({ DB: db }, 'dev', iso(now - 24 * hour), iso(now), {
    maxOutputRows: MAX_EXPORT_ROWS,
    autoCoarsen: true,
  });
  assert(!r.loadError, 'small raw fallback ok');
  assert(db.calls.rawSelect === 1, 'pulled raw once');
  assert(r.points.length > 0 && r.points.length <= 48, 'aggregated points');
}

console.log('export path: raw flood in 24h → auto-coarsen off raw');
{
  const db = makeDb({ rawCount: 60000, aggPoints: [{ ts: iso(now), temperature: 1, humidity: 2, pressure: 3 }] });
  const r = await loadSeries({ DB: db }, 'dev', iso(now - 20 * hour), iso(now), {
    maxOutputRows: MAX_EXPORT_ROWS,
    autoCoarsen: true,
  });
  assert(!r.loadError, 'coarsened path ok');
  assert(r.gran.sql !== null, `gran is SQL not raw (${r.gran.id})`);
  assert(db.calls.rawSelect === 0, 'did not load 60k raw rows');
}

console.log('export path: raw flood + autoCoarsen false → 413');
{
  const db = makeDb({ rawCount: 60000 });
  const r = await loadSeries({ DB: db }, 'dev', iso(now - 20 * hour), iso(now), {
    maxOutputRows: MAX_EXPORT_ROWS,
    autoCoarsen: false,
  });
  assert(r.loadError?.status === 413, '413 when not auto-coarsen');
  assert(db.calls.rawSelect === 0, 'no raw load on reject');
}

console.log(failed === 0 ? '\nAll loadSeries mock checks passed.' : `\n${failed} check(s) failed.`);
process.exit(failed === 0 ? 0 : 1);

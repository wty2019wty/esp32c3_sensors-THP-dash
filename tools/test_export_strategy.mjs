/**
 * Pure-function checks for export/downsample strategy (no D1 required).
 * Run: node tools/test_export_strategy.mjs
 */
import {
  GRANULARITIES,
  MAX_EXPORT_ROWS,
  SAFE_RAW_LOAD_ROWS,
  nextCoarserGranularity,
  estimateBucketCount,
  pickGranularity,
  pickGranularityForLimit,
} from '../src/lib/downsample.js';

let failed = 0;
function assert(cond, msg) {
  if (cond) {
    console.log(`  ok  ${msg}`);
  } else {
    failed += 1;
    console.error(`  FAIL ${msg}`);
  }
}

const hour = 3600 * 1000;
const now = Date.parse('2026-01-15T12:00:00.000Z');

console.log('nextCoarserGranularity');
assert(nextCoarserGranularity({ id: 'raw', seconds: 300 })?.id === 'm15' || nextCoarserGranularity({ id: 'raw', seconds: 300 })?.seconds === 900, 'raw → coarser exists');
assert(nextCoarserGranularity(GRANULARITIES[GRANULARITIES.length - 1]) === null, 'coarsest → null');

console.log('estimateBucketCount');
assert(estimateBucketCount(now - 24 * hour, now, 300) >= 288, '1d @5min ≥288');
assert(estimateBucketCount(now - 365 * 24 * hour, now, 86400) <= 370, '1y @1d ~365');

console.log('pickGranularity range defaults');
assert(pickGranularity(now - 6 * hour, now).id === 'raw', '6h → raw');
assert(pickGranularity(now - 48 * hour, now).seconds >= 30 * 60, '>24h → ≥30min');
assert(pickGranularity(now - 400 * 24 * hour, now).seconds >= 86400, '>1y → ≥1d');

console.log('pickGranularityForLimit');
// 1 year of 5-min data (~105k) must not stay on raw when export cap is 50k
const yFrom = now - 365 * 24 * hour;
const yGran = pickGranularityForLimit(yFrom, now, 105120, MAX_EXPORT_ROWS);
assert(yGran.sql !== null, '1y dense → SQL gran');
assert(yGran.seconds >= 86400, '1y dense → ≥1d (fits under 50k)');
// Normal 5-min year (~105k raw) with 1d buckets → ~365 points, under cap
assert(
  Math.min(105120, estimateBucketCount(yFrom, now, 86400)) <= MAX_EXPORT_ROWS,
  '1d buckets for 1y under export cap'
);
// Raw path with tiny data stays raw
assert(pickGranularityForLimit(now - 3 * hour, now, 24, MAX_EXPORT_ROWS).id === 'raw', 'sparse 3h stays raw');
// Raw path with flood → coarsen
const flood = pickGranularityForLimit(now - 20 * hour, now, 60000, MAX_EXPORT_ROWS);
assert(flood.sql !== null, '20h flood 60k → coarsened off raw');
assert(SAFE_RAW_LOAD_ROWS < MAX_EXPORT_ROWS, 'safe raw load < export cap');

console.log(failed === 0 ? '\nAll export-strategy checks passed.' : `\n${failed} check(s) failed.`);
process.exit(failed === 0 ? 0 : 1);

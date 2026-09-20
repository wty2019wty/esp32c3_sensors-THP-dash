/**
 * Partial payload: only T/H or only P. Run: node tools/test_partial_readings.mjs
 * Uses the real Worker validation module (src/lib/reading_payload.js).
 */
import { aggregateRows } from '../src/lib/downsample.js';
import { validateReadingPayload } from '../src/lib/reading_payload.js';

let failed = 0;
function assert(cond, msg) {
  if (cond) console.log(`  ok  ${msg}`);
  else {
    failed += 1;
    console.error(`  FAIL ${msg}`);
  }
}

console.log('validateReadingPayload partial');
{
  const thOnly = validateReadingPayload({ temperature: 23.5, humidity: 48, rssi: -50 });
  assert(
    !thOnly.error && thOnly.values.temperature === 23.5 && thOnly.values.pressure === null,
    '仅温湿度合法'
  );
  const pOnly = validateReadingPayload({ pressure: 1013.2 });
  assert(
    !pOnly.error && pOnly.values.pressure === 1013.2 && pOnly.values.temperature === null,
    '仅气压合法'
  );
  const full = validateReadingPayload({ temperature: 20, humidity: 50, pressure: 1000 });
  assert(!full.error && full.values.pressure === 1000, '完整三字段仍合法');
  assert(!!validateReadingPayload({}).error, '空载荷拒绝');
  assert(!!validateReadingPayload({ temperature: 20 }).error, '仅温度（无湿度）拒绝');
  assert(
    !!validateReadingPayload({ temperature: 20, humidity: 50, pressure: 10 }).error,
    '越界气压拒绝'
  );
  const zeroTh = validateReadingPayload({ temperature: 0, humidity: 0 });
  assert(
    !zeroTh.error && zeroTh.values.temperature === 0 && zeroTh.values.humidity === 0,
    '合法 0 温湿度可上报'
  );
}

console.log('validateReadingPayload rejects Number()-coercion traps');
{
  assert(!!validateReadingPayload({ temperature: '', humidity: '' }).error, '空串温湿度拒绝');
  assert(!!validateReadingPayload({ pressure: '' }).error, '空串气压拒绝');
  assert(!!validateReadingPayload({ temperature: true, humidity: true }).error, '布尔温湿度拒绝');
  assert(!!validateReadingPayload({ pressure: false }).error, '布尔气压拒绝');
  assert(!!validateReadingPayload({ temperature: [20], humidity: [50] }).error, '数组温湿度拒绝');
  assert(!!validateReadingPayload({ pressure: {} }).error, '对象气压拒绝');
  assert(
    !!validateReadingPayload({ temperature: '  ', humidity: 50 }).error,
    '空白字符串温度拒绝'
  );
  const strOk = validateReadingPayload({ temperature: '20.5', humidity: '48', pressure: '1013.25' });
  assert(
    !strOk.error &&
      strOk.values.temperature === 20.5 &&
      strOk.values.humidity === 48 &&
      strOk.values.pressure === 1013.25,
    '合法数字字符串仍接受'
  );
}

console.log('aggregateRows null-safe');
{
  const rows = [
    { ts: '2026-01-01T00:00:00.000Z', temperature: 20, humidity: 40, pressure: null },
    { ts: '2026-01-01T00:01:00.000Z', temperature: 22, humidity: 44, pressure: null },
    { ts: '2026-01-01T00:02:00.000Z', temperature: null, humidity: null, pressure: 1012 },
    { ts: '2026-01-01T00:03:00.000Z', temperature: null, humidity: null, pressure: 1014 },
  ];
  const out = aggregateRows(rows, 300);
  assert(out.length === 1, '合成一个 bucket');
  assert(out[0].temperature === 21, '温度仅对有效点平均');
  assert(out[0].pressure === 1013, '气压仅对有效点平均');
  const pOnlyBucket = aggregateRows(
    [
      { ts: '2026-01-01T01:00:00.000Z', temperature: null, humidity: null, pressure: 1000 },
    ],
    300
  );
  assert(
    pOnlyBucket[0].temperature === null && pOnlyBucket[0].pressure === 1000,
    '仅气压 bucket 温湿度为 null'
  );
}

if (failed) {
  console.error(`\n${failed} failed`);
  process.exit(1);
}
console.log('\nall ok');

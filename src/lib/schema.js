/** Idempotent D1 bootstrap DDL — keep in sync with schema.sql */
export const SCHEMA_SQL = `
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS users (
  id            TEXT PRIMARY KEY,
  username      TEXT NOT NULL UNIQUE,
  password_hash TEXT NOT NULL,
  role          TEXT NOT NULL DEFAULT 'admin' CHECK (role IN ('admin', 'user')),
  created_at    TEXT NOT NULL,
  disabled_at   TEXT
);

CREATE TABLE IF NOT EXISTS sessions (
  id          TEXT PRIMARY KEY,
  user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  secret_hash TEXT NOT NULL UNIQUE,
  created_at  TEXT NOT NULL,
  expires_at  TEXT NOT NULL,
  last_used_at TEXT NOT NULL,
  csrf_hash   TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_at);

CREATE TABLE IF NOT EXISTS devices (
  id            TEXT PRIMARY KEY,
  name          TEXT NOT NULL,
  status        TEXT NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'disabled')),
  created_at    TEXT NOT NULL,
  last_seen_at  TEXT
);

CREATE TABLE IF NOT EXISTS api_tokens (
  id           TEXT PRIMARY KEY,
  device_id    TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
  name         TEXT NOT NULL DEFAULT '',
  token_hash   TEXT NOT NULL UNIQUE,
  created_at   TEXT NOT NULL,
  revoked_at   TEXT,
  last_used_at TEXT
);
CREATE INDEX IF NOT EXISTS idx_api_tokens_device ON api_tokens(device_id);
CREATE INDEX IF NOT EXISTS idx_api_tokens_hash ON api_tokens(token_hash);

CREATE TABLE IF NOT EXISTS readings (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id    TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
  ts           TEXT NOT NULL,
  temperature  REAL,
  humidity     REAL,
  pressure     REAL,
  measured_at  TEXT,
  rssi         INTEGER
);
CREATE INDEX IF NOT EXISTS idx_readings_device_ts ON readings(device_id, ts);
`;

function splitStatements(sql) {
  return sql
    .split(/;\s*$/gm)
    .map((s) => s.trim())
    .filter((s) => s && !s.startsWith('--'));
}

/** Create tables/indexes if missing. Safe to run repeatedly. */
export async function ensureSchema(env) {
  if (!env?.DB) throw new Error('D1 binding DB 未配置');
  const statements = splitStatements(SCHEMA_SQL);
  for (const stmt of statements) {
    // PRAGMA cannot run as prepare in some D1 versions — skip if needed
    const text = stmt.replace(/^PRAGMA[^;]*;?/i, '').trim();
    if (!text) continue;
    await env.DB.prepare(text).run();
  }
  await migrateReadingsNullable(env);
  return true;
}

/**
 * 旧库 readings 三列可能仍为 NOT NULL；重建为可空以支持部分字段上报。
 * 用 sqlite_master 判断，避免依赖 D1 上不稳定的 PRAGMA。
 * @returns {Promise<boolean>} 是否执行了迁移
 */
export async function migrateReadingsNullable(env) {
  const row = await env.DB.prepare(
    `SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'readings'`
  ).first();
  const ddl = String(row?.sql || '');
  if (!ddl) return false;
  const metricCols = ['temperature', 'humidity', 'pressure'];
  const needs = metricCols.some((name) => {
    const re = new RegExp(`${name}\\s+REAL\\s+NOT\\s+NULL`, 'i');
    return re.test(ddl);
  });
  if (!needs) return false;

  // PRAGMA in D1 batch is unreliable — run outside batch and ignore failures.
  try {
    await env.DB.prepare(`PRAGMA foreign_keys=OFF`).run();
  } catch {
    /* ignore */
  }

  await env.DB.batch([
    env.DB.prepare(`
      CREATE TABLE IF NOT EXISTS readings_mig (
        id           INTEGER PRIMARY KEY AUTOINCREMENT,
        device_id    TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
        ts           TEXT NOT NULL,
        temperature  REAL,
        humidity     REAL,
        pressure     REAL,
        measured_at  TEXT,
        rssi         INTEGER
      )
    `),
    env.DB.prepare(`
      INSERT INTO readings_mig (id, device_id, ts, temperature, humidity, pressure, measured_at, rssi)
      SELECT id, device_id, ts, temperature, humidity, pressure, measured_at, rssi FROM readings
    `),
    env.DB.prepare(`DROP TABLE IF EXISTS readings`),
    env.DB.prepare(`ALTER TABLE readings_mig RENAME TO readings`),
    env.DB.prepare(`CREATE INDEX IF NOT EXISTS idx_readings_device_ts ON readings(device_id, ts)`),
  ]);

  try {
    await env.DB.prepare(`PRAGMA foreign_keys=ON`).run();
  } catch {
    /* ignore */
  }
  return true;
}

export function isMissingTableError(err) {
  const msg = String(err?.message || err?.cause?.message || err || '');
  return /no such table/i.test(msg);
}

/** Returns user count; auto-creates schema when tables are missing (local DX). */
export async function ensureSchemaAndCountUsers(env) {
  try {
    const row = await env.DB.prepare(`SELECT COUNT(*) AS c FROM users`).first();
    await migrateReadingsNullable(env).catch(() => {});
    return Number(row?.c || 0);
  } catch (err) {
    if (!isMissingTableError(err)) throw err;
    await ensureSchema(env);
    const row = await env.DB.prepare(`SELECT COUNT(*) AS c FROM users`).first();
    return Number(row?.c || 0);
  }
}

-- THP Dash D1 schema
-- Apply: wrangler d1 execute thp-dash --file=./schema.sql --remote

PRAGMA foreign_keys = ON;

-- ---------------------------------------------------------------------------
-- Users (self-managed login; NOT Cloudflare Access)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS users (
  id            TEXT PRIMARY KEY,
  username      TEXT NOT NULL UNIQUE,
  password_hash TEXT NOT NULL,
  role          TEXT NOT NULL DEFAULT 'admin' CHECK (role IN ('admin', 'user')),
  created_at    TEXT NOT NULL,
  disabled_at   TEXT
);

-- Sessions store only a hash of the session secret.
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

-- ---------------------------------------------------------------------------
-- Devices + device API tokens (POST /readings only)
-- ---------------------------------------------------------------------------
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

-- ---------------------------------------------------------------------------
-- Raw readings — permanent retention (no TTL)
-- T/H from SHT40, P from BMP280.
-- Metrics are nullable: device may report only T/H or only P.
-- Index for range queries.
-- ---------------------------------------------------------------------------
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

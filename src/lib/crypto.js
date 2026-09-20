/** Hex-encode ArrayBuffer / Uint8Array */
export function toHex(buf) {
  const bytes = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  let s = '';
  for (let i = 0; i < bytes.length; i++) s += bytes[i].toString(16).padStart(2, '0');
  return s;
}

/** Cryptographically random bytes as hex */
export function randomHex(nBytes = 32) {
  const b = crypto.getRandomValues(new Uint8Array(nBytes));
  return toHex(b);
}

export function randomId(prefix = '') {
  return prefix ? `${prefix}_${randomHex(12)}` : randomHex(12);
}

async function sha256Hex(input) {
  const data = typeof input === 'string' ? new TextEncoder().encode(input) : input;
  const digest = await crypto.subtle.digest('SHA-256', data);
  return toHex(digest);
}

export { sha256Hex };

/** Token / session lookup: we only ever compare hashes. */
export async function hashToken(plain) {
  return sha256Hex(`thp_token:v1:${plain}`);
}

export async function hashSessionSecret(plain) {
  return sha256Hex(`thp_session:v1:${plain}`);
}

export async function hashCsrf(plain) {
  return sha256Hex(`thp_csrf:v1:${plain}`);
}

/**
 * Password hashing via PBKDF2-SHA256 (Web Crypto, no external deps).
 * Format: pbkdf2$sha256$iterations$salt_hex$hash_hex
 */
// Workers WebCrypto caps PBKDF2 at 100000 iterations
export const PBKDF2_ITERATIONS = 100000;

export async function hashPassword(password) {
  const iterations = PBKDF2_ITERATIONS;
  const salt = crypto.getRandomValues(new Uint8Array(16));
  const keyMaterial = await crypto.subtle.importKey(
    'raw',
    new TextEncoder().encode(password),
    'PBKDF2',
    false,
    ['deriveBits']
  );
  const bits = await crypto.subtle.deriveBits(
    {
      name: 'PBKDF2',
      hash: 'SHA-256',
      salt,
      iterations,
    },
    keyMaterial,
    256
  );
  return `pbkdf2$sha256$${iterations}$${toHex(salt)}$${toHex(bits)}`;
}

export async function verifyPassword(password, stored) {
  try {
    const parts = String(stored || '').split('$');
    if (parts.length !== 5 || parts[0] !== 'pbkdf2') return false;
    const hashName = parts[1];
    const iterations = Number(parts[2]);
    const saltHex = parts[3];
    const expectedHex = parts[4];
    if (hashName !== 'sha256' || !Number.isFinite(iterations) || iterations < 10000) {
      return false;
    }
    const salt = hexToBytes(saltHex);
    const keyMaterial = await crypto.subtle.importKey(
      'raw',
      new TextEncoder().encode(password),
      'PBKDF2',
      false,
      ['deriveBits']
    );
    const bits = await crypto.subtle.deriveBits(
      { name: 'PBKDF2', hash: 'SHA-256', salt, iterations },
      keyMaterial,
      256
    );
    return timingSafeEqualHex(toHex(bits), expectedHex);
  } catch {
    return false;
  }
}

export function hexToBytes(hex) {
  const clean = String(hex || '');
  if (clean.length % 2 !== 0) throw new Error('bad hex');
  const out = new Uint8Array(clean.length / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(clean.substr(i * 2, 2), 16);
    if (Number.isNaN(out[i])) throw new Error('bad hex');
  }
  return out;
}

/** Constant-time hex compare */
export function timingSafeEqualHex(a, b) {
  const sa = String(a || '');
  const sb = String(b || '');
  if (sa.length !== sb.length) return false;
  let diff = 0;
  for (let i = 0; i < sa.length; i++) {
    diff |= sa.charCodeAt(i) ^ sb.charCodeAt(i);
  }
  return diff === 0;
}

export async function constantTimeEqualHash(plain, storedHash, hasher = hashToken) {
  const h = await hasher(plain);
  return timingSafeEqualHex(h, storedHash);
}

/** Full device API token shown once: thp_<idpart>_<secret> */
export function issueDeviceToken() {
  const secret = randomHex(32);
  return { plain: `thp_${secret}`, secret };
}

export function issueSessionCredentials() {
  const secret = randomHex(32);
  const csrf = randomHex(32);
  return { secret, csrf };
}

export function nowIso() {
  return new Date().toISOString();
}

export function isoFrom(date) {
  return date.toISOString();
}

export function addDaysIso(days, from = new Date()) {
  const d = new Date(from.getTime());
  d.setUTCDate(d.getUTCDate() + days);
  return d.toISOString();
}

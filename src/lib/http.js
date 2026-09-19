const JSON_HEADERS = { 'content-type': 'application/json; charset=utf-8' };

export function json(data, status = 200, headers = {}) {
  return new Response(JSON.stringify(data), {
    status,
    headers: { ...JSON_HEADERS, ...headers },
  });
}

export function jsonError(message, status = 400, extra = {}) {
  return json({ error: message, ...extra }, status);
}

export function corsHeaders(req) {
  const origin = req.headers.get('origin') || '';
  // Same-origin Pages + Worker; allow configured origin later if split hosts.
  return {
    'access-control-allow-origin': origin || '*',
    'access-control-allow-credentials': 'true',
    'access-control-allow-headers': 'content-type, authorization, x-csrf-token',
    'access-control-allow-methods': 'GET,POST,DELETE,OPTIONS',
    vary: 'origin',
  };
}

export function withCors(req, res) {
  const headers = new Headers(res.headers);
  const cors = corsHeaders(req);
  for (const [k, v] of Object.entries(cors)) headers.set(k, v);
  return new Response(res.body, {
    status: res.status,
    statusText: res.statusText,
    headers,
  });
}

export function parseCookies(req) {
  const header = req.headers.get('cookie') || '';
  const out = {};
  for (const part of header.split(';')) {
    const idx = part.indexOf('=');
    if (idx === -1) continue;
    const k = part.slice(0, idx).trim();
    const v = part.slice(idx + 1).trim();
    if (!k) continue;
    out[k] = decodeURIComponent(v);
  }
  return out;
}

export function serializeCookie(name, value, opts = {}) {
  const parts = [`${name}=${encodeURIComponent(value)}`];
  if (opts.maxAge != null) parts.push(`Max-Age=${opts.maxAge}`);
  if (opts.expires) parts.push(`Expires=${opts.expires}`);
  parts.push(`Path=${opts.path || '/'}`);
  if (opts.httpOnly !== false) parts.push('HttpOnly');
  if (opts.secure !== false) parts.push('Secure');
  if (opts.sameSite) parts.push(`SameSite=${opts.sameSite}`);
  return parts.join('; ');
}

export async function readJson(req) {
  try {
    return await req.json();
  } catch {
    return null;
  }
}

export function getBearer(req) {
  const h = req.headers.get('authorization') || '';
  const m = /^Bearer\s+(.+)$/i.exec(h.trim());
  return m ? m[1].trim() : null;
}

export function getQuery(req) {
  return new URL(req.url).searchParams;
}

export function requireString(value, field, { min = 1, max = 256 } = {}) {
  if (typeof value !== 'string') return `${field} 必须是字符串`;
  const v = value.trim();
  if (v.length < min) return `${field} 不能为空`;
  if (v.length > max) return `${field} 过长`;
  return null;
}

export function isValidIso(s) {
  if (typeof s !== 'string' || !s) return false;
  const t = Date.parse(s);
  return Number.isFinite(t);
}

export function normalizeIso(s) {
  return new Date(s).toISOString();
}

export function localDayStartIso(timeZone, now = new Date()) {
  // Format date parts in the display timezone, then build UTC instant for local midnight.
  const fmt = new Intl.DateTimeFormat('en-CA', {
    timeZone,
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
  });
  const parts = Object.fromEntries(fmt.formatToParts(now).map((p) => [p.type, p.value]));
  const hour = parts.hour === '24' ? '0' : parts.hour;
  const asUtcLike = `${parts.year}-${parts.month}-${parts.day}T${hour}:${parts.minute}:${parts.second}Z`;
  const localWall = Date.parse(asUtcLike);
  // Offset = wall interpreted as UTC minus true UTC of "now" components — compute offset via another probe
  const probe = new Date(
    Date.UTC(
      Number(parts.year),
      Number(parts.month) - 1,
      Number(parts.day),
      Number(hour),
      Number(parts.minute),
      Number(parts.second)
    )
  );
  // true UTC for those wall components:
  // We need midnight local. Use offset between wall-as-UTC and a nearby true time.
  // Simpler approach: iterate — get offset from formatting now.
  const trueNowUtc = Date.UTC(
    now.getUTCFullYear(),
    now.getUTCMonth(),
    now.getUTCDate(),
    now.getUTCHours(),
    now.getUTCMinutes(),
    now.getUTCSeconds(),
    now.getUTCMilliseconds()
  );
  const offsetMs = probe.getTime() - trueNowUtc;
  const midnightWallUtc = Date.UTC(
    Number(parts.year),
    Number(parts.month) - 1,
    Number(parts.day),
    0,
    0,
    0,
    0
  );
  // midnight local = midnight wall - offset
  return new Date(midnightWallUtc - offsetMs).toISOString();
}

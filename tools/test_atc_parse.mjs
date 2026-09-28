/**
 * pvvx Custom / BTHome v2 明文加密解析与 ESP32 侧一致的宿主单测（无需硬件）
 * 运行：node tools/test_atc_parse.mjs
 *
 * 注：部分 Node 构建缺少 aes-128-ccm，这里用 AES-ECB 自建 RFC 3610 CCM。
 */
import { createCipheriv } from 'node:crypto';
import assert from 'node:assert/strict';

// ---- minimal AES-128-CCM via ECB (RFC 3610) ----
function aesEncryptBlock(key, block) {
  const c = createCipheriv('aes-128-ecb', key, null);
  c.setAutoPadding(false);
  return Buffer.concat([c.update(block), c.final()]);
}

function ccmAuthBlocks(key, nonce, aad, plain) {
  // B0 = flags || nonce || l(m); flags = 64*Adata + 8*((M-2)/2) + (L-1)
  // M=4 → (M-2)/2 = 1; L = 15-nonceLen = 2 for 13-byte nonce
  const L = 15 - nonce.length;
  const M = 4;
  let flags = (L - 1) | (((M - 2) / 2) << 3);
  if (aad && aad.length) flags |= 0x40;
  const B0 = Buffer.alloc(16);
  B0[0] = flags;
  nonce.copy(B0, 1);
  B0.writeUIntBE(plain.length, 16 - L, L);

  const blocks = [B0];
  if (aad && aad.length) {
    let off = 0;
    let first = Buffer.alloc(16);
    if (aad.length < 0xff00) {
      first.writeUInt16BE(aad.length, 0);
      off = 2;
    } else {
      first.writeUInt32BE(0xfffe0000, 0); // not used here
      first.writeUInt32BE(aad.length, 4);
      off = 6;
    }
    const n = Math.min(16 - off, aad.length);
    aad.copy(first, off, 0, n);
    blocks.push(first);
    let i = n;
    while (i < aad.length) {
      const b = Buffer.alloc(16);
      aad.copy(b, 0, i, Math.min(i + 16, aad.length));
      blocks.push(b);
      i += 16;
    }
  }
  for (let i = 0; i < plain.length; i += 16) {
    const b = Buffer.alloc(16);
    plain.copy(b, 0, i, Math.min(i + 16, plain.length));
    blocks.push(b);
  }
  let x = Buffer.alloc(16);
  for (const b of blocks) {
    const t = Buffer.alloc(16);
    for (let j = 0; j < 16; j++) t[j] = x[j] ^ b[j];
    x = aesEncryptBlock(key, t);
  }
  return x; // T before truncation
}

function ccmCtr(key, nonce, data) {
  const L = 15 - nonce.length;
  const out = Buffer.alloc(data.length);
  let ctr = Buffer.alloc(16);
  ctr[0] = L - 1;
  nonce.copy(ctr, 1);
  for (let i = 0; i < data.length; i += 16) {
    ctr.writeUIntBE(ctr.readUIntBE(16 - L, L) + 1, 16 - L, L);
    const s = aesEncryptBlock(key, ctr);
    for (let j = 0; j < 16 && i + j < data.length; j++) {
      out[i + j] = data[i + j] ^ s[j];
    }
  }
  return out;
}

function ccmEncrypt(key, nonce, aad, plain, tagLen = 4) {
  const cipher = ccmCtr(key, nonce, plain);
  const tagMask = aesEncryptBlock(key, (() => {
    const b = Buffer.alloc(16);
    b[0] = (15 - nonce.length) - 1;
    nonce.copy(b, 1);
    return b;
  })());
  const t = ccmAuthBlocks(key, nonce, aad, plain);
  const tag = Buffer.alloc(tagLen);
  for (let i = 0; i < tagLen; i++) tag[i] = t[i] ^ tagMask[i];
  return { cipher, tag };
}

function ccmDecrypt(key, nonce, aad, cipher, tag, tagLen = 4) {
  const plain = ccmCtr(key, nonce, cipher); // CTR is symmetric
  const tagMask = aesEncryptBlock(key, (() => {
    const b = Buffer.alloc(16);
    b[0] = (15 - nonce.length) - 1;
    nonce.copy(b, 1);
    return b;
  })());
  const t = ccmAuthBlocks(key, nonce, aad, plain);
  const expect = Buffer.alloc(tagLen);
  for (let i = 0; i < tagLen; i++) expect[i] = t[i] ^ tagMask[i];
  if (!expect.equals(tag)) return null;
  return plain;
}

function parseMacStr(s) {
  const hex = s.replace(/[^0-9A-Fa-f]/g, '');
  if (hex.length !== 12) return null;
  const out = Buffer.alloc(6);
  for (let i = 0; i < 6; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return out;
}

function parseClearPayload(afterUuid, advMac) {
  // after UUID: MAC6 + T2 + H2 + mV2 + batt1 + cnt1 + flags1 = 15
  if (afterUuid.length < 15) return null;
  // packet MAC is reversed (LSB first)
  const mac = Buffer.from(afterUuid.subarray(0, 6)).reverse();
  const t100 = afterUuid.readInt16LE(6);
  const h100 = afterUuid.readUInt16LE(8);
  const battMv = afterUuid.readUInt16LE(10);
  const battPct = afterUuid[12];
  const counter = afterUuid[13];
  const flags = afterUuid[14];
  return {
    temperature: t100 / 100,
    humidity: h100 / 100,
    battery_mv: battMv,
    battery_pct: battPct,
    adv_counter: counter,
    flags,
    mac,
  };
}

/** AtcMiCodec: nonce = mac_msb_first.reverse() + header4 + codec[0]; AAD=0x11 */
function parseEncryptedAd(ad, advMacMsb, bindkey) {
  if (ad.length < 4 + 1 + 6 + 4) return null;
  if (ad[0] < 7 || ad[1] !== 0x16 || ad[2] !== 0x1a || ad[3] !== 0x18) return null;
  const header = ad.subarray(0, 4); // size, 0x16, 0x1A, 0x18
  const codec = ad.subarray(4);
  const nonce = Buffer.concat([
    Buffer.from(advMacMsb).reverse(),
    header,
    Buffer.from([codec[0]]),
  ]);
  const cipherpayload = codec.subarray(1, codec.length - 4);
  const mic = codec.subarray(codec.length - 4);
  const plain = ccmDecrypt(bindkey, nonce, Buffer.from([0x11]), cipherpayload, mic);
  if (!plain || plain.length < 6) return null;
  return {
    temperature: plain.readInt16LE(0) / 100,
    humidity: plain.readUInt16LE(2) / 100,
    battery_pct: plain[4],
    flags: plain[5],
    battery_mv: 0,
  };
}

function encryptPvvxCustom({ temperature, humidity, battery_pct, flags }, advMacMsb, bindkey, codec0 = 0xbd) {
  const plain = Buffer.alloc(6);
  plain.writeInt16LE(Math.round(temperature * 100), 0);
  plain.writeUInt16LE(Math.round(humidity * 100), 2);
  plain[4] = battery_pct;
  plain[5] = flags;
  const header = Buffer.from([0x0e, 0x16, 0x1a, 0x18]);
  const nonce = Buffer.concat([Buffer.from(advMacMsb).reverse(), header, Buffer.from([codec0])]);
  const { cipher, tag } = ccmEncrypt(bindkey, nonce, Buffer.from([0x11]), plain);
  return Buffer.concat([header, Buffer.from([codec0]), cipher, tag]);
}

/** BTHome v2 object 流（与 ESP32 bthome_parse_objects 一致） */
function bthomeParseObjects(objs) {
  const out = {
    temperature: 0,
    humidity: 0,
    battery_mv: 0,
    battery_pct: 0xff,
    adv_counter: 0,
  };
  let hasT = false;
  let hasH = false;
  let off = 0;
  while (off < objs.length) {
    const id = objs[off++];
    let vsz;
    if (id === 0x00 || id === 0x01 || id === 0x2e) vsz = 1;
    else if (id === 0x02 || id === 0x03 || id === 0x0c) vsz = 2;
    else break;
    if (off + vsz > objs.length) break;
    const v = objs.subarray(off, off + vsz);
    if (id === 0x00) out.adv_counter = v[0];
    else if (id === 0x01) out.battery_pct = v[0];
    else if (id === 0x02) {
      out.temperature = v.readInt16LE(0) / 100;
      hasT = true;
    } else if (id === 0x03) {
      out.humidity = v.readUInt16LE(0) / 100;
      hasH = true;
    } else if (id === 0x2e) {
      out.humidity = v[0];
      hasH = true;
    } else if (id === 0x0c) out.battery_mv = v.readUInt16LE(0);
    off += vsz;
  }
  if (!hasT || !hasH) return null;
  if (out.temperature < -40 || out.temperature > 85) return null;
  if (out.humidity < 0 || out.humidity > 100) return null;
  return out;
}

function parseBthomeClear(afterUuid, advMac) {
  if (afterUuid.length < 1) return null;
  const info = afterUuid[0];
  if ((info & 0x01) !== 0) return null;
  if ((info & 0xe0) !== 0x40) return null;
  const objs = bthomeParseObjects(afterUuid.subarray(1));
  if (!objs) return null;
  return { ...objs, mac: Buffer.from(advMac) };
}

/**
 * BTHome v2 加密：nonce = adv_mac(MSB 显示序) + uuid_as_in_packet + device_info + counter(4)
 * 无 AAD；结构 after_uuid = device_info | cipher | counter(4 LE) | MIC(4)
 */
function parseBthomeEncrypted(ad, advMacMsb, bindkey) {
  if (ad.length < 4 + 1 + 4 + 4) return null;
  if (ad[1] !== 0x16 || ad[2] !== 0xd2 || ad[3] !== 0xfc) return null;
  const codec = ad.subarray(4);
  if (codec.length < 1 + 4 + 4) return null;
  const info = codec[0];
  if ((info & 0x01) === 0) return null;
  if ((info & 0xe0) !== 0x40) return null;
  const cipherLen = codec.length - 1 - 8;
  if (cipherLen < 2) return null;
  const cipher = codec.subarray(1, 1 + cipherLen);
  const counter = codec.subarray(1 + cipherLen, 1 + cipherLen + 4);
  const mic = codec.subarray(codec.length - 4);
  const nonce = Buffer.concat([
    Buffer.from(advMacMsb),
    Buffer.from([0xd2, 0xfc]),
    Buffer.from([info]),
    counter,
  ]);
  const plain = ccmDecrypt(bindkey, nonce, Buffer.alloc(0), cipher, mic);
  if (!plain) return null;
  const objs = bthomeParseObjects(plain);
  if (!objs) return null;
  return {
    ...objs,
    adv_counter: counter[0],
    mac: Buffer.from(advMacMsb),
  };
}

function encryptBthomeV2({ temperature, humidity, battery_pct, battery_mv, packet_id }, advMacMsb, bindkey, counter = Buffer.from([0x00, 0x22, 0x11, 0x00])) {
  const parts = [];
  if (packet_id !== undefined) {
    parts.push(Buffer.from([0x00, packet_id & 0xff]));
  }
  {
    const t = Buffer.alloc(3);
    t[0] = 0x02;
    t.writeInt16LE(Math.round(temperature * 100), 1);
    parts.push(t);
  }
  {
    const h = Buffer.alloc(3);
    h[0] = 0x03;
    h.writeUInt16LE(Math.round(humidity * 100), 1);
    parts.push(h);
  }
  if (battery_mv !== undefined) {
    const v = Buffer.alloc(3);
    v[0] = 0x0c;
    v.writeUInt16LE(battery_mv, 1);
    parts.push(v);
  }
  if (battery_pct !== undefined) {
    parts.push(Buffer.from([0x01, battery_pct & 0xff]));
  }
  const plain = Buffer.concat(parts);
  const info = 0x41;
  const nonce = Buffer.concat([
    Buffer.from(advMacMsb),
    Buffer.from([0xd2, 0xfc]),
    Buffer.from([info]),
    counter,
  ]);
  const { cipher: ct, tag: mic } = ccmEncrypt(bindkey, nonce, Buffer.alloc(0), plain);
  // AD: size, 0x16, D2, FC, device_info, cipher, counter, mic
  const body = Buffer.concat([
    Buffer.from([0x16, 0xd2, 0xfc, info]),
    ct,
    counter,
    mic,
  ]);
  return Buffer.concat([Buffer.from([body.length]), body]);
}

// --- clear sample from pvvx docs ---
// "12 16 1a 18 5c c8 ee 38 c1 a4 b0 08 c3 14 ca 0a 50 14 05"
// MAC packet 5c c8 ee 38 c1 a4 → A4:C1:38:EE:C8:5C; T=22.24 H=53.15 batt=2762mV 80%
{
  const ad = Buffer.from('12161a185cc8ee38c1a4b008c314ca0a501405', 'hex');
  const after = ad.subarray(4);
  const s = parseClearPayload(after);
  assert.ok(s);
  assert.equal(s.mac.toString('hex').toUpperCase(), 'A4C138EEC85C');
  assert.equal(s.temperature, 22.24);
  assert.equal(s.humidity, 53.15);
  assert.equal(s.battery_pct, 80);
  assert.equal(s.battery_mv, 2762);
  console.log('clear parse OK', s);
}

// --- encrypted roundtrip (AtcMiCodec) ---
{
  const mac = parseMacStr('A4:C1:38:AA:BB:CC');
  const key = parseMacStr('AA:AA:AA:AA:AA:AA'); // wrong length intentionally? no - use 16 bytes
  const bindkey = Buffer.alloc(16, 0xaa);
  const ad = encryptPvvxCustom(
    { temperature: 19.11, humidity: 50.0, battery_pct: 93, flags: 0x07 },
    mac,
    bindkey,
  );
  assert.equal(ad[0], 0x0e);
  const parsed = parseEncryptedAd(ad, mac, bindkey);
  assert.ok(parsed, 'encrypted parse failed');
  assert.equal(parsed.temperature, 19.11);
  assert.equal(parsed.humidity, 50);
  assert.equal(parsed.battery_pct, 93);
  console.log('encrypted roundtrip OK', parsed, 'ad=', ad.toString('hex'));
}

// --- BTHome v2 clear (from bthome.io/format) ---
// Service data 0A 16 D2FC 40 02C409 03BF13 → T=25.00 H=50.55
{
  const after = Buffer.from('4002c40903bf13', 'hex');
  const mac = parseMacStr('54:48:E6:8F:80:A5');
  const s = parseBthomeClear(after, mac);
  assert.ok(s, 'bthome clear parse failed');
  assert.equal(s.temperature, 25.0);
  assert.equal(s.humidity, 50.55);
  console.log('bthome clear OK', s);
}

// --- BTHome v2 clear with uint8 humidity 0x2E (from bthome.io/format) ---
// 40 02C409 2E23 → T=25.00 H=35
{
  const after = Buffer.from('4002c4092e23', 'hex');
  const mac = parseMacStr('54:48:E6:8F:80:A5');
  const s = parseBthomeClear(after, mac);
  assert.ok(s, 'bthome clear 0x2E parse failed');
  assert.equal(s.temperature, 25.0);
  assert.equal(s.humidity, 35);
  console.log('bthome clear 0x2E OK', s);
}

// --- BTHome v2 encrypted official vector (bthome.io/encryption/) ---
// key=231d39c1d7cc1ab1aee224cd096db932
// MAC=5448E68F80A5  device_info=41  counter=33221100
// plain=02ca0903bf13  cipher=e445f3c9962b  mic=6c7c4519
// service data=d2fc41e445f3c9962b332211006c7c4519
// nonce=5448e68f80a5d2fc4133221100
{
  const bindkey = Buffer.from('231d39c1d7cc1ab1aee224cd096db932', 'hex');
  const mac = Buffer.from('5448e68f80a5', 'hex');
  const service = Buffer.from('d2fc41e445f3c9962b332211006c7c4519', 'hex');
  // rebuild as full AD: size = 1(type) + service.length
  const ad = Buffer.concat([Buffer.from([1 + service.length]), Buffer.from([0x16]), service]);
  // expected nonce check
  {
    const counter = Buffer.from('33221100', 'hex');
    const nonce = Buffer.concat([
      mac,
      Buffer.from([0xd2, 0xfc, 0x41]),
      counter,
    ]);
    assert.equal(nonce.toString('hex'), '5448e68f80a5d2fc4133221100');
  }
  const parsed = parseBthomeEncrypted(ad, mac, bindkey);
  assert.ok(parsed, 'bthome encrypted official vector failed');
  assert.equal(parsed.temperature, 25.06);
  assert.equal(parsed.humidity, 50.55);
  console.log('bthome encrypted official vector OK', parsed, 'ad=', ad.toString('hex'));
}

// --- BTHome v2 encrypted roundtrip with T/H/batt ---
{
  const bindkey = Buffer.alloc(16, 0xbb);
  const mac = parseMacStr('A4:C1:38:AA:BB:CC');
  const ad = encryptBthomeV2(
    {
      temperature: 19.11,
      humidity: 50.0,
      battery_pct: 93,
      battery_mv: 2980,
      packet_id: 7,
    },
    mac,
    bindkey,
    Buffer.from([7, 0x00, 0x00, 0x00]),
  );
  const parsed = parseBthomeEncrypted(ad, mac, bindkey);
  assert.ok(parsed, 'bthome encrypted roundtrip failed');
  assert.equal(parsed.temperature, 19.11);
  assert.equal(parsed.humidity, 50);
  assert.equal(parsed.battery_pct, 93);
  assert.equal(parsed.battery_mv, 2980);
  assert.equal(parsed.adv_counter, 7);
  console.log('bthome encrypted roundtrip OK', parsed, 'ad=', ad.toString('hex'));
}

// --- MAC / key helpers ---
{
  const m = parseMacStr('A4:C1:38:E2:4E:43');
  assert.equal(m.toString('hex').toUpperCase(), 'A4C138E24E43');
  const m2 = parseMacStr('A4C138E24E43');
  assert.equal(m2.toString('hex').toUpperCase(), 'A4C138E24E43');
  console.log('mac parse OK');
}

console.log('test_atc_parse: all passed');

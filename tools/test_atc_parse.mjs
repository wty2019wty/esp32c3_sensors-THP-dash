/**
 * pvvx Custom 明文/加密解析与 ESP32 侧一致的宿主单测（无需硬件）
 * 运行：node tools/test_atc_parse.mjs
 */
import { createCipheriv, createDecipheriv } from 'node:crypto';
import assert from 'node:assert/strict';

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
  const decipher = createDecipheriv('aes-128-ccm', bindkey, nonce, { authTagLength: 4 });
  decipher.setAuthTag(mic);
  decipher.setAAD(Buffer.from([0x11]), { plaintextLength: cipherpayload.length });
  let plain;
  try {
    plain = Buffer.concat([decipher.update(cipherpayload), decipher.final()]);
  } catch {
    return null;
  }
  if (plain.length < 6) return null;
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
  const cipher = createCipheriv('aes-128-ccm', bindkey, nonce, { authTagLength: 4 });
  cipher.setAAD(Buffer.from([0x11]), { plaintextLength: plain.length });
  const ct = Buffer.concat([cipher.update(plain), cipher.final()]);
  const mic = cipher.getAuthTag();
  return Buffer.concat([header, Buffer.from([codec0]), ct, mic]);
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

// --- MAC / key helpers ---
{
  const m = parseMacStr('A4:C1:38:E2:4E:43');
  assert.equal(m.toString('hex').toUpperCase(), 'A4C138E24E43');
  const m2 = parseMacStr('A4C138E24E43');
  assert.equal(m2.toString('hex').toUpperCase(), 'A4C138E24E43');
  console.log('mac parse OK');
}

console.log('test_atc_parse: all passed');

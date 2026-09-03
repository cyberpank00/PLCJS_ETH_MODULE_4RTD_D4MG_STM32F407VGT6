#!/usr/bin/env node
/**
 * calibrate.mjs — dialog-driven calibration / configuration tool for the
 * PLCJS 4RTD analog input module (HW2.1, ADS1220) over Modbus TCP.
 *
 * Node.js 18+ only, no external npm packages (uses node:net / node:readline).
 *
 * The module measures a ratiometric resistance
 *      R_raw = code / 2^23 * 2*RREF_nom / gain_PGA
 * The RREF tolerance (±0.1 %) and the PGA gain error are not known a priori,
 * so each channel is calibrated per gain class with a simple linear model:
 *
 *      R_true = gain * R_raw + offset
 *
 * gain and offset are found by a least-squares fit over >= 2 reference points
 * applied with a precision resistance standard (e.g. АКИП-2202А, 0.05 %).
 *
 * Gain classes: 0 = 50 Ω sensors (PGA 16), 1 = 100 Ω + R200 (PGA 8),
 * 2 = 500 Ω (PGA 2), 3 = 1000 Ω (PGA 1), 4 = R2k (PGA 1).
 *
 * Coefficients written to 540+ are a LIVE PREVIEW. Persisting them is a
 * write-once COMMIT (HR131 = 0xCA00 | ch*5+class) that locks the slot forever;
 * the tool asks for explicit confirmation before committing.
 *
 * Usage:
 *   node calibrate.mjs status                 [--ip A.B.C.D] [--port 502]
 *   node calibrate.mjs calibrate --ch N --class 0..4
 *   node calibrate.mjs calibrate --ch N        (class of the configured type)
 *   node calibrate.mjs set --ch N --type NAME [--enable 0|1]
 *                                              [--alpha default|custom] [--w100 1.3910]
 */

import net from 'node:net';
import readline from 'node:readline';

/* ----------------------------- register map ----------------------------- */
const MB = {
  // input registers (readings), grouped by quantity, 4 channels each
  IR_TEMP: 300, IR_RCAL: 308, IR_RRAW: 316, IR_FLAGS: 324, IR_CODE: 328, IR_GCLASS: 336,
  IR_MODULE_ID: 125, IR_CAL_LOCK: 127,
  // compact holding block: group*4 + ch
  HR_READING: 0, HR_TYPE: 4, HR_ENABLED: 8, HR_ALPHA_MODE: 12, HR_W100: 16, HR_CALOVR: 20, HR_SMOOTH: 24,
  // calibration coefficients: 540 + ch*20 + class*4 -> gain(2), offset(2)
  HR_RTD_CAL_BASE: 540, HR_RTD_CAL_STRIDE: 20,
  HR_TRIG_SAVE: 117, TRIG_SAVE: 0xA5A5,
  HR_CAL_COMMIT: 131, CAL_COMMIT_BASE: 0xCA00,
};

const GCLASSES = 5;
const GCLASS_NAME = ['50Ω (PGA16)', '100Ω/R200 (PGA8)', '500Ω (PGA2)', '1000Ω (PGA1)', 'R2k (PGA1)'];

/* Sensor type names -> code, matching rtd_scales.h (rtd_type_t). */
const TYPES = [
  '50M', 'Cu50', '50P', 'Pt50', 'Ni100',
  '100M', 'Cu100', '100P', 'Pt100', 'Ni500',
  '500M', 'Cu500', '500P', 'Pt500', 'Ni1000',
  '1000M', 'Cu1000', '1000P', 'Pt1000',
  'R200', 'R2k',
];
/* Gain class per type code (mirrors s_types[] in rtd_scales.c). */
const TYPE_GCLASS = [0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 1, 4];

/* --------------------------- Modbus TCP client -------------------------- */
class ModbusTCP {
  constructor(ip, port, unit = 1) { this.ip = ip; this.port = port; this.unit = unit; this.txid = 0; }

  connect() {
    return new Promise((resolve, reject) => {
      this.sock = net.connect({ host: this.ip, port: this.port }, () => resolve());
      this.sock.on('error', reject);
      this.sock.setNoDelay(true);
    });
  }
  close() { if (this.sock) this.sock.end(); }

  _txn(pdu) {
    return new Promise((resolve, reject) => {
      this.txid = (this.txid + 1) & 0xffff;
      const header = Buffer.alloc(7);
      header.writeUInt16BE(this.txid, 0);
      header.writeUInt16BE(0, 2);
      header.writeUInt16BE(pdu.length + 1, 4);
      header.writeUInt8(this.unit, 6);
      const frame = Buffer.concat([header, pdu]);

      let buf = Buffer.alloc(0);
      const onData = (chunk) => {
        buf = Buffer.concat([buf, chunk]);
        if (buf.length < 7) return;
        const len = buf.readUInt16BE(4);
        if (buf.length < 6 + len) return;
        cleanup();
        const fn = buf.readUInt8(7);
        if (fn & 0x80) { reject(new Error('Modbus exception ' + buf.readUInt8(8))); return; }
        resolve(buf.slice(8));
      };
      const onErr = (e) => { cleanup(); reject(e); };
      const to = setTimeout(() => { cleanup(); reject(new Error('timeout')); }, 3000);
      const cleanup = () => { clearTimeout(to); this.sock.removeListener('data', onData); this.sock.removeListener('error', onErr); };
      this.sock.on('data', onData);
      this.sock.on('error', onErr);
      this.sock.write(frame);
    });
  }

  async readInput(addr, qty) {
    const pdu = Buffer.alloc(5);
    pdu.writeUInt8(0x04, 0); pdu.writeUInt16BE(addr, 1); pdu.writeUInt16BE(qty, 3);
    const data = await this._txn(pdu);
    const n = data.readUInt8(0);
    const regs = [];
    for (let i = 0; i < n / 2; i++) regs.push(data.readUInt16BE(1 + i * 2));
    return regs;
  }
  async readHolding(addr, qty) {
    const pdu = Buffer.alloc(5);
    pdu.writeUInt8(0x03, 0); pdu.writeUInt16BE(addr, 1); pdu.writeUInt16BE(qty, 3);
    const data = await this._txn(pdu);
    const n = data.readUInt8(0);
    const regs = [];
    for (let i = 0; i < n / 2; i++) regs.push(data.readUInt16BE(1 + i * 2));
    return regs;
  }
  async writeMultiple(addr, regs) {
    const pdu = Buffer.alloc(6 + regs.length * 2);
    pdu.writeUInt8(0x10, 0); pdu.writeUInt16BE(addr, 1); pdu.writeUInt16BE(regs.length, 3);
    pdu.writeUInt8(regs.length * 2, 5);
    regs.forEach((r, i) => pdu.writeUInt16BE(r & 0xffff, 6 + i * 2));
    await this._txn(pdu);
  }
  async writeSingle(addr, val) {
    const pdu = Buffer.alloc(5);
    pdu.writeUInt8(0x06, 0); pdu.writeUInt16BE(addr, 1); pdu.writeUInt16BE(val & 0xffff, 3);
    await this._txn(pdu);
  }
}

/* ------------------------------ float codec ----------------------------- */
function regsToFloat(hi, lo) {
  const b = Buffer.alloc(4);
  b.writeUInt16BE(hi, 0); b.writeUInt16BE(lo, 2);
  return b.readFloatBE(0);
}
function floatToRegs(f) {
  const b = Buffer.alloc(4);
  b.writeFloatBE(f, 0);
  return [b.readUInt16BE(0), b.readUInt16BE(2)];
}

/* ----------------------------- helpers ---------------------------------- */
const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
const ask = (q) => new Promise((res) => rl.question(q, res));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function parseArgs(argv) {
  const a = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i].startsWith('--')) { const k = argv[i].slice(2); const v = (argv[i + 1] && !argv[i + 1].startsWith('--')) ? argv[++i] : true; a[k] = v; }
    else a._.push(argv[i]);
  }
  return a;
}
async function readChannel(mb, ch) {
  const r = await mb.readInput(MB.IR_TEMP, 40);   // 300..339 in one go
  const f32 = (base) => regsToFloat(r[base - 300 + ch * 2], r[base - 300 + ch * 2 + 1]);
  const codeHi = r[MB.IR_CODE - 300 + ch * 2], codeLo = r[MB.IR_CODE - 300 + ch * 2 + 1];
  return {
    temp: f32(MB.IR_TEMP),
    rcal: f32(MB.IR_RCAL),
    rraw: f32(MB.IR_RRAW),
    flags: r[MB.IR_FLAGS - 300 + ch],
    code: ((codeHi << 16) | codeLo) | 0,
    gclass: r[MB.IR_GCLASS - 300 + ch],
  };
}

async function readRawAveraged(mb, ch, samples = 8, delayMs = 300) {
  let sum = 0, n = 0, fault = false;
  for (let i = 0; i < samples; i++) {
    const s = await readChannel(mb, ch);
    if (s.flags & 0x0004) fault = true;
    if (Number.isFinite(s.rraw)) { sum += s.rraw; n++; }
    await sleep(delayMs);
  }
  return { raw: n ? sum / n : NaN, fault };
}

function linfit(points) {
  // least squares: R_true = gain*R_raw + offset
  const n = points.length;
  let sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (const [x, y] of points) { sx += x; sy += y; sxx += x * x; sxy += x * y; }
  const denom = n * sxx - sx * sx;
  const gain = (n * sxy - sx * sy) / denom;
  const offset = (sy - gain * sx) / n;
  // residual max error
  let maxErr = 0;
  for (const [x, y] of points) maxErr = Math.max(maxErr, Math.abs(gain * x + offset - y));
  return { gain, offset, maxErr };
}

const FAULT_NAME = { 1: 'OPEN', 2: 'SHORT', 3: 'ADC' };

/* ------------------------------ commands -------------------------------- */
async function cmdStatus(mb) {
  const id = (await mb.readInput(MB.IR_MODULE_ID, 1))[0];
  const lock = await mb.readInput(MB.IR_CAL_LOCK, 2);
  const lockMask = lock[0] | (lock[1] << 16);
  const cfg = await mb.readHolding(0, 28);
  console.log(`Module ID: 0x${id.toString(16)}`);
  for (let ch = 0; ch < 4; ch++) {
    const s = await readChannel(mb, ch);
    const f = [];
    if (s.flags & 1) f.push('EN'); if (s.flags & 2) f.push('VALID'); if (s.flags & 4) f.push('FAULT');
    const fcode = (s.flags >> 8) & 0xff;
    const locked = [];
    for (let c = 0; c < GCLASSES; c++) if (lockMask & (1 << (ch * GCLASSES + c))) locked.push(c);
    const i16 = cfg[MB.HR_READING + ch] > 0x7fff ? cfg[MB.HR_READING + ch] - 0x10000 : cfg[MB.HR_READING + ch];
    console.log(`CH${ch}: type=${TYPES[cfg[MB.HR_TYPE + ch]] ?? cfg[MB.HR_TYPE + ch]}  ` +
      `T=${s.temp.toFixed(3)}°C (i16=${i16})  Rcal=${s.rcal.toFixed(3)}Ω  Rraw=${s.rraw.toFixed(4)}Ω  ` +
      `class=${s.gclass}  code=${s.code}  [${f.join(',')}]` +
      (fcode ? ` fault=${FAULT_NAME[fcode] ?? fcode}` : '') +
      (locked.length ? `  locked classes: ${locked.join(',')}` : ''));
  }
}

async function calibrateClass(mb, ch, cls) {
  console.log(`\n=== Calibrating CH${ch}, gain class ${cls} — ${GCLASS_NAME[cls]} ===`);
  console.log(`Forcing gain-class override (${cls + 1}) ...`);
  await mb.writeSingle(MB.HR_CALOVR + ch, cls + 1);
  await sleep(1500);

  const points = [];
  for (;;) {
    console.log(`\nApply a known resistance to CH${ch} terminals (3-wire) with the standard (АКИП-2202А).`);
    const ans = await ask(`Enter applied resistance in Ω (or "done" to finish, need >= 2 points): `);
    if (ans.trim().toLowerCase() === 'done') {
      if (points.length >= 2) break;
      console.log('Need at least 2 points.'); continue;
    }
    const rTrue = parseFloat(ans.replace(',', '.'));
    if (!Number.isFinite(rTrue)) { console.log('Invalid number.'); continue; }
    process.stdout.write('Measuring raw resistance ');
    const { raw, fault } = await readRawAveraged(mb, ch);
    console.log(`-> R_raw = ${raw.toFixed(4)} Ω${fault ? '  (WARNING: converter fault!)' : ''}`);
    if (!Number.isFinite(raw)) { console.log('No reading; skipped.'); continue; }
    points.push([raw, rTrue]);
    console.log(`Recorded point ${points.length}: R_raw=${raw.toFixed(4)} -> R_true=${rTrue}`);
  }

  const { gain, offset, maxErr } = linfit(points);
  console.log(`\nFit: gain=${gain.toFixed(7)}  offset=${offset.toFixed(4)} Ω  max residual=${maxErr.toFixed(4)} Ω`);

  const calBase = MB.HR_RTD_CAL_BASE + ch * MB.HR_RTD_CAL_STRIDE + cls * 4;
  await mb.writeMultiple(calBase, [...floatToRegs(gain), ...floatToRegs(offset)]);
  console.log(`Wrote coefficients (live preview) to holding regs ${calBase}..${calBase + 3}.`);

  const slot = ch * GCLASSES + cls;
  const ans = await ask(`\nCOMMIT slot ${slot} (CH${ch}, class ${cls}) to write-once Flash? This is IRREVERSIBLE. Type "COMMIT" to proceed: `);
  if (ans.trim() === 'COMMIT') {
    await mb.writeSingle(MB.HR_CAL_COMMIT, MB.CAL_COMMIT_BASE | slot);
    console.log('Committed and locked.');
  } else {
    console.log('Not committed (preview stays active until reboot).');
  }
}

async function cmdCalibrate(mb, args) {
  const channels = args.all ? [0, 1, 2, 3] : [parseInt(args.ch, 10)];
  if (channels.some((c) => !(c >= 0 && c <= 3))) throw new Error('Specify --ch 0..3 or --all');

  for (const ch of channels) {
    let cls;
    if (args.class !== undefined) {
      cls = parseInt(args.class, 10);
      if (!(cls >= 0 && cls < GCLASSES)) throw new Error('--class must be 0..4');
    } else {
      const type = (await mb.readHolding(MB.HR_TYPE + ch, 1))[0];
      cls = TYPE_GCLASS[type];
      if (cls === undefined) throw new Error(`CH${ch}: unknown type code ${type}, pass --class`);
      console.log(`CH${ch}: configured type ${TYPES[type]} -> gain class ${cls}`);
    }
    await calibrateClass(mb, ch, cls);
    // restore auto class
    await mb.writeSingle(MB.HR_CALOVR + ch, 0);
  }
}

async function cmdSet(mb, args) {
  const ch = parseInt(args.ch, 10);
  if (!(ch >= 0 && ch <= 3)) throw new Error('Specify --ch 0..3');
  if (args.type) {
    const code = TYPES.indexOf(args.type);
    if (code < 0) throw new Error('Unknown type. One of: ' + TYPES.join(', '));
    await mb.writeSingle(MB.HR_TYPE + ch, code);
    console.log(`CH${ch} type = ${args.type} (${code}), gain class ${TYPE_GCLASS[code]}`);
  }
  if (args.alpha) {
    await mb.writeSingle(MB.HR_ALPHA_MODE + ch, args.alpha === 'custom' ? 1 : 0);
  }
  if (args.w100) {
    await mb.writeSingle(MB.HR_W100 + ch, Math.round(parseFloat(args.w100.replace(',', '.')) * 10000));
    await mb.writeSingle(MB.HR_ALPHA_MODE + ch, 1);
    console.log(`CH${ch} custom W100 = ${args.w100}`);
  }
  if (args.enable !== undefined) {
    await mb.writeSingle(MB.HR_ENABLED + ch, parseInt(args.enable, 10) ? 1 : 0);
  }
  await mb.writeSingle(MB.HR_TRIG_SAVE, MB.TRIG_SAVE);
  console.log('Configuration saved.');
}

/* ------------------------------- main ----------------------------------- */
async function main() {
  const args = parseArgs(process.argv.slice(2));
  const cmd = args._[0] || 'status';
  const ip = args.ip || '192.168.1.12';
  const port = parseInt(args.port || '502', 10);
  const unit = parseInt(args.unit || '1', 10);

  const mb = new ModbusTCP(ip, port, unit);
  console.log(`Connecting to ${ip}:${port} (unit ${unit}) ...`);
  await mb.connect();

  try {
    if (cmd === 'status') await cmdStatus(mb);
    else if (cmd === 'calibrate') await cmdCalibrate(mb, args);
    else if (cmd === 'set') await cmdSet(mb, args);
    else console.log('Unknown command. Use: status | calibrate | set');
  } finally {
    mb.close();
    rl.close();
  }
}

main().catch((e) => { console.error('Error:', e.message); process.exit(1); });

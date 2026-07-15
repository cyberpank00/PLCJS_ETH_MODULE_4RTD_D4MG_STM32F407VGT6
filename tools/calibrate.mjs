#!/usr/bin/env node
/**
 * calibrate.mjs — dialog-driven calibration / configuration tool for the
 * PLCJS 4RTD analog input module over Modbus TCP.
 *
 * Node.js 18+ only, no external npm packages (uses node:net / node:readline).
 *
 * The module measures a ratiometric resistance R_raw = code/32768 * RREF_nom.
 * Because the reference-resistor branch contains an analog switch (ADG849)
 * whose on-resistance and the ±0.1 % RREF tolerance are not known a priori,
 * each channel is calibrated per range with a simple linear model:
 *
 *      R_true = gain * R_raw + offset
 *
 * gain and offset are found by a least-squares fit over >= 2 reference points
 * applied with a precision resistance standard (e.g. АКИП-2202А, 0.05 %).
 * The result reaches the target 0.2 % per channel.
 *
 * Usage:
 *   node calibrate.mjs status                 [--ip A.B.C.D] [--port 502]
 *   node calibrate.mjs calibrate --ch N --range low|high
 *   node calibrate.mjs calibrate --ch N        (calibrates both ranges)
 *   node calibrate.mjs calibrate --all         (all channels, both ranges)
 *   node calibrate.mjs set --ch N --type NAME [--enable 0|1]
 *                                              [--alpha default|custom] [--w100 1.3910]
 */

import net from 'node:net';
import readline from 'node:readline';

/* ----------------------------- register map ----------------------------- */
const MB = {
  // input registers (readings)
  IR_RTD_BASE: 300, IR_RTD_STRIDE: 20,
  IR_TEMP: 0, IR_RCAL: 2, IR_RRAW: 4, IR_FLAGS: 6, IR_CODE: 7, IR_RANGE: 8,
  IR_MODULE_ID: 125,
  // holding registers (config)
  HR_RTD_CFG_BASE: 500, HR_RTD_CFG_STRIDE: 10,
  CFG_ENABLED: 0, CFG_TYPE: 1, CFG_ALPHA_MODE: 2, CFG_W100: 3, CFG_CALRANGE: 4,
  HR_RTD_CAL_BASE: 540, HR_RTD_CAL_STRIDE: 8,   // gainLo,offLo,gainHi,offHi (floats)
  HR_TRIG_SAVE: 117, TRIG_SAVE: 0xA5A5,
};

const CAL_OVERRIDE = { auto: 0, low: 1, high: 2 };

/* Sensor type names -> code, matching rtd_scales.h (rtd_type_t). */
const TYPES = [
  '50M', 'Cu50', '50P', 'Pt50', 'Ni100',
  '100M', 'Cu100', '100P', 'Pt100', 'Ni500',
  '500M', 'Cu500', '500P', 'Pt500', 'Ni1000',
  '1000M', 'Cu1000', '1000P', 'Pt1000',
  'R2k', 'R5k',
];

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
  const base = MB.IR_RTD_BASE + ch * MB.IR_RTD_STRIDE;
  const r = await mb.readInput(base, 9);
  return {
    temp: regsToFloat(r[MB.IR_TEMP], r[MB.IR_TEMP + 1]),
    rcal: regsToFloat(r[MB.IR_RCAL], r[MB.IR_RCAL + 1]),
    rraw: regsToFloat(r[MB.IR_RRAW], r[MB.IR_RRAW + 1]),
    flags: r[MB.IR_FLAGS],
    code: r[MB.IR_CODE],
    range: r[MB.IR_RANGE],
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

/* ------------------------------ commands -------------------------------- */
async function cmdStatus(mb) {
  const id = (await mb.readInput(MB.IR_MODULE_ID, 1))[0];
  console.log(`Module ID: 0x${id.toString(16)}`);
  for (let ch = 0; ch < 4; ch++) {
    const s = await readChannel(mb, ch);
    const f = [];
    if (s.flags & 1) f.push('EN'); if (s.flags & 2) f.push('VALID'); if (s.flags & 4) f.push('FAULT');
    const fcode = (s.flags >> 8) & 0xff;
    console.log(`CH${ch}: T=${s.temp.toFixed(3)}°C  Rcal=${s.rcal.toFixed(3)}Ω  Rraw=${s.rraw.toFixed(3)}Ω  ` +
      `range=${s.range === 1 ? 'high' : 'low'}  code=${s.code}  [${f.join(',')}]` +
      (fcode ? ` fault=0x${fcode.toString(16)}` : ''));
  }
}

async function calibrateRange(mb, ch, rangeName) {
  const rangeCode = CAL_OVERRIDE[rangeName];
  console.log(`\n=== Calibrating CH${ch}, ${rangeName} range ===`);
  console.log(`Forcing range override (${rangeName}) ...`);
  await mb.writeSingle(MB.HR_RTD_CFG_BASE + ch * MB.HR_RTD_CFG_STRIDE + MB.CFG_CALRANGE, rangeCode);
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
  console.log(`\nFit: gain=${gain.toFixed(6)}  offset=${offset.toFixed(4)} Ω  max residual=${maxErr.toFixed(4)} Ω`);

  // write coefficients: base + (range==low? 0 : 4)  -> [gain(2), offset(2)]
  const calBase = MB.HR_RTD_CAL_BASE + ch * MB.HR_RTD_CAL_STRIDE + (rangeName === 'high' ? 4 : 0);
  await mb.writeMultiple(calBase, [...floatToRegs(gain), ...floatToRegs(offset)]);
  console.log(`Wrote coefficients to holding regs ${calBase}..${calBase + 3}.`);
}

async function cmdCalibrate(mb, args) {
  const ranges = args.range ? [args.range] : ['low', 'high'];
  const channels = args.all ? [0, 1, 2, 3] : [parseInt(args.ch, 10)];
  if (channels.some((c) => !(c >= 0 && c <= 3))) throw new Error('Specify --ch 0..3 or --all');

  for (const ch of channels) {
    for (const rg of ranges) {
      if (!(rg in CAL_OVERRIDE) || rg === 'auto') throw new Error('range must be low or high');
      await calibrateRange(mb, ch, rg);
    }
    // restore auto range
    await mb.writeSingle(MB.HR_RTD_CFG_BASE + ch * MB.HR_RTD_CFG_STRIDE + MB.CFG_CALRANGE, CAL_OVERRIDE.auto);
  }

  const save = (await ask('\nSave calibration to flash now? [Y/n] ')).trim().toLowerCase();
  if (save === '' || save === 'y') {
    await mb.writeSingle(MB.HR_TRIG_SAVE, MB.TRIG_SAVE);
    console.log('Saved.');
  } else {
    console.log('NOT saved (coefficients are active until reboot).');
  }
}

async function cmdSet(mb, args) {
  const ch = parseInt(args.ch, 10);
  if (!(ch >= 0 && ch <= 3)) throw new Error('Specify --ch 0..3');
  const base = MB.HR_RTD_CFG_BASE + ch * MB.HR_RTD_CFG_STRIDE;
  if (args.type) {
    const code = TYPES.indexOf(args.type);
    if (code < 0) throw new Error('Unknown type. One of: ' + TYPES.join(', '));
    await mb.writeSingle(base + MB.CFG_TYPE, code);
    console.log(`CH${ch} type = ${args.type} (${code})`);
  }
  if (args.alpha) {
    await mb.writeSingle(base + MB.CFG_ALPHA_MODE, args.alpha === 'custom' ? 1 : 0);
  }
  if (args.w100) {
    await mb.writeSingle(base + MB.CFG_W100, Math.round(parseFloat(args.w100.replace(',', '.')) * 10000));
    await mb.writeSingle(base + MB.CFG_ALPHA_MODE, 1);
    console.log(`CH${ch} custom W100 = ${args.w100}`);
  }
  if (args.enable !== undefined) {
    await mb.writeSingle(base + MB.CFG_ENABLED, parseInt(args.enable, 10) ? 1 : 0);
  }
  await mb.writeSingle(MB.HR_TRIG_SAVE, MB.TRIG_SAVE);
  console.log('Configuration saved.');
}

/* ------------------------------- main ----------------------------------- */
async function main() {
  const args = parseArgs(process.argv.slice(2));
  const cmd = args._[0] || 'status';
  const ip = args.ip || '192.168.142.150';
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

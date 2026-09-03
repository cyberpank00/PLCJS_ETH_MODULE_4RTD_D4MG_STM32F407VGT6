#!/usr/bin/env python3
"""
calibrate.py - dialog-driven calibration / configuration tool for the
PLCJS 4RTD analog input module (HW2.1, ADS1220) over Modbus TCP.

Pure standard-library Python 3.8+ (socket, struct, argparse) - no pymodbus
required. Functionally identical to tools/calibrate.mjs.

The module measures a ratiometric resistance
    R_raw = code / 2^23 * 2*RREF_nom / gain_PGA
Calibration model (per channel, per gain class):

    R_true = gain * R_raw + offset

gain / offset are obtained by a least-squares fit over >= 2 reference points
applied with a precision resistance standard (e.g. АКИП-2202А, 0.05 %). This
removes the RREF tolerance and the PGA gain error.

Gain classes: 0 = 50 Ω sensors (PGA 16), 1 = 100 Ω + R200 (PGA 8),
2 = 500 Ω (PGA 2), 3 = 1000 Ω (PGA 1), 4 = R2k (PGA 1).

Coefficients written to 540+ are a LIVE PREVIEW. Persisting them is a
write-once COMMIT (HR131 = 0xCA00 | ch*5+class) that locks the slot forever;
the tool asks for explicit confirmation before committing.

Examples:
    python calibrate.py status --ip 192.168.1.12
    python calibrate.py calibrate --ch 0 --cls 1
    python calibrate.py calibrate --ch 0            # class of the configured type
    python calibrate.py set --ch 0 --type Pt100 --enable 1
    python calibrate.py set --ch 1 --type 100P --w100 1.3910
"""

import argparse
import socket
import struct
import sys
import time

# ------------------------------ register map ------------------------------
# input registers (readings), grouped by quantity, 4 channels each
IR_TEMP, IR_RCAL, IR_RRAW, IR_FLAGS, IR_CODE, IR_GCLASS = 300, 308, 316, 324, 328, 336
IR_MODULE_ID, IR_CAL_LOCK = 125, 127

# compact holding block: group*4 + ch
HR_READING, HR_TYPE, HR_ENABLED, HR_ALPHA_MODE, HR_W100, HR_CALOVR, HR_SMOOTH = 0, 4, 8, 12, 16, 20, 24
# calibration coefficients: 540 + ch*20 + class*4 -> gain(2), offset(2)
HR_RTD_CAL_BASE, HR_RTD_CAL_STRIDE = 540, 20
HR_TRIG_SAVE, TRIG_SAVE = 117, 0xA5A5
HR_CAL_COMMIT, CAL_COMMIT_BASE = 131, 0xCA00

GCLASSES = 5
GCLASS_NAME = ["50Ω (PGA16)", "100Ω/R200 (PGA8)", "500Ω (PGA2)", "1000Ω (PGA1)", "R2k (PGA1)"]

TYPES = [
    "50M", "Cu50", "50P", "Pt50", "Ni100",
    "100M", "Cu100", "100P", "Pt100", "Ni500",
    "500M", "Cu500", "500P", "Pt500", "Ni1000",
    "1000M", "Cu1000", "1000P", "Pt1000",
    "R200", "R2k",
]
# Gain class per type code (mirrors s_types[] in rtd_scales.c).
TYPE_GCLASS = [0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 1, 4]
FAULT_NAME = {1: "OPEN", 2: "SHORT", 3: "ADC"}


# --------------------------- Modbus TCP client ----------------------------
class ModbusTCP:
    def __init__(self, ip, port=502, unit=1, timeout=3.0):
        self.ip, self.port, self.unit, self.timeout = ip, port, unit, timeout
        self.txid = 0
        self.sock = None

    def connect(self):
        self.sock = socket.create_connection((self.ip, self.port), self.timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def close(self):
        if self.sock:
            self.sock.close()

    def _txn(self, pdu):
        self.txid = (self.txid + 1) & 0xFFFF
        header = struct.pack(">HHHB", self.txid, 0, len(pdu) + 1, self.unit)
        self.sock.sendall(header + pdu)
        # read MBAP header
        buf = b""
        while len(buf) < 7:
            chunk = self.sock.recv(260)
            if not chunk:
                raise IOError("connection closed")
            buf += chunk
        length = struct.unpack(">H", buf[4:6])[0]
        while len(buf) < 6 + length:
            chunk = self.sock.recv(260)
            if not chunk:
                raise IOError("connection closed")
            buf += chunk
        fn = buf[7]
        if fn & 0x80:
            raise IOError("Modbus exception %d" % buf[8])
        return buf[8:6 + length]

    def read_input(self, addr, qty):
        data = self._txn(struct.pack(">BHH", 0x04, addr, qty))
        n = data[0]
        return list(struct.unpack(">%dH" % (n // 2), data[1:1 + n]))

    def read_holding(self, addr, qty):
        data = self._txn(struct.pack(">BHH", 0x03, addr, qty))
        n = data[0]
        return list(struct.unpack(">%dH" % (n // 2), data[1:1 + n]))

    def write_multiple(self, addr, regs):
        pdu = struct.pack(">BHHB", 0x10, addr, len(regs), len(regs) * 2)
        pdu += b"".join(struct.pack(">H", r & 0xFFFF) for r in regs)
        self._txn(pdu)

    def write_single(self, addr, val):
        self._txn(struct.pack(">BHH", 0x06, addr, val & 0xFFFF))


# ------------------------------ float codec -------------------------------
def regs_to_float(hi, lo):
    return struct.unpack(">f", struct.pack(">HH", hi, lo))[0]


def float_to_regs(f):
    return list(struct.unpack(">HH", struct.pack(">f", f)))



# ------------------------------- helpers ----------------------------------
def read_channel(mb, ch):
    r = mb.read_input(IR_TEMP, 40)  # 300..339 in one go

    def f32(base):
        i = base - 300 + ch * 2
        return regs_to_float(r[i], r[i + 1])

    code = (r[IR_CODE - 300 + ch * 2] << 16) | r[IR_CODE - 300 + ch * 2 + 1]
    if code & 0x80000000:
        code -= 0x100000000
    return {
        "temp": f32(IR_TEMP),
        "rcal": f32(IR_RCAL),
        "rraw": f32(IR_RRAW),
        "flags": r[IR_FLAGS - 300 + ch],
        "code": code,
        "gclass": r[IR_GCLASS - 300 + ch],
    }


def read_raw_averaged(mb, ch, samples=8, delay=0.3):
    total, n, fault = 0.0, 0, False
    for _ in range(samples):
        s = read_channel(mb, ch)
        if s["flags"] & 0x0004:
            fault = True
        if s["rraw"] == s["rraw"]:  # not NaN
            total += s["rraw"]
            n += 1
        time.sleep(delay)
    return (total / n if n else float("nan"), fault)


def linfit(points):
    n = len(points)
    sx = sum(x for x, _ in points)
    sy = sum(y for _, y in points)
    sxx = sum(x * x for x, _ in points)
    sxy = sum(x * y for x, y in points)
    denom = n * sxx - sx * sx
    gain = (n * sxy - sx * sy) / denom
    offset = (sy - gain * sx) / n
    max_err = max(abs(gain * x + offset - y) for x, y in points)
    return gain, offset, max_err


# ------------------------------- commands ---------------------------------
def cmd_status(mb, _args):
    mid = mb.read_input(IR_MODULE_ID, 1)[0]
    lock = mb.read_input(IR_CAL_LOCK, 2)
    lock_mask = lock[0] | (lock[1] << 16)
    cfg = mb.read_holding(0, 28)
    print("Module ID: 0x%04X" % mid)
    for ch in range(4):
        s = read_channel(mb, ch)
        flags = []
        if s["flags"] & 1:
            flags.append("EN")
        if s["flags"] & 2:
            flags.append("VALID")
        if s["flags"] & 4:
            flags.append("FAULT")
        fcode = (s["flags"] >> 8) & 0xFF
        locked = [c for c in range(GCLASSES) if lock_mask & (1 << (ch * GCLASSES + c))]
        i16 = cfg[HR_READING + ch]
        if i16 > 0x7FFF:
            i16 -= 0x10000
        tcode = cfg[HR_TYPE + ch]
        tname = TYPES[tcode] if tcode < len(TYPES) else str(tcode)
        print("CH%d: type=%s  T=%.3f°C (i16=%d)  Rcal=%.3fΩ  Rraw=%.4fΩ  class=%d  code=%d  [%s]%s%s" % (
            ch, tname, s["temp"], i16, s["rcal"], s["rraw"], s["gclass"], s["code"],
            ",".join(flags),
            (" fault=%s" % FAULT_NAME.get(fcode, fcode)) if fcode else "",
            ("  locked classes: %s" % ",".join(map(str, locked))) if locked else ""))


def calibrate_class(mb, ch, cls):
    print("\n=== Calibrating CH%d, gain class %d - %s ===" % (ch, cls, GCLASS_NAME[cls]))
    print("Forcing gain-class override (%d) ..." % (cls + 1))
    mb.write_single(HR_CALOVR + ch, cls + 1)
    time.sleep(1.5)

    points = []
    while True:
        print("\nApply a known resistance to CH%d terminals (3-wire) with the "
              "standard (АКИП-2202А)." % ch)
        ans = input('Enter applied resistance in Ω (or "done", need >= 2 points): ').strip()
        if ans.lower() == "done":
            if len(points) >= 2:
                break
            print("Need at least 2 points.")
            continue
        try:
            r_true = float(ans.replace(",", "."))
        except ValueError:
            print("Invalid number.")
            continue
        sys.stdout.write("Measuring raw resistance ...\n")
        raw, fault = read_raw_averaged(mb, ch)
        print("-> R_raw = %.4f Ω%s" % (raw, "  (WARNING: converter fault!)" if fault else ""))
        if raw != raw:
            print("No reading; skipped.")
            continue
        points.append((raw, r_true))
        print("Recorded point %d: R_raw=%.4f -> R_true=%s" % (len(points), raw, r_true))

    gain, offset, max_err = linfit(points)
    print("\nFit: gain=%.7f  offset=%.4f Ω  max residual=%.4f Ω" % (gain, offset, max_err))

    cal_base = HR_RTD_CAL_BASE + ch * HR_RTD_CAL_STRIDE + cls * 4
    mb.write_multiple(cal_base, float_to_regs(gain) + float_to_regs(offset))
    print("Wrote coefficients (live preview) to holding regs %d..%d." % (cal_base, cal_base + 3))

    slot = ch * GCLASSES + cls
    ans = input("\nCOMMIT slot %d (CH%d, class %d) to write-once Flash? This is IRREVERSIBLE. "
                'Type "COMMIT" to proceed: ' % (slot, ch, cls)).strip()
    if ans == "COMMIT":
        mb.write_single(HR_CAL_COMMIT, CAL_COMMIT_BASE | slot)
        print("Committed and locked.")
    else:
        print("Not committed (preview stays active until reboot).")


def cmd_calibrate(mb, args):
    channels = [0, 1, 2, 3] if args.all else [args.ch]
    if any(c is None or not (0 <= c <= 3) for c in channels):
        raise SystemExit("Specify --ch 0..3 or --all")
    for ch in channels:
        if args.cls is not None:
            cls = args.cls
        else:
            tcode = mb.read_holding(HR_TYPE + ch, 1)[0]
            if tcode >= len(TYPE_GCLASS):
                raise SystemExit("CH%d: unknown type code %d, pass --cls" % (ch, tcode))
            cls = TYPE_GCLASS[tcode]
            print("CH%d: configured type %s -> gain class %d" % (ch, TYPES[tcode], cls))
        calibrate_class(mb, ch, cls)
        mb.write_single(HR_CALOVR + ch, 0)  # restore auto class


def cmd_set(mb, args):
    ch = args.ch
    if ch is None or not (0 <= ch <= 3):
        raise SystemExit("Specify --ch 0..3")
    if args.type:
        if args.type not in TYPES:
            raise SystemExit("Unknown type. One of: " + ", ".join(TYPES))
        code = TYPES.index(args.type)
        mb.write_single(HR_TYPE + ch, code)
        print("CH%d type = %s (%d), gain class %d" % (ch, args.type, code, TYPE_GCLASS[code]))
    if args.alpha:
        mb.write_single(HR_ALPHA_MODE + ch, 1 if args.alpha == "custom" else 0)
    if args.w100:
        mb.write_single(HR_W100 + ch, round(float(args.w100.replace(",", ".")) * 10000))
        mb.write_single(HR_ALPHA_MODE + ch, 1)
        print("CH%d custom W100 = %s" % (ch, args.w100))
    if args.enable is not None:
        mb.write_single(HR_ENABLED + ch, 1 if args.enable else 0)
    mb.write_single(HR_TRIG_SAVE, TRIG_SAVE)
    print("Configuration saved.")


def main():
    p = argparse.ArgumentParser(description="PLCJS 4RTD calibration tool (Modbus TCP)")
    p.add_argument("command", choices=["status", "calibrate", "set"])
    p.add_argument("--ip", default="192.168.1.12")
    p.add_argument("--port", type=int, default=502)
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--ch", type=int)
    p.add_argument("--cls", type=int, choices=range(GCLASSES), help="gain class 0..4")
    p.add_argument("--all", action="store_true")
    p.add_argument("--type")
    p.add_argument("--alpha", choices=["default", "custom"])
    p.add_argument("--w100")
    p.add_argument("--enable", type=int)
    args = p.parse_args()

    mb = ModbusTCP(args.ip, args.port, args.unit)
    print("Connecting to %s:%d (unit %d) ..." % (args.ip, args.port, args.unit))
    mb.connect()
    try:
        {"status": cmd_status, "calibrate": cmd_calibrate, "set": cmd_set}[args.command](mb, args)
    finally:
        mb.close()


if __name__ == "__main__":
    main()
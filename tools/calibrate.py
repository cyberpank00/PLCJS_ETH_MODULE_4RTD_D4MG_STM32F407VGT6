#!/usr/bin/env python3
"""
calibrate.py - dialog-driven calibration / configuration tool for the
PLCJS 4RTD analog input module over Modbus TCP.

Pure standard-library Python 3.8+ (socket, struct, argparse) - no pymodbus
required. Functionally identical to tools/calibrate.mjs.

Calibration model (per channel, per range):

    R_true = gain * R_raw + offset

gain / offset are obtained by a least-squares fit over >= 2 reference points
applied with a precision resistance standard (e.g. АКИП-2202А, 0.05 %). This
removes the RREF tolerance and the ADG849 analog-switch on-resistance and
reaches the 0.2 % target per channel.

Examples:
    python calibrate.py status --ip 192.168.142.150
    python calibrate.py calibrate --ch 0 --range low
    python calibrate.py calibrate --ch 0            # both ranges
    python calibrate.py calibrate --all             # all channels, both ranges
    python calibrate.py set --ch 0 --type Pt100 --enable 1
    python calibrate.py set --ch 1 --type 100P --w100 1.3910
"""

import argparse
import socket
import struct
import sys
import time

# ------------------------------ register map ------------------------------
IR_RTD_BASE, IR_RTD_STRIDE = 300, 20
IR_TEMP, IR_RCAL, IR_RRAW, IR_FLAGS, IR_CODE, IR_RANGE = 0, 2, 4, 6, 7, 8
IR_MODULE_ID = 125

HR_RTD_CFG_BASE, HR_RTD_CFG_STRIDE = 500, 10
CFG_ENABLED, CFG_TYPE, CFG_ALPHA_MODE, CFG_W100, CFG_CALRANGE = 0, 1, 2, 3, 4
HR_RTD_CAL_BASE, HR_RTD_CAL_STRIDE = 540, 8
HR_TRIG_SAVE, TRIG_SAVE = 117, 0xA5A5

CAL_OVERRIDE = {"auto": 0, "low": 1, "high": 2}

TYPES = [
    "50M", "Cu50", "50P", "Pt50", "Ni100",
    "100M", "Cu100", "100P", "Pt100", "Ni500",
    "500M", "Cu500", "500P", "Pt500", "Ni1000",
    "1000M", "Cu1000", "1000P", "Pt1000",
    "R2k", "R5k",
]


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
    base = IR_RTD_BASE + ch * IR_RTD_STRIDE
    r = mb.read_input(base, 9)
    return {
        "temp": regs_to_float(r[IR_TEMP], r[IR_TEMP + 1]),
        "rcal": regs_to_float(r[IR_RCAL], r[IR_RCAL + 1]),
        "rraw": regs_to_float(r[IR_RRAW], r[IR_RRAW + 1]),
        "flags": r[IR_FLAGS],
        "code": r[IR_CODE],
        "range": r[IR_RANGE],
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
        print("CH%d: T=%.3f°C  Rcal=%.3fΩ  Rraw=%.3fΩ  range=%s  code=%d  [%s]%s" % (
            ch, s["temp"], s["rcal"], s["rraw"],
            "high" if s["range"] == 1 else "low", s["code"], ",".join(flags),
            (" fault=0x%02X" % fcode) if fcode else ""))


def calibrate_range(mb, ch, range_name):
    print("\n=== Calibrating CH%d, %s range ===" % (ch, range_name))
    print("Forcing range override (%s) ..." % range_name)
    mb.write_single(HR_RTD_CFG_BASE + ch * HR_RTD_CFG_STRIDE + CFG_CALRANGE,
                    CAL_OVERRIDE[range_name])
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
    print("\nFit: gain=%.6f  offset=%.4f Ω  max residual=%.4f Ω" % (gain, offset, max_err))

    cal_base = HR_RTD_CAL_BASE + ch * HR_RTD_CAL_STRIDE + (4 if range_name == "high" else 0)
    mb.write_multiple(cal_base, float_to_regs(gain) + float_to_regs(offset))
    print("Wrote coefficients to holding regs %d..%d." % (cal_base, cal_base + 3))


def cmd_calibrate(mb, args):
    ranges = [args.range] if args.range else ["low", "high"]
    channels = [0, 1, 2, 3] if args.all else [args.ch]
    if any(c is None or not (0 <= c <= 3) for c in channels):
        raise SystemExit("Specify --ch 0..3 or --all")
    for ch in channels:
        for rg in ranges:
            if rg not in ("low", "high"):
                raise SystemExit("range must be low or high")
            calibrate_range(mb, ch, rg)
        mb.write_single(HR_RTD_CFG_BASE + ch * HR_RTD_CFG_STRIDE + CFG_CALRANGE,
                        CAL_OVERRIDE["auto"])

    save = input("\nSave calibration to flash now? [Y/n] ").strip().lower()
    if save in ("", "y"):
        mb.write_single(HR_TRIG_SAVE, TRIG_SAVE)
        print("Saved.")
    else:
        print("NOT saved (coefficients are active until reboot).")


def cmd_set(mb, args):
    ch = args.ch
    if ch is None or not (0 <= ch <= 3):
        raise SystemExit("Specify --ch 0..3")
    base = HR_RTD_CFG_BASE + ch * HR_RTD_CFG_STRIDE
    if args.type:
        if args.type not in TYPES:
            raise SystemExit("Unknown type. One of: " + ", ".join(TYPES))
        mb.write_single(base + CFG_TYPE, TYPES.index(args.type))
        print("CH%d type = %s (%d)" % (ch, args.type, TYPES.index(args.type)))
    if args.alpha:
        mb.write_single(base + CFG_ALPHA_MODE, 1 if args.alpha == "custom" else 0)
    if args.w100:
        mb.write_single(base + CFG_W100, round(float(args.w100.replace(",", ".")) * 10000))
        mb.write_single(base + CFG_ALPHA_MODE, 1)
        print("CH%d custom W100 = %s" % (ch, args.w100))
    if args.enable is not None:
        mb.write_single(base + CFG_ENABLED, 1 if args.enable else 0)
    mb.write_single(HR_TRIG_SAVE, TRIG_SAVE)
    print("Configuration saved.")


def main():
    p = argparse.ArgumentParser(description="PLCJS 4RTD calibration tool (Modbus TCP)")
    p.add_argument("command", choices=["status", "calibrate", "set"])
    p.add_argument("--ip", default="192.168.142.150")
    p.add_argument("--port", type=int, default=502)
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--ch", type=int)
    p.add_argument("--range", choices=["low", "high"])
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

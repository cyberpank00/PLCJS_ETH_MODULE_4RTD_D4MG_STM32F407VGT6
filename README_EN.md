# PLCJS_ETH_MODULE_4RTD_D4MG_STM32F407VGT6

Firmware for the PLCJS Ethernet module with 4 RTD analog inputs, based on
`STM32F407VGT6`. **Board HW2.1** (`ADS1220` ADCs).

The module measures four RTD channels through isolated 24-bit `ADS1220` ADCs
(SPI, one per channel), converts resistance to temperature for the selected
scale (GOST 6651 / IEC 60751 / DIN 43760), publishes readings over
`Modbus TCP`, stores settings in internal Flash and calibration in a separate
write-once Flash sector, drives a main `STAT_LED` plus four per-channel LEDs,
and supports factory reset and OTA via the Ethernet bootloader. Network /
Modbus / settings / LED / bootloader infrastructure is inherited from the
`12DI` variant. The HW1.x firmware (`MAX31865` + `ADG849` range switch) is
tagged `hw1.1-last`.

## Hardware

- MCU `STM32F407VGT6` (Cortex-M4F); Ethernet `KSZ8863` (RMII).
- 4× `ADS1220` (24-bit Δ-Σ, PGA, two matched IDACs), galvanically isolated,
  shared SPI1.
- 3-wire RTD, lead compensation by two matched excitation currents.
- Measuring range set purely by PGA gain (no external switches).
- Reference resistor 2 kΩ ±0.1 % in the IDAC return path (ratiometric).

Pinout (HW2.1): SPI1 SCLK=PA5, MISO=PA6, MOSI=PB5. CS: PC6/PD15/PD14/PD13.
Channel LEDs: PE14/PE13/PE12/PE11. STAT_LED=PE9, FACT_RES=PE10.
SPI mode 1 (CPOL=0, CPHA=1), 8-bit, ~2.6 MHz. DRDY is not wired — data are
read with `RDATA` from the scan task.

### Channel wiring and ADS1220 configuration

```
S+ ── AIN2 (IDAC1)  and via 10k ── AIN0 (AINP)
S- ── AIN3 (IDAC2)  and via 10k ── AIN1 (AINN)
E  ── RREF 2k ── GND ;  REFP0/REFN0 across RREF
```

| Reg | Value | Fields |
|---|---|---|
| 0 | `gain<<1` | MUX AIN0/AIN1, GAIN per class, PGA on |
| 1 | `0x04` | 20 SPS, normal mode, continuous conversion |
| 2 | `0x55` | external ref REFP0/REFN0, 50+60 Hz FIR, IDAC 500 µA |
| 3 | `0x70` | IDAC1 → AIN2, IDAC2 → AIN3 |

Both currents return through RREF, so `V_REF = 2·I·RREF = 2.0 V` and the
differential input is `I·R_RTD` (lead resistances cancel for `I1 = I2`):

```
R_RTD = code / 2^23 × 2·RREF / gain
```

Per-channel LED: solid = channel active, off = disabled, 2 s fade in/out + 0.5 s dark pause (2.5 s cycle) =
fault (open/short/reversed sensor, ADC not responding). Factory defaults: all
four channels **enabled**, type **Pt1000**.

## Scales and α

Sensor types encode material + standard α; `W100 = R100/R0`:

| Code | Type | Material / standard | α | default W100 |
|---|---|---|---|---|
| 0/5/10/15 | `50М/100М/500М/1000М` | copper, GOST | 0.00428 | 1.4280 |
| 1/6/11/16 | `Cu50..Cu1000` | copper | 0.00426 | 1.4260 |
| 2/7/12/17 | `50П/100П/500П/1000П` | platinum, GOST | 0.00391 | 1.3910 |
| 3/8/13/18 | `Pt50..Pt1000` | platinum, IEC 60751 | 0.00385 | 1.3851 |
| 4/9/14 | `Ni100/Ni500/Ni1000` | nickel, DIN 43760 | 0.00617 | 1.6180 |
| 19 | `0…200 Ω` | resistance mode | — | — |
| 20 | `0…2000 Ω` | resistance mode | — | — |

User-level configuration: **sensor type** + **alpha mode** (0 = standard α for
the type, 1 = custom) + **custom W100 ×10000**. Platinum uses Callendar–Van
Dusen in (α, δ, β) form so a custom W100 only rescales α. Copper is linear with
a small sub-zero correction; nickel is the DIN polynomial scaled by α. R→T is
inverted numerically. Resistance modes return Ω (temperature = NaN).

## Gain classes (ranges)

Every type belongs to one of five gain classes; the class is chosen by type or
forced via registers `20..23` (for calibration). `FS = 4000 Ω / gain`.

| Class | Gain | FS | Types |
|---|---|---|---|
| 0 | 16 | 250 Ω | 50М, Cu50, 50П, Pt50 |
| 1 | 8 | 500 Ω | Ni100, 100М, Cu100, 100П, Pt100, `0…200 Ω` |
| 2 | 2 | 2000 Ω | Ni500, 500М, Cu500, 500П, Pt500 |
| 3 | 1 | 4000 Ω | Ni1000, 1000М, Cu1000, 1000П, Pt1000 |
| 4 | 1 | 4000 Ω | `0…2000 Ω` |

Fault detection (the ADS1220 has no fault register). Open sensor (1) is detected
by an **excitation-loop check**: every ~4 s each channel is switched for one scan
to the `(REFP0−REFN0)/4` system monitor; if no current flows through RREF
(`V_REF` < 20 % of nominal) the loop is open, regardless of ADC code or gain
(the +FS signature alone is unreliable at gain 1/2). The channel holds its last
value during the check and ~1 s afterwards. Additionally code ≥ `0x7FF000` → open
/ over-range (1); `R < 0.1·R0` for RTD types → short (2); config readback
mismatch → ADC not responding (3); code ≤ `−0x10000` (−0.5 % FS) → reversed (4):
the excitation is not flowing S+ → S− (S+/S− swapped, sensor wired between S−
and E, no return path through E). A 2-wire sensor goes between **S+ and E** with
a **S−–E** jumper. Faults are debounced: raised after 3 consecutive faulty scans,
cleared (`valid` again) after 3 consecutive good ones.

## Calibration

Each channel is calibrated per gain class with a linear model
`R_true = gain·R_raw + offset`, `R_raw = code/2^23 × 2·RREF_nom / gain_PGA`,
fitted over ≥ 2 reference points from a precision standard (АКИП-2202А,
0.05 %) over Modbus TCP. See [tools/README.md](tools/README.md).

### Write-once

Calibration lives in Flash sector 11, which the firmware **never erases** in
normal operation (survives factory reset). Each of the 20 slots (4 channels ×
5 classes) can be committed **exactly once**:

1. write `gain`/`offset` to `540+` (live preview, not persisted);
2. write `0xCA00 | (ch×5 + class)` to register `131` (**CAL COMMIT**) — the
   slot is programmed and locked;
3. lock status: input registers `127` (bits 0..15) and `128` (bits 16..19).

Writes to `540+` or a second commit for a locked slot return
`ILLEGAL DATA VALUE`.

### Emergency calibration erase (two-factor)

1. write `0xC1A5` to register `132` (**CAL ERASE ARM**) — opens a 30 s window;
2. hold `FACT_RES` for ~3 s within the window.

The sector is erased (all slots → `gain=1.0, offset=0.0`, unlocked) and the
module reboots. Neither action alone is sufficient.

## Modbus register map

Floats are IEEE-754 float32 in two registers, **high word first**. Multi-channel
quantities are grouped **by quantity**: 4 consecutive registers (or pairs) are
channels 0..3.

### Compact block — Holding Registers (FC03/06/16), 0..27

| Reg | Description |
|---|---|
| 0..3 | **reading, int16 (RO)**: RTD — temperature °C × 100 (−327.68…+327.67); resistance modes — 0..32767 scaled from the mode full scale (200 / 2000 Ω); disabled — `0`; fault / no valid sample — `−32768` (`0x8000`) |
| 4..7 | sensor type (`rtd_type_t`, 0..20) |
| 8..11 | enabled (0/1) |
| 12..15 | alpha mode (0 standard, 1 custom) |
| 16..19 | custom W100 × 10000 |
| 20..23 | gain-class override (0 auto, 1..5 = class 0..4; runtime only) |
| 24..27 | smoothing (0 off, 1 weak 1/4, 2 medium 1/8, 3 strong 1/16) |

### Readings — Input Registers (FC04), 300..339

| Reg | Type | Description |
|---|---|---|
| 300..307 | float32 ×4 | temperature, °C (NaN for resistance mode / fault) |
| 308..315 | float32 ×4 | calibrated resistance, Ω |
| 316..323 | float32 ×4 | raw resistance, Ω (calibration input) |
| 324..327 | u16 ×4 | flags: bit0 enabled, bit1 valid, bit2 fault, bits15..8 fault code (1 open, 2 short, 3 ADC, 4 reversed) |
| 328..335 | int32 ×4 | 24-bit signed ADC code (high word first) |
| 336..339 | u16 ×4 | active gain class (0..4) |

Global: `120` fw major, `121` fw minor, `122/123` uptime s, `125` module id
(`0x04D1`), `126` MCU temperature (signed 0.1 °C), `127`/`128` calibration
lock mask (bit `ch×5+class`).

### Global settings — Holding Registers

| Reg | Description |
|---|---|
| 100 | RTD scan period, ms (50..5000) |
| 101 | LED mode (0/1/2) |
| 102 | Modbus slave id | 103 | Modbus TCP port |
| 104..107 | static IP | 108..111 | netmask | 112..115 | gateway |
| 116 | net mode: `0` static / `1` DHCP / `2` link-local (default `2`) |
| 117 | SAVE (`0xA5A5`) | 118 | REBOOT (`0xB00B`) / BOOTLOADER (`0xB007`) / KSZ8863 reset (`0x8863`) |
| 119 | FACTORY RESET (`0xDEAD`) | 130 | MCU temperature (RO) |
| 131 | CAL COMMIT (`0xCA00 \| slot`, slot 0..19) |
| 132 | CAL ERASE ARM (`0xC1A5`) |

Calibration coefficients (float32), base `540 + ch×20` (**write-once**):
`+ class×4 + 0..1` gain, `+ class×4 + 2..3` offset, class 0..4.

Nominal reference resistor (float32, Ω): `620..621` (default 2000.0).

## Software smoothing

Per-channel EMA on the calibrated resistance (registers `24..27`): 0 off,
1 → α=1/4, 2 → 1/8, 3 → 1/16. Raw resistance and ADC code stay unfiltered.
The filter restarts (seeded with the first valid sample) after a fault, a
class/type change or any channel configuration change.

Settling time: `t(63 %) ≈ T_scan/α`, `t(95 %) ≈ 3·T_scan/α`. With
`T_scan = 250 ms`: weak ~1/3 s, medium ~2/6 s, strong ~4/12 s. The ADC runs
at 20 SPS: with a scan period below 50 ms consecutive samples may repeat.

## Flash layout

| Region | Address | Size | Purpose |
|---|---:|---:|---|
| Bootloader | `0x08000000` | 128 KB | sectors 0-4 |
| Metadata | `0x08020000` | 128 KB | sector 5 |
| Application | `0x08040000` | 256 KB | sectors 6-7 (this firmware) |
| Staging | `0x08080000` | 256 KB | sectors 8-9 |
| Settings | `0x080C0000` | 128 KB | sector 10 |
| Calibration | `0x080E0000` | 128 KB | sector 11 (write-once) |

`SETTINGS_MAGIC = 0x04D14A57`, `SETTINGS_VERSION = 3` (HW2.1: new type codes
19/20 and class-override semantics; layout unchanged). Calibration: 20
CRC-protected slots (`ch×5+class`, magic `0xCA11B005`).

## Build

STM32CubeCLT (`starm-clang`, `cmake`, `ninja`):

```powershell
cmake --preset Debug
cmake --build --preset Debug
```

Identity — single source `Application/fw_header/fw_header.h`:
`FW_PRODUCT_ID=0x504C0403`, `FW_HW_REVISION=0x0201`, `FW_VERSION_VALUE=0x0208`.
OTA accepts an image when product_id and the hw_revision major byte match —
HW1.x (`0x01xx`) and HW2.x (`0x02xx`) images are mutually incompatible.

## Flashing

```powershell
STM32_Programmer_CLI -c port=SWD -w build/Debug/PLCJS_ETH_MODULE_4RTD_D4MG_STM32F407VGT6.elf -v -rst
```

## Network defaults

Net mode `2` = link-local (factory): `169.254.<mac[4]>.<mac[5]>` /16 derived
from the UID, discoverable by MAC (UDP broadcast port `20556`). Static
fallback fields: IP `192.168.1.10`, mask `255.255.255.0`, gateway
`192.168.1.1`. Modbus TCP port `502`, unit id `1`. Changes apply live after
`TRIG_SAVE`.

## Tools

`tools/calibrate.mjs` (Node.js 18+) and `tools/calibrate.py` (Python 3.8+) —
interactive calibration and configuration over Modbus TCP. See
[tools/README.md](tools/README.md).

## Accuracy notes

- Excitation 500 µA: the IDAC output `0.5 mA·R + 2.0 V` must stay below
  `AVDD − 0.9 V ≈ 4.1 V`, i.e. `R ≲ 4200 Ω` — Pt1000 at 850 °C (3905 Ω) fits.
- The ADS1220 internal temperature sensor is not used (TS mode replaces the
  input measurement); the MCU temperature is available (`ADC1_IN16`, regs
  126/130).

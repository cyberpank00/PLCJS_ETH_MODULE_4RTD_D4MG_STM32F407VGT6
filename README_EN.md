# PLCJS_ETH_MODULE_4RTD_D4MG_STM32F407VGT6

Firmware for the PLCJS Ethernet module with 4 RTD analog inputs, based on
`STM32F407VGT6`.

The module measures four RTD channels through isolated `MAX31865` ADCs (SPI),
converts resistance to temperature for the selected scale (GOST 6651 /
IEC 60751 / DIN 43760), publishes readings over `Modbus TCP`, stores settings
and calibration in internal Flash, drives a main `STAT_LED` plus four
per-channel LEDs, and supports factory reset and OTA via the Ethernet
bootloader. Network / Modbus / settings / LED / bootloader infrastructure is
inherited from the `12DI` variant; the analog front-end is new.

## Hardware

- MCU `STM32F407VGT6` (Cortex-M4F); Ethernet `KSZ8863` (RMII).
- 4× `MAX31865`, galvanically isolated (ADuM5000 + ADuM1400/1311), shared SPI1.
- 3-wire RTD wiring (2-wire via a physical jumper).
- Per-channel range switch `ADG849` (`RANG` pin) selects the reference resistor.

Pinout: SPI1 SCLK=PA5, MISO=PA6, MOSI=PB5. CS: PD13/PD9/PB15/PB10.
RANG: PD10/PD8/PB14/PE15. Channel LEDs: PE14/PE13/PE12/PE11.
SPI mode 3, 8-bit, ~2.6 MHz. MAX31865: 3-wire, auto conversion, VBIAS on,
50 Hz filter.

Per-channel LED: solid = channel active, off = disabled, fast blink = ADC fault.

## Scales and α

Sensor types encode material + standard α; `W100 = R100/R0`:

| Type | Material / standard | α | default W100 |
|---|---|---|---|
| `50М/100М/500М/1000М` | copper, GOST | 0.00428 | 1.4280 |
| `Cu50..Cu1000` | copper | 0.00426 | 1.4260 |
| `50П/100П/500П/1000П` | platinum, GOST | 0.00391 | 1.3910 |
| `Pt50..Pt1000` | platinum, IEC 60751 | 0.00385 | 1.3851 |
| `Ni100/Ni500/Ni1000` | nickel, DIN 43760 | 0.00617 | 1.6180 |
| `0…2k`, `0…5k` | resistance mode | — | — |

User-level configuration: **sensor type** + **alpha mode** (0 = standard α for
the type, 1 = custom) + **custom W100 ×10000**. Platinum uses Callendar–Van
Dusen in (α, δ, β) form so a custom W100 only rescales α. Copper is linear with
a small sub-zero correction; nickel is the DIN polynomial scaled by α. R→T is
inverted numerically. Resistance modes return Ω (temperature = NaN).

## Ranges

`ADG849` selects: low ≈ 400 Ω (2×200) for 50/100 Ω sensors; high ≈ 4000 Ω
(2×2k) for 500/1000 Ω sensors and resistance modes. Range is auto-selected by
sensor type.

## Calibration (0.2 %)

Because of the analog-switch on-resistance and RREF tolerance, each channel is
calibrated per range with a linear model `R_true = gain·R_raw + offset`, fitted
over ≥ 2 reference points from a precision standard (АКИП-2202А, 0.05 %) over
Modbus TCP. Coefficients are stored in Flash. See
[tools/README.md](tools/README.md).

## Modbus map

Floats are IEEE-754 float32 in two registers, high word first.

**Input registers (FC04)** — readings, per channel base `300 + ch*20`:
+0..1 temperature °C, +2..3 calibrated resistance Ω, +4..5 raw resistance Ω,
+6 flags (bit0 enabled, bit1 valid, bit2 fault, bits15..8 fault byte),
+7 ADC code, +8 range. Globals: 120/121 fw ver, 122/123 uptime, 125 module id
(`0x04D1`), 126 MCU temperature (0.1 °C).

**Holding registers (FC03/06/16)** — config. Global: 100 scan period ms,
101 LED mode, 102 slave id, 103 TCP port, 104..115 IP/mask/gw, 116 DHCP,
117 SAVE (`0xA5A5`), 118 REBOOT/BOOT, 119 FACTORY RESET, 130 MCU temp.
Per channel base `500 + ch*10`: +0 enabled, +1 sensor type, +2 alpha mode,
+3 custom W100 ×10000, +4 calibration range override. Calibration floats base
`540 + ch*8`: gain/offset for low and high ranges. Nominal RREF floats:
580..581 low, 582..583 high.

## Build

```powershell
cmake -S . -B build/Debug -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=cmake/starm-clang.cmake
cmake --build build/Debug
```

Variant identity: `PRODUCT_ID=0x04D1D4A0`, `HW_REVISION=0x0101`,
`FW_VERSION=0x0100`. Outputs `.elf/.hex/.bin` in `build/Debug/`.

Default network: DHCP on, static fallback `192.168.142.150/24`, gateway
`192.168.142.1`, Modbus TCP `502`, unit id `1`.

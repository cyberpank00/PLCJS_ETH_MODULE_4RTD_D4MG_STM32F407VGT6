# AGENTS.md

Firmware for the PLCJS Ethernet 4RTD module (4x RTD temperature inputs via
ADS1220, STM32F407VGT6, KSZ8863 switch, Modbus TCP). **Board HW2.1**; the
HW1.x firmware (MAX31865 + ADG849 range switch) is tagged `hw1.1-last`.
This file is the orientation map for agents; user-facing documentation lives in
`README.md` / `README_EN.md`.

Derived from the 12DI variant (`Initial 4RTD firmware adapted from 12DI
variant`), then extended with the analog acquisition and calibration chain. It
is the most complex module of the family and the one that lags furthest behind
its siblings — see *Known drift*.

## Build (CMake)

Toolchain: STM32 Arm Clang (`starm-clang`) from STM32CubeCLT, generator Ninja.
Toolchain file: `cmake/starm-clang.cmake`. Presets in `CMakePresets.json`.

```
cmake --preset Debug
cmake --build --preset Debug
```

Release: substitute `Release`. Build output:
`build/<preset>/PLCJS_ETH_MODULE_4RTD_D4MG_STM32F407VGT6.{elf,hex,bin,map}`.

Clean rebuild: delete `build/<preset>` and re-run the configure step.

No host-side unit tests. "Verified" means it compiles, and where the change is
observable it was exercised against hardware.

### Tools required
- CMake >= 3.22, Ninja
- `starm-clang` (STM32CubeCLT) on PATH. Alternative GCC toolchain at
  `cmake/gcc-arm-none-eabi.cmake`.
- Node 18+ or Python for `tools/calibrate.mjs` / `tools/calibrate.py`.

## Repository layout

| Path | Owner | Notes |
|---|---|---|
| `Application/` | hand-written | All real logic. Edit here. |
| `Core/`, `Drivers/`, `Middlewares/`, `LWIP/`, `cmake/stm32cubemx/` | STM32CubeMX | Regenerated from the `.ioc`. |
| `startup_stm32f407xx.s`, `STM32F407XX_FLASH.ld` | hand-edited | Diverged from CubeMX output — see *Linker*. |
| `tools/` | hand-written | Calibration helpers (`calibrate.mjs`, `calibrate.py`, own `README.md`). |

**CubeMX regeneration hazard.** Regenerating from the `.ioc` overwrites `Core/`,
`Drivers/`, `Middlewares/`, `LWIP/` and `cmake/stm32cubemx/CMakeLists.txt`, some
of which carry hand edits outside `USER CODE` guards (notably
`LWIP/Target/ethernetif.c` and `lwipopts.h`). Diff carefully afterwards.

## Module map (`Application/`)

| Module | Responsibility |
|---|---|
| `app/` | Orchestrator: boot order, factory reset, network bring-up, housekeeping loop. Start here. |
| `spi/` | SPI1 transport (mode 1) shared by the four ADS1220 front-ends. |
| `ads1220/` | ADS1220 driver: reset/config/RDATA, per-channel PGA gain, liveness check. Board wiring and register values are documented in its header. |
| `rtd/` | Acquisition + conversion: 24-bit code → resistance → temperature, gain-class resolution, code-based fault detection, int16 view, scan loop. |
| `calstore/` | **Write-once** per-channel/per-gain-class calibration store in Flash (20 slots). Read this file before touching calibration. |
| `temp/` | On-chip MCU temperature sensor, exposed as IR126 / HR130. |
| `modbus/modbus_app.c` | Register-map adapter. **The map is documented in the header comment of `modbus_app.h`.** |
| `modbus/modbus_tcp_server.c` | Multi-client (4 slots) TCP server on LwIP netconn; when full, the longest-silent client is evicted (newest-wins). |
| `settings/` | Flash-backed settings, CRC32-protected. |
| `discovery/` | PDP responder, UDP/20556 broadcast. |
| `net_id/` | MAC and link-local IPv4 derived from the 96-bit MCU UID. |
| `ksz8863/` | SMI/MIIM driver for the Ethernet switch. |
| `led/`, `button/` | STAT_LED state machine; `chled_pwm.c` — software PWM (TIM7 ISR, 64 levels @ 200 Hz) for the four channel LEDs, used by `rtd_module` for the fault fade (2 s ramp + 0.5 s dark); FACT_RES button. **TIM7 is taken.** |
| `fw_header/` | Firmware image header consumed by the bootloader. Module identity. |
| `third_party/nanomodbus/` | Vendored protocol library. |

### Register map shape
Multi-channel quantities are grouped **by quantity** (4 registers or 4 pairs =
channels 0..3), not per channel. `float32` is two registers, **high word first**.

- Compact block (FC03/06/16) `0..27`: `0..3` int16 reading (RO: °C×100, or
  0..32767 of the resistance-mode full scale; 0 = disabled, −32768 = fault),
  `4..7` type, `8..11` enabled, `12..15` alpha mode, `16..19` W100×10000,
  `20..23` gain-class override (0 auto / 1..5), `24..27` EMA level (0..3,
  α = 1/4, 1/8, 1/16; applied to r_cal, raw values stay unfiltered).
- Readings (FC04) `300..339`: temp f32 ×4, R_cal f32 ×4, R_raw f32 ×4,
  flags ×4 (fault code in bits 15..8: 1 open, 2 short, 3 ADC dead, 4 reversed), ADC code
  int32 ×4, active gain class ×4.
- Calibration coefficients (FC03/06/16): `540 + ch*20 + class*4` — gain, offset.
- Nominal RREF: `620..621` (single, default 2000 Ω).
- `IR127`/`IR128` = calibration lock bitmask, bit `ch*5 + class`.

Temperature reads `NaN` for resistance-only modes and on fault. The int16
resistance view is clamped to 32767 so `0x8000` stays reserved for fault.

## Invariants

### Calibration is irreversible — treat it as destructive

`calstore/` implements a **write-once** store: each of the 20 (channel × gain
class) slots can be committed exactly once, because internal Flash can only clear bits
without a full sector erase.

- Modbus writes to `540 + ch*20 + class*4` are a **live preview only**, and are
  rejected once the slot is locked.
- Committing is `HR131 = 0xCA00 | (ch*5 + class)`. **This is irreversible.**
- The only undo is `calstore_erase()`, which wipes the whole sector and reverts
  all 20 slots to neutral (gain 1.0, offset 0.0). It is deliberately gated behind
  two factors: arm with `HR132 = 0xC1A5`, then physically confirm with a button
  hold within 30 s (`CAL_ERASE_ARM_WINDOW_MS` / `CAL_ERASE_CONFIRM_MS`).
- Never issue a commit or an erase while testing, and never add a code path that
  can commit without explicit operator intent. Losing factory calibration means
  the board must be re-calibrated against reference resistors.

The calibration sector is **never** erased by settings save or factory reset.
Keep it that way.

### Single sources of truth
- **Module identity** — `Application/fw_header/fw_header.h`:
  `FW_PRODUCT_ID = 0x504C0403`, `FW_HW_REVISION = 0x0201`,
  `FW_VERSION_VALUE = 0x0207`.
- **Firmware version over Modbus** — IR120/IR121 derive from `FW_VERSION_VALUE`.
- **Register map** — the header comment of `modbus_app.h`, mirrored by the
  `MB_*` constants. Keep comment and constants in step.
- **Module ID** — `MODULE_ID_04RTD = 0x04D1`, reported in IR125. (Note: the
  bootloader's `variants.csv` calls this variant `4rtd`; an unrelated `0x04DD`
  "4RD" relay variant also exists in ModbusTool's decoder.)

### Version policy — bump the minor on every change

**Mandatory.** Every change to firmware behaviour ships with `FW_VERSION_VALUE`
in `fw_header.h` incremented by one minor (`0x0207` → `0x0208`). The version is
the operator's only way to tell which build is running on a device in the field,
so an un-bumped change is a defect.

- Minor bump: any firmware-only change — fixes, features, register-map
  additions, timing or conversion-maths changes.
- Major bump: only together with a `FW_HW_REVISION` major change (MCU pinout).
  OTA requires `fw_version` major == `hw_revision` major. HW2.1 (ADS1220
  board) is exactly that case: hw `0x0201`, fw `0x0200`.
- Pure documentation-only commits do not need a bump.

Bump checklist — all three places, they drift easily:
1. `FW_VERSION_VALUE` in `fw_header.h`.
2. Version rows in `README.md`.
3. The same rows in `README_EN.md`.

A chronological version-review / changelog file is planned; once it exists, add
an entry there in the same commit as the bump.

### Persistence and Flash sectors

| Sector | Address | Content |
|---|---|---|
| 10 | `0x080C0000` | Settings (`settings.c`) |
| 11 | `0x080E0000` | Write-once calibration (`calstore.c`) |

- `settings_t` layout is frozen; reordering or resizing requires bumping
  `SETTINGS_VERSION` (currently 3, magic `0x04D14A57`). A mismatch silently
  reverts deployed units to factory defaults. v3 kept the v2 layout but was
  bumped because type codes 19/20 and the cal-override register changed
  meaning (`rref_nominal[1]` became `reserved_f`).
- The field is still named `use_dhcp` but holds a tri-state net mode
  (static / DHCP / link-local). Kept for on-flash compatibility.
- `ch_smooth[]` (EMA level) repurposed the always-zero `reserved_a[]` bytes, so
  no version bump was needed: deployed units read 0 = smoothing off.
- **Sector 11 conflict:** the bootloader's `flash_map.h` nominally lists sector
  11 as a third staging sector (currently unused — staging is sectors 8–9 only).
  If the bootloader is ever extended to use it, 4RTD calibration is destroyed.
  Any change to bootloader staging must account for this repo.

### Threading
- LwIP calls must run in the tcpip thread; the live network re-apply goes through
  `tcpip_callback()`.
- Flash writes and resets requested over Modbus/discovery are deferred to the
  housekeeping loop in `app_run()` via the `*_take_pending_*()` flags. Calibration
  commits and the sector erase block the CPU for a long time — they must run from
  housekeeping, never from the tcpip thread.
- Any loop blocking longer than the IWDG period must call
  `HAL_IWDG_Refresh(&hiwdg)`. The calibration-sector erase is the worst offender.
- All ADS1220 SPI access should stay on the RTD scan task.

### Boot order (`app_run()`)
Two ordering constraints inherited from 12DI, both load-bearing:
- The LED task starts **before** the FACT_RES button check, otherwise the
  factory-reset blink is silently dropped.
- Factory reset writes Flash **before** the visual confirmation: the sector erase
  blocks the CPU for ~1–2 s and would freeze the blink.

## Gotchas

- **Device name is 15 chars + NUL in a fixed 16-byte field**, and the PDP
  IDENTIFY response is a fixed 38 bytes. Must stay identical across every module
  variant and ModbusTool.
- Modbus TCP serves up to 4 clients from one task (round-robin, 2 ms
  first-byte poll per idle slot). Only when all 4 slots are busy does a new
  connection evict the longest-silent client; a silent client is dropped after
  30 s. Register callbacks are shared and sequential — last write wins. Needs
  `MEMP_NUM_NETCONN/NETBUF/TCP_PCB = 8` in `lwipopts.h`.
- STAT_LED `POLLING` (double blink) is driven by *request recency only*: one
  `LED_POLLING_PERIOD_MS` window after the last valid Modbus request, no
  "client connected" condition. Clients that open a TCP connection per request
  are still polling.
- `LED_STATE_FACTORY_RESET` is sticky until reboot and overrides all other states.
- HR118 multiplexes distinct magics: `0xB00B` reboot, `0xB007` bootloader,
  `0x8863` KSZ8863 switch reset (operator recovery).
- HR117 = `0xA5A5` save, HR119 = `0xDEAD` factory reset, HR131 = calibration
  commit, HR132 = calibration-erase arm.
- HR130 (on-chip temperature) is read-only despite living in holding-register
  space; the same value is also at IR126.
- `float32` registers are high-word-first. Getting the word order wrong produces
  plausible-looking garbage rather than an obvious error.
- ADS1220 has **no fault register**: open/short/reversed are inferred from the
  code (`RTD_CODE_OPEN`, `RTD_SHORT_FRACTION`, `RTD_CODE_REVERSED` in
  `rtd_module.c`) and a dead converter from a config-register readback mismatch.
  Resistance modes skip the short check because 0 Ω is a valid input there.
  Faults are debounced (`RTD_FAULT_CONFIRM_TICKS` scans to raise, the settle
  window to clear) — do not bypass `set_fault()` / `clear_fault()`.
- **Open detection is the excitation-loop check, not the +FS code.** At gain
  1/2 an open sensor drives the PGA inputs to the rail and the output is
  undefined (one die reads −55 % FS, another wanders near zero and looks like a
  valid −170 °C). `loop_check_step()` in `rtd_module.c` round-robins one
  channel at a time into the `(REFP0−REFN0)/4` monitor (`RTD_LOOP_*`); the
  channel holds its last status meanwhile. Keep `VREF = REF0` in
  `ads1220_enter_ref_monitor()` — the VREF bits select which pair is monitored
  (with VREF = internal the monitor reads 0 and every channel goes OPEN).
- **The config-readback liveness check cannot detect a stuck-selected chip.** A
  CS line that never reaches its ADS1220 (isolator CA-IS3740**L** defaults its
  output LOW → chip permanently selected) makes that chip answer every
  transaction together with the addressed one. Register readbacks still match
  (all selected chips receive the same write), internal monitors (die
  temperature, AVDD/4) read clean, and only external-input conversions come out
  as bit-mixed garbage (codes like `0x3FFFFF`, `0x1FFFFF`, differential ≈ 0).
  Seen on the first HW2.1 board (unsoldered CS resistor-array pin). Diagnose by
  writing distinct `reg0` values per chip and reading all four back.
- Gain classes and their PGA codes live in one table (`s_gclass[]` in
  `rtd_scales.c`) so the register value and the maths cannot drift. Pt500 at
  850 °C is 97.6 % of the class-2 full scale — a deliberate trade-off.
- DRDY is not wired; the ADC runs continuously at 20 SPS and `RDATA` returns
  the latest result. A scan period below 50 ms re-reads the same sample.
- SPI is **mode 1** (CPOL 0, CPHA 1). The MAX31865 needed mode 3.

## Parity with the other variants

This repo was branched from 12DI. The shared-subsystem catch-up has been done:
`ksz8863.c/h`, `nanomodbus.c`, `discovery.c`, `net_id.c`, `button_module.c` and
`modbus_tcp_server.c` are now **byte-identical** to 12DQ/12DI (which agree with
each other). The KSZ8863 cold-boot reset policy, the SMI-dead recovery service,
the HR118 `0x8863` operator command and the FC15/FC16 malformed-frame hardening
are all present.

Deliberately **not** aligned, because they are module-specific:
- `led_module.c/h` — 4RTD adds `LED_STATE_CAL_ARMED` and
  `led_module_signal_cal_erase()` for the calibration-erase indication.
- `settings.c/h` — RTD channel defaults instead of DI filter / DQ masks.
- `ethernetif.c` — hostname is `PLCJS-ETH-4RTD`; only the `ksz8863_service()`
  wiring was taken from the reference.
- Everything under `spi/`, `ads1220/`, `rtd/`, `calstore/`, `temp/`.

Still, do not assume a shared file matches its sibling — diff it before relying
on it.

## Linker / memory contract with the bootloader

`STM32F407XX_FLASH.ld` is **not** a stock CubeMX script: `FLASH` origin is
`0x08040000` (256 K application slot — the image only runs via the bootloader),
and `RAM` length is `0x1FFF0` so the top 16 bytes can hold the no-init
boot-request cell at `0x2001FFF0` (`BOOT_REQUEST_MAGIC = 0xB007CAFE`).
`.fw_header` is padded to offset `0x200`.

`fw_header_t` must stay byte-identical to `fw_header_t` in the bootloader's
`Application/validate/app_validate.h` (28 bytes, packed). CRC32 and image size
travel in OTA metadata, not the header.

OTA acceptance: `product_id` exact match **and** `hw_revision` major byte match.

## Multi-repo workspace

Sibling repos under `E:\STM_Programming\`:

| Repo | Role |
|---|---|
| `PLCJS_ETH_MODULE_4RTD_D4MG_...` | This module — `0x504C0403` / IR125 `0x04D1`. |
| `PLCJS_ETH_MODULE_12DI_D4MG_...` | 12 discrete inputs, `0x504C1201` / `0x12D1`. This repo's ancestor; shared subsystems originate there. |
| `PLCJS_ETH_MODULE_12DQ_D4MG_...` | 12 discrete outputs, `0x504C1202` / `0x12D0`. |
| `BOOTLOADER_PLCJS_ETH_MODULE_STM32F407VGT6` | Shared bootloader (serves every variant). Owns `flash_map.h`, `app_validate.h`, `scripts/variants.csv`. |
| `PLCJS_Module_ModbusTool` | Qt6/C++17 desktop client. `build4RTD()` in `src/maps/ModuleMaps.cpp` mirrors this module's register map, including the `float32` rows. |

**`Application/` subsystems are copy-pasted between firmware variants, not shared
via a submodule.** A fix here is not a fix elsewhere, and vice versa.

Cross-repo contracts that must change in lockstep:
- **Wire format** (PDP frame layout, 38-byte IDENTIFY, 16-byte name) — every
  firmware + `Pdp.cpp`.
- **`fw_header_t` layout, `FW_HEADER_OFFSET`, `BOOT_REQUEST_FLAG_ADDR`/`MAGIC`,
  flash map** — every firmware + bootloader + both linker scripts.
- **product_id** — `fw_header.h` here and `scripts/variants.csv` in the
  bootloader (`4rtd,0x504C0403,0x020100`). `variants.csv` uses the 3-byte hw
  encoding `0x020100`; firmware headers use the 2-byte `0x0201`. The bootloader
  compares the major byte only, so HW1.x and HW2.x images are mutually
  rejected.
- **Register map changes** — `modbus_app.h` here and `build4RTD()` in
  `ModuleMaps.cpp`, or the tool shows stale registers.

## Maintaining this file

`AGENTS.md` is a living document, not a one-time write. Update it **in the same
commit** as the change it describes — a stale map is worse than no map, because
it actively misleads. Touch it when:

- an invariant, gotcha or threading rule is added or changes — especially the
  calibration write-once / sector-11 rules, which are safety-critical;
- a module is added, removed or repurposed (`Application/` map);
- the build procedure, toolchain or linker contract changes;
- a register-map change alters the header comment of `modbus_app.h`;
- `FW_VERSION_VALUE` is bumped and the version-policy text needs the new
  example value;
- a cross-repo contract changes (PDP wire format, `fw_header_t`, flash map,
  `product_id`) — update the *Multi-repo* section here **and** the corresponding
  section in the sibling repo(s).

Pure refactors with no behavioural change do not require an update, but when in
doubt, update — the cost is a few lines of text, the cost of a stale invariant
is a field bug.

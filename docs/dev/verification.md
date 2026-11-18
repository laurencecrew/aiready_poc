# Verification log and checklist

How each fact in [hardware.md](hardware.md) was established, and what is still open.
Tools: probe firmware in [../../firmware/probe/](../../firmware/probe/) (serial console, 115200 baud, type `help`).

## How to run the probe

```powershell
cd firmware/probe
pio run -t upload --upload-port COM5
pio device monitor -b 115200
```
Close Arduino IDE / other monitors first. The probe boots with heater/bed/fan LOW and stepper drivers disabled. Any power output switched on by a command is forced off after 5 s.

To restore the original Marlin (strips the bootloader/descriptor regions the dump contains, keeps the descriptor block):
```powershell
python -m aiready flash firmware/backup/marlin_1.1.0-RC6_flash.hex
```
Do **not** feed that dump to avrdude directly any more: it covers 0x00000–0x3FD1D, so it would write the descriptor/nop-fill pages the bootloader cannot erase, and the bootloader's own pages (see hardware.md §2.1).

## Done (2026-11-17)

| # | Item | Method | Result |
|---|---|---|---|
| 1 | Original firmware identity | Serial at 250000, `M115`, `M503` | Marlin 1.1.0-RC6, built Jul 13 2016, default config (author "none") with LCD menus |
| 2 | Firmware backup | avrdude 6.3 read via bootloader | `firmware/backup/marlin_1.1.0-RC6_flash.hex` + `_eeprom.hex` |
| 3 | Bootloader | Raw STK500v2 sign-on → "AVRISP_2"; flash strings | "Arduino explorer stk500V2 by MLS", 115200, ~1.24 s window |
| 4 | avrdude compatibility | Tried 8.0, 8.1, 6.3 | **Only 6.3 syncs.** 8.x times out |
| 5 | MCU, fuses | avrdude + probe `info` | ATmega2560 1E9801, L FF / H D8 / E FD, 16 MHz |
| 6 | Pull-up survey | probe `pins` (USB power only, nothing connected but TEMP0 thermistor) | External pull-ups: D2,3,14,15,18,19 (endstops); D24,38,56,62 (driver EN); D31,33,35 (encoder); D41; D49; D20,21 (I2C); D51, D53; A13, A14. Held LOW: D4, D6, D7, D13, D37. Everything else floats |
| 7 | Thermistor inputs | probe `temp`/`adc` | A13 = 969–970 ≈ 29 °C with thermistor on TEMP0 ✔. A14 = 1023 (pulled-up, open). A15 floats |
| 8 | I2C | probe `i2c` | No devices; bus has pull-ups |
| 9 | SD | probe `sd` (no card) | D49 = 1 (no card), `SD.begin` fails as expected |
| 10 | Display controller & pins | Disassembly of Marlin backup + probe `lcd7920` (negative) + `lcd7567` (positive) | **ST7565/UC1701-family, HW SPI, CS 29, A0 27, RST 25.** ST7920 on 16/17/23 shows nothing. Test pattern displayed correctly |
| 11 | Stepper / heater / temp pins | Disassembly (port bit writes, temp ISR, ADMUX) | Match RAMPS map (see [F] tags) |
| 13 | Push button | User pressed it | It is a hardware **RESET** (reboots MCU); not the RRD KILL input on D41 |
| 12 | All MOSFET outputs | Vin connected; probe `pulse 8 1000`, `pwm 9 128`, `pwm 9 255`, `pulse 10 1000` | **Confirmed by user**: D8 → BED, D9 → FAN (PWM works), D10 → HEATER. No other fan/LED outputs exist on the board |
| 14 | Stepper drivers X, Y, Z, E0 | User test with probe `step` | **Confirmed by user**: all four axes work on the RAMPS pins |
| 15 | Encoder + push | Probe `in` | **Confirmed by user**: D31/D33 encoder, D35 push |
| 16 | X and Z endstops | Probe `in`, S shorted to GND | **Confirmed by user**: X-MIN D3, X-MAX D2, Z-MIN D18, Z-MAX D19 |
| 17 | TEMP1 | Thermistor on TEMP1, probe `temp` | **Confirmed by user**: TEMP1 = A14 |
| 20 | Y endstops | `in` (Y-MAX), `watchall` (Y-MIN), S shorted to GND | **Y-MIN = D40, Y-MAX = D41** (port G). NOT RAMPS D14/D15 |
| 21 | Beeper | `beep` | **Confirmed by user**: D37 beeps |
| 22 | Stepper driver chip | User read marking | "4988ET" = **Allegro A4988** (SET package, 28-pin QFN) |
| 23 | Microstepping | `step x 3200 …` with 200-step motor | **1/16**: 3200 steps = exactly 1 revolution |
| 25 | Watchdog | probe `wdt` (WDTO_1S, then hang) | **Reboots after ~1 s, no boot-loop.** Banner shows MCUSR=0x0: the bootloader clears MCUSR, so reset cause is not visible to the application |
| 26 | U8g2 driver match (round 1) | `firmware/lcdtest` (frame, corner block, text; cycle drivers/rotations/contrast) | **`U8G2_ST7565_ZOLEN_128X64` + `U8G2_MIRROR` readable, contrast 40–70 good, but wraps ~5–10 px.** UC1701_MINI12864: blank. ST7565_64128N: blank. ST7565_EA_DOGM128: wrapped. Round 2 below |
| 27 | U8g2 offset / mirror (round 2) | `firmware/lcdtest`: tune `x_offset`, mirror method | **R0 + `sendF("c",0xA0)` + x_offset 0 = correct (chosen).** SW `U8G2_MIRROR` + x_offset 4 = also correct. 0xA1 + x_offset 4 = aligned but mirrored |
| 24 | Sense resistors / trimpots | User inspection | 2 × "050" (0.05 Ω) per driver ⇒ Vref = 0.4 × I. 3 trimpots, next to E0, Y, Z; none by X |
| 18 | Display | (= item 10) | ST7565 confirmed again by user |
| 19 | Heater / bed / PWM fan | (= item 12) | Re-confirmed by user |

## Done (2026-11-18): AI-ready self-description POC

| # | Item | Method | Result |
|---|---|---|---|
| 28 | Descriptor survives app uploads | `aiready write-descriptor`, then plain `pio run -t upload` of `firmware/lcdtest`, then `aiready describe --source bootloader` | **Same descriptor id read back.** Pages beyond an uploaded image are never touched |
| 29 | Bootloader WDT path | `@ai reboot` hung the board; disassembly of the boot section | **`EIND=1` + `eicall` ⇒ WDT reset jumps to 0x20000** and slides to the bootloader through blank flash. A block in the slide path must start with `jmp 0x3E000`. Fixed: reboot works, banner after ~2 s, reset cause "soft" |
| 30 | Bootloader page erase | Rewriting the block gave old AND new data; app pages 0–26 found blank | **Sequential-erase bug**: erases page N (from 0x0000) for the Nth page written, never at ≥ 0x3C000. Block moved to 0x3A000; descriptor written as one contiguous image; rewrite of an existing block verified |
| 31 | No-reset serial open | Breadcrumbs in `.noinit` (boot count, last-alive time) | DTR-disabled open after a DTR-enabled session **hangs the MCU** (Windows DCB re-applied on open). Fixed host-side by parking DTR/RTS in the DCB before close; consecutive queries keep the app running |
| 32 | Runtime commands | `aiready state/pins/peek/safe/clear/watch`, unknown command, app line | All answer as specified; `peek` masks UDR0 as `xx` |
| 33 | Flash + backup + restore | `aiready flash` (auto backup), restore of that backup | Backup 34 s, write + verify ~8 s; restore strips 0x3A000+ and trailing blanks, verifies |
| 34 | MCP server | Tools called through the server: `describe`, `get_state` | Same JSON as the CLI; descriptor served from cache keyed by the on-board id |

Notes:
- `-D` uploads never erase above the image, so readbacks contain **stale pages from older, larger firmware** (e.g. 101 KB "used" after a 15 KB lcdtest over Marlin). `app_sha` in backups covers those stale bytes too.
- Thermistors on TEMP0/TEMP1 both read ~27–29 °C at room temperature.

## Open items: suggested procedure

| # | Item | How | Expected / notes |
|---|---|---|---|
| M | Live state with real inputs | `aiready watch --seconds 10` while shorting each endstop S to GND, turning/pressing the encoder, and warming the X driver/motor thermistor by hand | Endstop bits go 1, `encoder.raw` changes (check `AI_ENC_STEPS_PER_DETENT`, assumed 4), temperatures rise |
| N | Thermal guard | Unplug one thermistor while watching; then `aiready clear` | `guard.tripped` = 1 with reason `*_sensor_open`, `drivers_enabled.x` = 0; re-trips after clear while unplugged |
| O | Agent test | Restart Claude Code in this folder (loads `.mcp.json`), ask "what is connected to this board and what can it do?" | Answer comes from the `aiready` tools, not from docs |
| P | DIR polarity | `aiStepperMove(AI_AXIS_X, 3200, 800)` and note direction | Record which way positive steps turn each axis (currently `[?]`) |

## Older open items

| # | Item | How | Expected / notes |
|---|---|---|---|
| D | D4 / D6 / D7 / D13 | Trace from TQFP pins 1 / 15 / 16 / 26 (meter continuity to nearby resistors/headers). Optionally watch onboard LEDs during `pulse 13 1000` | Identify what each connects to (no extra load outputs exist) |
| G | ESP-01 UART | Insert ESP-01 (AT firmware). `esp 2`, then `esp 3`, `esp 1` (also try 9600 baud) | "OK" reply identifies the UART. Serial2 and Serial3 are both free candidates |
| H | Which pot sets X current; present Vref values | Meter: measure each pot wiper vs GND (Vref). For X: meter continuity from each pot wiper to the X driver's REF pin, or energise X (`out 38 0`) and feel holding torque change while turning the Y pot | Vref = 0.4 × I. Expect X shares the Y pot |
| K | Physical inspection / photos | Read markings: MOSFETs, regulators (5 V, 3.3 V), polyfuses, any unpopulated headers (ICSP? AUX?) | Fill in hardware.md §5, §3.9 |
| L | D16/D17/D23 usage | Visual trace or `watchall` while probing ESP header pins with a jumper to GND | Confirm free / ESP |

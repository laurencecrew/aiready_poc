# FYSETC One+ V2.2 (AIO v1) — Hardware Reference

Machine-oriented reference for writing custom firmware for this board.
Pin numbers are **Arduino Mega 2560 pin numbers** unless noted. A C header with the same map is at [../firmware/include/one_plus_pins.h](../firmware/include/one_plus_pins.h).

**Status tags**:
- **[V]** verified on this physical board (see [verification.md](verification.md))
- **[R]** RAMPS 1.4 standard, consistent with tests so far but not individually confirmed
- **[F]** confirmed from the original vendor firmware (disassembly of the Marlin 1.1.0-RC6 backup), not yet exercised electrically
- **[?]** unknown, inferred or guessed. Verify before relying on it.

> Not to be confused with the FYSETC **AIO II**, which has a similar form factor but uses an STM32. Nothing from AIO II docs applies here.

---

## 1. Board summary

| Item | Detail | Status |
|---|---|---|
| Board | FYSETC "One+" V2.2, a.k.a. AIO / AIO_I. Sold e.g. as ledsandchips LC-711. | — |
| Electrical compatibility | RAMPS 1.4 pin assignment (Arduino Mega 2560 + RAMPS on one PCB) | [V] partly |
| MCU | ATmega2560, signature 1E 98 01, 16 MHz external crystal | [V] |
| Flash / SRAM / EEPROM | 256 KB (248 KB usable, 8 KB boot section) / 8 KB / 4 KB | [V] |
| Fuses | L=0xFF H=0xD8 E=0xFD (ext. full-swing crystal; BOOTRST, 4 K-word boot; JTAG off; SPIEN; BOD 2.7 V) | [V] |
| Bootloader | "Arduino explorer stk500V2 by MLS" (classic Mega2560 STK500v2), 115200 baud, ~1.24 s window after reset | [V] |
| USB-serial | CH340G on UART0 (D0 RX, D1 TX); DTR auto-reset (opening the port resets the MCU) | [V] |
| Stepper drivers | 4 × Allegro **A4988** (marked "4988ET" = A4988SET, 28-pin QFN, soldered on board). Axes X, Y, Z, E0. **1/16 microstep** (3200 steps/rev with a 1.8° motor). | [V] |
| Power input | 12 V or 24 V DC. Heater/bed/fan outputs switch Vin. | store page |
| MOSFET outputs | Hot-end heater (D10), bed (D8, ≤10 A), PWM fan (D9). These are the only load outputs on the board. | [V] |
| Thermistor inputs | 2: TEMP0 = A13, TEMP1/bed = A14 | [V] |
| Limit-switch inputs | 6: X/Y/Z min & max, signal + GND (+5 V option via jumper/solder change). **Y on D40/D41, not RAMPS D14/D15** | [V] |
| Display | 2.4" 128×64 mono, **ST7565/UC1701-family controller on hardware SPI** (CS D29, A0 D27, RST D25). **Not** the ST7920/RRD wiring. | [V] |
| Input | Rotary encoder with push switch | [V] |
| Beeper | On D37 (active buzzer: HIGH = sound) | [V] |
| microSD (TF) | Vertical slot on hardware SPI, SS D53, card-detect D49 | [R] |
| I2C header | 4-pin: **SCL, SDA, GND, 5V** (left→right). External pull-ups present. | [V] pull-ups |
| ESP-01 header | 2×4 ESP8266 module socket. UART **not identified yet** | [?] |
| Original firmware | Marlin 1.1.0-RC6, default config, LCD menus enabled, 250000 baud. Backup in `firmware/backup/`. | [V] |

---

## 2. Programming / toolchain

- **Upload over USB works** with the existing bootloader. No ISP programmer needed.
- **avrdude 8.x does NOT sync with this bootloader**; **avrdude 6.3 works**. With PlatformIO, pin it:
  ```ini
  [env:one_plus]
  platform = atmelavr
  board = megaatmega2560
  framework = arduino
  upload_speed = 115200
  platform_packages = platformio/tool-avrdude@~1.60300.0
  ```
  Manual: `avrdude -c wiring -p m2560 -P COM5 -b 115200 -D -U flash:w:firmware.hex:i` (avrdude 6.3 at `%USERPROFILE%\.platformio\packages\tool-avrdude@1.60300.200527\`).
  Arduino IDE 2.x ships avrdude 8. Uploads from the IDE will fail unless its avrdude is replaced or PlatformIO is used.
- Board was on **COM5** (CH340). Close Arduino IDE / serial monitors before uploading ("Access is denied").
- Old STK500v2 Mega bootloaders have a known quirk: firmware images containing the byte sequence `!!!` can hang the upload (monitor-mode trigger). Avoid `"!!!"` in string literals. (Not tested on this unit.)
- **Watchdog works [V]**: probe `wdt` (`wdt_enable(WDTO_1S)` then hang) reboots cleanly after ~1 s and runs the application again. No boot-loop.
- **The bootloader clears `MCUSR` [V]**: the application always sees `MCUSR = 0` (seen after both watchdog and USB/DTR resets; presumably the same for all reset types). Firmware **cannot** use MCUSR to detect the reset cause. Workaround if needed: before an intentional reset, write a magic value to a `__attribute__((section(".noinit")))` variable and check it at boot (a power-on reset leaves random RAM, so use a 32-bit magic).
- Still call `MCUSR = 0; wdt_disable();` early in `setup()` (harmless, and safe if the bootloader is ever replaced).

### 2.1 Bootloader behaviour (from disassembly of the boot section + on-board tests, 2026-11-18) [V]

These four quirks decide how flash can be used. The `aiready` host tool handles all of them.

1. **Watchdog reset jumps to 0x20000, not 0x0000.** The bootloader's startup sets `EIND = 1`. On a WDT reset (`MCUSR.WDRF`) it disables the WDT and `eicall`s its zeroed app pointer, which lands at byte 0x20000. On blank (0xFF) flash the CPU slides up to the bootloader at 0x3E000, which then boots normally (~1.24 s window, then `ijmp 0`). That's why the probe `wdt` test "worked".
   - Application code must stay **below 0x20000 (128 KB)**, or a WDT reset will execute whatever is at 0x20000.
   - Anything else stored in 0x20000–0x3DFFF must not break the slide. The aiready descriptor block starts with `jmp 0x3E000` for this reason.
2. **The page being written is not erased.** The bootloader erases pages **sequentially from 0x0000**, one per page written in that session, whatever the target address. Normal uploads (contiguous from 0x0000) are fine, and pages beyond the image are never touched. A partial write elsewhere (e.g. a HEX that starts at 0x3A000) **erases the start of the application** and programs the target pages without an erase (result = old AND new). Only write contiguous images from 0x0000.
3. **Pages at or above 0x3C000 are never erased** (its erase limit is 0x3C000, below the real boot section at 0x3E000). 0x3C000–0x3DFFF can only have bits cleared via this bootloader. It is kept as all-0x00 (AVR `nop`) fill.
4. **Any byte received during the ~1.24 s window keeps the bootloader resident** (it waits for STK500 frames indefinitely). After a reset, wait for the app's banner before sending.

Also see §2.2 for a host-side (Windows driver) trap when opening the port without a reset.

### 2.2 Opening the port without resetting the board [V]

Opening COM5 with pyserial defaults asserts DTR and resets the MCU. To query a running app without restarting it, open with `dtr=False, rts=False`. On Windows the CH340 driver first re-applies the *saved* port configuration (DCB) when the port is opened. If the previous session left DTR enabled there, this open makes a short DTR pulse that **leaves the MCU hung (not reset)** until a proper reset. pyserial's `ser.dtr = False` only toggles the line, so before closing also rewrite the DCB (`ser.dtr = ser.rts = False; ser.baudrate = ser.baudrate`). `host/aiready/link.py: park_and_close()` does this.

### 2.3 Flash memory map (aiready layout)

| Range | Use |
|---|---|
| 0x00000–0x1FFFF | Application (watchdog-safe) |
| 0x20000–0x39FFF | Application overflow (watchdog-unsafe, see §2.1), otherwise blank 0xFF |
| 0x3A000–0x3BFFF | aiready descriptor block: `jmp 0x3E000`, "AIRD" header, zlib JSON. Survives app uploads |
| 0x3C000–0x3DFFF | 0x00 (nop) fill; the bootloader cannot erase it |
| 0x3E000–0x3FFFF | STK500v2 bootloader |

PlatformIO cap for apps: `board_upload.maximum_size = 237568`. `aiready write-descriptor` rewrites 0x0000–0x3DFFF as one image (app + fill + block + nop fill, ~70 s). `aiready flash` writes the app only (contiguous from 0x0000) and never touches the block.

---

## 3. Pin map (by function)

Columns: Arduino pin · AVR port pin · alternate functions relevant to firmware · notes.

### 3.1 Stepper drivers (Allegro A4988, chip marking "4988ET" [V])

| Signal | X | Y | Z | E0 |
|---|---|---|---|---|
| STEP | 54 (A0, PF0) | 60 (A6, PF6) | 46 (PL3, OC5A) | 26 (PA4) |
| DIR | 55 (A1, PF1) | 61 (A7, PF7) | 48 (PL1) | 28 (PA6) |
| ENABLE (active LOW) | 38 (PD7) | 56 (A2, PF2) | 62 (A8, PK0) | 24 (PA2) |

- **All four axes (X, Y, Z, E0) verified on the board [V]** (user-confirmed motor operation with the probe `step` command). They also match the original Marlin's direct port writes [F].
- ENABLE pins have **external pull-ups [V]**, so drivers are disabled when the MCU is in reset or the pin is an input.
- A4988 timing: STEP min high 1 µs, min low 1 µs; DIR setup/hold 200 ns; max step rate is limited by firmware (≈ 40 kHz practical on 16 MHz AVR with an ISR).
- X STEP and Y STEP are on the same port (PORTF), so they can be pulsed simultaneously with one register write.
- Z STEP is on OC5A (Timer5), so hardware-timer step generation is possible for Z.
- **Microstepping = 1/16 [V]** (as shipped; whether MS jumpers exist to change it is unchecked. A 200-step motor needs 3200 steps per revolution, confirmed with `step x 3200`).
- **Current sense resistors: 0.05 Ω ("050"), 2 per driver [V].** A4988 current limit: `Itrip = Vref / (8 × 0.05) = Vref / 0.4` ⇒ **Vref = 0.4 × I** (e.g. 0.4 V → 1.0 A, 0.6 V → 1.5 A, 0.8 V → 2.0 A absolute max). Measure Vref on the trimpot wiper vs GND.
- **3 trimpots**, physically next to the E0, Y and Z drivers [V]. The X driver has no pot of its own. X probably shares a pot (likely the Y one) [?]. If so, X and Y have the same current limit. Current Vref settings not measured [?].
- Current limit is hardware-only; firmware cannot change it.
- Motor supply = Vin (12/24 V). A4988: 8–35 V, ≤2 A peak with cooling, ~1 A RMS practical without heatsink/airflow.

### 3.2 Limit-switch inputs

| Connector | Pin | Port | Interrupt | Also | Status |
|---|---|---|---|---|---|
| X-MIN | 3 | PE5 | INT5 | OC3C (Timer3 PWM) | [V] |
| X-MAX | 2 | PE4 | INT4 | OC3B | [V] |
| Y-MIN | **40** (not RAMPS D14) | PG1 | none (no INT/PCINT on port G) | — | [V] |
| Y-MAX | **41** (not RAMPS D15) | PG0 | none (no INT/PCINT on port G) | — | [V] |
| Z-MIN | 18 | PD3 | INT3 | **TXD1** | [V] |
| Z-MAX | 19 | PD2 | INT2 | **RXD1** | [V] |

- All six have **external pull-ups [V]** (read HIGH with no pull-up enabled and nothing connected). Switch closes signal→GND ⇒ **LOW = triggered** for a mechanical NO switch. Enabling INPUT_PULLUP too is harmless.
- Store page: optical endstops can use S/GND/5V after changing a "white" jumper/connector to soldered pins.
- **The Y endstops are NOT on RAMPS D14/D15. They are on D40 (Y-MIN) and D41 (Y-MAX), port G [V].** Port G has no INT or PCINT, so Y endstops must be polled. The X endstops (INT4/5) and Z endstops (INT2/3) have hardware interrupts, which suits counters, encoders or tachometers.
- D14/D15 (UART3) are therefore not endstops. They may be the ESP-01 UART or simply unused (see §3.8).
- Using UART1 sacrifices the Z endstop pair.

### 3.3 Power outputs (N-MOSFET low-side, load voltage = Vin)

| Output | Pin | Port | PWM timer | Notes |
|---|---|---|---|---|
| HEATER (hot end) | 10 | PB4 | Timer2 OC2A (8-bit) | "max 240 W" per store page. **[V]** switches HEATER output |
| BED | 8 | PH5 | Timer4 OC4C (16-bit) | ≤10 A per store page (120 W @12 V, 240 W @24 V). **[V]** switches BED output |
| FAN (PWM) | 9 | PH6 | Timer2 OC2B (8-bit) | **[V]** switches FAN output, PWM works (analogWrite 128/255) |


The store page mentions two extra fixed "FAN/LED" outputs; **none are present on this board** (user inspection). D9 FAN is the only fan output.

- HEATER = PB4 (D10) and BED = PH5 (D8) are the pins soft-PWMed by the original Marlin temperature ISR **[F]**.
- **Safety rule**: set these pins `OUTPUT, LOW` as the first action in `setup()`. Never leave them as `INPUT_PULLUP`; the internal pull-up can partly turn the MOSFET on.
- Default Arduino PWM on Timer2 is ~490 Hz and on Timer4 ~490 Hz. For heaters prefer slow PWM (≤ 10 Hz) or bang-bang/PID with time-proportioning; for fans, 490 Hz–25 kHz depending on fan.
- MOSFET part numbers, gate resistors/pull-downs and the polyfuse ratings are **unknown [?]**.

### 3.4 Temperature inputs (ADC)

| Connector | ADC channel | Arduino pin | Port | Status |
|---|---|---|---|---|
| TEMP0 (hot end) | A13 | 67 | PK5 | [V] reads ≈28 °C with a 100 k thermistor at room temp. **cnc-poc-1 profile: on the X driver chip** |
| TEMP1 (bed) | A14 | 68 | PK6 | [V] reads room temp with thermistor; 1023 when open. **cnc-poc-1 profile: on the X motor** |
| (A15) | A15 | 69 | PK7 | Floating. Not a populated thermistor input. |

- Original Marlin selects ADC channel 13 (ADCSRB.MUX5=1, ADMUX=0x45) for TEMP0 **[F]**.
- Circuit (RAMPS standard, assumed): 4.7 kΩ pull-up to 5 V, thermistor to GND, ~10 µF filter cap. `R_therm = 4700 × adc / (1023 − adc)`.
- ADC is 10-bit, reference AVCC = 5 V (verify actual 5 V rail with a meter for accuracy).
- Open sensor → ADC ≈ 1023; short → ≈ 0. Firmware must treat both as faults and shut heaters off.
- A13/A14 are also PCINT21/22 (port K).

### 3.5 Display, encoder, beeper

**Display = ST7565/UC1701-family 128×64 on hardware SPI [V].** This was recovered by disassembling the original Marlin (`u8g_InitHWSPI(cs=29, a0=27)`, `LCD_RESET_PIN 25`, init table `E2 40 A0 C8 A6 A3 2F F8 00 23 81 27 AC 00 AF`), then confirmed with the probe's `lcd7567` test pattern. It does **not** use the RAMPS/RRD ST7920 nets (D16/D17/D23). Bit-banging an ST7920 on those pins does nothing.

| Function | Pin | Port | Notes |
|---|---|---|---|
| LCD SCK | 52 | PB1 | hardware SPI, **shared with SD card** |
| LCD MOSI (SDA/SI) | 51 | PB2 | hardware SPI, shared with SD card |
| LCD CS | 29 | PA7 | active LOW [V] |
| LCD A0 (RS/DC) | 27 | PA5 | LOW = command, HIGH = data [V] |
| LCD RESET | 25 | PA3 | active LOW; drive HIGH for operation [V] |
| "LCD backlight" | 65 | PK3 (A11) | Marlin drove HIGH; backlight is on regardless on this unit, so effect unknown [?] |
| Encoder A (BTN_EN1) | 31 | PC6 | pulled-up, works [V] |
| Encoder B (BTN_EN2) | 33 | PC4 | pulled-up, works [V] |
| Encoder push (BTN_ENC) | 35 | PC2 | pulled-up, LOW = pressed [V] |
| Beeper | 37 | PC0 | **[V]** works with probe `beep`. Drive HIGH = beep. No hardware PWM: don't use `tone()` |
| (RAMPS KILL) | 41 | PG0 | **Used as the Y-MAX endstop input on this board [V]**. There is no KILL button: the on-board button is RESET |
| RESET button | MCU RESET pin | — | **[V]** reboots the MCU (hardware reset, not readable by firmware) |

- SPI mode 0, MSB first. Original Marlin ran it at F_CPU/4 = 4 MHz. Page-addressed RAM: 8 pages × 8 rows; `0xB0|page`, column `0x10|hi`, `0x00|lo`, then data with A0 HIGH.
- Init sequence used by the original firmware (u8glib device, bias 1/7):
  `E2` reset, `40` start line 0, `A0` ADC normal, `C8` COM reverse, `A6` normal, `A3` bias 1/7, `2F` power all on, `F8 00` booster ×4, `23` regulator ratio, `81 27` contrast 0x27, `AC 00` indicator off, `AF` display on.
- **U8g2 (verified [V])**:
  ```cpp
  U8G2_ST7565_ZOLEN_128X64_F_4W_HW_SPI u8g2(U8G2_R0, /*cs=*/29, /*dc=*/27, /*reset=*/25);

  void lcdBegin() {              // call after the safe-state block in setup()
    u8g2.begin();
    u8g2.sendF("c", 0xA0);       // ADC select normal (segment direction), as original Marlin
    u8g2.getU8x8()->x_offset = 0;// no column offset in this orientation
    u8g2.setContrast(56);        // 40..70 good
  }
  ```
  - ST7565_ZOLEN is the only constructor that gives a usable image. Its own init reverses the segment direction (image mirrored, and 4 columns off). The `0xA0` command after `begin()` fixes both in hardware.
  - **Re-apply `0xA0` and `x_offset` after every `begin()` or `setFlipMode()`**, since those re-send the driver's init. `setContrast()` and `setPowerSave()` are safe.
  - Equivalent alternative (software mirror, slower): `U8G2_MIRROR` rotation + `x_offset = 4`, no `0xA0`. Tested result for `0xA1` + R0: aligns at offset 4 but mirrored (unusable).
  - Use `_1_` or `_2_` instead of `_F_` to save RAM (page buffer 128/256 B vs 1 KB).
  - Other constructors failed: UC1701_MINI12864 and ST7565_64128N blank, ST7565_EA_DOGM128 wrapped.
  - Working example: `firmware/lcdtest`.
  - Good contrast range is `setContrast(40…70)` (U8g2 scale 0–255); 56 is a sensible default.
  - Use `_1_` or `_2_` instead of `_F_` to save RAM (1 KB full buffer vs 128/256 B page buffer).
  - Tested with `firmware/lcdtest`. Other constructors failed: UC1701_MINI12864 and ST7565_64128N showed nothing; ST7565_EA_DOGM128 showed a wrapped/shifted image.
- **SPI bus is shared with the SD card**: deselect LCD (D29 HIGH) before SD access and SD (D53 HIGH) before LCD access. With U8g2 + SdFat, both libraries manage their own CS, but never call them from different ISRs/contexts concurrently.
- D16, D17, D23 (the RRD/ST7920 nets) appear to be **unused** on this board, which frees UART2 (D16/D17) for other uses [?, verify nothing is attached].
- **Encoder pins are on port C, which has no pin-change interrupts.** Poll them at ≥ 1 kHz (e.g. from a timer ISR).

### 3.6 microSD (TF card)

| Signal | Pin | Port |
|---|---|---|
| MISO | 50 | PB3 |
| MOSI | 51 | PB2 |
| SCK | 52 | PB1 |
| CS (SDSS) | 53 | PB0 |
| Card detect | 49 | PL0 (pulled-up [V]; LOW = card present [R]) |

- Keep D53 as OUTPUT even if not using SD (AVR SPI master requirement).
- 5 V MCU ↔ 3.3 V card: board presumably has a level shifter/regulator [?].

### 3.7 I2C header

| Header pin (L→R) | Signal | Arduino pin |
|---|---|---|
| 1 | SCL | 21 (PD0, INT0) |
| 2 | SDA | 20 (PD1, INT1) |
| 3 | GND | — |
| 4 | 5 V | — |

- External pull-ups present [V] (value unknown). Bus is 5 V logic. No devices on-board (scan found none).
- Pins can alternatively be used as GPIO / external interrupts INT0/INT1.

### 3.8 ESP-01 header [?]

UART not yet identified. Candidates:

| UART | Pins | Conflicts with |
|---|---|---|
| Serial1 | 18 TX / 19 RX | Z-MIN / Z-MAX |
| Serial2 | 16 TX / 17 RX | Nothing known: the display does not use these. **Strong candidate.** |
| Serial3 | 14 TX / 15 RX | Nothing known: the Y endstops are on D40/D41. **Strong candidate.** |
| Serial0 | 1 TX / 0 RX | USB (CH340G) |

ESP-01 needs 3.3 V (≥ 300 mA peaks). Whether the header has its own 3.3 V regulator and RX level shifting is unknown.

### 3.9 Other pins observed

| Pin | Observation | Hypothesis |
|---|---|---|
| D13 (PB7) | Held LOW with pull-up | On-board LED (Arduino `LED_BUILTIN`) [?] |
| **D4, D6, D7** (TQFP pins 1, 15, 16) | Held LOW with pull-up (external load/pull-down) | Something is wired here, but there are no extra load outputs on the board. Possibly pull-downs on unrouted nets, unpopulated footprints, or internal functions. **Do not drive HIGH until traced.** |
| D12 (PB6) | not probed (RAMPS PS_ON) | — |
| D5, D11 | Floating | RAMPS servo pins; probably not broken out |
| All others | Floating | Not connected, or MCU-driven-only nets (STEP/DIR, LCD) |

---

## 4. MCU resource map and conflicts

### Timers
| Timer | Arduino default use | PWM pins on this board | Recommendation |
|---|---|---|---|
| Timer0 (8-bit) | `millis()`, `delay()` | D4 (OC0B), D13 (OC0A) | Leave alone |
| Timer1 (16-bit) | free | D11, D12, D13 | Stepper/motion ISR (Marlin does this) |
| Timer2 (8-bit) | `tone()` | **D9 fan, D10 heater** | Reserve for heater/fan PWM. **Do not use `tone()`** (it takes Timer2 and breaks D9/D10). |
| Timer3 (16-bit) | free | D2, D3 (X endstops), D5 | Free if endstops used as inputs |
| Timer4 (16-bit) | free | D6, D7, **D8 bed** | Bed PWM; changing its prescaler also affects D6/D7 |
| Timer5 (16-bit) | `Servo` library | D44, D45, **D46 Z_STEP** | Can generate Z step pulses in hardware; Servo lib conflicts |

### Interrupts
- External INT: INT0 D21 (SCL), INT1 D20 (SDA), INT2 D19 (Z-MAX), INT3 D18 (Z-MIN), INT4 D2 (X-MAX), INT5 D3 (X-MIN).
- Pin-change: PCINT0–7 port B (D53, D52, D51, D50, D10, D11, D12, D13), PCINT8–10 (D0, D15, D14), PCINT16–23 port K (A8–A15 = Z_EN, …, TEMP0, TEMP1).
- No interrupts on port C (encoder), port G (Y endstops D40/D41) or ports A/F/L.

### UARTs
- UART0 = USB. UART1 = Z endstops. UART2 (D16/D17) and UART3 (D14/D15) = apparently unused; one is probably the ESP-01. See §3.8.

### SPI
- Hardware SPI (D50/51/52) is **shared by the SD card (CS D53) and the display (CS D29)**. D53 must stay OUTPUT for SPI master mode.

---

## 5. Power and electrical limits

| Parameter | Value | Source |
|---|---|---|
| Input voltage | 12 V or 24 V DC | store page |
| Heater output | "max 240 W" (depends on MOSFET) | store page |
| Bed output | ≤ 10 A, ≤ 240 W, bed < 100 °C recommended | store page |
| Output voltage on heater/bed/fan/FAN-LED | = Vin | store page |
| MCU logic | 5 V. GPIO 20 mA continuous, 40 mA abs max per pin. | ATmega2560 datasheet |
| 5 V rail source | On-board regulator from Vin and/or USB [?] | — |
| 3.3 V rail | Unknown (needed for SD/ESP-01) [?] | — |

**MCU powered from USB alone** (no Vin): logic works (all probe tests so far ran this way); motors/heaters do not.

---

## 6. Unknowns that matter for firmware

1. Which trimpot sets the X driver current (shared with Y?), and the present Vref values. (Chip A4988, 1/16 microstep, Rs = 0.05 Ω confirmed.)
3. ESP-01 UART (Serial2 on D16/D17 or Serial3 on D14/D15) and its 3.3 V supply.
4. What D4, D6, D7 are connected to.
5. D13 LED presence. (No KILL button exists: the on-board button is RESET.)
6. MOSFET and fuse ratings; 5 V regulator capacity (for powering external sensors from I2C/endstop 5 V).

Track progress in [verification.md](verification.md).

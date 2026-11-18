# FYSETC One+ V2.2 (AIO v1) controller — repurposing project

ATmega2560 3D-printer all-in-one board (RAMPS 1.4 compatible) being repurposed for custom, non-printer firmware.

**The board is self-describing ("AI ready hardware" POC, see [AI ready hardware.md](AI%20ready%20hardware.md)).** Start with the `aiready` MCP tools (`discover`, `describe`, `get_state`, `flash`, ...) or the CLI (`python -m aiready discover | describe [section] | state | flash <hex> | readback`; JSON output). `describe` without a section gives a summary and table of contents; ask for sections rather than `all`. Current profile `cnc-poc-1`: generalised CNC controller, heater/bed/fan unused, TEMP0 = X driver chip temperature, TEMP1 = X motor temperature.

**For using the board, the on-board description (`aiready describe`) is authoritative.** `docs/dev/` holds the maintainers' records: [docs/dev/hardware.md](docs/dev/hardware.md) (human reference with the reasoning: pin map, timers, conflicts, limits, bootloader quirks §2.1) and [docs/dev/verification.md](docs/dev/verification.md) (evidence for each status tag). When a hardware fact changes or is verified, update `descriptor/one_plus_v2.2.json`, both dev docs and the pin header together, then `aiready write-descriptor`. Read hardware.md before changing the HAL or the description. Use [firmware/include/one_plus_pins.h](firmware/include/one_plus_pins.h) for pin names. Build new apps from [firmware/aiready_base/](firmware/aiready_base/) with the HAL in [firmware/lib/aiready/](firmware/lib/aiready/) (`aiSafeInit()`, `AiReady.begin/poll`, thermistors, X thermal guard, endstops, encoder, steppers, display). Items tagged [?] are unverified; check [docs/dev/verification.md](docs/dev/verification.md) and say when code depends on one.

## Hard rules
- Toolchain: PlatformIO, `board = megaatmega2560`, `framework = arduino`, `upload_protocol = arduino`, `board_upload.maximum_size = 244736`. The bootloader is **Optiboot** (installed by ISP 2026-11-19; avrdude 6.3 and 8.x both work). Copy `firmware/probe/platformio.ini` as a template (it also keeps build output off Google Drive).
- Board is on **COM5 or COM6** (CH340G, 1A86:7523; check `aiready discover`). Opening the port resets the MCU.
- First lines of `setup()`: heater D10, bed D8, fan D9 → `OUTPUT, LOW`; stepper ENABLE pins (38, 56, 62, 24) → `OUTPUT, HIGH` (disabled). Never set MOSFET gate pins to `INPUT_PULLUP`.
- Any heater control must include thermistor open/short detection and a thermal-runaway / max-on-time cutoff.
- Display is **ST7565 on hardware SPI (CS 29, A0 27, RST 25)**, not the RAMPS ST7920 wiring. SPI is shared with the SD card (CS 53). U8g2: `U8G2_ST7565_ZOLEN_128X64_*_4W_HW_SPI(U8G2_R0, 29, 27, 25)`, then after **every** `begin()`: `sendF("c", 0xA0)`, `getU8x8()->x_offset = 0`, `setContrast(56)`. Without the 0xA0 the image is mirrored and wraps. See docs/dev/hardware.md §3.5.
- **Y endstops are D40 (Y-MIN) / D41 (Y-MAX), not RAMPS D14/D15.** Port G has no interrupts: poll them. There is no KILL button (the button is hardware RESET).
- Don't use `tone()` (steals Timer2 = heater/fan PWM). The beeper (D37) is **passive**: use `aiBeep()` (Timer3 square wave), a steady HIGH only clicks. Encoder (D31/33/35) has no pin-change interrupt: poll it; 2 counts per detent.
- Do not drive D4, D6 or D7 HIGH until identified (they have external loads).
- Watchdog is safe to use: Optiboot jumps straight to the app after a watchdog reset. MCUSR reaches the app; the HAL captures it in `.init3` (`aiResetCause()`, `aiResetFlags()`).
- **Flash layout** (docs/dev/hardware.md §2.1–2.3): app 0x00000–0x3BBFF (max 244736 bytes), self-description block 0x3BC00–0x3FBFF (16 KB), Optiboot 0x3FC00–0x3FFFF (lock-protected).
- Change the descriptor only with `aiready write-descriptor` (writes just the block's pages). Prefer `aiready flash` for apps (backs up first, strips the protected regions). The USBasp is only needed to change the bootloader or fuses (full ISP backup: `backups/isp_20261119_102942_*`).
- Keep the aiready runtime in apps (`AiReady.poll()` in `loop()`), so the board stays queryable. Lines starting `@ai ` belong to the runtime.
- To talk to a running app without resetting it, use `host/aiready` (it parks DTR/RTS correctly). A DTR-disabled open after a DTR-enabled session that was not closed that way resets the board once (§2.2).
- After any reset, don't send serial data until the app's banner appears (~1 s): bytes sent during the bootloader window are lost (Optiboot restarts into the app).
- Original firmware backup: `firmware/backup/` (restore: `python -m aiready flash firmware/backup/marlin_1.1.0-RC6_flash.hex`). `aiready flash` also saves timestamped backups in `backups/`.

## Layout
- `descriptor/one_plus_v2.2.json`: machine-readable self-description (source of truth for what is stored on the board); `schema.json` is its format
- `host/aiready/`: host package (CLI, MCP server `python -m aiready.mcp_server`, registered in `.mcp.json`); tests: `python -m pytest host/tests`
- `firmware/lib/aiready/`: runtime + thin HAL library; `firmware/aiready_base/`: factory image and app template
- `docs/dev/hardware.md`: authoritative hardware reference
- `docs/dev/verification.md`: evidence log + open checks
- `firmware/include/one_plus_pins.h`: pin definitions
- `firmware/probe/`: characterisation console firmware (serial 115200, `help`)
- `firmware/lcdtest/`: U8g2 display test (working U8g2 example)
- `firmware/backup/`: original Marlin 1.1.0-RC6 flash/EEPROM dump

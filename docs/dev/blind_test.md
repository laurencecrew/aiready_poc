# Blind test: is the board self-describing enough?

Goal: a fresh agent, with no project docs, no CLAUDE.md and no memory of this project, writes and flashes a small app using only the board and its tools. Everything it misses or guesses is a gap in the description.

## Setup

1. Close any session or serial monitor using COM5. Board running `aiready_base` with the current descriptor (`python -m aiready check` from the repo).
2. Optional but more realistic: hide the already-installed tools so the agent has to find them from the board's banner:
   ```powershell
   python -m pip uninstall -y aiready
   Remove-Item -Recurse "$env:USERPROFILE\.aiready"     # cache + installed-mode backups
   ```
   Restore afterwards with `python -m pip install -e "host[mcp,test]"` from the repo.
3. Test folder: `C:\Users\laure\Documents\aiready_blind_test\` (local disk, outside the repo, no CLAUDE.md, no `.mcp.json`). It contains only `TASK.md`.
4. Start a new Claude Code session in that folder (e.g. a new VS Code window on it) and say: "Read TASK.md and do it."
5. Be there to approve flashing and motor energising. Have X endstop leads ready to short to GND, and the thermistors on the X driver and motor.

## What the task exercises

| Task element | What the agent must learn from the board/tools |
|---|---|
| Finding the board | Port, baud, that opening resets it, waiting for the banner |
| Finding the tools | `tools` / `install` / `mcp` fields in the banner or `@ai id` |
| Encoder jog | Encoder pins, no interrupts (polling), detent size `[?]`, or the HAL's `AiEncoder` |
| Endstops | X_MIN D3 / X_MAX D2, active LOW, or `aiEndstop()` |
| Display | ST7565 + U8g2 ZOLEN constructor + the 0xA0 / x_offset / contrast recipe |
| Temperatures | TEMP0 = X driver, TEMP1 = X motor, thermistor formula, fault thresholds |
| Overheat cut-off | Thermal guard limits, X ENABLE active LOW, beeper HIGH = sound, no `tone()` |
| Build + flash | PlatformIO, avrdude 6.3 pin, app size cap, contiguous-image rule, `aiready flash` |
| Safety | `safety_rules`: safe-state first in `setup()`, D4/D6/D7 untouched, heaters off |

## Scoring checklist

| # | Check | Pass? | Notes |
|---|---|---|---|
| 1 | Found the board's port and read the banner without wedging the bootloader | | |
| 2 | Found and installed the tools from the banner (if step 2 of setup was done) | | |
| 3 | Used `describe` summary + sections rather than dumping `all` | | |
| 4 | Read `safety_rules` before writing code; safe state is first in `setup()` | | |
| 5 | Pinned avrdude 6.3; respected the app size cap | | |
| 6 | Flashed via `aiready flash` (or an equivalent contiguous upload); descriptor survived | | |
| 7 | Display correct first time (not mirrored or wrapped) | | |
| 8 | Encoder direction/detent handled; noticed the `[?]` on detent size | | |
| 9 | X stops at both endstops | | |
| 10 | Overheat / sensor-fault cut-off works (unplug a thermistor) | | |
| 11 | Kept the runtime (`AiReady.poll()`) so the board stays queryable | | |
| 12 | Asked before energising the motor | | |
| 13 | Its "couldn't find out" list, copied below | | |

### Expected gap (known before the test)

The HAL library (`firmware/lib/aiready`) and pin header (`firmware/include`) are **not in the pip package**. The description names them by repo path only, so an agent must clone the repo to use them, or write its own drivers from the description. PlatformIO can't take a library from a subfolder of a git repo, so the fix is to publish the library separately or move it to its own repo.

### Gaps found

(fill in after the run)

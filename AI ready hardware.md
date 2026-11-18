# AI ready hardware

This is a proof of concept for a hardware product that is targeted toward a specific range of use cases (e.g. generalised CNC machine control, 3D printer control, robotics, vehicular motor control, media distribution and processing - any use case that benefits from generic processing coupled with specific I/O capabilities, power supply, environmental protection and form factor) but is supplied in a state that is optimised for user-led AI powered development of a custom application.

The target user experience is:
* User acquires the hardware based on suitability for their described use case
* On connection to a host PC via USB, BT, Ethernet etc it can be discovered by the AI coding agent and queried to understand its capabilities ('self describing hardware')
* The user is able to prompt the AI agent to describe their target application
* The AI agent is able to assess the suitability of the hardware to the application before assisting in its development and/or recommending adaptations
* The AI agent is able to profile the hardware for suitable performance of the desired application, ideally auto-calibrating through the use of in-built feedback mechanisms, by using external feedback mechanisms supplied with the hardware or procured by the user
* The AI is able to, within reason, iteratively develop, test and optimise the application with minimal user interaction once the application is sufficiently described

Self described capabilities might include:
* I/O
* Port mappings and other internacl connections
* User interface elements including buttons, encoders and screens
* Programmable components and associated interfaces, platform etc
* Internal and external interfaces such as UART, SPI, I2C, USB, Ethernet, Wifi and Bluetooth
* Any user configurable hardware options such as puluggable modules, hardware jumpers and switches, trimpot etc and their effect on operation
* Limitations such as current and voltage handling, power dissipation and cooling, I/O speed and resolution

The hardware should be supplied with a protected bootloader / thin abstraction layer that supports:
* Discovery
* Self description of capabilities
* Fast and reliable firmware reprogramming, including reading back of prior firmware
* Querying of low level state and/or live debugging

The 'thin abstraction layer' is optional, but would include low level drivers that are common to any application built on the hardware such as display drivers, conversion of rotary encoders to readable outputs, conversion of sensor ouputs to readable values. The abstrations should only go as far as saving unnecessary AI token usage in redeveloping non-application specific functionality and avoiding ambiguity in how on-board components should be interfaced with. The abstraction layer may be deferred to the programming framework instead.

The bootloader and programming environment may be based on an existing framework like Arduino, Platform.io but the intention is to optimise for AI driven implementation not hand coding so a completely new environment is not ruled out, as long as a range of AI models are able to work with it. Programming languages are similarly open to recommendation,  with the most operationally efficient option that can be easily managed by AI models beiung preferred.

## Specific environment for the POC

Selected due to availability but by no means optimised for this project, I have a 'FYSETC One+ V2.2' 3D printer controller available for the initial POC. See docs\dev\hardware.md

The controller should be treated as a generalised CNC machine controller (not a 3D printer controller in this instance). The thermistors connected to Temp0 and Temp1 are to be attached to the X stepper driver chip and X stepper motor respectively to enable basic monitoring of the effect of the current setting.

The exact use case is to be described in a separate exercise. The purpoose of this exercise is to enable the hardware to self describe in order to test the concept of 'AI ready hardware'.


## POC implementation (2026-11-18)

How the requirements above map onto the One+ board:

| Requirement | POC implementation |
|---|---|
| Discovery | `aiready discover`: finds the CH340 (1A86:7523), asks the runtime for `@ai id`, or falls back to reading flash through the bootloader |
| Self description | `descriptor/one_plus_v2.2.json` (format: `descriptor/schema.json`), compressed into a 16 KB block in board flash at 0x3BC00, just below the bootloader. It survives normal app uploads and can be read through the bootloader even when the app is broken or has no runtime. `aiready describe [section]` returns a summary or one section, to keep token use low |
| Fast, reliable reprogramming + readback | `aiready flash <hex>`: backup first (`backups/`), strips protected regions, uploads through Optiboot with avrdude, verifies, then checks that the runtime and descriptor are still there. `aiready readback` dumps flash + EEPROM |
| Low-level state / live debugging | Runtime library (`firmware/lib/aiready`) linked into every app: `@ai state`, `watch`, `pins`, `peek`, `safe`, `clear`, `reboot`, all one-line JSON over USB serial. Queries do not reset the running app |
| Thin abstraction layer | Same library: safe-state init, thermistors with fault detection, X thermal guard, polled endstops and encoder (1 kHz), basic stepper moves, beeper, verified display setup. Template app: `firmware/aiready_base` |
| Works with a range of AI models | Python CLI with JSON output (any agent that can run a shell), plus an MCP server (`python -m aiready.mcp_server`, `.mcp.json`) exposing the same functions as tools |

Language and framework: C++ on Arduino/PlatformIO for firmware (the best-known embedded stack for current models), Python for host tools.

Lessons for real AI-ready hardware, from building this on a board that wasn't designed for it:
- **The bootloader is the weakest link.** The factory one had three undocumented bugs: watchdog resets jumped to 0x20000, pages were erased in sequence from 0 rather than at the target address, and nothing at or above 0x3C000 was ever erased. They were found only by disassembling it, and every rule they forced (128 KB code limit, contiguous-only writes, a landing pad, a dead 8 KB region) disappeared when it was replaced with Optiboot over ISP. A product should ship a bootloader that is documented, protects its own region, and has a native "describe" command.
- **Host-side serial quirks matter as much as the firmware.** Opening the port the wrong way resets the board (with the old bootloader it hung the MCU; docs/dev/hardware.md §2.2). Purpose-built hardware should not reset on port open, or should offer a separate debug channel.
- **The description must carry verification status** (V/R/F/? tags). Otherwise an agent can't tell measured facts from assumptions.
- **"Verified" needs a defined test, not an impression.** The blind test (docs/dev/blind_test.md) found the beeper tagged verified as "active" when it is passive: the original check took a click for a beep. A fresh agent working only from the description is a good way to find such errors, and it measured the encoder resolution the description had guessed.
- **8 KB was tight.** The compressed descriptor reached 7.9 KB; the block is now 16 KB. A real product should reserve generously, or store a full description on the host keyed by a hash the board reports.

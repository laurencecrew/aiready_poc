# aiready_poc: self-describing hardware for AI coding agents

Proof of concept for **AI-ready hardware**: a controller board that an AI coding agent can discover, query for its capabilities, program, read back and inspect live, without hand-written board knowledge. The concept is described in [AI ready hardware.md](AI%20ready%20hardware.md).

POC board: **FYSETC One+ V2.2** (ATmega2560, RAMPS 1.4 compatible, 4 × A4988 steppers, 128×64 LCD, encoder), repurposed as a generalised CNC controller (profile `cnc-poc-1`).

## For AI agents

If a board's serial banner or `@ai id` pointed you here:

1. Install the host tools (Python ≥ 3.10):
   ```
   pip install mcp git+https://github.com/laurencecrew/aiready_poc#subdirectory=host
   ```
2. Find the board and read its self-description:
   ```
   python -m aiready discover
   python -m aiready describe                 # summary + table of contents
   python -m aiready describe safety_rules    # then any section, e.g. pins, peripherals.steppers
   python -m aiready state                    # live state, does not reset the app
   ```
   Every command prints JSON.
3. Or add the MCP server (stdio) to your client: command `python`, args `["-m", "aiready.mcp_server"]`. Tools: `discover`, `describe`, `get_state`, `watch`, `read_registers`, `peek`, `safe_state`, `clear_guard`, `flash`, `read_back`.
4. No install possible? The serial protocol is plain JSON lines: see [docs/protocol.md](docs/protocol.md), which includes a 25-line reader.

**Read `safety_rules` before writing firmware.** Flash only through `aiready flash`: this board's bootloader has erase bugs that a naive upload of a partial image will trip over (`describe programming.quirks`).

## Repository layout

| Path | Contents |
|---|---|
| `descriptor/` | Machine-readable self-description (`one_plus_v2.2.json`) and its format (`schema.json`) |
| `host/aiready/` | Host package: CLI, MCP server, Optiboot (STK500v1) client, avrdude wrapper. Tests: `python -m pytest host/tests` |
| `firmware/lib/aiready/` | Runtime (`@ai` protocol) + thin HAL library for apps |
| `firmware/aiready_base/` | Factory image and template for new apps (PlatformIO) |
| `firmware/include/one_plus_pins.h` | Pin map |
| `firmware/probe/`, `firmware/lcdtest/` | Characterisation firmware used to verify the hardware |
| `docs/protocol.md` | Serial protocol and description format (for use without the tools) |
| `docs/dev/` | Maintainers' records: hardware reference, verification log (not needed to use the board) |

## Building firmware

PlatformIO, `upload_protocol = arduino` (the board runs Optiboot; avrdude 6.3 or 8.x):

```
cd firmware/aiready_base
pio run
python -m aiready flash .../one_plus/firmware.hex     # backs up the current firmware first
```

Developing from a clone: `pip install -e "host[mcp,test]"`; the tools then use the repo's `descriptor/` and `backups/`.

## Status

Proof of concept. Verified on one board. Values tagged `"s": "?"` in the description are unverified.

The board and the tools are meant to be enough to work with the hardware. [docs/dev/](docs/dev/) holds the maintainers' records: the human hardware reference the description was written from ([hardware.md](docs/dev/hardware.md)) and the evidence log behind each status tag ([verification.md](docs/dev/verification.md)).

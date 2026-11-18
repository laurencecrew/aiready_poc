# aiready serial protocol (format 1)

How to talk to an AI-ready board **without** the host tools. With them installed (`pip install mcp git+https://github.com/laurencecrew/aiready_poc#subdirectory=host`), use `python -m aiready ...` or the MCP server instead.

## 1. Connection

| Item | Value |
|---|---|
| Link | USB serial (CH340G, VID:PID 1A86:7523 on the POC board) |
| Settings | 115200 baud, 8N1, no flow control |
| Reset | Opening the port with DTR asserted (the default) resets the board |
| After a reset | Bootloader listens ~1 s, then the app prints a banner line. **Wait for the banner before sending**: bytes sent earlier are lost |
| Querying without a reset | Open with DTR and RTS disabled. On Windows also close with both disabled *in the port configuration* (re-apply the baud rate before closing), or the next such open pulses DTR and resets the board |

## 2. Commands

Send `@ai <command>` followed by a newline. Every reply is **one JSON line** whose `"r"` field names the command it answers (`"err"` for errors). Other lines belong to the application.

| Command | Reply (main fields) |
|---|---|
| `help` | `cmds`, `protocol`, `desc_format`, `tools`, `docs` |
| `id` | `board`, `profile`, `fw`, `ver`, `built`, `desc` (descriptor id, or `"none"`), `desc_addr`, `reset` (`soft`, `power_on`, `brownout`, `watchdog`, `external`, `jtag`, `unknown`), `mcusr`, `up_ms`, `ram_free`, `tools`, `install`, `mcp` |
| `desc <off> <len>` | `off`, `len` (≤ 128), `hex`: raw bytes of the descriptor block (see §3) |
| `state` | `endstops`, `encoder`, `temp` (`x_driver`/`x_motor`: `c`, `adc`, `fault`), `drivers_enabled`, `guard`, `mosfet_level`, `sd_card`, `ram_free` |
| `watch <ms>` | Streams `state` lines every `ms` (50–60000); `watch 0` stops |
| `pins` | `ports`: per GPIO port `"PIN DDR PORT"` in hex |
| `peek <addr> <n>` | Read-only data-space bytes (0..0x21FF, n ≤ 32); side-effect registers show `xx` |
| `safe` | Power outputs LOW, all stepper drivers disabled |
| `clear` | Clears a latched thermal-guard trip |
| `reboot` | Watchdog reboot (banner again after ~2 s, `reset` = `soft`) |

The boot banner has the same fields as `id`, with `"r":"banner"`.

## 3. Descriptor block

The board's self-description is a 16 KB block in flash (0x3BC00 on the POC board; `desc_addr` in `id`). Read it with `desc`, 128 bytes at a time. Header, 32 bytes, little-endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Reserved (an AVR `jmp` to the bootloader; ignore) |
| 4 | 4 | Magic `AIRD` (absent: no descriptor written) |
| 8 | 2 | Format = 1 |
| 10 | 2 | Compression = 1 (zlib) |
| 12 | 4 | Payload length (compressed) |
| 16 | 4 | JSON length |
| 20 | 4 | CRC32 of the payload |
| 24 | 8 | Id = first 8 bytes of SHA-256 of the JSON (same as `desc` in `id`) |

The payload starts at offset 32: a zlib stream of minified JSON. Structure: [../descriptor/schema.json](../descriptor/schema.json). Start with `identity`, `safety_rules` and `unknowns`; values tagged `"s": "?"` are unverified.

### Minimal reader (Python, pyserial only)

```python
import json, struct, time, zlib, serial

s = serial.Serial("COM5", 115200, timeout=0.1)        # resets the board
t = time.time()
while b'"r":"banner"' not in s.readline():            # wait for the app
    if time.time() - t > 5: raise SystemExit("no aiready runtime")

def ask(cmd):
    s.write(f"@ai {cmd}\n".encode())
    while True:
        line = s.readline()
        if line.startswith(b"{"):
            r = json.loads(line)
            if r.get("r") in (cmd.split()[0], "err"): return r

head = bytes.fromhex(ask("desc 0 32")["hex"])
assert head[4:8] == b"AIRD"
length = struct.unpack_from("<I", head, 12)[0]
blk = bytearray(head)
while len(blk) < 32 + length:
    blk += bytes.fromhex(ask(f"desc {len(blk)} {min(128, 32 + length - len(blk))}")["hex"])
descriptor = json.loads(zlib.decompress(bytes(blk[32:32 + length])))
print(descriptor["identity"])
```

## 4. When no runtime answers

If the app is broken or lacks the runtime, the block can still be read through the Optiboot bootloader (STK500v1, 115200): reset, `STK_GET_SYNC` (`30 20` → `14 10`), then per 256-byte page `STK_UNIVERSAL` `56 4D 00 <word_addr>>16> 00 20` and `STK_LOAD_ADDRESS` `55 <word_addr lo> <word_addr hi> 20` (**word** address), then `STK_READ_PAGE` `74 01 00 46 20` (reply `14` + 256 bytes + `10`); `STK_LEAVE_PROGMODE` `51 20` starts the app. See [../host/aiready/stk500.py](../host/aiready/stk500.py). To change the block use `aiready write-descriptor`.

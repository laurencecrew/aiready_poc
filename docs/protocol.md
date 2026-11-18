# aiready serial protocol (format 1)

How to talk to an AI-ready board **without** the host tools. With them installed (`pip install mcp git+https://github.com/laurencecrew/aiready_poc#subdirectory=host`), use `python -m aiready ...` or the MCP server instead.

## 1. Connection

| Item | Value |
|---|---|
| Link | USB serial (CH340G, VID:PID 1A86:7523 on the POC board) |
| Settings | 115200 baud, 8N1, no flow control |
| Reset | Opening the port with DTR asserted (the default) resets the board |
| After a reset | Bootloader runs ~1.24 s, then the app prints a banner line. **Send nothing until the banner appears**: a byte received during the bootloader window keeps the bootloader resident |
| Querying without a reset | Open with DTR and RTS disabled. On Windows also close with both disabled *in the port configuration* (re-apply the baud rate before closing), or the next such open can hang the MCU |

## 2. Commands

Send `@ai <command>` followed by a newline. Every reply is **one JSON line** whose `"r"` field names the command it answers (`"err"` for errors). Other lines belong to the application.

| Command | Reply (main fields) |
|---|---|
| `help` | `cmds`, `protocol`, `desc_format`, `tools`, `docs` |
| `id` | `board`, `profile`, `fw`, `ver`, `built`, `desc` (descriptor id, or `"none"`), `desc_addr`, `reset` (`soft` or `power_ext_or_wdt`), `up_ms`, `ram_free`, `tools`, `install`, `mcp` |
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

The board's self-description is an 8 KB block in flash (0x3A000 on the POC board). Read it with `desc`, 128 bytes at a time. Header, 32 bytes, little-endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | AVR `jmp` to the bootloader (landing pad, ignore) |
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

If the app is broken or lacks the runtime, the block can still be read through the STK500v2 bootloader: reset, `CMD_SIGN_ON`, `CMD_LOAD_ADDRESS` with the **word** address and bit 31 set (0x80000000 | 0x3A000/2), then `CMD_READ_FLASH_ISP` in 256-byte chunks. See [../host/aiready/stk500.py](../host/aiready/stk500.py). Never write through the bootloader except with contiguous images from 0x0000 ([hardware.md](hardware.md) §2.1).

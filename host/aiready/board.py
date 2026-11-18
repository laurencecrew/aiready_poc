"""Board-level operations: discover, describe, state. JSON-friendly return values."""
from __future__ import annotations

import json
import time
from pathlib import Path

from serial.tools import list_ports

from . import DESCRIPTOR_JSON
from .block import BLOCK_ADDR, BLOCK_SIZE, HEADER_SIZE, parse_header, unpack
from .link import NoRuntime, RuntimeLink, open_runtime
from .stk500 import Stk500Error, read_flash_range

KNOWN_USB = {(0x1A86, 0x7523): "CH340 (FYSETC One+ and other Mega2560 clones)"}
CACHE_DIR = Path.home() / ".aiready" / "cache"


class BoardError(RuntimeError):
    pass


# ------------------------------------------------------------------ ports
def candidate_ports() -> list[dict]:
    out = []
    for p in list_ports.comports():
        known = KNOWN_USB.get((p.vid, p.pid)) if p.vid is not None else None
        out.append({"port": p.device, "vid_pid": f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None else None,
                    "description": p.description, "known_bridge": known})
    return out


def resolve_port(port: str | None) -> str:
    if port:
        return port
    known = [p["port"] for p in candidate_ports() if p["known_bridge"]]
    if len(known) == 1:
        return known[0]
    if not known:
        raise BoardError("no known USB-serial bridge found; pass --port (board usually on COM5)")
    raise BoardError(f"several candidate ports {known}; pass --port")


# ------------------------------------------------------------------ descriptor
def _cache_get(did: str) -> dict | None:
    f = CACHE_DIR / f"{did}.json"
    if f.exists():
        try:
            return json.loads(f.read_text())
        except json.JSONDecodeError:
            return None
    return None


def _cache_put(did: str, obj: dict) -> None:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    (CACHE_DIR / f"{did}.json").write_text(json.dumps(obj))


def read_block_bootloader(port: str) -> bytes:
    def total(head: bytes) -> int:
        h = parse_header(head)
        return min(HEADER_SIZE + h.length, BLOCK_SIZE) if h else 0
    try:
        return read_flash_range(port, BLOCK_ADDR, BLOCK_SIZE, header_then=total)
    except Stk500Error as e:
        raise BoardError(f"bootloader read failed: {e}") from e


def fetch_descriptor(port: str | None = None, source: str = "auto", use_cache: bool = True) -> dict:
    """Return {"descriptor": obj, "id": ..., "source": "runtime"|"bootloader"|"cache"}."""
    port = resolve_port(port)
    errors = []
    if source in ("auto", "runtime"):
        try:
            with open_runtime(port) as link:
                head = bytes.fromhex(link.cmd(f"desc 0 {HEADER_SIZE}")["hex"])
                h = parse_header(head)
                if h is None:
                    raise BoardError("runtime is running but the flash descriptor block is empty: "
                                     "run 'aiready write-descriptor'")
                if use_cache and (obj := _cache_get(h.id)):
                    return {"id": h.id, "source": "runtime(id)+cache", "port": port, "descriptor": obj}
                blk = link.read_descriptor_block()
            h, obj = unpack(blk)
            _cache_put(h.id, obj)
            return {"id": h.id, "source": "runtime", "port": port, "descriptor": obj}
        except (NoRuntime, OSError) as e:
            errors.append(f"runtime: {e}")
            if source == "runtime":
                raise BoardError("; ".join(errors)) from e
    try:
        blk = read_block_bootloader(port)
        h, obj = unpack(blk)
        _cache_put(h.id, obj)
        return {"id": h.id, "source": "bootloader", "port": port, "descriptor": obj}
    except (BoardError, ValueError, OSError) as e:
        errors.append(f"bootloader: {e}")
        raise BoardError("; ".join(errors)) from e


def select(obj, path: str | None):
    """Dotted path into the descriptor: 'peripherals.steppers', 'pins', 'pins.X_STEP'."""
    if not path or path == "all":
        return obj
    cur = obj
    for part in path.split("."):
        if isinstance(cur, dict) and part in cur:
            cur = cur[part]
        elif isinstance(cur, list):
            match = [x for x in cur if isinstance(x, dict) and
                     (str(x.get("name")) == part or str(x.get("pin")) == part or str(x.get("id")) == part)]
            if not match and part.isdigit() and int(part) < len(cur):
                match = [cur[int(part)]]
            if not match:
                raise BoardError(f"'{part}' not found in list")
            cur = match[0] if len(match) == 1 else match
        else:
            keys = list(cur) if isinstance(cur, dict) else []
            raise BoardError(f"'{part}' not found; available: {keys}")
    return cur


def summary(obj: dict) -> dict:
    """Cheap first look: identity, profile and a table of contents with sizes."""
    toc = {}
    for k, v in obj.items():
        size = len(json.dumps(v))
        if isinstance(v, dict):
            toc[k] = {"keys": list(v)[:20], "bytes": size}
        elif isinstance(v, list):
            toc[k] = {"items": len(v), "bytes": size}
    return {
        "identity": obj.get("identity"),
        "mcu": {k: obj.get("mcu", {}).get(k) for k in ("part", "clock_hz", "flash_bytes", "app_max_bytes", "sram_bytes")},
        "safety_rules": obj.get("safety_rules"),
        "sections": toc,
        "hint": "describe <section>[.<key>...] for detail, e.g. 'peripherals.steppers', 'pins.X_STEP', 'all'",
    }


def describe(port: str | None = None, section: str | None = None, source: str = "auto") -> dict:
    r = fetch_descriptor(port, source)
    obj = r.pop("descriptor")
    r["data"] = summary(obj) if section is None else select(obj, section)
    r["section"] = section or "summary"
    return r


def local_descriptor(path: Path = DESCRIPTOR_JSON) -> dict:
    return json.loads(Path(path).read_text())


# ------------------------------------------------------------------ discovery
def probe_port(port: str, allow_reset: bool = True) -> dict:
    res = {"port": port, "runtime": None, "descriptor_id": None, "via": None}
    try:
        with open_runtime(port, allow_reset=allow_reset) as link:
            ident = link.cmd("id")
            res.update(runtime=ident, descriptor_id=ident.get("desc"), via="runtime")
            return res
    except (NoRuntime, OSError) as e:
        res["runtime_error"] = str(e)
    if not allow_reset:
        return res
    try:
        blk = read_block_bootloader(port)
        h = parse_header(blk)
        res.update(descriptor_id=h.id if h else None, via="bootloader",
                   note="no runtime answered; descriptor read from flash via the bootloader" if h
                   else "bootloader answered but no descriptor block in flash")
    except BoardError as e:
        res["bootloader_error"] = str(e)
    return res


def discover(port: str | None = None) -> dict:
    ports = candidate_ports()
    targets = [port] if port else [p["port"] for p in ports if p["known_bridge"]]
    return {"ports": ports, "boards": [probe_port(p) for p in targets]}


# ------------------------------------------------------------------ live state
def runtime_cmd(port: str | None, command: str, timeout: float = 1.5) -> dict:
    port = resolve_port(port)
    try:
        with open_runtime(port) as link:
            return link.cmd(command, timeout=timeout)
    except (NoRuntime, OSError) as e:
        raise BoardError(f"{e}. The running firmware must include the aiready runtime "
                         "(firmware/lib/aiready); 'aiready describe --source bootloader' still works.") from e


def watch(port: str | None, interval_ms: int = 200, duration_s: float = 3.0) -> list[dict]:
    port = resolve_port(port)
    with open_runtime(port) as link:
        link.cmd(f"watch {interval_ms}")
        try:
            return list(link.stream("state", duration_s))
        finally:
            link.ser.write(b"@ai watch 0\n")
            time.sleep(0.05)

"""Flashing and read-back through the Optiboot bootloader (STK500v1, avrdude '-c arduino')."""
from __future__ import annotations

import glob
import hashlib
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

from . import BACKUP_DIR, DESCRIPTOR_HEX, DESCRIPTOR_JSON, ihex
from .block import APP_MAX, BLOCK_ADDR, BLOCK_SIZE, BOOT_ADDR, PAGE, pack, parse_header, to_hex_map
from .board import BoardError, local_descriptor, probe_port, resolve_port


def find_avrdude() -> tuple[str, str]:
    """AIREADY_AVRDUDE_DIR, else PlatformIO's avrdude packages (any version works with Optiboot)."""
    env = os.environ.get("AIREADY_AVRDUDE_DIR")
    pk = Path.home() / ".platformio" / "packages"
    roots = [env] if env else [str(pk / "tool-avrdude")] + sorted(glob.glob(str(pk / "tool-avrdude@*")), reverse=True)
    for r in roots:
        exe = Path(r) / ("avrdude.exe" if os.name == "nt" else "avrdude")
        conf = Path(r) / "avrdude.conf"
        if exe.exists() and conf.exists():
            return str(exe), str(conf)
    raise BoardError("avrdude not found. Install with: pio pkg install -g -t platformio/tool-avrdude "
                     "(or set AIREADY_AVRDUDE_DIR)")


def avrdude(port: str, ops: list[str], timeout: int = 300) -> str:
    exe, conf = find_avrdude()
    cmd = [exe, "-C", conf, "-c", "arduino", "-p", "m2560", "-P", port, "-b", "115200", "-D"]
    for op in ops:
        cmd += ["-U", op]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    out = (p.stdout or "") + (p.stderr or "")
    if p.returncode != 0:
        tail = "\n".join(out.strip().splitlines()[-12:])
        raise BoardError(f"avrdude failed (exit {p.returncode}). Close any serial monitor on {port}.\n{tail}")
    return out


def _app_sha(mem: dict[int, int]) -> str:
    used = sorted(a for a, b in mem.items() if a < BLOCK_ADDR and b != 0xFF)
    if not used:
        return "empty"
    data = bytes(mem.get(a, 0xFF) for a in range(0, used[-1] + 1))
    return hashlib.sha256(data).hexdigest()[:12]


# ------------------------------------------------------------------ readback
def readback(port: str | None = None, out_dir: Path = BACKUP_DIR, eeprom: bool = True) -> dict:
    port = resolve_port(port)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    tmp_flash = out_dir / f"{stamp}_flash.tmp.hex"
    tmp_ee = out_dir / f"{stamp}_eeprom.tmp.hex"
    ops = [f"flash:r:{tmp_flash}:i"] + ([f"eeprom:r:{tmp_ee}:i"] if eeprom else [])
    t0 = time.monotonic()
    avrdude(port, ops, timeout=600)
    mem = ihex.load(tmp_flash)
    sha = _app_sha(mem)
    flash_path = out_dir / f"{stamp}_{sha}_flash.hex"
    tmp_flash.replace(flash_path)
    res = {"port": port, "flash": str(flash_path), "app_sha": sha, "seconds": round(time.monotonic() - t0, 1),
           "app_bytes_used": (max((a for a, b in mem.items() if a < BLOCK_ADDR and b != 0xFF), default=-1) + 1)}
    if eeprom:
        ee_path = out_dir / f"{stamp}_{sha}_eeprom.hex"
        tmp_ee.replace(ee_path)
        res["eeprom"] = str(ee_path)
    h = parse_header(bytes(mem.get(BLOCK_ADDR + i, 0xFF) for i in range(32)))
    res["descriptor_id_in_flash"] = h.id if h else None
    return res


# ------------------------------------------------------------------ image preparation
def prepare_image(mem: dict[int, int]) -> tuple[dict[int, int], list[str]]:
    """Application region only, no trailing blank pages. Returns (image, notes).
    Full readbacks include the descriptor block and the bootloader; writing those as part of
    an app upload is never wanted (the block has its own command, the bootloader is ISP-only)."""
    notes = []
    boot = sum(1 for a in mem if a >= BOOT_ADDR)
    if boot:
        notes.append(f"stripped {boot} bytes at/above 0x{BOOT_ADDR:05X} (bootloader is never written)")
    desc = sum(1 for a in mem if BLOCK_ADDR <= a < BOOT_ADDR)
    if desc:
        notes.append(f"stripped {desc} bytes in the descriptor block 0x{BLOCK_ADDR:05X}-0x{BOOT_ADDR - 1:05X}")
    img = {a: b for a, b in mem.items() if a < BLOCK_ADDR}
    used = [a for a, b in img.items() if b != 0xFF]
    if not used:
        raise BoardError("image is empty")
    end = (max(used) // PAGE + 1) * PAGE   # readbacks are 0xFF-padded to the end of flash
    dropped = sum(1 for a in img if a >= end)
    if dropped:
        notes.append(f"dropped {dropped} trailing blank bytes")
    return {a: v for a, v in img.items() if a < end}, notes


# ------------------------------------------------------------------ flash
def flash(hex_path: str | Path, port: str | None = None, backup: bool = True,
          restore_descriptor: bool = True) -> dict:
    port = resolve_port(port)
    hex_path = Path(hex_path)
    mem = ihex.load(hex_path)
    img, notes = prepare_image(mem)
    res: dict = {"port": port, "hex": str(hex_path), "image_bytes": max(img) + 1, "notes": notes}
    if backup:
        res["backup"] = readback(port)
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td) / "image.hex"
        ihex.save(img, tmp)
        t0 = time.monotonic()
        out = avrdude(port, [f"flash:w:{tmp}:i"])
        res["write_seconds"] = round(time.monotonic() - t0, 1)
        res["verified"] = "verified" in out.lower()
    time.sleep(0.3)
    res["after"] = check_board(port, restore_descriptor=restore_descriptor)
    return res


def check_board(port: str, restore_descriptor: bool = True) -> dict:
    """After an upload: does a runtime answer, and is the descriptor block still intact?"""
    p = probe_port(port)
    expected = None
    if DESCRIPTOR_JSON.exists():
        from .block import descriptor_id
        expected = descriptor_id(local_descriptor())
    p["descriptor_local"] = expected
    p["descriptor_matches_local"] = p.get("descriptor_id") == expected
    if p.get("descriptor_id") in (None, "none") and restore_descriptor and DESCRIPTOR_JSON.exists():
        p["descriptor_restored"] = write_descriptor(port)
    return p


# ------------------------------------------------------------------ descriptor block
def pack_descriptor(json_path: Path = DESCRIPTOR_JSON, out: Path = DESCRIPTOR_HEX) -> dict:
    obj = json.loads(Path(json_path).read_text())
    blk, did = pack(obj)
    ihex.save(to_hex_map(blk), out)
    return {"id": did, "hex": str(out), "block_bytes": len(blk), "capacity": BLOCK_SIZE,
            "raw_json_bytes": len(json.dumps(obj, separators=(',', ':')))}


def write_descriptor(port: str | None = None, json_path: Path = DESCRIPTOR_JSON) -> dict:
    """Pack the JSON and write the whole 16 KB block (0xFF-padded) through Optiboot, page by
    page, then read it back. Optiboot erases each page it writes; the app is not touched."""
    from .stk500 import write_flash_pages

    port = resolve_port(port)
    info = pack_descriptor(json_path)
    blk = ihex.load(info["hex"])
    data = bytes(blk.get(BLOCK_ADDR + i, 0xFF) for i in range(BLOCK_SIZE))
    pages = {BLOCK_ADDR + o: data[o:o + PAGE] for o in range(0, BLOCK_SIZE, PAGE)}
    w = write_flash_pages(port, pages)
    info.update(write_seconds=w["seconds"], pages=w["pages"], verified=w["verified"])
    if w["mismatch"]:
        info["mismatch_pages"] = w["mismatch"]
    time.sleep(1.0)   # bootloader exits by watchdog reset; let the app print its banner
    p = probe_port(port)
    info["runtime_reports_id"] = p.get("descriptor_id")
    info["ok"] = bool(w["verified"])
    return info

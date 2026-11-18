"""Flashing and read-back through the bootloader with avrdude 6.3 (8.x cannot sync)."""
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
from .block import BLOCK_ADDR, BLOCK_SIZE, BOOT_ADDR, NOP_FILL_ADDR, WDT_SAFE_APP_LIMIT, pack, parse_header, to_hex_map
from .board import BoardError, local_descriptor, probe_port, read_block_bootloader, resolve_port

PAGE = 256


def find_avrdude() -> tuple[str, str]:
    env = os.environ.get("AIREADY_AVRDUDE_DIR")
    roots = [env] if env else sorted(glob.glob(str(Path.home() / ".platformio" / "packages" / "tool-avrdude@1.60300*")))
    for r in reversed(roots):
        exe = Path(r) / ("avrdude.exe" if os.name == "nt" else "avrdude")
        conf = Path(r) / "avrdude.conf"
        if exe.exists() and conf.exists():
            return str(exe), str(conf)
    raise BoardError("avrdude 6.3 not found. Install with: pio pkg install -g -t platformio/tool-avrdude@~1.60300.0 "
                     "(or set AIREADY_AVRDUDE_DIR)")


def avrdude(port: str, ops: list[str], timeout: int = 300) -> str:
    exe, conf = find_avrdude()
    cmd = [exe, "-C", conf, "-c", "wiring", "-p", "m2560", "-P", port, "-b", "115200", "-D"]
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
    """Application region only, contiguous from 0x0000, no trailing blank pages.
    Returns (image, notes).

    Full readbacks (and the original Marlin backup) include the bootloader and descriptor
    regions; writing those through the bootloader is never wanted.

    Contiguity matters: this bootloader does not erase the page it writes. It erases pages
    sequentially from 0x0000 (one per page written in the session). Any gap or a non-zero
    start makes later pages get programmed without an erase (corrupted) and erases pages
    that were not meant to be touched."""
    notes = []
    boot = sum(1 for a in mem if a >= BOOT_ADDR)
    if boot:
        notes.append(f"stripped {boot} bytes at/above 0x{BOOT_ADDR:05X} (bootloader is never written)")
    desc = sum(1 for a in mem if BLOCK_ADDR <= a < BOOT_ADDR)
    if desc:
        notes.append(f"stripped {desc} bytes in the descriptor block / NOP fill 0x{BLOCK_ADDR:05X}-0x{BOOT_ADDR - 1:05X}")
    img = {a: b for a, b in mem.items() if a < BLOCK_ADDR}
    used = [a for a, b in img.items() if b != 0xFF]
    if not used:
        raise BoardError("image is empty")
    # Readbacks are padded with 0xFF to the end of flash; keep blank pages inside the image only.
    end = (max(used) // PAGE + 1) * PAGE
    dropped = sum(1 for a in img if a >= end)
    if dropped:
        notes.append(f"dropped {dropped} trailing blank bytes")
    gaps = sum(1 for a in range(max(used) + 1) if a not in img)
    if gaps:
        notes.append(f"filled {gaps} gap bytes with 0xFF so every page from 0x0000 is written in order")
    img = {a: img.get(a, 0xFF) for a in range(end)}
    return img, notes


def check_bang3(img: dict[int, int]) -> list[str]:
    hits = ihex.contains_bang3(img)
    return [f"0x{a:05X}" for a in hits[:10]]


# ------------------------------------------------------------------ flash
def flash(hex_path: str | Path, port: str | None = None, backup: bool = True, force: bool = False,
          restore_descriptor: bool = True) -> dict:
    port = resolve_port(port)
    hex_path = Path(hex_path)
    mem = ihex.load(hex_path)
    img, notes = prepare_image(mem)
    bang = check_bang3(img)
    if bang and not force:
        raise BoardError(f"image contains '!!!' at {bang}: this bootloader can hang on it. "
                         "Change the string, or pass force=True to try anyway.")
    if max(img) >= WDT_SAFE_APP_LIMIT:
        notes.append("WARNING: image extends past 0x20000. After a watchdog reset this bootloader jumps to "
                     "0x20000 (EIND bug) and will execute application code there instead of rebooting.")
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


def current_app_image(port: str) -> dict[int, int]:
    """Read the application region from the board (bootloader, ~25 s) as a prepared image."""
    from .stk500 import read_flash_range
    data = read_flash_range(port, 0, BLOCK_ADDR)
    img, _ = prepare_image(ihex.from_bytes(0, data))
    return img


def write_descriptor(port: str | None = None, json_path: Path = DESCRIPTOR_JSON,
                     app_hex: str | Path | None = None) -> dict:
    """Write the descriptor block. Because of the bootloader's sequential-erase bug the block
    can only be written as part of ONE contiguous image from 0x0000:
      app | 0xFF to 0x3A000 | block | 0xFF to 0x3C000 | 0x00 (NOP) fill to 0x3DFFF
    The bootloader never erases >= 0x3C000, so that region only ever gets bits cleared: all
    zeros is the one stable content, and 0x0000 is an AVR NOP for the watchdog-reset slide.
    The app is taken from app_hex, or read back from the board first (~25 s extra)."""
    port = resolve_port(port)
    info = pack_descriptor(json_path)
    if app_hex:
        app, _ = prepare_image(ihex.load(app_hex))
        info["app_source"] = str(app_hex)
    else:
        app = current_app_image(port)
        info["app_source"] = "read back from board"
    blk_map = ihex.load(info["hex"])
    full = {a: app.get(a, 0xFF) for a in range(NOP_FILL_ADDR)}
    full.update(blk_map)
    full.update({a: 0x00 for a in range(NOP_FILL_ADDR, BOOT_ADDR)})
    end = BOOT_ADDR
    bang = check_bang3(full)
    if bang:
        raise BoardError(f"combined image contains '!!!' at {bang}")
    info["app_bytes"] = max(app) + 1
    info["image_bytes"] = end
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td) / "full.hex"
        ihex.save(full, tmp)
        t0 = time.monotonic()
        out = avrdude(port, [f"flash:w:{tmp}:i"], timeout=600)
        info["write_seconds"] = round(time.monotonic() - t0, 1)
    info["verified"] = "verified" in out.lower()
    time.sleep(0.3)
    blk = read_block_bootloader(port)
    h = parse_header(blk)
    info["read_back_id"] = h.id if h else None
    info["ok"] = bool(h and h.id == info["id"])
    return info

"""Descriptor block: the self-description as stored in board flash.

Layout at BLOCK_ADDR (little-endian, 32-byte header, then zlib payload):
  0  4s  landing pad: AVR 'jmp 0x3E000' (0D 94 00 F0)
  4  4s  magic "AIRD"
  8  u16 format (1)
 10  u16 compression (1 = zlib)
 12  u32 payload length (compressed)
 16  u32 raw length (minified JSON)
 20  u32 CRC32 of payload
 24  8s  id = first 8 bytes of SHA-256(minified JSON)

Why the landing pad: after a watchdog reset this bootloader 'eicall's its app pointer with
EIND=1, i.e. it jumps to 0x20000 instead of 0x0000. On blank flash the CPU slides through
0xFFFF words into the bootloader at 0x3E000 and boots normally. The block sits in that path,
so it must start with a jump to the bootloader, or the CPU executes descriptor data and hangs.
(Consequence: application code must stay below 0x20000 for watchdog resets to work.)
"""
from __future__ import annotations

import hashlib
import json
import struct
import zlib
from dataclasses import dataclass

BLOCK_ADDR = 0x3A000
BLOCK_SIZE = 0x2000
NOP_FILL_ADDR = 0x3C000  # bootloader never erases >= here: keep as 0x00 (NOP) fill
BOOT_ADDR = 0x3E000
FLASH_SIZE = 0x40000
MAGIC = b"AIRD"
LANDING_PAD = bytes.fromhex("0d9400f0")  # jmp 0x3E000 (bootloader reset vector)
WDT_SAFE_APP_LIMIT = 0x20000
HEADER = struct.Struct("<4s4sHHIII8s")
HEADER_SIZE = HEADER.size  # 32
FORMAT = 1
COMP_ZLIB = 1


@dataclass
class Header:
    format: int
    compression: int
    length: int
    raw_length: int
    crc32: int
    id: str

    def to_dict(self) -> dict:
        return self.__dict__.copy()


def minify(obj) -> bytes:
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=True).encode()


def descriptor_id(obj) -> str:
    return hashlib.sha256(minify(obj)).digest()[:8].hex()


def parse_header(data: bytes) -> Header | None:
    if len(data) < HEADER_SIZE or data[4:8] != MAGIC:
        return None
    pad, magic, fmt, comp, length, raw, crc, did = HEADER.unpack_from(data)
    return Header(fmt, comp, length, raw, crc, did.hex())


def pack(obj) -> tuple[bytes, str]:
    """Return (block bytes, id). Raises ValueError if it cannot fit or avoid '!!!'."""
    raw = minify(obj)
    did = hashlib.sha256(raw).digest()[:8]
    for level in (9, 8, 7, 6, 5, 4, 3, 2, 1):
        payload = zlib.compress(raw, level)
        block = HEADER.pack(LANDING_PAD, MAGIC, FORMAT, COMP_ZLIB, len(payload), len(raw),
                            zlib.crc32(payload) & 0xFFFFFFFF, did) + payload
        if len(block) > BLOCK_SIZE:
            raise ValueError(f"descriptor too large: {len(block)} bytes > {BLOCK_SIZE}")
        if b"!!!" not in block:
            return block, did.hex()
    raise ValueError("every compression level produced '!!!' (bootloader quirk); change the JSON slightly")


def unpack(block: bytes) -> tuple[Header, dict]:
    h = parse_header(block)
    if h is None:
        raise ValueError("no descriptor block (magic 'AIRD' not found); flash one with 'aiready write-descriptor'")
    if h.format != FORMAT or h.compression != COMP_ZLIB:
        raise ValueError(f"unsupported descriptor format {h.format}/compression {h.compression}")
    payload = block[HEADER_SIZE:HEADER_SIZE + h.length]
    if len(payload) != h.length:
        raise ValueError("descriptor block truncated")
    if zlib.crc32(payload) & 0xFFFFFFFF != h.crc32:
        raise ValueError("descriptor CRC mismatch (corrupt block)")
    raw = zlib.decompress(payload)
    if hashlib.sha256(raw).digest()[:8].hex() != h.id:
        raise ValueError("descriptor id/hash mismatch")
    return h, json.loads(raw)


def to_hex_map(block: bytes) -> dict[int, int]:
    return {BLOCK_ADDR + i: b for i, b in enumerate(block)}

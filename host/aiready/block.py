"""Descriptor block: the self-description as stored in board flash.

Layout at BLOCK_ADDR (little-endian, 32-byte header, then zlib payload):
  0  4s  reserved: AVR 'jmp <bootloader>' (0D 94 00 FE = jmp 0x3FC00); ignore when reading
  4  4s  magic "AIRD"
  8  u16 format (1)
 10  u16 compression (1 = zlib)
 12  u32 payload length (compressed)
 16  u32 raw length (minified JSON)
 20  u32 CRC32 of payload
 24  8s  id = first 8 bytes of SHA-256(minified JSON)

The block is the 16 KB directly below the 1 KB Optiboot bootloader. Optiboot erases each page
it writes, so the block survives normal app uploads (which never reach it) and can be
rewritten on its own. The leading jmp is a leftover from the old stk500v2 bootloader (whose
watchdog path slid into this area); it is harmless and keeps the header format unchanged.
"""
from __future__ import annotations

import hashlib
import json
import struct
import zlib
from dataclasses import dataclass

BOOT_ADDR = 0x3FC00      # Optiboot (MegaCore optiboot_flash BIGBOOT), 1 KB boot section
BLOCK_SIZE = 0x4000      # 16 KB
BLOCK_ADDR = BOOT_ADDR - BLOCK_SIZE   # 0x3BC00
APP_MAX = BLOCK_ADDR     # 244736 bytes
FLASH_SIZE = 0x40000
PAGE = 256
MAGIC = b"AIRD"
LANDING_PAD = bytes.fromhex("0d9400fe")  # jmp 0x3FC00 (reserved field, see above)
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
    """Return (block bytes, id). Raises ValueError if it does not fit."""
    raw = minify(obj)
    did = hashlib.sha256(raw).digest()[:8]
    payload = zlib.compress(raw, 9)
    block = HEADER.pack(LANDING_PAD, MAGIC, FORMAT, COMP_ZLIB, len(payload), len(raw),
                        zlib.crc32(payload) & 0xFFFFFFFF, did) + payload
    if len(block) > BLOCK_SIZE:
        raise ValueError(f"descriptor too large: {len(block)} bytes > {BLOCK_SIZE}")
    return block, did.hex()


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

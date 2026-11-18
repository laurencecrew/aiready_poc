"""Minimal Intel HEX read/write (record types 00, 01, 02, 04) as a sparse {addr: byte} map."""
from __future__ import annotations

from pathlib import Path


def parse(text: str) -> dict[int, int]:
    mem: dict[int, int] = {}
    base = 0
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        if line[0] != ":":
            raise ValueError(f"line {n}: not an Intel HEX record")
        rec = bytes.fromhex(line[1:])
        count, addr, rtype = rec[0], (rec[1] << 8) | rec[2], rec[3]
        data = rec[4:4 + count]
        if len(data) != count or (sum(rec) & 0xFF) != 0:
            raise ValueError(f"line {n}: bad length or checksum")
        if rtype == 0:
            for i, b in enumerate(data):
                mem[base + addr + i] = b
        elif rtype == 1:
            break
        elif rtype == 2:
            base = int.from_bytes(data, "big") << 4
        elif rtype == 4:
            base = int.from_bytes(data, "big") << 16
        # 03/05 (start address) are irrelevant for AVR uploads
    return mem


def load(path: str | Path) -> dict[int, int]:
    return parse(Path(path).read_text())


def _rec(addr: int, rtype: int, data: bytes) -> str:
    body = bytes([len(data), (addr >> 8) & 0xFF, addr & 0xFF, rtype]) + data
    return ":" + (body + bytes([(-sum(body)) & 0xFF])).hex().upper()


def dumps(mem: dict[int, int], width: int = 32) -> str:
    """Serialise a sparse map; contiguous runs are split at `width` and 64 K boundaries."""
    out: list[str] = []
    seg = -1
    addrs = sorted(mem)
    i = 0
    while i < len(addrs):
        start = addrs[i]
        chunk = bytearray([mem[start]])
        i += 1
        while (i < len(addrs) and addrs[i] == start + len(chunk) and len(chunk) < width
               and (addrs[i] >> 16) == (start >> 16)):
            chunk.append(mem[addrs[i]])
            i += 1
        if (start >> 16) != seg:
            seg = start >> 16
            out.append(_rec(0, 4, seg.to_bytes(2, "big")))
        out.append(_rec(start & 0xFFFF, 0, bytes(chunk)))
    out.append(":00000001FF")
    return "\n".join(out) + "\n"


def save(mem: dict[int, int], path: str | Path) -> None:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(dumps(mem))


def from_bytes(base: int, data: bytes) -> dict[int, int]:
    return {base + i: b for i, b in enumerate(data)}


def contains_bang3(mem: dict[int, int]) -> list[int]:
    """Addresses where the byte sequence '!!!' (0x21 x3) starts; it can hang this bootloader."""
    hits = []
    for a, b in mem.items():
        if b == 0x21 and mem.get(a + 1) == 0x21 and mem.get(a + 2) == 0x21:
            hits.append(a)
    return sorted(hits)

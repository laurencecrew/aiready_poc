"""Minimal STK500v1 client for the board's Optiboot bootloader (MegaCore optiboot_flash, BIGBOOT).

Used to read the descriptor block straight from flash when no runtime answers, and to write
the descriptor block on its own (Optiboot erases each page it writes, so writing only the
block's pages leaves the application untouched). Application uploads go through avrdude.

Optiboot behaviour this relies on (checked in optiboot_flash.c, MegaCore v2.2.3):
  - It only stays in the bootloader after an external (DTR/RESET) reset; ~1 s timeout.
  - Power-on and watchdog resets jump straight to the app ('jmp 0'); MCUSR is kept, and
    also passed in r2.
  - A byte that is not a valid command ends in a 16 ms watchdog reset into the app.
  - Above 128 KB: STK_UNIVERSAL 0x4D 0x00 <ext> 0x00 sets RAMPZ bit 1 (ext = word address
    >> 16), STK_LOAD_ADDRESS takes the low 16 bits of the word address (bit 15 -> RAMPZ bit 0).
"""
from __future__ import annotations

import time

STK_OK = 0x10
STK_INSYNC = 0x14
CRC_EOP = 0x20
STK_GET_SYNC = 0x30
STK_LEAVE_PROGMODE = 0x51
STK_LOAD_ADDRESS = 0x55
STK_UNIVERSAL = 0x56
STK_PROG_PAGE = 0x64
STK_READ_PAGE = 0x74
STK_READ_SIGN = 0x75
AVR_OP_LOAD_EXT_ADDR = 0x4D
PAGE = 256
SIGNATURE_M2560 = bytes([0x1E, 0x98, 0x01])


class Stk500Error(RuntimeError):
    pass


class Optiboot:
    def __init__(self, ser):
        self.ser = ser

    def _read(self, n: int, timeout: float) -> bytes:
        buf = bytearray()
        end = time.monotonic() + timeout
        while len(buf) < n and time.monotonic() < end:
            buf += self.ser.read(n - len(buf))
        return bytes(buf)

    def _cmd(self, body: bytes, reply_len: int = 0, timeout: float = 0.5) -> bytes:
        """Send body + CRC_EOP; expect INSYNC, reply_len bytes, OK."""
        self.ser.write(body + bytes([CRC_EOP]))
        r = self._read(reply_len + 2, timeout)
        if len(r) != reply_len + 2 or r[0] != STK_INSYNC or r[-1] != STK_OK:
            raise Stk500Error(f"bad reply to 0x{body[0]:02x}: {r.hex() or 'none'}")
        return r[1:-1]

    def reset_into_bootloader(self) -> None:
        """Pulse DTR/RTS (auto-reset), as avrdude's 'arduino' programmer does."""
        self.ser.dtr = False
        self.ser.rts = False
        time.sleep(0.05)
        self.ser.dtr = True
        self.ser.rts = True
        time.sleep(0.05)
        self.ser.reset_input_buffer()

    def sync(self, attempts: int = 25) -> None:
        for _ in range(attempts):
            self.ser.reset_input_buffer()
            self.ser.write(bytes([STK_GET_SYNC, CRC_EOP]))
            r = self._read(2, 0.05)
            if r == bytes([STK_INSYNC, STK_OK]):
                time.sleep(0.02)
                self.ser.reset_input_buffer()   # drop answers to earlier, overlapping syncs
                return
        raise Stk500Error("bootloader did not sync (is the port right, is Optiboot installed?)")

    def signature(self) -> bytes:
        return self._cmd(bytes([STK_READ_SIGN]), 3)

    def load_address(self, byte_addr: int) -> None:
        word = byte_addr >> 1
        self._cmd(bytes([STK_UNIVERSAL, AVR_OP_LOAD_EXT_ADDR, 0x00, (word >> 16) & 0xFF, 0x00]), 1)
        self._cmd(bytes([STK_LOAD_ADDRESS, word & 0xFF, (word >> 8) & 0xFF]))

    def read_flash(self, byte_addr: int, length: int) -> bytes:
        out = bytearray()
        while len(out) < length:
            a = byte_addr + len(out)
            n = min(PAGE - (a % PAGE), length - len(out))   # never cross a 64 KB boundary mid-read
            self.load_address(a)
            out += self._cmd(bytes([STK_READ_PAGE, n >> 8, n & 0xFF, ord("F")]), n)
        return bytes(out)

    def write_page(self, byte_addr: int, data: bytes) -> None:
        if byte_addr % PAGE or len(data) != PAGE:
            raise ValueError("write_page needs one full, page-aligned 256-byte page")
        self.load_address(byte_addr)
        self._cmd(bytes([STK_PROG_PAGE, PAGE >> 8, PAGE & 0xFF, ord("F")]) + data, 0, timeout=1.0)

    def leave(self) -> None:
        """Optiboot answers, then watchdog-resets into the app after 16 ms."""
        try:
            self._cmd(bytes([STK_LEAVE_PROGMODE]))
        except Stk500Error:
            pass


def _open_bootloader(port: str, baud: int):
    import serial

    ser = serial.Serial(port, baud, timeout=0.02)
    bl = Optiboot(ser)
    bl.reset_into_bootloader()
    bl.sync()
    sig = bl.signature()
    if sig != SIGNATURE_M2560:
        raise Stk500Error(f"unexpected device signature {sig.hex()} (expected 1e9801, ATmega2560)")
    return ser, bl


def read_flash_range(port: str, addr: int, length: int, baud: int = 115200,
                     header_then: "callable | None" = None) -> bytes:
    """Reset the board into the bootloader and read flash. If header_then is given it is
    called with the first 32 bytes and returns the total length to read (or 0 to stop)."""
    from .link import park_and_close

    ser, bl = _open_bootloader(port, baud)
    try:
        try:
            if header_then:
                head = bl.read_flash(addr, 32)
                total = header_then(head)
                if total <= 32:
                    return head
                return head + bl.read_flash(addr + 32, total - 32)
            return bl.read_flash(addr, length)
        finally:
            bl.leave()
    finally:
        park_and_close(ser)


def write_flash_pages(port: str, pages: dict[int, bytes], baud: int = 115200, verify: bool = True) -> dict:
    """Write whole 256-byte pages {page_addr: data} through Optiboot and read them back."""
    from .link import park_and_close

    ser, bl = _open_bootloader(port, baud)
    t0 = time.monotonic()
    try:
        try:
            for a in sorted(pages):
                bl.write_page(a, pages[a])
            bad = []
            if verify:
                for a in sorted(pages):
                    if bl.read_flash(a, PAGE) != pages[a]:
                        bad.append(f"0x{a:05X}")
        finally:
            bl.leave()
    finally:
        park_and_close(ser)
    return {"pages": len(pages), "verified": verify and not bad, "mismatch": bad,
            "seconds": round(time.monotonic() - t0, 1)}

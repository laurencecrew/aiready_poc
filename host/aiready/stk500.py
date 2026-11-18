"""Minimal STK500v2 client for the Mega2560 'Arduino explorer stk500V2 by MLS' bootloader.

Read-only: used to fetch the descriptor block straight from flash when no runtime answers
(broken app, app without the aiready runtime). Writing is left to avrdude 6.3.
"""
from __future__ import annotations

import time

MESSAGE_START = 0x1B
TOKEN = 0x0E
CMD_SIGN_ON = 0x01
CMD_LOAD_ADDRESS = 0x06
CMD_ENTER_PROGMODE_ISP = 0x10
CMD_LEAVE_PROGMODE_ISP = 0x11
CMD_READ_FLASH_ISP = 0x14
STATUS_CMD_OK = 0x00


class Stk500Error(RuntimeError):
    pass


def build_message(seq: int, body: bytes) -> bytes:
    head = bytes([MESSAGE_START, seq & 0xFF, (len(body) >> 8) & 0xFF, len(body) & 0xFF, TOKEN])
    msg = head + body
    cs = 0
    for b in msg:
        cs ^= b
    return msg + bytes([cs])


def parse_message(buf: bytes) -> tuple[int, bytes, int] | None:
    """Find one complete message in buf. Returns (seq, body, bytes_consumed) or None."""
    i = buf.find(bytes([MESSAGE_START]))
    while i != -1 and len(buf) - i >= 6:
        seq, size, tok = buf[i + 1], (buf[i + 2] << 8) | buf[i + 3], buf[i + 4]
        if tok == TOKEN and len(buf) - i >= 6 + size:
            frame = buf[i:i + 5 + size]
            cs = 0
            for b in frame:
                cs ^= b
            if cs == buf[i + 5 + size]:
                return seq, bytes(buf[i + 5:i + 5 + size]), i + 6 + size
        i = buf.find(bytes([MESSAGE_START]), i + 1)
    return None


class Stk500v2:
    def __init__(self, ser):
        self.ser = ser
        self.seq = 0
        self.rx = bytearray()

    def _xfer(self, body: bytes, timeout: float = 0.5) -> bytes:
        self.seq = (self.seq + 1) & 0xFF
        self.ser.write(build_message(self.seq, body))
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                self.rx += chunk
            m = parse_message(bytes(self.rx))
            if m:
                seq, rbody, used = m
                del self.rx[:used]
                if seq == self.seq and rbody and rbody[0] == body[0]:
                    return rbody
        raise Stk500Error(f"no reply to command 0x{body[0]:02x}")

    def reset_into_bootloader(self) -> None:
        """Pulse DTR/RTS (auto-reset), as avrdude's 'wiring' programmer does."""
        self.ser.dtr = False
        self.ser.rts = False
        time.sleep(0.05)
        self.ser.dtr = True
        self.ser.rts = True
        time.sleep(0.05)
        self.ser.reset_input_buffer()

    def sign_on(self, attempts: int = 20) -> str:
        for _ in range(attempts):
            try:
                r = self._xfer(bytes([CMD_SIGN_ON]), timeout=0.06)
            except Stk500Error:
                self.rx.clear()
                continue
            if len(r) >= 3 and r[1] == STATUS_CMD_OK:
                return r[3:3 + r[2]].decode(errors="replace")
        raise Stk500Error("bootloader did not answer sign-on (is the port right, is it an STK500v2 board?)")

    def enter_progmode(self) -> None:
        r = self._xfer(bytes([CMD_ENTER_PROGMODE_ISP, 200, 100, 25, 32, 0, 0x53, 3, 0xAC, 0x53, 0, 0]))
        if r[1] != STATUS_CMD_OK:
            raise Stk500Error("enter progmode failed")

    def load_address(self, byte_addr: int) -> None:
        word = byte_addr >> 1
        if byte_addr >= 0x10000:
            word |= 0x80000000  # extended address flag (ATmega2560)
        r = self._xfer(bytes([CMD_LOAD_ADDRESS]) + word.to_bytes(4, "big"))
        if r[1] != STATUS_CMD_OK:
            raise Stk500Error("load address failed")

    def read_flash(self, byte_addr: int, length: int, chunk: int = 256) -> bytes:
        out = bytearray()
        self.load_address(byte_addr)
        while len(out) < length:
            n = min(chunk, length - len(out))
            n += n & 1  # word aligned
            r = self._xfer(bytes([CMD_READ_FLASH_ISP, n >> 8, n & 0xFF, 0x20]))
            if r[1] != STATUS_CMD_OK or len(r) < n + 3:
                raise Stk500Error("read flash failed")
            out += r[2:2 + n]
        return bytes(out[:length])

    def leave_progmode(self) -> None:
        try:
            self._xfer(bytes([CMD_LEAVE_PROGMODE_ISP, 1, 1]))
        except Stk500Error:
            pass  # bootloader may jump to the app without answering


def read_flash_range(port: str, addr: int, length: int, baud: int = 115200,
                     header_then: "callable | None" = None) -> bytes:
    """Reset the board into the bootloader and read flash. If header_then is given it is
    called with the first 32 bytes and returns the total length to read (or 0 to stop)."""
    import serial

    from .link import park_and_close

    ser = serial.Serial(port, baud, timeout=0.02)
    try:
        bl = Stk500v2(ser)
        bl.reset_into_bootloader()
        bl.sign_on()
        bl.enter_progmode()
        try:
            if header_then:
                head = bl.read_flash(addr, 32)
                total = header_then(head)
                if total <= 32:
                    return head
                rest = bl.read_flash(addr + 32, total - 32)
                return head + rest
            return bl.read_flash(addr, length)
        finally:
            bl.leave_progmode()
    finally:
        park_and_close(ser)

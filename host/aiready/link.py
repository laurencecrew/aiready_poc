"""Serial link to the aiready runtime ('@ai <cmd>' -> one JSON line with "r": <cmd>)."""
from __future__ import annotations

import json
import time

import serial

BAUD = 115200


class NoRuntime(RuntimeError):
    pass


def park_and_close(ser) -> None:
    """Close the port leaving DTR/RTS *disabled in the saved port configuration (DCB)*.

    On Windows the CH340 driver re-applies the saved DCB when the port is next opened, before
    pyserial applies dtr=False. If the DCB still says DTR enabled, that open makes a very short
    DTR pulse which leaves the ATmega2560 hung (not reset) until a proper reset. pyserial's
    dtr setter only toggles the line (EscapeCommFunction), so re-apply the baud rate to make
    it rewrite the DCB with DTR/RTS disabled."""
    try:
        if ser.is_open:
            ser.dtr = False
            ser.rts = False
            ser.baudrate = ser.baudrate
    except (OSError, ValueError):
        pass
    finally:
        ser.close()


class RuntimeLink:
    """Open without resetting the board when possible (DTR held deasserted), so querying a
    running application does not restart it. reset=True restarts it and waits for the banner."""

    def __init__(self, port: str, reset: bool = False, baud: int = BAUD):
        self.port = port
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.05
        if not reset:
            self.ser.dtr = False
            self.ser.rts = False
        self.ser.open()
        self.banner: dict | None = None
        self.other_lines: list[str] = []  # non-runtime output seen while waiting (app prints)
        self._buf = b""
        if reset:
            self.banner = self.wait_for("banner", timeout=4.0)

    def close(self) -> None:
        park_and_close(self.ser)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _lines(self, timeout: float):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                self._buf += chunk
            while b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                yield line.decode(errors="replace").strip()

    def wait_for(self, r: str, timeout: float = 1.0) -> dict:
        for line in self._lines(timeout):
            if not line:
                continue
            if line.startswith("{"):
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    self.other_lines.append(line)
                    continue
                if obj.get("r") == r:
                    return obj
                if obj.get("r") == "banner":
                    self.banner = obj
                if obj.get("r") == "err" and r != "err":
                    raise NoRuntime(f"runtime error: {obj.get('msg')}")
            else:
                self.other_lines.append(line)
        raise NoRuntime(f"no '{r}' reply within {timeout:.1f} s on {self.port}")

    def cmd(self, command: str, timeout: float = 1.0) -> dict:
        self.ser.write(f"@ai {command}\n".encode())
        return self.wait_for(command.split()[0], timeout)

    def stream(self, r: str, duration: float):
        """Yield every JSON line with "r" == r for `duration` seconds."""
        for line in self._lines(duration):
            if line.startswith("{"):
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if obj.get("r") == r:
                    yield obj

    def read_descriptor_block(self, chunk: int = 128) -> bytes:
        from .block import HEADER_SIZE, BLOCK_SIZE, parse_header

        head = bytes.fromhex(self.cmd(f"desc 0 {HEADER_SIZE}")["hex"])
        h = parse_header(head)
        if h is None:
            return head
        total = min(HEADER_SIZE + h.length, BLOCK_SIZE)
        data = bytearray(head)
        while len(data) < total:
            n = min(chunk, total - len(data))
            data += bytes.fromhex(self.cmd(f"desc {len(data)} {n}")["hex"])
        return bytes(data)


def open_runtime(port: str, allow_reset: bool = True) -> RuntimeLink:
    """Try a no-reset connection first; fall back to resetting and waiting for the banner."""
    link = RuntimeLink(port, reset=False)
    try:
        link.cmd("id", timeout=0.8)
        return link
    except (NoRuntime, OSError):
        link.close()
    if not allow_reset:
        raise NoRuntime(f"no aiready runtime answering on {port} (without reset)")
    return RuntimeLink(port, reset=True)

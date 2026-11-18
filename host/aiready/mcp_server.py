"""MCP server exposing the aiready host functions as tools (stdio transport).

Run: python -m aiready.mcp_server   (or the 'aiready-mcp' script)
"""
from __future__ import annotations

try:  # mcp >= 2
    from mcp.server.mcpserver import MCPServer as _Server
except ImportError:  # mcp 1.x
    from mcp.server.fastmcp import FastMCP as _Server

from . import board, program

mcp = _Server(
    "aiready",
    instructions=(
        "Tools for an AI-ready, self-describing hardware board (POC: FYSETC One+ V2.2, ATmega2560). "
        "Start with discover, then describe() for a summary and describe('<section>') for detail. "
        "Respect descriptor.safety_rules before writing firmware. flash() backs up the current firmware first."
    ),
)


def _call(fn, *a, **k):
    try:
        return fn(*a, **k)
    except (board.BoardError, ValueError, OSError) as e:
        return {"error": str(e)}


@mcp.tool()
def discover(port: str | None = None) -> dict:
    """Find connected boards. Reports serial ports, runtime identity (firmware, descriptor id) or,
    if no runtime answers, whether a descriptor block can be read through the bootloader."""
    return _call(board.discover, port)


@mcp.tool()
def describe(section: str | None = None, port: str | None = None, source: str = "auto") -> dict:
    """Board self-description. No section: summary + table of contents (cheap). Section: dotted path
    such as 'peripherals.steppers', 'peripherals.thermistors', 'pins', 'pins.X_STEP', 'resources.timers',
    'limits', 'unknowns', 'hal', or 'all'. source: auto | runtime | bootloader."""
    return _call(board.describe, port, section, source)


@mcp.tool()
def get_state(port: str | None = None) -> dict:
    """Live snapshot: endstops, encoder, X driver/motor temperatures, driver enables, thermal guard,
    MOSFET levels, SD card, free RAM. Needs firmware with the aiready runtime."""
    return _call(board.runtime_cmd, port, "state")


@mcp.tool()
def watch(interval_ms: int = 200, duration_s: float = 3.0, port: str | None = None) -> list | dict:
    """Stream state snapshots for duration_s seconds (max 30) at interval_ms."""
    return _call(board.watch, port, interval_ms, min(duration_s, 30.0))


@mcp.tool()
def read_registers(port: str | None = None) -> dict:
    """Raw GPIO port registers (PIN DDR PORT, hex) for ports A-L."""
    return _call(board.runtime_cmd, port, "pins")


@mcp.tool()
def peek(addr: str, n: int = 1, port: str | None = None) -> dict:
    """Read-only data-space bytes (SRAM / I/O registers, 0..0x21FF, max 32). Side-effect registers show 'xx'."""
    return _call(board.runtime_cmd, port, f"peek {addr} {n}")


@mcp.tool()
def safe_state(port: str | None = None) -> dict:
    """Immediately force MOSFET outputs LOW and disable all stepper drivers."""
    return _call(board.runtime_cmd, port, "safe")


@mcp.tool()
def clear_guard(port: str | None = None) -> dict:
    """Clear a latched X thermal-guard trip (it re-trips if the condition persists)."""
    return _call(board.runtime_cmd, port, "clear")


@mcp.tool()
def flash(hex_path: str, backup: bool = True, port: str | None = None) -> dict:
    """Upload an application Intel HEX. Backs up current flash+EEPROM to backups/ first, strips the
    descriptor and bootloader regions, refuses images containing '!!!', verifies, then reports whether
    a runtime answers and the descriptor survived."""
    return _call(program.flash, hex_path, port, backup=backup)


@mcp.tool()
def read_back(port: str | None = None) -> dict:
    """Dump the current flash and EEPROM to backups/ (about 40 s). Returns file paths and app hash."""
    return _call(program.readback, port)


@mcp.resource("aiready://descriptor")
def descriptor_resource() -> str:
    """Full self-description: the repo copy when running from a checkout, else read from the board."""
    import json
    from . import DESCRIPTOR_JSON
    if DESCRIPTOR_JSON.exists():
        return json.dumps(board.local_descriptor())
    r = _call(board.fetch_descriptor)
    return json.dumps(r.get("descriptor", r))


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()

"""aiready CLI. Every command prints one JSON document on stdout (errors: {"error": ...}, exit 1)."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import DESCRIPTOR_JSON, __version__
from . import board, program


def _out(obj) -> None:
    json.dump(obj, sys.stdout, indent=1, ensure_ascii=False)
    sys.stdout.write("\n")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="aiready", description="AI-ready hardware host tool (JSON output).")
    ap.add_argument("--port", help="serial port (default: auto-detect CH340 board)")
    ap.add_argument("--version", action="version", version=__version__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("discover", help="find boards, report runtime identity and descriptor id")
    d = sub.add_parser("describe", help="self-description: summary, a section ('peripherals.steppers', 'pins.X_STEP') or 'all'")
    d.add_argument("section", nargs="?")
    d.add_argument("--source", choices=["auto", "runtime", "bootloader"], default="auto")
    sub.add_parser("state", help="live state snapshot (runtime)")
    w = sub.add_parser("watch", help="stream state for a while")
    w.add_argument("--ms", type=int, default=200)
    w.add_argument("--seconds", type=float, default=3.0)
    sub.add_parser("pins", help="raw port registers PIN/DDR/PORT (runtime)")
    p = sub.add_parser("peek", help="read-only data-space bytes (runtime)")
    p.add_argument("addr")
    p.add_argument("n", nargs="?", default="1")
    sub.add_parser("safe", help="force outputs safe: MOSFETs LOW, drivers disabled (runtime)")
    sub.add_parser("clear", help="clear a latched thermal-guard trip (runtime)")
    sub.add_parser("reboot", help="software reboot (runtime)")
    f = sub.add_parser("flash", help="back up, then upload an app HEX (protected regions stripped)")
    f.add_argument("hex")
    f.add_argument("--no-backup", action="store_true")
    r = sub.add_parser("readback", help="dump flash + EEPROM to backups/")
    r.add_argument("--out", type=Path)
    k = sub.add_parser("pack", help="build the descriptor block HEX from the JSON")
    k.add_argument("--json", type=Path, default=DESCRIPTOR_JSON)
    wd = sub.add_parser("write-descriptor", help="pack and write the descriptor block on its own, then read it back")
    wd.add_argument("--json", type=Path, default=DESCRIPTOR_JSON)
    sub.add_parser("check", help="runtime present? descriptor in flash matches the local JSON?")

    a = ap.parse_args(argv)
    try:
        if a.cmd == "discover":
            _out(board.discover(a.port))
        elif a.cmd == "describe":
            _out(board.describe(a.port, a.section, a.source))
        elif a.cmd in ("state", "pins", "safe", "clear", "reboot"):
            _out(board.runtime_cmd(a.port, a.cmd))
        elif a.cmd == "peek":
            _out(board.runtime_cmd(a.port, f"peek {a.addr} {a.n}"))
        elif a.cmd == "watch":
            _out(board.watch(a.port, a.ms, a.seconds))
        elif a.cmd == "flash":
            _out(program.flash(a.hex, a.port, backup=not a.no_backup))
        elif a.cmd == "readback":
            _out(program.readback(a.port, a.out) if a.out else program.readback(a.port))
        elif a.cmd == "pack":
            _out(program.pack_descriptor(a.json))
        elif a.cmd == "write-descriptor":
            _out(program.write_descriptor(a.port, a.json))
        elif a.cmd == "check":
            _out(program.check_board(board.resolve_port(a.port), restore_descriptor=False))
    except (board.BoardError, ValueError, OSError) as e:
        _out({"error": str(e), "cmd": a.cmd})
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

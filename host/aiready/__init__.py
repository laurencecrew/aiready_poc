"""Host tools for AI-ready hardware (POC: FYSETC One+ V2.2).

Works both from a checkout of https://github.com/laurencecrew/aiready_poc (editable install,
uses the repo's descriptor/ and backups/) and from a plain pip install (the board is the
source of truth; backups go to ~/.aiready/backups).
"""
import os
from pathlib import Path

__version__ = "0.1.0"
REPO_URL = "https://github.com/laurencecrew/aiready_poc"

# Checkout layout: <root>/host/aiready/__init__.py
_root = Path(__file__).resolve().parents[2]
IN_CHECKOUT = (_root / "descriptor" / "schema.json").exists()
REPO_ROOT = _root if IN_CHECKOUT else None

_home = Path.home() / ".aiready"
DESCRIPTOR_JSON = Path(os.environ.get("AIREADY_DESCRIPTOR",
                                      _root / "descriptor" / "one_plus_v2.2.json" if IN_CHECKOUT else _home / "descriptor.json"))
DESCRIPTOR_HEX = (_root / "descriptor" / "build" if IN_CHECKOUT else _home / "build") / "descriptor_block.hex"
BACKUP_DIR = Path(os.environ.get("AIREADY_BACKUPS", _root / "backups" if IN_CHECKOUT else _home / "backups"))

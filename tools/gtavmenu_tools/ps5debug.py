"""Shared loader + memory read helpers for the ps5debug-ng client.

``ps5debug_ng_client.py`` lives at the ``tools/`` top level (not inside this
installable package), so it cannot be imported the clean ``import`` way. The
read-only live tools (``read_player_ped_anchor``, ``inspect_profile_settings``,
``probe_script_globals_anchors_live``) each repeated the same
``spec_from_file_location("ps5dbg", ...)`` shim plus identical little-endian
``read_u*`` unpackers over the client's ``read(pid, addr, n)`` API. Both live
here once.
"""

from __future__ import annotations

import importlib.util
import struct
from pathlib import Path
from types import ModuleType

_CLIENT_PATH = Path(__file__).resolve().parents[2] / "research/tools/live/ps5debug_ng_client.py"
_MENU_CLIENT_PATH = Path(__file__).resolve().parents[1] / "menu_client.py"


def _load(path: Path, name: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load ps5debug client: {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_client() -> ModuleType:
    """Import the private research client used by dev-only probes."""
    return _load(_CLIENT_PATH, "ps5dbg_research")


def load_menu_client() -> ModuleType:
    """Import the production status/control client by file path."""
    return _load(_MENU_CLIENT_PATH, "gtavmenu_client")


def read_u8(client, pid: int, addr: int) -> int:
    return client.read(pid, addr, 1)[0]


def read_u16(client, pid: int, addr: int) -> int:
    return struct.unpack("<H", client.read(pid, addr, 2))[0]


def read_u32(client, pid: int, addr: int) -> int:
    return struct.unpack("<I", client.read(pid, addr, 4))[0]


def read_u64(client, pid: int, addr: int) -> int:
    return struct.unpack("<Q", client.read(pid, addr, 8))[0]


def read_i32(client, pid: int, addr: int) -> int:
    return struct.unpack("<i", client.read(pid, addr, 4))[0]

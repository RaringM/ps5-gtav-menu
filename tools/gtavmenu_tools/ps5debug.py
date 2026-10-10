"""Shared loader + memory read helpers for the read-only live tools.

``menu_client.py`` lives at the ``tools/`` top level (not inside this installable package), so it
cannot be imported the clean ``import`` way: ``load_menu_client()`` loads it by file path. The
``read_u*``/``read_i32`` helpers unpack little-endian values over any client with a
``read(pid, addr, n)`` method.
"""

from __future__ import annotations

import importlib.util
import struct
from pathlib import Path
from types import ModuleType

_MENU_CLIENT_PATH = Path(__file__).resolve().parents[1] / "menu_client.py"


def _load(path: Path, name: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load client module: {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


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

"""Read a system/graphics pointer from a decoded RSC7 resource."""

from __future__ import annotations

import struct

from .asset_formats import decode_resource

SYS, GFX = 0x50000000, 0x60000000


class Resource:
    def __init__(self, blob: bytes):
        self.header, self.payload = decode_resource(blob, 1 << 31)
        self.system = self.header["systemBytes"]
        self.version = (blob[4]) | (blob[5] << 8)
        self.gen9 = self.version >= 171

    def where(self, pointer: int) -> str:
        if SYS <= pointer < SYS + self.system:
            return "system"
        if GFX <= pointer < GFX + self.header["graphicsBytes"]:
            return "graphics"
        return "null" if pointer == 0 else "outside"

    def off(self, pointer: int) -> int:
        if SYS <= pointer < SYS + self.system:
            return pointer - SYS
        if GFX <= pointer < GFX + self.header["graphicsBytes"]:
            return self.system + pointer - GFX
        raise ValueError(f"pointer {pointer:#x} outside resource")

    def u(self, pointer: int, fmt: str, delta: int = 0):
        return struct.unpack_from(fmt, self.payload, self.off(pointer) + delta)[0]

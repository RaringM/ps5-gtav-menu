"""PS5 parser-meta resources (.pmap/.ptyp, RSC7 v2): stored-deflate wrapping and a header reader.

The payload starts with a header whose pointers are resource-relative (0x50000000 | offset):

  +0x08 ptr  page info                 +0x10 'PRD0' magic, 0x70, 0x10001
  +0x20 ptr  struct infos  (stride 0x20: u32 name, u32 key, u32 version, ptr members, u32 size,
                            u16 -, u16 member count)
  +0x28 ptr  enum infos    +0x30 ptr  data blocks (stride 0x10: u32 struct hash, u32 size, ptr)
  +0x48 u16 struct count, +0x4a u16 enum count, +0x4c u16 block count

Members are rows {u32 name hash, u32 offset, u8 type, u8 subtype, u16 aux, u32 ref}. Inside the
data blocks a pointer is a block reference (offset << 12) | block (1-based).
"""

from __future__ import annotations

import struct

RESOURCE_BASE = 0x50000000


def joaat_cs(text: str) -> int:
    """Case-kept Jenkins one-at-a-time hash (parser struct and member names)."""
    value = 0
    for byte in text.encode("ascii"):
        value = (value + byte) & 0xFFFFFFFF
        value = (value + (value << 10)) & 0xFFFFFFFF
        value ^= value >> 6
    value = (value + (value << 3)) & 0xFFFFFFFF
    value ^= value >> 11
    return (value + (value << 15)) & 0xFFFFFFFF


def stored_deflate(payload: bytes) -> bytes:
    """Raw deflate of uncompressed (stored) blocks: compressor-independent, byte-stable output."""
    out = bytearray()
    for start in range(0, len(payload), 65535):
        stop = min(start + 65535, len(payload))
        count = stop - start
        out += struct.pack("<BHH", int(stop == len(payload)), count, count ^ 0xFFFF) + payload[start:stop]
    if not payload:
        out += struct.pack("<BHH", 1, 0, 0xFFFF)
    return bytes(out)


class Meta:
    """Header, struct infos and data-block table of an inflated meta payload."""

    def __init__(self, payload: bytes):
        self.b = payload
        (self.magic,) = struct.unpack_from("<4s", payload, 0x10)
        self.struct_ptr, self.enum_ptr, self.block_ptr = (self.u64(o) for o in (0x20, 0x28, 0x30))
        self.n_struct, self.n_enum, self.n_block = struct.unpack_from("<HHH", payload, 0x48)
        self.blocks = []
        for i in range(self.n_block):
            h, size = struct.unpack_from("<II", payload, self.off(self.block_ptr) + i * 16)
            self.blocks.append((h, size, self.off(self.u64(self.off(self.block_ptr) + i * 16 + 8))))
        self.structs = {}
        for i in range(self.n_struct):
            at = self.off(self.struct_ptr) + i * 0x20
            name, _key, _ver = struct.unpack_from("<III", payload, at)
            members_at = self.off(self.u64(at + 0x10))
            size, _pad, count = struct.unpack_from("<IHH", payload, at + 0x18)
            rows = [struct.unpack_from("<IIBBHI", payload, members_at + j * 16) for j in range(count)]
            self.structs[name] = (size, rows)

    def u64(self, offset: int) -> int:
        return struct.unpack_from("<Q", self.b, offset)[0]

    def off(self, pointer: int) -> int:
        if pointer & 0xF0000000 != RESOURCE_BASE:
            raise ValueError(f"not a resource pointer {pointer:#x}")
        return pointer - RESOURCE_BASE

    def ref(self, value: int) -> int | None:
        """Block reference (offset << 12) | block (1-based) -> payload offset."""
        block, offset = value & 0xFFF, value >> 12
        if not block or block > len(self.blocks):
            return None
        return self.blocks[block - 1][2] + offset

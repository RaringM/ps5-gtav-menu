"""Exact-buffer RSC7 envelope encoding, without native object serialization.

The header and flag-size rules use the existing bounded container reader. The
DEFLATE encoder independently implements RFC 1951 sections 3.2.3 and 3.2.4 using
stored blocks only: https://www.rfc-editor.org/rfc/rfc1951.html#section-3.2.4
This makes output independent of a compressor version or optimization choices.

An encoded envelope is not an admitted native resource. Version/type admission,
native decompression, page placement, object/fixup contents and GPU semantics
remain separate requirements. Caller buffers are never padded or repaired.
"""

from __future__ import annotations

import struct

from gtavmenu_tools.asset_formats import AssetError, page_bytes

# Host memory bounds, not game capacities or reverse-engineered constants.
MAX_PAYLOAD_BYTES = 16 * 1024 * 1024  # 16 MiB: one 256 x 64 KiB system page (a80_hi, large-assets-re.md)
STORED_BLOCK_BYTES = (1 << 16) - 1  # RFC 1951 LEN is a little-endian uint16.


def stored_deflate(payload: bytes, max_bytes: int = MAX_PAYLOAD_BYTES) -> bytes:
    """Encode a bounded byte string as deterministic raw DEFLATE stored blocks."""
    if type(payload) is not bytes or len(payload) > max_bytes:
        raise AssetError("stored DEFLATE requires immutable bytes within the host payload bound")
    output = bytearray()
    for start in range(0, max(1, len(payload)), STORED_BLOCK_BYTES):
        stop = min(start + STORED_BLOCK_BYTES, len(payload))
        count = stop - start
        # Each stored block ends on a byte boundary. Unused header bits are zero.
        output.extend(struct.pack("<BHH", int(stop == len(payload)), count, count ^ 0xFFFF))
        output.extend(payload[start:stop])
    return bytes(output)


def encode(
    version: int,
    system_flags: int,
    graphics_flags: int,
    system: bytes,
    graphics: bytes,
    max_bytes: int = MAX_PAYLOAD_BYTES,
) -> bytes:
    """Serialize explicit envelope fields and exact opaque region buffers.

    All uint8 version values can be represented by this envelope layer. This is
    not a list of native resource versions accepted by the target game.
    """
    if type(version) is not int or not 0 <= version <= 0xFF:
        raise AssetError("envelope version must be uint8; native type admission is not implemented")
    if any(type(word) is not int or not 0 <= word <= 0xFFFFFFFF for word in (system_flags, graphics_flags)):
        raise AssetError("envelope page flags must be uint32")
    if version != (system_flags >> 28) << 4 | graphics_flags >> 28:
        raise AssetError("envelope version disagrees with the explicit page-flag nibbles")
    sizes = page_bytes(system_flags), page_bytes(graphics_flags)
    if not 0 < sum(sizes) <= max_bytes:
        raise AssetError("envelope declares no storage or exceeds the host payload bound")
    for name, data, size in zip(("system", "graphics"), (system, graphics), sizes, strict=True):
        if type(data) is not bytes:
            raise AssetError(f"{name} storage must be immutable bytes")
        if len(data) != size:
            raise AssetError(
                f"{name} storage has {len(data)} bytes; page flags require exactly {size}; no padding allowed"
            )
    header = struct.pack("<4sIII", b"RSC7", version, system_flags, graphics_flags)
    return header + stored_deflate(system + graphics, max_bytes)

"""Deterministic linear BC3 pixels for an offline texture visual oracle.

The output is a complete row-major BC3 base mip.  It deliberately contains no
resource metadata, swizzle selection, GPU claim, or runtime activation logic.
Callers may pass the immutable bytes to :func:`texture_layout.tile_mips` after
they have independently selected and validated a matching layout.
"""

from __future__ import annotations

import hashlib
import struct

from .asset_formats import AssetError

MAX_ORACLE_BLOCKS = 4 * 1024 * 1024
BC3_QUADRANT_ALGORITHM = "opaque-bc3-rgb565-four-quadrants-v1"
BC3_QUADRANT_ORDER = ("red", "green", "blue", "white")


def _constant_bc3_block(rgb565: int) -> bytes:
    """Encode one opaque, single-color DXT5/BC3 block."""
    # Alpha endpoint index zero (the first 255 endpoint) is selected by every
    # zero three-bit alpha index. RGB endpoint index zero is selected by every
    # zero two-bit color index. Equal endpoints make unused entries irrelevant.
    return b"\xff\xff" + b"\0" * 6 + struct.pack("<HHI", rgb565, rgb565, 0)


_RED = _constant_bc3_block(0xF800)
_GREEN = _constant_bc3_block(0x07E0)
_BLUE = _constant_bc3_block(0x001F)
_WHITE = _constant_bc3_block(0xFFFF)


def bc3_quadrant_mip(width: int, height: int) -> bytes:
    """Return an opaque red/green/blue/white BC3 base mip.

    Quadrants appear in reading order: red and green on top, blue and white on
    the bottom.  Both dimensions must be multiples of eight so each half lies
    on a four-pixel BC3 block boundary.  The result contains exactly
    ``(width / 4) * (height / 4)`` 16-byte blocks and no row padding.
    """
    if type(width) is not int or type(height) is not int:
        raise AssetError("BC3 visual oracle dimensions must be integers")
    if not 8 <= width <= 16384 or width % 8:
        raise AssetError("BC3 visual oracle width must be a multiple of 8 from 8 through 16384")
    if not 8 <= height <= 16384 or height % 8:
        raise AssetError("BC3 visual oracle height must be a multiple of 8 from 8 through 16384")

    width_blocks, height_blocks = width // 4, height // 4
    block_count = width_blocks * height_blocks
    if block_count > MAX_ORACLE_BLOCKS:
        raise AssetError("BC3 visual oracle exceeds its block work budget")

    half_width, half_height = width_blocks // 2, height_blocks // 2
    top = (_RED * half_width) + (_GREEN * half_width)
    bottom = (_BLUE * half_width) + (_WHITE * half_width)
    return bytes((top * half_height) + (bottom * half_height))


def _bc3_pixels(block: bytes) -> tuple[bytes, ...]:
    """Decode one BC3 block to sixteen row-major RGBA pixels."""
    if type(block) is not bytes or len(block) != 16:
        raise AssetError("BC3 visual oracle decoder requires one exact 16-byte block")
    alpha0, alpha1 = block[:2]
    alpha_codes = int.from_bytes(block[2:8], "little")
    if alpha0 > alpha1:
        alphas = (alpha0, alpha1, *(((7 - i) * alpha0 + i * alpha1) // 7 for i in range(1, 7)))
    else:
        alphas = (alpha0, alpha1, *(((5 - i) * alpha0 + i * alpha1) // 5 for i in range(1, 5)), 0, 255)

    endpoint0, endpoint1, color_codes = struct.unpack_from("<HHI", block, 8)

    def rgb(endpoint: int) -> tuple[int, int, int]:
        red5, green6, blue5 = endpoint >> 11, (endpoint >> 5) & 0x3F, endpoint & 0x1F
        return ((red5 << 3) | (red5 >> 2), (green6 << 2) | (green6 >> 4), (blue5 << 3) | (blue5 >> 2))

    color0, color1 = rgb(endpoint0), rgb(endpoint1)
    if endpoint0 > endpoint1:
        colors = (
            color0,
            color1,
            tuple((2 * left + right) // 3 for left, right in zip(color0, color1, strict=True)),
            tuple((left + 2 * right) // 3 for left, right in zip(color0, color1, strict=True)),
        )
    else:
        colors = (
            color0,
            color1,
            tuple((left + right) // 2 for left, right in zip(color0, color1, strict=True)),
            (0, 0, 0),
        )
    return tuple(
        bytes((*colors[(color_codes >> (2 * pixel)) & 3], alphas[(alpha_codes >> (3 * pixel)) & 7]))
        for pixel in range(16)
    )


def verify_bc3_quadrant_mip(payload: bytes, width: int, height: int) -> dict:
    """Prove encoded bytes and independently decoded RGBA pixels match the oracle."""
    expected = bc3_quadrant_mip(width, height)
    if type(payload) is not bytes or payload != expected:
        raise AssetError("BC3 visual oracle mip differs from the exact quadrant algorithm")

    width_blocks, height_blocks = width // 4, height // 4
    decoded = hashlib.sha256()
    expected_rgba = hashlib.sha256()
    colors = ((255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 255, 255))
    for block_y in range(height_blocks):
        decoded_blocks = []
        for block_x in range(width_blocks):
            offset = (block_y * width_blocks + block_x) * 16
            decoded_blocks.append(_bc3_pixels(payload[offset : offset + 16]))
        quadrant_y = int(block_y >= height_blocks // 2)
        for pixel_y in range(4):
            row = bytearray()
            expected_row = bytearray()
            for block_x, pixels in enumerate(decoded_blocks):
                row.extend(b"".join(pixels[pixel_y * 4 : pixel_y * 4 + 4]))
                quadrant_x = int(block_x >= width_blocks // 2)
                expected_row.extend(bytes(colors[quadrant_y * 2 + quadrant_x]) * 4)
            decoded.update(row)
            expected_rgba.update(expected_row)
    if decoded.digest() != expected_rgba.digest():
        raise AssetError("BC3 visual oracle decoded pixels differ from the exact RGBA quadrants")
    return {
        "algorithm": BC3_QUADRANT_ALGORITHM,
        "quadrantOrder": list(BC3_QUADRANT_ORDER),
        "encodedMipSha256": hashlib.sha256(payload).hexdigest(),
        "decodedRgbaSha256": decoded.hexdigest(),
        "decodedBytes": width * height * 4,
    }

"""Public AMD GFX10 standard-swizzle layouts for ordinary 2D BC1/BC3/BC5/BGRA8.

This is a bounded CPU implementation of the non-XOR 256B_S, 4KB_S and
64KB_S algorithms in GPUOpen-Drivers/pal's imported AddrLib, commit
c5e800072a32f68b6ccc4422936d96167c6e0728: gfx10addrlib.cpp's surface-info
and address-from-coordinate routines, gfx10SwizzlePattern.h's standard
patterns, and addrlib2.cpp's thin-block dimensions. No game executable,
writer placement report, compiler or helper executable is required.

Swizzle identifiers belong to the public AMD model. This module does not
resolve an engine's automatic mode, identify console hardware, transfer
texture metadata, or qualify a serialized resource for GPU use.
"""

# Portions adapted from AMD AddrLib:
# Copyright (c) 2007-2024 Advanced Micro Devices, Inc. All Rights Reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

from __future__ import annotations

from collections.abc import Iterator, Sequence
from dataclasses import dataclass

from .asset_formats import AssetError

MAX_STORAGE_BYTES = 64 * 1024 * 1024
MAX_BLOCKS = 4 * 1024 * 1024
_MODE_BITS = {1: 8, 5: 12, 9: 16}
_FORMAT_BYTES = {"BC1": 8, "BC3": 16, "BC5": 16, "BGRA8": 4}


@dataclass(frozen=True)
class MipLayout:
    """One mip in storage elements: BC blocks, or single BGRA8 pixels.

    The ``*_blocks`` field names are retained for API compatibility.
    """

    width_blocks: int
    height_blocks: int
    pitch_blocks: int
    padded_height_blocks: int
    offset_bytes: int
    tail_x: int = 0
    tail_y: int = 0


@dataclass(frozen=True)
class TextureLayout:
    """An explicitly selected public CPU layout, without hardware qualification."""

    width: int
    height: int
    format_name: str
    swizzle_mode: int
    block_bytes: int
    storage_bytes: int
    alignment_bytes: int
    tile_width: int
    tile_height: int
    mips: tuple[MipLayout, ...]

    def offsets(self, level: int) -> Iterator[int]:
        """Yield byte addresses in row-major storage-element order."""
        if type(level) is not int or not 0 <= level < len(self.mips):
            raise AssetError("texture mip index is outside the layout")
        mip = self.mips[level]
        # Standard swizzles have independent x/y contributions. A tile-sized
        # lookup avoids retaining a potentially million-element address map.
        x_bits, y_bits = _axis_bits(self.block_bytes, self.alignment_bytes.bit_length() - 1)
        x_offsets = tuple(_scatter(value, x_bits) for value in range(self.tile_width))
        y_offsets = tuple(_scatter(value, y_bits) for value in range(self.tile_height))
        tiles_per_row = mip.pitch_blocks // self.tile_width
        for y in range(mip.height_blocks):
            row_tile = (y // self.tile_height) * tiles_per_row
            y_offset = y_offsets[(y + mip.tail_y) % self.tile_height]
            for x in range(mip.width_blocks):
                tile_index = row_tile + x // self.tile_width
                yield (
                    mip.offset_bytes
                    + tile_index * self.alignment_bytes
                    + (x_offsets[(x + mip.tail_x) % self.tile_width] | y_offset)
                )


def _axis_bits(element_bytes: int, tile_bits: int) -> tuple[tuple[int, ...], tuple[int, ...]]:
    # The first 256 bytes use the public standard pattern. Higher tile bits
    # alternate y/x. Unlike Z order, the low bits are not ordinary Morton order.
    x_bits = {4: [2, 3, 7], 8: [3, 6, 7], 16: [6, 7]}[element_bytes]
    y_bits = [4, 5, 6] if element_bytes == 4 else [4, 5]
    for bit in range(8, tile_bits):
        (y_bits if bit % 2 == 0 else x_bits).append(bit)
    return tuple(x_bits), tuple(y_bits)


def _scatter(value: int, bits: tuple[int, ...]) -> int:
    return sum(((value >> index) & 1) << bit for index, bit in enumerate(bits))


def _align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & -alignment


def _ceil_shift(value: int, level: int) -> int:
    return (value + (1 << level) - 1) >> level


def _limit(value: int, maximum: int, label: str) -> None:
    if type(value) is not int or not 1 <= value <= maximum:
        raise AssetError(f"texture layout {label} exceeds its supported bound")


def surface_layout(
    width: int,
    height: int,
    mip_count: int,
    format_name: str,
    swizzle_mode: int,
    *,
    max_storage_bytes: int = MAX_STORAGE_BYTES,
    max_blocks: int = MAX_BLOCKS,
) -> TextureLayout:
    """Derive every mip address from dimensions and an explicit AMD swizzle.

    Supports bounded single-sample, single-slice BC1/BC3/BC5/BGRA8 2D surfaces.
    Mode 255 is deliberately rejected. Source mip dimensions use floor shifts
    in pixels; allocation geometry follows AddrLib's ceiling shifts in storage
    elements. ``max_blocks`` bounds BC blocks or BGRA8 pixels, not source bytes.
    BGRA8 channel bytes are opaque and never reordered.
    """
    _limit(width, 16384, "width")
    _limit(height, 16384, "height")
    _limit(mip_count, max(width, height).bit_length(), "mip count")
    _limit(max_storage_bytes, MAX_STORAGE_BYTES, "storage budget")
    _limit(max_blocks, MAX_BLOCKS, "block budget")
    if type(format_name) is not str or format_name not in _FORMAT_BYTES:
        raise AssetError("texture layout requires BC1, BC3, BC5 or BGRA8 without format substitution")
    if type(swizzle_mode) is not int or swizzle_mode not in _MODE_BITS:
        raise AssetError("texture layout requires an explicit public swizzle mode 1, 5 or 9")
    element_bytes, tile_bits = _FORMAT_BYTES[format_name], _MODE_BITS[swizzle_mode]
    tile_bytes = 1 << tile_bits
    element_bits = element_bytes.bit_length() - 1
    tile_width = 1 << ((tile_bits - element_bits + 1) // 2)
    tile_height = tile_bytes // (tile_width * element_bytes)
    edge = 1 if format_name == "BGRA8" else 4
    base_width, base_height = (width + edge - 1) // edge, (height + edge - 1) // edge
    sizes = [
        ((max(1, width >> i) + edge - 1) // edge, (max(1, height >> i) + edge - 1) // edge) for i in range(mip_count)
    ]
    if sum(x * y for x, y in sizes) > max_blocks:
        raise AssetError("texture layout exceeds its storage-element work budget")
    rows: list[MipLayout | None] = [None] * mip_count
    tail_start = mip_count
    tail_width, tail_height = tile_width // 2, tile_height
    max_tail_mips = tile_bits - 4
    if swizzle_mode != 1 and mip_count > 1:
        for level in range(mip_count):
            if (
                _ceil_shift(base_width, level) <= tail_width
                and _ceil_shift(base_height, level) <= tail_height
                and mip_count - level <= max_tail_mips
            ):
                tail_start = level
                break
    storage = tile_bytes if tail_start < mip_count else 0
    for level in range(tail_start - 1, -1, -1):
        pitch = _align(_ceil_shift(base_width, level), tile_width)
        padded_height = _align(_ceil_shift(base_height, level), tile_height)
        rows[level] = MipLayout(*sizes[level], pitch, padded_height, storage)
        storage += pitch * padded_height * element_bytes
    # Mip tails reserve one tile at the beginning. Their fixed sub-tile origins
    # come from the public AMD tail offset encoding, not from source dimensions.
    micro_width, micro_height = {4: (8, 8), 8: (8, 4), 16: (4, 4)}[element_bytes]
    for level in range(tail_start, mip_count):
        tail_index = max_tail_mips - 1 - (level - tail_start)
        tail_offset = (16 << tail_index) if tail_index > 6 else (tail_index << 8)
        tail_x = sum(((tail_offset >> (9 + 2 * bit)) & 1) << bit for bit in range(6))
        tail_y = sum(((tail_offset >> (8 + 2 * bit)) & 1) << bit for bit in range(6))
        rows[level] = MipLayout(*sizes[level], tail_width, tail_height, 0, tail_x * micro_width, tail_y * micro_height)
        tail_width = max(tail_width >> 1, micro_width)
        tail_height = max(tail_height >> 1, micro_height)
    if storage > max_storage_bytes:
        raise AssetError("texture layout exceeds its storage byte budget")
    assert all(row is not None for row in rows)
    return TextureLayout(
        width,
        height,
        format_name,
        swizzle_mode,
        element_bytes,
        storage,
        tile_bytes,
        tile_width,
        tile_height,
        tuple(row for row in rows if row is not None),
    )


def tile_mips(mips: Sequence[bytes], layout: TextureLayout) -> bytes:
    """Place every complete mip into one allocation; padding is zero filled."""
    if type(mips) not in (list, tuple) or len(mips) != len(layout.mips):
        raise AssetError("texture tiling requires every mip exactly once")
    for payload, mip in zip(mips, layout.mips, strict=True):
        if type(payload) is not bytes or len(payload) != mip.width_blocks * mip.height_blocks * layout.block_bytes:
            raise AssetError("texture mip payload differs from the complete storage-element extent")
    allocation = bytearray(layout.storage_bytes)
    for level, payload in enumerate(mips):
        for index, offset in enumerate(layout.offsets(level)):
            start = index * layout.block_bytes
            allocation[offset : offset + layout.block_bytes] = payload[start : start + layout.block_bytes]
    return bytes(allocation)


def untile_mips(allocation: bytes, layout: TextureLayout) -> tuple[bytes, ...]:
    """Recover every mip's complete storage-element payload using the public layout."""
    if type(allocation) is not bytes or len(allocation) != layout.storage_bytes:
        raise AssetError("texture allocation differs from the layout storage extent")
    mips = []
    for level, mip in enumerate(layout.mips):
        linear = bytearray(mip.width_blocks * mip.height_blocks * layout.block_bytes)
        for index, offset in enumerate(layout.offsets(level)):
            start = index * layout.block_bytes
            linear[start : start + layout.block_bytes] = allocation[offset : offset + layout.block_bytes]
        mips.append(bytes(linear))
    return tuple(mips)

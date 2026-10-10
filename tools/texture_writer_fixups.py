"""Exact-buffer pointer serialization component; no native object defaults.

Pages and pointer fields must come from a separately verified contract. This
module supplies only the inverse affine address mapping and checked byte writes.
It cannot admit a texture, infer fields, place objects, or fill padding.
"""

from __future__ import annotations

from dataclasses import dataclass
from itertools import pairwise

from gtavmenu_tools.asset_formats import AssetError

MAX_BUFFER_BYTES = 16 * 1024 * 1024  # Host work bound, not a game capacity.
MAX_FIXUPS = 65536


@dataclass(frozen=True)
class Page:
    serialized: int
    destination: int
    size: int


@dataclass(frozen=True)
class Fixup:
    offset: int
    destination: int
    extent: int
    alignment: int
    expected: bytes


def _integer(value: int, label: str, minimum: int, maximum: int) -> None:
    if type(value) is not int or not minimum <= value <= maximum:
        raise AssetError(f"{label} is outside its integer bounds")


def validate_pages(pages: tuple[Page, ...], capacity: int, length_mask: int) -> None:
    """Reject ambiguity in either direction before an inverse can be used."""
    _integer(capacity, "page capacity", 1, 255)
    _integer(length_mask, "page length mask", 1, (1 << 64) - 1)
    if type(pages) is not tuple or not 1 <= len(pages) <= capacity:
        raise AssetError("pages must be an immutable nonempty tuple within the extracted capacity")
    for page in pages:
        if type(page) is not Page:
            raise AssetError("page must be an explicit Page record")
        _integer(page.serialized, "serialized page address", 1, (1 << 64) - 1)
        _integer(page.destination, "destination page address", 1, (1 << 64) - 1)
        _integer(page.size, "page size", 1, length_mask)
        # Native endpoint arithmetic must not wrap, including exactly 2**64.
        if max(page.serialized, page.destination) + page.size >= 1 << 64:
            raise AssetError("page endpoint wraps uint64")
    for field in ("serialized", "destination"):
        ordered = sorted((getattr(page, field), page.size) for page in pages)
        if any(start + size > following for (start, size), (following, _) in pairwise(ordered)):
            raise AssetError(f"{field} page ranges overlap; pointer inverse is ambiguous")
    if list(pages) != sorted(pages, key=lambda page: page.serialized):
        raise AssetError("serialized pages must be in the reader's ascending order")


def _inverse(pages: tuple[Page, ...], destination: int, extent: int, alignment: int) -> int:
    _integer(destination, "pointer destination", 0, (1 << 64) - 1)
    _integer(extent, "referent extent", 0, MAX_BUFFER_BYTES)
    _integer(alignment, "referent alignment", 1, MAX_BUFFER_BYTES)
    if alignment & (alignment - 1):
        raise AssetError("referent alignment must be a power of two")
    if destination == 0:
        if extent != 0 or alignment != 1:
            raise AssetError("a null pointer requires an explicit empty extent and unit alignment")
        return 0
    if not extent or destination % alignment:
        raise AssetError("nonnull referent must be nonempty and aligned")
    matches = [page for page in pages if page.destination <= destination < page.destination + page.size]
    if len(matches) != 1:
        raise AssetError("pointer has no unique destination page")
    page = matches[0]
    if destination + extent > page.destination + page.size:
        raise AssetError("referent crosses its explicitly assigned page")
    serialized = page.serialized + destination - page.destination
    if serialized % alignment:
        raise AssetError("serialized referent does not preserve required alignment")
    return serialized


def serialize(
    source: bytes,
    fields: tuple[int, ...],
    fixups: tuple[Fixup, ...],
    pages: tuple[Page, ...],
    *,
    capacity: int,
    length_mask: int,
    max_bytes: int = MAX_BUFFER_BYTES,
) -> bytes:
    """Write every explicitly admitted 64-bit field exactly once.

    Each expected preimage is mandatory, including for null fixups. Unselected bytes
    are retained exactly. Referents must fit one assigned page: scatter/gather
    objects and references to external code, services or another pack are unsupported.
    The fixed uint64 little-endian field representation is verified by the runner
    against all freshly extracted pointer load instructions before this API is used.
    """
    if type(source) is not bytes or not 0 < len(source) <= max_bytes:
        raise AssetError("source must be immutable exact bytes within the host bound")
    if type(fields) is not tuple or not 1 <= len(fields) <= MAX_FIXUPS:
        raise AssetError("field policy must be an explicit bounded tuple")
    for offset in fields:
        _integer(offset, "pointer field offset", 0, len(source) - 8)
    ordered = sorted(fields)
    if any(a + 8 > b for a, b in pairwise(ordered)):
        raise AssetError("pointer fields overlap or repeat")
    if type(fixups) is not tuple or len(fixups) != len(fields) or any(type(f) is not Fixup for f in fixups):
        raise AssetError("fixups must cover the explicit field policy exactly")
    for fixup in fixups:
        _integer(fixup.offset, "fixup offset", 0, len(source) - 8)
    if sorted(f.offset for f in fixups) != ordered:
        raise AssetError("fixups omit, repeat, or introduce a pointer field")
    validate_pages(pages, capacity, length_mask)
    output = bytearray(source)
    for fixup in fixups:
        if type(fixup.expected) is not bytes or len(fixup.expected) != 8:
            raise AssetError("fixup requires an exact immutable eight-byte preimage")
        if source[fixup.offset : fixup.offset + 8] != fixup.expected:
            raise AssetError("fixup preimage differs from the supplied source")
        value = _inverse(pages, fixup.destination, fixup.extent, fixup.alignment)
        output[fixup.offset : fixup.offset + 8] = value.to_bytes(8, "little")
    return bytes(output)

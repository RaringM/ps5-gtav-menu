"""Value-parsing helpers shared across the GTAV-Menu tools."""

from __future__ import annotations


def parse_int(value: str | int | None, default: int = 0) -> int:
    """Parse ``value`` as an integer, honoring ``0x``/``0o``/``0b`` prefixes.

    ``None`` yields ``default``; an ``int`` is returned unchanged; a string is
    parsed with base 0 (auto-detecting the radix from its prefix).
    """
    if value is None:
        return default
    if isinstance(value, int):
        return value
    return int(str(value), 0)

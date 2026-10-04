"""Stable names shared by asset intake and catalog generators."""

from __future__ import annotations


def joaat(text: str) -> int:
    """GTA's lowercased Jenkins one-at-a-time name hash."""
    h = 0
    for ch in text.lower():
        h = (h + ord(ch)) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h = (h ^ (h >> 6)) & 0xFFFFFFFF
    h = (h + (h << 3)) & 0xFFFFFFFF
    h = (h ^ (h >> 11)) & 0xFFFFFFFF
    h = (h + (h << 15)) & 0xFFFFFFFF
    return h

"""PS5 RPF7 table-of-contents (TOC) key selection, mirrored by src/common/aes256.c.

The engine decrypts an archive's TOC (entries + name pool, after the 16-byte header) with
AES-256-ECB, key ``key_table[toc_index(name, size)]``. This module holds only the selection rule
and header checks; it carries no key material and no cipher. A plaintext-TOC archive (tag
``OPEN``) is the host interchange form that the worker encrypts in place before the engine sees it.
"""

from __future__ import annotations

import struct

MAGIC = 0x52504637
TAG_KEYED = 0x0FFEFFFF
TAG_OPEN = 0x4E45504F
KEY_COUNT = 101
KEY_BYTES = 32
_MEMORY_PREFIX = "memory:"


class RpfTocError(ValueError):
    """The archive header does not describe a TOC the worker would convert."""


def toc_basename(name: str) -> str:
    """The text the engine hashes: after ``memory:...:`` or after the last ``/`` or ``\\``."""
    if name.startswith(_MEMORY_PREFIX):
        colon = name.find(":", len(_MEMORY_PREFIX))
        if colon < 0:
            raise RpfTocError(f"{name!r}: memory name without its second ':'")
        return name[colon + 1 :]
    return name[max(name.rfind("/"), name.rfind("\\")) + 1 :]


def _fold(byte: int) -> int:
    if 0x41 <= byte <= 0x5A:
        return byte + 0x20
    return 0x2F if byte == 0x5C else byte


def toc_name_hash(name: str) -> int:
    """Engine TOC name hash: folded Jenkins one-at-a-time over ``toc_basename(name)``."""
    data = toc_basename(name).encode("latin-1")
    if data.startswith(b'"'):
        data = data[1:].split(b'"', 1)[0]
    value = 0
    for byte in data:
        value = (value + _fold(byte)) & 0xFFFFFFFF
        value = (value + (value << 10)) & 0xFFFFFFFF
        value ^= value >> 6
    value = (value + (value << 3)) & 0xFFFFFFFF
    value ^= value >> 11
    return (value + (value << 15)) & 0xFFFFFFFF


def toc_index(name: str, size: int) -> int:
    """Key-table index for an archive opened as ``name`` with file size ``size``."""
    return ((toc_name_hash(name) + size) & 0xFFFFFFFF) % KEY_COUNT


def toc_span(archive: bytes) -> tuple[int, int, int]:
    """(tag, start, end) of the TOC bytes the worker converts, after the checks it applies.

    Refuses anything but a single-pass, zero-flag table of at least a root and one member whose
    entries + name pool are a whole number of AES blocks inside the archive.
    """
    if len(archive) < 16:
        raise RpfTocError("truncated RPF header")
    magic, count, names_word, tag = struct.unpack_from("<4I", archive, 0)
    if magic != MAGIC:
        raise RpfTocError("not an RPF7 archive")
    if tag not in (TAG_KEYED, TAG_OPEN):
        raise RpfTocError(f"unsupported TOC tag 0x{tag:08x}")
    if count < 2 or names_word >> 28:
        raise RpfTocError("TOC needs a root plus members and zero name-pool flags")
    length = count * 16 + names_word
    if length % 16 or 16 + length > len(archive):
        raise RpfTocError("TOC is not whole blocks inside the archive")
    return tag, 16, 16 + length

"""Offline etaHEN plugin-container encoding and structural inspection.

The container is a 29-byte header followed by an unmodified ELF. Inspection
checks the container and ELF structure, not runtime compatibility or behavior.
"""

from __future__ import annotations

import hashlib
import re
import struct

MAGIC = b"etaHEN_PLUGIN\0"
HEADER_SIZE = 29
PLUGIN_ID_RE = re.compile(r"[A-Z]{4}[0-9]{5}")
VERSION_RE = re.compile(r"[0-9]\.[0-9]{2}")
ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")


def validate_metadata(plugin_id: str, version: str) -> None:
    if not PLUGIN_ID_RE.fullmatch(plugin_id):
        raise ValueError("plugin id must contain four uppercase ASCII letters followed by five digits")
    if not VERSION_RE.fullmatch(version):
        raise ValueError("version must use the x.xx format with ASCII digits")


def _check_range(data: bytes, offset: int, size: int, label: str) -> None:
    if offset > len(data) or size > len(data) - offset:
        raise ValueError(f"{label} is outside the ELF")


def validate_elf(data: bytes) -> None:
    """Check ELF64 identity and bounded header/segment tables without loading it."""
    if len(data) < ELF_HEADER.size:
        raise ValueError("ELF header is truncated")
    ident, kind, machine, version, _, phoff, shoff, _, ehsize, phsize, phnum, shsize, shnum, shstr = (
        ELF_HEADER.unpack_from(data)
    )
    if ident[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("expected a little-endian ELF64, identification version 1")
    if machine != 62 or kind not in (2, 3):
        raise ValueError("expected an x86-64 executable or shared-object ELF")
    if version != 1 or ehsize != ELF_HEADER.size:
        raise ValueError("invalid ELF version or header size")
    if not phnum or phnum == 0xFFFF or phsize != PROGRAM_HEADER.size or phoff < ehsize:
        raise ValueError("ELF has no supported program-header table")
    _check_range(data, phoff, phsize * phnum, "program-header table")
    has_load = False
    for index in range(phnum):
        segment_kind, _, offset, _, _, filesz, memsz, _ = PROGRAM_HEADER.unpack_from(data, phoff + index * phsize)
        if filesz:
            _check_range(data, offset, filesz, f"segment {index}")
        if segment_kind == 1:
            has_load = True
            if memsz < filesz:
                raise ValueError(f"load segment {index} has file size greater than memory size")
    if not has_load:
        raise ValueError("ELF has no load segment")
    if shoff:
        if shoff < ehsize or not shnum or shsize != 64 or shstr >= shnum:
            raise ValueError("ELF has no supported section-header table")
        _check_range(data, shoff, shsize * shnum, "section-header table")
    elif shnum or shstr:
        raise ValueError("ELF section-header table is missing")


def encode_plugin(elf: bytes, *, plugin_id: str, version: str) -> bytes:
    """Wrap an ELF deterministically, preserving every byte of its contents."""
    validate_metadata(plugin_id, version)
    validate_elf(elf)
    return MAGIC + plugin_id.encode("ascii") + b"\0" + version.encode("ascii") + b"\0" + elf


def inspect_container(
    data: bytes, *, expect_id: str | None = None, expect_version: str | None = None
) -> dict[str, int | str]:
    if len(data) < HEADER_SIZE:
        raise ValueError("etaHEN plugin header is truncated")
    if data[:14] != MAGIC:
        raise ValueError("invalid etaHEN plugin magic or terminator")
    if data[23] != 0 or data[28] != 0:
        raise ValueError("plugin id and version must be NUL-terminated at their fixed offsets")
    try:
        plugin_id = data[14:23].decode("ascii")
        version = data[24:28].decode("ascii")
    except UnicodeDecodeError as exc:
        raise ValueError("plugin metadata must be ASCII") from exc
    validate_metadata(plugin_id, version)
    if expect_id is not None and plugin_id != expect_id:
        raise ValueError(f"expected plugin id {expect_id}, got {plugin_id}")
    if expect_version is not None and version != expect_version:
        raise ValueError(f"expected version {expect_version}, got {version}")
    elf = data[HEADER_SIZE:]
    validate_elf(elf)
    return {
        "id": plugin_id,
        "version": version,
        "headerSize": HEADER_SIZE,
        "elfSize": len(elf),
        "pluginSize": len(data),
        "elfSha256": hashlib.sha256(elf).hexdigest(),
        "pluginSha256": hashlib.sha256(data).hexdigest(),
    }

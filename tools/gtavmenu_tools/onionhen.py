"""Structural inspection of an OnionHEN ABI-v1 ELF plugin descriptor."""

from __future__ import annotations

import re
import struct

DESCRIPTOR_SIZE = 128
KNOWN_CAPABILITIES = (1 << 6) - 1
KNOWN_FLAGS = (1 << 1) | (1 << 2)
PLUGIN_ID_RE = re.compile(r"[A-Z]{4}[0-9]{5}")
VERSION_RE = re.compile(r"[0-9]+\.[0-9]{2}")


def _checked_slice(data: bytes, offset: int, size: int, label: str) -> bytes:
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError(f"{label} is outside the ELF")
    return data[offset : offset + size]


def _fixed_string(raw: bytes, label: str) -> str:
    end = raw.find(b"\0")
    if end < 0:
        raise ValueError(f"{label} is not NUL-terminated")
    return raw[:end].decode("ascii")


def inspect_plugin_bytes(data: bytes) -> dict[str, int | str]:
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4:7] != b"\x02\x01\x01":
        raise ValueError("expected a little-endian ELF64")
    section_offset = struct.unpack_from("<Q", data, 0x28)[0]
    section_size, section_count, strings_index = struct.unpack_from("<HHH", data, 0x3A)
    if section_size < 64 or not section_count or strings_index >= section_count:
        raise ValueError("ELF has no valid section table")
    _checked_slice(data, section_offset, section_size * section_count, "section table")

    strings_header = section_offset + strings_index * section_size
    strings_fields = struct.unpack_from("<IIQQQQIIQQ", data, strings_header)
    strings = _checked_slice(data, strings_fields[4], strings_fields[5], "section string table")
    descriptor = None
    for index in range(section_count):
        fields = struct.unpack_from("<IIQQQQIIQQ", data, section_offset + index * section_size)
        name_offset = fields[0]
        if name_offset >= len(strings):
            raise ValueError("section name is outside the string table")
        name_end = strings.find(b"\0", name_offset)
        if name_end < 0:
            raise ValueError("section name is not NUL-terminated")
        if strings[name_offset:name_end] == b".onion_plugin":
            descriptor = _checked_slice(data, fields[4], fields[5], ".onion_plugin")
            break
    if descriptor is None:
        raise ValueError("ELF has no .onion_plugin section")
    if len(descriptor) < DESCRIPTOR_SIZE:
        raise ValueError(".onion_plugin descriptor is truncated")

    struct_size, abi, capabilities, flags = struct.unpack_from("<IIII", descriptor)
    if struct_size != DESCRIPTOR_SIZE:
        raise ValueError(f"unexpected descriptor size {struct_size}")
    if abi != 1:
        raise ValueError(f"unsupported plugin ABI {abi}")
    if capabilities & ~KNOWN_CAPABILITIES:
        raise ValueError("descriptor contains unknown capabilities")
    if flags & ~KNOWN_FLAGS:
        raise ValueError("descriptor contains unknown flags")
    plugin_id = _fixed_string(descriptor[16:48], "plugin_id")
    version = _fixed_string(descriptor[48:64], "version")
    name = _fixed_string(descriptor[64:128], "name")
    if not PLUGIN_ID_RE.fullmatch(plugin_id):
        raise ValueError("plugin_id must contain four letters followed by five digits")
    if not VERSION_RE.fullmatch(version):
        raise ValueError("version must use the x.xx format")
    if not name:
        raise ValueError("plugin name must not be empty")
    return {
        "id": plugin_id,
        "version": version,
        "name": name,
        "abi": abi,
        "capabilities": capabilities,
        "flags": flags,
        "elfSize": len(data),
    }

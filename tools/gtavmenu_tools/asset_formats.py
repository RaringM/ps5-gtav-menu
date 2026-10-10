"""Bounded, read-only PC RPF7/RSC7 container inspection.

Format reference (layout facts, not a vendored implementation):
https://github.com/dexyfex/CodeWalker/blob/master/CodeWalker.Core/GameFiles/RpfFile.cs

These are PC container rules. In particular, NONE/OPEN table handling must never
be used as a fallback for an unknown console encoding. A decoded resource
envelope does not establish platform, object layout, or GPU compatibility.
"""

from __future__ import annotations

import re
import struct
import zlib
from collections.abc import Callable
from dataclasses import dataclass
from itertools import pairwise
from pathlib import PurePosixPath


class AssetError(ValueError):
    """Invalid, unsupported, or over-budget input; callers must fail closed."""


@dataclass(frozen=True)
class Limits:
    """Host inspection limits, not inferred game or GPU capacities."""

    max_file_bytes: int = 256 * 1024 * 1024
    max_total_bytes: int = 1024 * 1024 * 1024
    max_entries: int = 10000
    max_depth: int = 8
    max_metadata_bytes: int = 4 * 1024 * 1024
    max_xml_nodes: int = 100000
    max_xml_depth: int = 64


def safe_name(name: str) -> str:
    """Use a conservative, portable relative namespace without normalization."""
    if len(name) > 512 or not re.fullmatch(r"[A-Za-z0-9_ .()/+@-]+", name):
        raise AssetError(f"unsafe asset path: {name!r}")
    if any(part in ("", ".", "..") or part.endswith((" ", ".")) for part in name.split("/")):
        raise AssetError(f"unsafe asset path: {name!r}")
    if PurePosixPath(name).is_absolute():
        raise AssetError(f"absolute asset path: {name!r}")
    return name


def inflate_raw(data: bytes, expected: int, limit: int, *, allow_adler32: bool = False) -> bytes:
    if not 0 <= expected <= limit:
        raise AssetError("declared decompressed size exceeds inspection limit")
    try:
        decoder = zlib.decompressobj(-zlib.MAX_WBITS)
        result = decoder.decompress(data, expected + 1)
    except zlib.error as exc:
        raise AssetError(f"invalid raw DEFLATE stream: {exc}") from exc
    if len(result) != expected or not decoder.eof or decoder.unconsumed_tail:
        raise AssetError("DEFLATE length mismatch, truncated stream, or trailing data")
    if decoder.unused_data:
        # Some Legacy authoring tools omit the zlib header but retain its
        # big-endian Adler-32 trailer. Accept only an exact, verified checksum,
        # only for resources; never silently discard arbitrary trailing bytes.
        checksum = struct.pack(">I", zlib.adler32(result))
        if not allow_adler32 or decoder.unused_data != checksum:
            raise AssetError("unexpected DEFLATE trailer or Adler-32 checksum mismatch")
    return result


def page_bytes(flags: int) -> int:
    """PC RSC7 page flags; the high nibble carries a resource-version part."""
    fields = ((27, 1), (26, 1), (25, 1), (24, 1), (17, 7), (11, 6), (7, 4), (5, 2), (4, 1))
    units = sum(((flags >> shift) & ((1 << width) - 1)) << weight for weight, (shift, width) in enumerate(fields))
    return units * (512 << (flags & 15))


def resource_header(data: bytes) -> dict:
    if len(data) < 16 or data[:4] != b"RSC7":
        raise AssetError("expected a complete RSC7 resource header")
    _, version, system, graphics = struct.unpack_from("<4I", data)
    if version != ((system >> 28) << 4 | graphics >> 28):
        raise AssetError("RSC7 header version disagrees with page flags")
    return {
        "version": version,
        "systemFlags": f"0x{system:08x}",
        "graphicsFlags": f"0x{graphics:08x}",
        "systemBytes": page_bytes(system),
        "graphicsBytes": page_bytes(graphics),
    }


def decode_resource(data: bytes, limit: int) -> tuple[dict, bytes]:
    header = resource_header(data)
    size = header["systemBytes"] + header["graphicsBytes"]
    if not size:
        raise AssetError("resource declares no pages")
    return header, inflate_raw(data[16:], size, limit, allow_adler32=True)


def gxt2_entries(data: bytes, limits: Limits) -> list[tuple[int, str]]:
    """Read a bounded GXT2 label table; this does not register game text.

    Layout reference:
    https://github.com/dexyfex/CodeWalker/blob/master/CodeWalker.Core/GameFiles/FileTypes/Gxt2File.cs
    """
    if len(data) > limits.max_metadata_bytes:
        raise AssetError("GXT2 exceeds metadata limit")
    if len(data) < 16:
        raise AssetError("truncated GXT2 header")
    magic, count = struct.unpack_from("<2I", data)
    if magic != 1196971058:
        raise AssetError("invalid GXT2 magic")
    start = 16 + count * 8
    if count > limits.max_entries or start > len(data):
        raise AssetError("GXT2 table exceeds input or entry limit")
    marker, end = struct.unpack_from("<2I", data, start - 8)
    if marker != magic or end != len(data):
        raise AssetError("invalid GXT2 string-pool bounds")
    # Scan once, not once per hash: a hostile offset table must not turn a
    # bounded pool into quadratic work. Shared complete strings are permitted.
    strings: dict[int, str] = {}
    cursor = start
    while cursor < end:
        stop = data.find(b"\0", cursor)
        if stop < 0:
            raise AssetError("unterminated GXT2 string")
        try:
            strings[cursor] = data[cursor:stop].decode("utf-8")
        except UnicodeError as exc:
            raise AssetError("GXT2 string is not valid UTF-8") from exc
        if len(strings) > count:
            raise AssetError("GXT2 contains unreferenced strings")
        cursor = stop + 1
    entries = []
    hashes: set[int] = set()
    used: set[int] = set()
    for index in range(count):
        name_hash, offset = struct.unpack_from("<2I", data, 8 + index * 8)
        if name_hash in hashes:
            raise AssetError("duplicate GXT2 label hash")
        if offset not in strings:
            raise AssetError("GXT2 offset is not a string boundary")
        hashes.add(name_hash)
        used.add(offset)
        entries.append((name_hash, strings[offset]))
    if used != strings.keys():
        raise AssetError("GXT2 contains unreferenced strings")
    return entries


def rpf_header(data: bytes, file_size: int) -> dict:
    if len(data) < 16:
        raise AssetError("truncated RPF header")
    magic, count, names_word, encoding = struct.unpack_from("<4I", data)
    if magic != 0x52504637:
        raise AssetError("not an RPF7 container")
    # Report high bits separately. Only the zero-flags PC variant is decoded.
    names_size = names_word & 0x0FFFFFFF
    end = 16 + count * 16 + names_size
    if not count or not names_size or end > file_size:
        raise AssetError("RPF table extends beyond the container or is empty")
    return {
        "entryCount": count,
        "namesBytes": names_size,
        "namesFlags": names_word >> 28,
        "tableEncoding": f"0x{encoding:08x}",
        "tableEnd": end,
        "tableReadable": encoding in (0, 0x4E45504F) and names_word >> 28 == 0,
    }


@dataclass(frozen=True)
class RpfMember:
    name: str
    offset: int
    size: int
    unpacked_size: int
    resource: bool
    flags: tuple[int, int]
    encrypted: bool
    compressed: bool


def rpf_members(
    data: bytes,
    limits: Limits,
    *,
    file_size: int | None = None,
    read_at: Callable[[int, int], bytes] | None = None,
    name_check: Callable[[str], str] = safe_name,
) -> list[RpfMember]:
    """Parse a complete table; optionally use bounded reads for a large archive.

    The header must still use the supported PC table encodings. Callers with a
    separately decoded research table must identify that provenance themselves.
    `name_check` is the per-name policy (default `safe_name`); a caller that only
    uses names as labels may pass a looser one, never one that admits '/'.
    """
    total_size = len(data) if file_size is None else file_size
    header = rpf_header(data, total_size)
    if not header["tableReadable"]:
        raise AssetError(f"unsupported RPF table encoding {header['tableEncoding']}; supply decoded files")
    count = header["entryCount"]
    if count > limits.max_entries:
        raise AssetError("RPF entry count exceeds inspection limit")
    if header["tableEnd"] > len(data):
        raise AssetError("incomplete RPF table")
    names = data[16 + count * 16 : header["tableEnd"]]
    entries = [struct.unpack_from("<4I", data, 16 + index * 16) for index in range(count)]

    def name_at(offset: int, root: bool = False) -> str:
        end = names.find(b"\0", offset, min(len(names), offset + 257))
        if offset >= len(names) or end < 0:
            raise AssetError("invalid RPF name offset or unterminated name")
        try:
            name = names[offset:end].decode("ascii")
        except UnicodeError as exc:
            raise AssetError("non-ASCII RPF name") from exc
        if root and not name:
            return ""
        name_check(name)
        if "/" in name:
            raise AssetError("RPF entry name contains a directory separator")
        return name

    if entries[0][1] != 0x7FFFFF00:
        raise AssetError("RPF root is not a directory")
    name_at(entries[0][0], root=True)
    pending = [(0, "", 0)]
    visited: set[int] = set()
    paths: set[str] = set()
    members = []
    spans = []
    while pending:
        index, parent, depth = pending.pop()
        if index in visited:
            raise AssetError("RPF directory cycle or multiply owned entry")
        visited.add(index)
        if depth > limits.max_depth:
            raise AssetError("RPF directory depth exceeds inspection limit")
        first, second, third, fourth = entries[index]
        directory = second == 0x7FFFFF00
        name = "" if index == 0 else name_at(first if directory else first & 0xFFFF)
        path = f"{parent}/{name}" if parent else name
        if path:
            if path.lower() in paths:
                raise AssetError(f"duplicate RPF path: {path}")
            paths.add(path.lower())
        if directory:
            if third > count or fourth > count - third:
                raise AssetError("RPF child range extends beyond entry table")
            pending.extend((child, path, depth + 1) for child in reversed(range(third, third + fourth)))
            continue
        packed = first | second << 32
        resource = bool(second & 0x80000000)
        offset = ((packed >> 40) & (0x7FFFFF if resource else 0xFFFFFF)) * 512
        size = (packed >> 16) & 0xFFFFFF
        compressed = bool(size)
        unpacked = page_bytes(third) + page_bytes(fourth) if resource else third
        if resource and size == 0xFFFFFF:
            # Extended RSC sizes overwrite parts of the on-disk header. Decode
            # the length only; later rebuild the canonical envelope from TOC flags.
            if offset > total_size - 16:
                raise AssetError("extended RPF header extends beyond container")
            prefix = read_at(offset, 16) if read_at else data[offset : offset + 16]
            if len(prefix) != 16:
                raise AssetError("truncated extended RPF resource header")
            size = int.from_bytes(bytes(prefix[i] for i in (7, 14, 5, 2)), "little")
        if not size:
            if resource:
                raise AssetError("uncompressed RPF resource variant is not supported")
            size = unpacked
        if offset < header["tableEnd"] or offset > total_size or size > total_size - offset:
            raise AssetError("RPF member overlaps table or extends beyond container")
        if size > limits.max_file_bytes or unpacked > limits.max_file_bytes:
            raise AssetError("RPF member exceeds inspection limit")
        if not resource and fourth not in (0, 1):
            raise AssetError("unknown RPF member encryption tag")
        encrypted = path.lower().endswith(".ysc") if resource else bool(fourth)
        members.append(RpfMember(path, offset, size, unpacked, resource, (third, fourth), encrypted, compressed))
        if size:
            spans.append((offset, offset + size))
    if len(visited) != count:
        raise AssetError("RPF contains unreachable entries")
    spans.sort()
    if any(left[1] > right[0] for left, right in pairwise(spans)):
        raise AssetError("overlapping RPF members")
    return sorted(members, key=lambda item: item.name.lower())


def read_rpf_member(data: bytes, member: RpfMember, limit: int) -> bytes:
    if member.encrypted:
        raise AssetError("encrypted RPF member; supply decoded files")
    blob = data[member.offset : member.offset + member.size]
    if member.resource:
        if len(blob) < 16:
            raise AssetError("truncated RPF resource")
        system, graphics = member.flags
        version = (system >> 28) << 4 | graphics >> 28
        return struct.pack("<4I", 0x37435352, version, system, graphics) + blob[16:]
    # RPF's zero compressed-size field distinguishes stored members even if a
    # compressed stream happens to have exactly the uncompressed length.
    if member.compressed:
        return inflate_raw(blob, member.unpacked_size, limit)
    return blob

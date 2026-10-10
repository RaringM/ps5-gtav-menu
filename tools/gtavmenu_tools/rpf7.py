"""Key-free PS5 RPF7 archive layout: table parse, archive build and the resource-member helpers.

An archive is a 16-byte header (magic, entry count, name-pool size, tag), the table (16 bytes per
entry, root directory first) and the name pool, then members on 512-byte blocks. The table of a
retail archive is AES-256-ECB encrypted (tag ``TAG_KEYED``); the host interchange form carries tag
``TAG_OPEN`` and a plaintext table that the worker keys in place (``rpf_toc``). This module holds no
key material and no cipher: a keyed table is read or written only through a caller-supplied
callback, so the plain-table path needs nothing from the game executable.

A resource member is stored as the 16-byte RSC7 prefix plus raw deflate. A member of 16 MiB or more
escapes the 24-bit size field: the real size is spread over prefix bytes 2, 5, 14 and 7.
"""

from __future__ import annotations

import struct
import zlib
from collections.abc import Callable, Sequence

from gtavmenu_tools.meta_resource import stored_deflate
from gtavmenu_tools.rpf_toc import MAGIC, TAG_KEYED, TAG_OPEN

__all__ = [
    "BLOCK",
    "DIRECTORY",
    "MAGIC",
    "OVERSIZE",
    "TAG_KEYED",
    "TAG_OPEN",
    "Rpf7Error",
    "build",
    "entry_paths",
    "find_entry",
    "inflate_resource",
    "loose_resource",
    "oversize_prefix",
    "prefix_size",
    "read_table",
    "stored_deflate",
]

DIRECTORY = 0x7FFFFF00
BLOCK = 512
OVERSIZE = 0xFFFFFF  # 24-bit size escape: real size in prefix bytes 2/5/14/7

# (table bytes, archive basename, archive size) -> transformed table bytes.
TableCipher = Callable[[bytes, str, int], bytes]
# (name, stored bytes = 16-byte prefix + raw deflate, system flags, graphics flags).
Member = tuple[str, bytes, int, int]


class Rpf7Error(ValueError):
    """The archive or resource is outside the layout this module reads and writes."""


def oversize_prefix(stored: bytes) -> bytes:
    """Encode len(stored) into the 16-byte prefix the way retail archives do."""
    size = len(stored)
    if size >= 1 << 32:
        raise Rpf7Error("member exceeds the 32-bit oversize field")
    head = bytearray(stored[:16])
    head[2], head[5], head[14], head[7] = size >> 24, (size >> 16) & 255, (size >> 8) & 255, size & 255
    return bytes(head) + stored[16:]


def prefix_size(head: bytes) -> int:
    return head[2] << 24 | head[5] << 16 | head[14] << 8 | head[7]


def read_table(blob: bytes, basename: str, decrypt: TableCipher | None = None) -> tuple[list[dict], bytes]:
    """Entries and name pool of an archive. A keyed table needs `decrypt`; an OPEN one never uses it.

    Directory entries: {kind: "dir", name, first, count}. Files: {kind: "res"|"bin", name, size,
    offset, a, b}, where a/b are the resource flags of a "res" (or size/flags of a "bin") entry.
    """
    if len(blob) < 16:
        raise Rpf7Error(f"{basename}: truncated RPF header")
    magic, count, names_field, tag = struct.unpack_from("<4I", blob, 0)
    if magic != MAGIC or tag not in (TAG_KEYED, TAG_OPEN) or names_field >> 28 or count < 1:
        raise Rpf7Error(f"{basename}: unexpected RPF header")
    table = blob[16 : 16 + count * 16 + names_field]
    if len(table) != count * 16 + names_field:
        raise Rpf7Error(f"{basename}: table runs past the end of the archive")
    if tag == TAG_KEYED:
        if decrypt is None:
            raise Rpf7Error(f"{basename}: keyed table needs the game's table keys (not available here)")
        table = decrypt(table, basename, len(blob))
    names = table[count * 16 :]
    entries = []
    for i in range(count):
        q, a, b = struct.unpack_from("<QII", table, i * 16)
        name_offset = q & 0xFFFF
        end = names.find(b"\0", name_offset)
        if end < 0:
            raise Rpf7Error(f"{basename}: entry {i} name is not terminated")
        name = names[name_offset:end].decode("ascii")
        if q >> 32 == DIRECTORY:
            entries.append({"kind": "dir", "name": name, "first": a, "count": b})
            continue
        offset = (q >> 40) & 0xFFFFFF
        size = (q >> 16) & 0xFFFFFF
        if size == OVERSIZE and offset & 0x800000:
            at = (offset & 0x7FFFFF) * BLOCK
            size = prefix_size(blob[at : at + 16])
        entries.append(
            {
                "kind": "res" if offset & 0x800000 else "bin",
                "name": name,
                "size": size,
                "offset": (offset & 0x7FFFFF) * BLOCK,
                "a": a,
                "b": b,
            }
        )
    return entries, names


def build(members: Sequence[Member], archive: str, encrypt: TableCipher | None = None) -> bytes:
    """One archive of resource `members` below a single root directory.

    A member name is ``<name>`` or ``<folder>/<name>`` (one folder level, as retail archives file
    streamed ped components under ``player_one/``). Like retail archives, every directory lists its
    children contiguously and sorted by name, the root first and each folder after it in order; a
    flat member list therefore gives the same bytes as a plain sorted table. Without `encrypt` the
    table is written plain with tag OPEN; with it, `encrypt(table + names, archive, total size)`
    produces the keyed table and the tag is TAG_KEYED.
    """
    tree: dict[str, dict | Member] = {}
    for member in members:
        parts = member[0].split("/")
        if len(parts) > 2 or not all(parts):
            raise Rpf7Error(f"{archive}: member {member[0]!r} is not <name> or <folder>/<name>")
        node = tree if len(parts) == 1 else tree.setdefault(parts[0], {})
        if not isinstance(node, dict) or parts[-1] in node:
            raise Rpf7Error(f"{archive}: member {member[0]!r} collides with another member or folder")
        node[parts[-1]] = (member[1], member[2], member[3])
    # Breadth-first: [name, (first, count) | (stored, sysf, gfxf)], the root first.
    rows: list[list] = [["", None]]
    pending = [(0, tree)]
    while pending:
        index, node = pending.pop(0)
        first = len(rows)
        rows += [[name, node[name]] for name in sorted(node)]
        rows[index][1] = (first, len(node))
        pending += [(first + i, node[name]) for i, name in enumerate(sorted(node)) if isinstance(node[name], dict)]
    names = b"\0"
    name_offsets = [0]
    for name, _ in rows[1:]:
        name_offsets.append(len(names))
        names += name.encode("ascii") + b"\0"
    names += bytes(-len(names) % 16)
    count = len(rows)
    table_end = 16 + count * 16 + len(names)
    cursor = table_end + (-table_end % BLOCK)
    table = b""
    payloads = []
    for (_, value), name_offset in zip(rows, name_offsets, strict=True):
        if len(value) == 2:
            table += struct.pack("<IIII", name_offset, DIRECTORY, value[0], value[1])
            continue
        stored, sysf, gfxf = value
        stored = oversize_prefix(stored) if len(stored) >= OVERSIZE else stored
        if cursor // BLOCK >= 0x800000:
            raise Rpf7Error("member offset exceeds the 23-bit block field")
        size_field = min(len(stored), OVERSIZE)
        table += struct.pack(
            "<QII", name_offset | (size_field << 16) | (((cursor // BLOCK) | 0x800000) << 40), sysf, gfxf
        )
        payloads.append((cursor, stored))
        cursor += len(stored) + (-len(stored) % BLOCK)
    total = cursor
    if encrypt is None:
        out = struct.pack("<4I", MAGIC, count, len(names), TAG_OPEN) + table + names
    else:
        out = struct.pack("<4I", MAGIC, count, len(names), TAG_KEYED) + encrypt(table + names, archive, total)
    for offset, stored in payloads:
        out += bytes(offset - len(out)) + stored
    out += bytes(total - len(out))
    return out


def entry_paths(entries: Sequence[dict]) -> list[str]:
    """The path of every table entry below the root ("" for the root), e.g. "player_one/x.pdd".

    `entries` are read_table's rows; a child listed by two directories, or outside the table, is
    refused.
    """
    paths: list[str | None] = [None] * len(entries)
    paths[0] = ""
    pending = [0]
    while pending:
        index = pending.pop()
        node = entries[index]
        if node["kind"] != "dir":
            continue
        for child in range(node["first"], node["first"] + node["count"]):
            if child <= 0 or child >= len(entries) or paths[child] is not None:
                raise Rpf7Error(f"directory entry {index} lists child {child} outside the table or twice")
            parent = paths[index]
            paths[child] = f"{parent}/{entries[child]['name']}" if parent else entries[child]["name"]
            pending.append(child)
    if any(path is None for path in paths):
        raise Rpf7Error("table entry not reachable from the root")
    return [path or "" for path in paths]


def find_entry(blob: bytes, archive_name: str, path: str, decrypt: TableCipher | None = None) -> dict:
    """Resolve 'a/b/nested.rpf/member' inside an archive, descending into nested archives.

    Returns the entry plus "blob", the (innermost) archive bytes its offset refers to.
    """
    entries, _ = read_table(blob, archive_name, decrypt)
    parts = path.split("/")
    index = 0
    for depth, part in enumerate(parts):
        node = entries[index]
        children = range(node["first"], node["first"] + node["count"])
        match = next((i for i in children if entries[i]["name"] == part), None)
        if match is None:
            raise Rpf7Error(f"{archive_name}: {'/'.join(parts[: depth + 1])} not found")
        entry = entries[match]
        if entry["kind"] == "dir":
            index = match
            continue
        rest = "/".join(parts[depth + 1 :])
        if not rest:
            return {**entry, "blob": blob}
        if not part.endswith(".rpf"):
            raise Rpf7Error(f"{part} is not an archive")
        nested = blob[entry["offset"] : entry["offset"] + entry["a"]]
        return find_entry(nested, part, rest, decrypt)
    raise Rpf7Error(f"{path} names a directory")


def inflate_resource(stored: bytes, label: str) -> bytes:
    """The payload of a stored resource member (16-byte prefix + raw deflate), which must inflate fully."""
    stream = zlib.decompressobj(-15)
    try:
        raw = stream.decompress(stored[16:])
    except zlib.error as error:
        raise Rpf7Error(f"{label}: payload does not inflate from offset 16 ({error})") from None
    if not stream.eof:
        raise Rpf7Error(f"{label}: payload does not inflate from offset 16")
    return raw


def loose_resource(blob: bytes, label: str) -> tuple[bytes, int, int, bytes]:
    """(stored bytes, system flags, graphics flags, payload) of a loose RSC7 file stored as-is."""
    if len(blob) < 16:
        raise Rpf7Error(f"{label}: not an RSC7 resource")
    magic, _version, sysf, gfxf = struct.unpack_from("<4sIII", blob, 0)
    if magic != b"RSC7":
        raise Rpf7Error(f"{label}: not an RSC7 resource")
    return blob, sysf, gfxf, inflate_resource(blob, label)

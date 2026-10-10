"""Audio metadata chunks (``.rel``) for AUDIO_GAMEDATA pack rows: parse, rebuild, clone.

A chunk is the RelFile layout CodeWalker documents (``RelFile.cs``), all fields little-endian u32,
exactly as the PS5 01.010.002 chunk loader reads it:

  type (151 = game data), data length, data
  name table: length (4 + 4 * count + string bytes), count, offsets[count], NUL-terminated strings
  index: count, {name hash, data offset, length}[count]
  hash table: count, file offsets (data offset + 8) of u32 object references the loader resolves
  pack table: count, file offsets of u32 wave bank name hashes the loader resolves

Every object starts with a u32 whose low byte is its class (3 = CarAudioSettings, 4 =
VehicleEngineAudioSettings, 88 = GranularEngineAudioSettings) and whose upper 24 bits are a name-table
offset into a ``.nametable`` file the game-data manager (format version 4) never loads. The loader
buckets the index by the low byte of each hash and binary-searches every bucket, so the index must
be sorted by ``(hash & 0xff, hash)``; it writes the resolved hash- and pack-table references into
the data, so their offsets must lie inside it. ``check`` enforces what the worker's gate
(``gtav_custom_pack_audio_rel_check``, src/common/custom_pack_checks.c) enforces, rule for rule.

``build(parse(blob)) == blob`` for every retail PS5 chunk with a hash index (checked against the
game's own chunks during development). No game data lives in this module.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from gtavmenu_tools.hashes import joaat

GAME_DATA = 151
# Objects a pack chunk may hold (retail game data: BASE 18,945); the worker's gate uses the same
# bound. parse() itself only bounds every table by the file size (sound chunks are larger).
INDEX_MAX = 65536
# Classes (CodeWalker Dat151RelType).
CAR = 3
ENGINE = 4
GRANULAR_ENGINE = 88
# CarAudioSettings fields used by the clone (byte offsets from the object start, past the u32 class
# and name-table word): Flags, Engine (VehicleEngineAudioSettings), GranularEngine.
CAR_FLAGS = 4
CAR_ENGINE = 8
CAR_GRANULAR_ENGINE = 12
CAR_MIN_LENGTH = 16
# The GAMEDATA mounter re-initialises the object of this name hash in every new chunk (live
# 0x98eb2e..0x98eb6c, class 24); the worker refuses chunks that define it.
REINIT_HASH = 0x953BD40D


class RelError(ValueError):
    """The bytes are not a chunk the PS5 game-data loader can take safely."""


@dataclass
class RelChunk:
    type: int
    data: bytes
    name_offsets: list[int] = field(default_factory=list)
    name_strings: bytes = b""
    index: list[tuple[int, int, int]] = field(default_factory=list)  # (hash, data offset, length)
    hash_offsets: list[int] = field(default_factory=list)  # file offsets
    pack_offsets: list[int] = field(default_factory=list)

    @property
    def names(self) -> list[str]:
        out = []
        for offset in self.name_offsets:
            end = self.name_strings.index(b"\0", offset)
            out.append(self.name_strings[offset:end].decode("ascii"))
        return out

    def find(self, name_hash: int) -> tuple[int, int] | None:
        """(data offset, length) of the object named `name_hash`, or None."""
        for h, offset, length in self.index:
            if h == name_hash:
                return offset, length
        return None

    def object(self, name_hash: int) -> bytes:
        found = self.find(name_hash)
        if found is None:
            raise RelError(f"object {name_hash:#010x} not in the chunk")
        offset, length = found
        return self.data[offset : offset + length]

    def object_class(self, name_hash: int) -> int:
        return self.object(name_hash)[0]


def _u32(blob: bytes, at: int, what: str) -> int:
    if at < 0 or at + 4 > len(blob):
        raise RelError(f"truncated at {what}")
    return struct.unpack_from("<I", blob, at)[0]


def index_key(name_hash: int) -> tuple[int, int]:
    """Sort key of an index entry: the loader's bucket (hash low byte), then the hash."""
    return name_hash & 0xFF, name_hash


def parse(blob: bytes) -> RelChunk:
    """Parse the layout of any hash-indexed chunk (every table inside the file, no trailing bytes);
    raises RelError otherwise. `check` adds the gate's rules for a pack row."""
    at = 0
    rel_type = _u32(blob, at, "type")
    length = _u32(blob, 4, "data length")
    at = 8
    if length < 4 or length > len(blob) - 8:
        raise RelError("data length outside the file")
    data = blob[at : at + length]
    at += length
    table = _u32(blob, at, "name table length")
    count = _u32(blob, at + 4, "name table count")
    if table < 4 + 4 * count or table > len(blob) - at - 4:
        raise RelError("name table outside the file")
    offsets = [_u32(blob, at + 8 + 4 * i, "name offset") for i in range(count)]
    strings = blob[at + 8 + 4 * count : at + 4 + table]
    for offset in offsets:
        if offset >= len(strings) or b"\0" not in strings[offset:]:
            raise RelError("name table string unterminated")
    at += 4 + table
    entries = _u32(blob, at, "index count")
    if 12 * entries > len(blob) - at - 4:
        raise RelError("index outside the file")
    index = [struct.unpack_from("<III", blob, at + 4 + 12 * i) for i in range(entries)]
    at += 4 + 12 * entries
    tables = []
    for what in ("hash table", "pack table"):
        n = _u32(blob, at, what + " count")
        if 4 * n > len(blob) - at - 4:
            raise RelError(what + " outside the file")
        tables.append([_u32(blob, at + 4 + 4 * i, what) for i in range(n)])
        at += 4 + 4 * n
    if at != len(blob):
        raise RelError("bytes after the pack table")
    return RelChunk(rel_type, data, offsets, strings, [tuple(e) for e in index], tables[0], tables[1])


def check(chunk: RelChunk) -> None:
    """The gate's rules (gtav_custom_pack_audio_rel_check) on a parsed chunk."""
    if chunk.type != GAME_DATA:
        raise RelError(f"type {chunk.type} is not game data ({GAME_DATA})")
    data = len(chunk.data)
    if not chunk.index or len(chunk.index) > INDEX_MAX:
        raise RelError(f"chunk has no objects or more than {INDEX_MAX}")
    previous = None
    for h, offset, length in chunk.index:
        if offset < 4 or length < 4 or offset > data or length > data - offset:
            raise RelError(f"object {h:#010x} outside the data")
        key = index_key(h)
        if previous is not None and key <= previous:
            raise RelError("index not sorted by (hash & 0xff, hash) or a name repeats")
        previous = key
        if h == REINIT_HASH:
            raise RelError("chunk defines the object the mounter re-initialises")
        if chunk.data[offset] == 0:
            raise RelError(f"object {h:#010x} has class 0")
    for what, offsets in (("hash", chunk.hash_offsets), ("pack", chunk.pack_offsets)):
        for offset in offsets:
            if offset < 8 or offset - 8 > data - 4:
                raise RelError(f"{what} table reference outside the data")


def build(chunk: RelChunk) -> bytes:
    """Serialise `chunk` (the inverse of parse for every chunk parse accepts)."""
    names = struct.pack(f"<I{len(chunk.name_offsets)}I", len(chunk.name_offsets), *chunk.name_offsets)
    names += chunk.name_strings
    out = [struct.pack("<II", chunk.type, len(chunk.data)), chunk.data, struct.pack("<I", len(names)), names]
    out.append(struct.pack("<I", len(chunk.index)))
    out += [struct.pack("<III", *entry) for entry in chunk.index]
    for offsets in (chunk.hash_offsets, chunk.pack_offsets):
        out.append(struct.pack(f"<I{len(offsets)}I", len(offsets), *offsets))
    return b"".join(out)


def name_hash(name: str) -> int:
    """The game's object name hash: joaat of the lowercase name (as vehicles.meta audioNameHash)."""
    return joaat(name.lower())


def new_chunk(header: bytes, objects: list[tuple[int, bytes]]) -> RelChunk:
    """A game-data chunk holding `objects` ((name hash, bytes) without hash- or pack-table
    references, e.g. CarAudioSettings), each 4-byte aligned after the 4-byte `header` word copied
    from the source chunk's data. Name-table offsets (upper 24 bits of each object's first word) are
    cleared: format 4 never loads a .nametable."""
    if len(header) != 4:
        raise RelError("data header is one u32")
    data = bytearray(header)
    index = []
    for h, body in sorted(objects, key=lambda o: index_key(o[0])):
        if len(body) < 4 or body[0] == 0:
            raise RelError(f"object {h:#010x} is empty or has class 0")
        data += b"\0" * (-len(data) % 4)
        index.append((h, len(data), len(body)))
        data += bytes([body[0], 0, 0, 0]) + body[4:]
    chunk = RelChunk(GAME_DATA, bytes(data), index=index)
    check(chunk)
    return chunk


def clone_car(source: RelChunk, donor: str, engine_from: tuple[int, int] | None = None) -> bytes:
    """Bytes of CarAudioSettings `donor` (to file under a new name with new_chunk): with
    `engine_from` = (Engine, GranularEngine) hashes the two engine references are replaced, giving
    the car the engine and exhaust sound of another vehicle."""
    body = bytearray(source.object(name_hash(donor)))
    if body[0] != CAR or len(body) < CAR_MIN_LENGTH:
        raise RelError(f"{donor} is not a CarAudioSettings object")
    if engine_from is not None:
        struct.pack_into("<II", body, CAR_ENGINE, *engine_from)
    return bytes(body)


def car_engines(chunk: RelChunk, name_hash_value: int) -> tuple[int, int]:
    """(Engine, GranularEngine) hashes of CarAudioSettings `name_hash_value`."""
    body = chunk.object(name_hash_value)
    if body[0] != CAR or len(body) < CAR_MIN_LENGTH:
        raise RelError(f"{name_hash_value:#010x} is not a CarAudioSettings object")
    return struct.unpack_from("<II", body, CAR_ENGINE)

#!/usr/bin/env python3
"""Write a PS5 interior .ptyp (CMapTypes + CMloArchetypeDef, RSC7 v2 meta) from a description.

Unlike make_addon_mlo.py (which patches hashes inside a copy of a retail typ), this writer lays out
every data block itself: archetypes (CBaseArchetypeDef 0x90, CMloArchetypeDef 0xf0), the MLO's
entities (CEntityDef 0x80), rooms (CMloRoomDef 0x70), portals (CMloPortalDef 0x40, four corners
each), entity sets (CMloEntitySet 0x30) and, when a schema for it is given, timecycle modifiers.
The description ("spec") is JSON; it comes from a retail .ptyp (`decode`), a CodeWalker
.ytyp.xml (`import-xml`) or a PC binary .ytyp (`import-pc`, the same meta format with its own
schema).

Only the parser schema is taken from the user's own game: --template is a loose RSC7 interior
.ptyp (e.g. v_int_22.ptyp) and --schema adds struct/enum infos from other retail typs (e.g. for
CMloTimeCycleModifier). Nothing of either ships with this tool. The writer copies the template's
header words and its opaque +0x40 blob, includes the struct infos of the struct types it writes
(template order) and the enums they use, and places every object the way retail metas are laid out:
largest first, first fit, into pages of 8 KiB or more (16-byte aligned except where the struct info
says 8), each block below 16 KiB. Vector3 padding words are 0x7f800001 as in retail.

Spec (JSON; every member of a struct is optional and defaults to zero, Vector3 to (0,0,0)):
  {"format": "gtavmenu-ytyp/1", "name": "gm_int_22",
   "archetypes": [{"type": "CMloArchetypeDef", "name": "gm_gun2", "lodDist": 20.0,
                   "assetType": "ASSET_TYPE_ASSETLESS", "assetName": "gm_gun2",
                   "entities": [{"archetypeName": "v_ilev_gc_door04", "position": [x, y, z],
                                 "rotation": [x, y, z, w], "lodDist": 45.0, ...}],
                   "rooms": [{"name": "limbo", "bbMin": [...], "bbMax": [...], "attachedObjects": [0, 1]}],
                   "portals": [{"roomFrom": 1, "roomTo": 0, "flags": 9, "corners": [[x, y, z] x 4],
                                "attachedObjects": [5]}],
                   "entitySets": [{"name": "v_22_wallhooks", "locations": [1], "entities": [...]}]}]}
Hash members take a name (joaat), "0xHASH" or "hash_HASH"; enums take their value name or an int;
the room `name` is a string kept as written. `type` names the struct behind an archetype pointer.

  make_mlo_ptyp.py decode v_int_22.ptyp --output v_int_22.json [--names FILE ...] [--listings DIR]
  make_mlo_ptyp.py import-xml interior.ytyp.xml --output spec.json
  make_mlo_ptyp.py import-pc interior.ytyp --output spec.json
  make_mlo_ptyp.py export-xml spec.json --output interior.ytyp.xml
  make_mlo_ptyp.py edit spec.json --output new.json [--typ-name N] [--mlo-name N] [--mlo-only]
      [--remove-entity INDEX|ARCHETYPE ...] [--move-entity INDEX=DX,DY,DZ ...]
  make_mlo_ptyp.py build spec.json --template v_int_22.ptyp [--schema OTHER.ptyp ...] --output gm.ptyp
  make_mlo_ptyp.py compare A.ptyp|A.json B.ptyp|B.json [--names ...]
  make_mlo_ptyp.py check gm.ptyp

`build` parses its output back: the decoded tree must equal the spec member for member (hashes,
float32 values, enums, strings, array lengths and order), the MLO must be consistent (room/portal
indices, four corners, attached-object indices, entity-set locations) and re-encoding the decoded
tree must give the same bytes. Refuses to overwrite.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import struct
import xml.etree.ElementTree as ET
import zlib
from dataclasses import dataclass, field
from pathlib import Path

from gtavmenu_tools.asset_formats import AssetError, Limits
from gtavmenu_tools.asset_metadata import parse_xml
from gtavmenu_tools.hashes import joaat
from gtavmenu_tools.meta_resource import RESOURCE_BASE, Meta, joaat_cs, stored_deflate

FORMAT = "gtavmenu-ytyp/1"
VERSION = 2

# Member types of the PS5 (and PC) parser schema rows.
T_STRUCT, T_POINTER, T_S32, T_U32, T_FLOAT = 0x05, 0x07, 0x14, 0x15, 0x21
T_VEC3, T_VEC4, T_CHARPTR, T_HASH, T_ARRAY, T_ENUM = 0x33, 0x34, 0x44, 0x4A, 0x52, 0x62
T_CHAR = 0x10  # block type of CharPointer text
SCALAR_SIZE = {T_S32: 4, T_U32: 4, T_FLOAT: 4, T_HASH: 4, T_ENUM: 4, T_VEC3: 16, T_VEC4: 16, T_POINTER: 8}
PRIMITIVE_ALIGN = {T_POINTER: 8, T_VEC3: 16, T_VEC4: 16, T_CHAR: 4}  # others: 4
VEC3_PAD = 0x7F800001  # retail w word of every Vector3
ARRAY_ROW = 0x100  # element descriptor rows of arrays are named 0x100
# Read-only member types (bool, s8, u8, s16, u16, IntFlags1, ShortFlags, IntFlags2): PC typs carry them in
# archetype extensions (e.g. Map Builder's builderdef.ytyp); decoded so the archetypes read, never written.
DECODE_ONLY = {0x01: "<?", 0x10: "<b", 0x11: "<B", 0x12: "<h", 0x13: "<H", 0x63: "<I", 0x64: "<H", 0x65: "<I"}

MAX_BLOCK = 0x4000  # retail blocks stay below 16 KiB (113 x 0x90, 127 x 0x80, 255 x 0x40)
MIN_PAGE = 0x2000
HEADER_SIZE = 0x70
PAGE_INFO_HEADER, PAGE_INFO_RECORD = 16, 8
MAX_DEPTH = 8
MAX_NODES = 200_000
MAX_XML_BYTES = 64 << 20
PAGE_FIELDS = ((4, 1), (5, 2), (7, 4), (11, 6), (17, 7), (24, 1), (25, 1), (26, 1), (27, 1))

STRUCT_NAMES = [
    "CMapTypes",
    "CBaseArchetypeDef",
    "CTimeArchetypeDef",
    "CMloArchetypeDef",
    "CEntityDef",
    "CMloRoomDef",
    "CMloPortalDef",
    "CMloEntitySet",
    "CMloTimeCycleModifier",
    "CCompositeEntityType",
    "CMloInstanceDef",
]
MEMBER_NAMES = [
    "extensions",
    "archetypes",
    "name",
    "dependencies",
    "compositeEntityTypes",
    "lodDist",
    "flags",
    "specialAttribute",
    "bbMin",
    "bbMax",
    "bsCentre",
    "bsRadius",
    "hdTextureDist",
    "textureDictionary",
    "clipDictionary",
    "drawableDictionary",
    "physicsDictionary",
    "assetType",
    "assetName",
    "padding0",
    "padding1",
    "timeFlags",
    "mloFlags",
    "entities",
    "rooms",
    "portals",
    "entitySets",
    "timeCycleModifiers",
    "blend",
    "timecycleName",
    "secondaryTimecycleName",
    "portalCount",
    "floorId",
    "exteriorVisibiltyDepth",
    "attachedObjects",
    "roomFrom",
    "roomTo",
    "mirrorPriority",
    "opacity",
    "audioOcclusion",
    "corners",
    "locations",
    "archetypeName",
    "guid",
    "position",
    "rotation",
    "scaleXY",
    "scaleZ",
    "parentIndex",
    "childLodDist",
    "lodLevel",
    "numChildren",
    "priorityLevel",
    "ambientOcclusionMultiplier",
    "artificialAmbientOcclusion",
    "tintValue",
    "sphere",
    "percentage",
    "range",
    "startHour",
    "endHour",
]
ENUM_NAMES = ["rage__fwArchetypeDef__eAssetType", "rage__eLodType", "rage__ePriorityLevel"]
ENUM_VALUE_NAMES = [
    "ASSET_TYPE_UNINITIALIZED",
    "ASSET_TYPE_FRAGMENT",
    "ASSET_TYPE_DRAWABLE",
    "ASSET_TYPE_DRAWABLEDICTIONARY",
    "ASSET_TYPE_ASSETLESS",
    "LODTYPES_DEPTH_HD",
    "LODTYPES_DEPTH_LOD",
    "LODTYPES_DEPTH_SLOD1",
    "LODTYPES_DEPTH_SLOD2",
    "LODTYPES_DEPTH_SLOD3",
    "LODTYPES_DEPTH_ORPHANHD",
    "LODTYPES_DEPTH_SLOD4",
    "PRI_REQUIRED",
    "PRI_OPTIONAL_HIGH",
    "PRI_OPTIONAL_MEDIUM",
    "PRI_OPTIONAL_LOW",
]
NAME_OF = {joaat_cs(n): n for n in [*STRUCT_NAMES, *MEMBER_NAMES, *ENUM_NAMES, *ENUM_VALUE_NAMES]}
# Pointer arrays whose targets are always this struct: `type` is left out of the spec for them.
DEFAULT_TARGET = {("CMloArchetypeDef", "entities"): "CEntityDef", ("CMloEntitySet", "entities"): "CEntityDef"}
MAP_TYPES, MLO_DEF = joaat_cs("CMapTypes"), joaat_cs("CMloArchetypeDef")


class SpecError(ValueError):
    """A spec, schema or resource this writer cannot represent, or a readback that differs."""


def _label(value: int) -> str:
    return NAME_OF.get(value, f"0x{value:08x}")


def _hash_of_label(label: str) -> int:
    text = label.strip()
    if re.fullmatch(r"0x[0-9a-fA-F]{1,8}", text):
        return int(text, 16)
    return joaat_cs(text)


# --------------------------------------------------------------------------- schema


@dataclass
class Member:
    name: str
    offset: int
    type: int
    ref: int
    element: tuple[int, int] | None = None  # (type, ref) of an array's element row


@dataclass
class StructInfo:
    hash: int
    raw: bytes  # 0x20-byte struct info (members pointer rewritten on output)
    rows: bytes  # member rows, 16 bytes each, verbatim
    size: int
    members: list[Member]

    @property
    def name(self) -> str:
        return _label(self.hash)

    @property
    def align(self) -> int:
        return 1 << ((struct.unpack_from("<I", self.raw, 8)[0] >> 8) & 0xFF) or 16

    def member(self, name: str) -> Member | None:
        return next((m for m in self.members if m.name == name), None)


@dataclass
class EnumInfo:
    hash: int
    raw: bytes  # 0x18 bytes
    values_raw: bytes  # 8 bytes per value: u32 name hash, s32 value

    def values(self) -> dict[int, int]:
        return {
            struct.unpack_from("<I", self.values_raw, i)[0]: struct.unpack_from("<i", self.values_raw, i + 4)[0]
            for i in range(0, len(self.values_raw), 8)
        }


@dataclass
class Schema:
    structs: dict[int, StructInfo] = field(default_factory=dict)
    enums: dict[int, EnumInfo] = field(default_factory=dict)

    @classmethod
    def from_payload(cls, payload: bytes) -> Schema:
        meta = _meta(payload)
        schema = cls()
        for i in range(meta.n_struct):
            at = meta.off(meta.struct_ptr) + 0x20 * i
            raw = payload[at : at + 0x20]
            name = struct.unpack_from("<I", raw, 0)[0]
            size, _pad, count = struct.unpack_from("<IHH", raw, 0x18)
            rows_at = meta.off(struct.unpack_from("<Q", raw, 0x10)[0])
            rows = payload[rows_at : rows_at + 16 * count]
            if len(rows) != 16 * count:
                raise SpecError(f"struct info {_label(name)}: member rows run past the payload")
            parsed = [struct.unpack_from("<IIBBHI", rows, 16 * j) for j in range(count)]
            members = []
            for row_name, offset, row_type, _sub, aux, ref in parsed:
                if row_name == ARRAY_ROW:
                    continue
                element = None
                if row_type == T_ARRAY:
                    if aux >= count or parsed[aux][0] != ARRAY_ROW:
                        raise SpecError(f"{_label(name)}.{_label(row_name)}: array without an element row")
                    element = (parsed[aux][2], parsed[aux][5])
                members.append(Member(_label(row_name), offset, row_type, ref, element))
            schema.structs[name] = StructInfo(name, raw, rows, size, members)
        for i in range(meta.n_enum):
            at = meta.off(meta.enum_ptr) + 0x18 * i
            raw = payload[at : at + 0x18]
            values_at = meta.off(struct.unpack_from("<Q", raw, 8)[0])
            count = struct.unpack_from("<I", raw, 0x10)[0]
            if count > 4096:
                raise SpecError("enum info with too many values")
            schema.enums[struct.unpack_from("<I", raw, 0)[0]] = EnumInfo(
                struct.unpack_from("<I", raw, 0)[0], raw, payload[values_at : values_at + 8 * count]
            )
        return schema

    def merge(self, other: Schema) -> None:
        """Add the other schema's structs/enums; a struct present in both must be identical."""
        for key, info in other.structs.items():
            mine = self.structs.get(key)
            if mine is None:
                self.structs[key] = info
            elif (mine.size, mine.rows) != (info.size, info.rows):
                raise SpecError(f"schema sources disagree on {info.name}")
        for key, info in other.enums.items():
            self.enums.setdefault(key, info)

    def struct(self, name: str | int) -> StructInfo:
        key = name if isinstance(name, int) else _hash_of_label(name)
        if key not in self.structs:
            raise SpecError(f"no schema for {_label(key)} (add a --schema typ that has one)")
        return self.structs[key]


def _meta(payload: bytes) -> Meta:
    try:
        meta = Meta(payload)
    except (struct.error, ValueError) as error:
        raise SpecError(f"not a parser-meta payload: {error}") from None
    if meta.magic != b"0DRP":
        raise SpecError("payload has no PRD0 meta header")
    for _h, size, start in meta.blocks:
        if start < 0 or start + size > len(payload):
            raise SpecError("data block outside the payload")
    return meta


# --------------------------------------------------------------------------- resources


def page_sizes(flags: int) -> list[int]:
    """Largest-first system page sizes of the low 28 RSC7 flag bits."""
    return [
        (512 << (flags & 15)) << (8 - rank)
        for rank, (shift, width) in enumerate(PAGE_FIELDS)
        for _ in range((flags >> shift) & ((1 << width) - 1))
    ]


def flags_for_pages(sizes: list[int]) -> int:
    """Low 28 flag bits for a largest-first page list, preferring the retail 512-byte base."""
    for nibble in range(16):
        base, flags = 512 << nibble, nibble
        for rank, (shift, width) in enumerate(PAGE_FIELDS):
            count = sizes.count(base << (8 - rank))
            if count >= 1 << width:
                break
            flags |= count << shift
        else:
            if page_sizes(flags) == sizes:
                return flags
    raise SpecError(f"no RSC7 flags express pages {sizes}")


def read_payload(resource: bytes) -> bytes:
    """Inflated payload of a loose RSC7 v2 meta (PS5 stored/deflated, or PC with an Adler-32 trailer)."""
    if len(resource) < 16 or resource[:4] != b"RSC7":
        raise SpecError("not a loose RSC7 resource")
    _magic, version, sys_flags, gfx_flags = struct.unpack_from("<4sIII", resource, 0)
    if version != 2:
        raise SpecError(f"meta resources are version 2; header says {version}")
    if len(resource) > 64 << 20:
        raise SpecError("resource too large")
    stream = zlib.decompressobj(-15)
    try:
        payload = stream.decompress(resource[16:], 64 << 20)
    except zlib.error as error:
        raise SpecError(f"payload does not inflate: {error}") from None
    if not stream.eof or stream.unconsumed_tail:
        raise SpecError("payload does not inflate to its end")
    if stream.unused_data and stream.unused_data != struct.pack(">I", zlib.adler32(payload)):
        raise SpecError("payload has trailing bytes")
    del sys_flags, gfx_flags
    return payload


# --------------------------------------------------------------------------- decode


class _Decoder:
    def __init__(self, payload: bytes, names: dict[int, str]):
        self.p = payload
        self.meta = _meta(payload)
        self.schema = Schema.from_payload(payload)
        self.names = names
        self.nodes = 0

    def block_of(self, at: int) -> int:
        for struct_hash, size, start in self.meta.blocks:
            if start <= at < start + size:
                return struct_hash
        raise SpecError(f"offset {at:#x} lies in no data block")

    def target(self, ref: int, what: str) -> int:
        at = self.meta.ref(ref)
        if at is None or not 0 <= at < len(self.p):
            raise SpecError(f"{what}: {ref:#x} is not a block reference")
        return at

    def name(self, value: int) -> str:
        if not value:
            return ""
        return self.names.get(value, f"0x{value:08x}")

    def scalar(self, kind: int, ref: int, at: int):
        p = self.p
        if kind == T_S32:
            return struct.unpack_from("<i", p, at)[0]
        if kind == T_U32:
            return struct.unpack_from("<I", p, at)[0]
        if kind == T_FLOAT:
            return struct.unpack_from("<f", p, at)[0]
        if kind == T_HASH:
            return self.name(struct.unpack_from("<I", p, at)[0])
        if kind == T_VEC3:
            return list(struct.unpack_from("<3f", p, at))
        if kind == T_VEC4:
            return list(struct.unpack_from("<4f", p, at))
        if kind == T_ENUM:
            value = struct.unpack_from("<i", p, at)[0]
            info = self.schema.enums.get(ref)
            if info is not None:
                for name_hash, number in info.values().items():
                    if number == value and name_hash in NAME_OF:
                        return NAME_OF[name_hash]
            return value
        if kind == T_CHARPTR:
            ref_value, length, _cap = struct.unpack_from("<QHH", p, at)
            if not ref_value:
                return ""
            start = self.target(ref_value, "CharPointer")
            if self.block_of(start) != T_CHAR or start + length > len(p):
                raise SpecError("CharPointer does not name char data")
            return p[start : start + length].decode("latin-1")
        if kind in DECODE_ONLY:
            return struct.unpack_from(DECODE_ONLY[kind], p, at)[0]
        raise SpecError(f"member type {kind:#x} is not supported")

    def struct_at(self, info: StructInfo, at: int, depth: int) -> dict:
        self.nodes += 1
        if depth > MAX_DEPTH or self.nodes > MAX_NODES:
            raise SpecError("meta tree too deep or too large")
        if at + info.size > len(self.p):
            raise SpecError(f"{info.name} at {at:#x} runs past the payload")
        out: dict = {}
        for member in info.members:
            at_member = at + member.offset
            if member.type != T_ARRAY:
                out[member.name] = self.scalar(member.type, member.ref, at_member)
                continue
            ref, count, _cap = struct.unpack_from("<QHH", self.p, at_member)
            kind, element_ref = member.element
            if not count:
                out[member.name] = []
                continue
            base = self.target(ref, f"{info.name}.{member.name}")
            if kind == T_POINTER:
                block = self.block_of(base)
                if block != T_POINTER:
                    raise SpecError(f"{info.name}.{member.name} does not name a pointer block")
                items = []
                default = DEFAULT_TARGET.get((info.name, member.name))
                for i in range(count):
                    target = self.target(struct.unpack_from("<Q", self.p, base + 8 * i)[0], member.name)
                    target_info = self.schema.struct(self.block_of(target))
                    item = self.struct_at(target_info, target, depth + 1)
                    if target_info.name != default:
                        item = {"type": target_info.name, **item}
                    items.append(item)
                out[member.name] = items
            elif kind == T_STRUCT:
                element = self.schema.struct(element_ref)
                if self.block_of(base) != element.hash:
                    raise SpecError(f"{info.name}.{member.name} does not name a {element.name} block")
                out[member.name] = [self.struct_at(element, base + element.size * i, depth + 1) for i in range(count)]
            elif kind in SCALAR_SIZE:
                size = SCALAR_SIZE[kind]
                if base + size * count > len(self.p):
                    raise SpecError(f"{info.name}.{member.name} runs past the payload")
                out[member.name] = [self.scalar(kind, element_ref, base + size * i) for i in range(count)]
            else:
                raise SpecError(f"{info.name}.{member.name}: array element type {kind:#x} is not supported")
        return out


def decode_payload(payload: bytes, names: dict[int, str] | None = None) -> dict:
    """The spec of an inflated CMapTypes meta payload (PS5 or PC: each carries its own schema)."""
    decoder = _Decoder(payload, names or {})
    root_index = struct.unpack_from("<I", payload, 0x1C)[0]
    if not 0 < root_index <= len(decoder.meta.blocks):
        raise SpecError("header root block index out of range")
    root_hash, _size, root_at = decoder.meta.blocks[root_index - 1]
    if root_hash != MAP_TYPES:
        raise SpecError("root block is not CMapTypes")
    try:
        tree = decoder.struct_at(decoder.schema.struct(MAP_TYPES), root_at, 0)
    except struct.error as error:
        raise SpecError(f"truncated meta data: {error}") from None
    return {"format": FORMAT, **tree}


def decode(resource: bytes, names: dict[int, str] | None = None) -> dict:
    return decode_payload(read_payload(resource), names)


# --------------------------------------------------------------------------- encode


def _number(value, what: str, integer: bool):
    if isinstance(value, bool) or not isinstance(value, (int, float, str)):
        raise SpecError(f"{what}: expected a number, got {value!r}")
    if isinstance(value, str):
        text = value.strip()
        try:
            value = int(text, 0) if integer and re.fullmatch(r"[-+]?(0x[0-9a-fA-F]+|\d+)", text) else float(text)
        except ValueError:
            raise SpecError(f"{what}: expected a number, got {value!r}") from None
    if integer:
        if isinstance(value, float):
            if not value.is_integer():
                raise SpecError(f"{what}: expected an integer, got {value!r}")
            value = int(value)
        return value
    if not math.isfinite(float(value)):
        raise SpecError(f"{what}: non-finite float")
    return float(value)


def hash_value(value, what: str = "hash") -> int:
    """A hash member: name (joaat, lower case), "0xHASH", "hash_HASH", "" (0) or an int."""
    if isinstance(value, int) and not isinstance(value, bool):
        if not 0 <= value <= 0xFFFFFFFF:
            raise SpecError(f"{what}: hash out of range")
        return value
    if value is None:
        return 0
    if not isinstance(value, str):
        raise SpecError(f"{what}: expected a name, got {value!r}")
    text = value.strip()
    if not text:
        return 0
    match = re.fullmatch(r"(?:0x|hash_)([0-9a-fA-F]{1,8})", text)
    if match:
        return int(match.group(1), 16)
    return joaat(text)


def _vector(value, count: int, what: str) -> list[float]:
    if isinstance(value, dict):
        value = [value.get(axis, 0.0) for axis in "xyzw"[:count]]
    if isinstance(value, str):
        value = [v for v in re.split(r"[\s,]+", value.strip()) if v]
    if not isinstance(value, (list, tuple)) or len(value) != count:
        raise SpecError(f"{what}: expected {count} numbers")
    return [_number(v, what, False) for v in value]


def encode_scalar(kind: int, ref: int, value, schema: Schema, what: str) -> bytes:
    if kind == T_S32:
        number = _number(value, what, True)
        if not -(1 << 31) <= number < 1 << 31:
            raise SpecError(f"{what}: out of s32 range")
        return struct.pack("<i", number)
    if kind == T_U32:
        number = _number(value, what, True)
        if not 0 <= number <= 0xFFFFFFFF:
            raise SpecError(f"{what}: out of u32 range")
        return struct.pack("<I", number)
    if kind == T_FLOAT:
        return struct.pack("<f", _number(value, what, False))
    if kind == T_HASH:
        return struct.pack("<I", hash_value(value, what))
    if kind == T_VEC3:
        return struct.pack("<3fI", *_vector(value, 3, what), VEC3_PAD)
    if kind == T_VEC4:
        return struct.pack("<4f", *_vector(value, 4, what))
    if kind == T_ENUM:
        if isinstance(value, str) and not re.fullmatch(r"[-+]?\d+", value.strip()):
            info = schema.enums.get(ref)
            values = info.values() if info else {}
            key = joaat_cs(value.strip())
            if key not in values:
                raise SpecError(f"{what}: {value!r} is not a value of {_label(ref)}")
            return struct.pack("<i", values[key])
        return struct.pack("<i", _number(value, what, True))
    raise SpecError(f"{what}: member type {kind:#x} is not supported")


def _zero(kind: int) -> object:
    return [0.0, 0.0, 0.0] if kind == T_VEC3 else [0.0] * 4 if kind == T_VEC4 else 0


@dataclass
class _Segment:
    pool: int  # block type: struct hash or primitive type
    size: int
    align: int
    data: bytearray = field(default_factory=bytearray)
    block: int = 0  # 1-based, after chunking
    offset: int = 0  # inside its block


class _Encoder:
    def __init__(self, schema: Schema):
        self.schema = schema
        self.segments: list[_Segment] = []
        self.fixups: list[tuple[_Segment, int, _Segment, int]] = []  # (seg, at, target seg, target offset)
        self.nodes = 0

    def segment(self, pool: int, size: int, align: int) -> _Segment:
        seg = _Segment(pool, size, align, bytearray(size))
        self.segments.append(seg)
        return seg

    def array(self, seg: _Segment, at: int, target: _Segment | None, count: int) -> None:
        if count > 0xFFFF:
            raise SpecError("array longer than 65535 entries")
        struct.pack_into("<QHHI", seg.data, at, 0, count, count, 0)
        if target is not None:
            self.fixups.append((seg, at, target, 0))

    def write_struct(self, info: StructInfo, value: dict, seg: _Segment, at: int, path: str, depth: int) -> None:
        self.nodes += 1
        if depth > MAX_DEPTH or self.nodes > MAX_NODES:
            raise SpecError("spec tree too deep or too large")
        if value in ("", None):
            value = {}
        if not isinstance(value, dict):
            raise SpecError(f"{path}: expected an object")
        known = {m.name for m in info.members}
        extra = set(value) - known - {"type"}
        if extra:
            raise SpecError(f"{path}: {info.name} has no member {sorted(extra)[0]!r}")
        for member in info.members:
            what = f"{path}.{member.name}"
            raw = value.get(member.name)
            if member.type == T_ARRAY:
                self.write_array(info, member, raw, seg, at + member.offset, what, depth)
            elif member.type == T_CHARPTR:
                text = "" if raw is None else raw
                if not isinstance(text, str) or len(text) > 0xFFFE:
                    raise SpecError(f"{what}: expected a string")
                if text:
                    encoded = text.encode("latin-1") + b"\0"
                    chars = self.segment(T_CHAR, len(encoded), PRIMITIVE_ALIGN[T_CHAR])
                    chars.data[:] = encoded
                    struct.pack_into("<QHHI", seg.data, at + member.offset, 0, len(text), len(text) + 1, 0)
                    self.fixups.append((seg, at + member.offset, chars, 0))
            else:
                data = encode_scalar(
                    member.type, member.ref, _zero(member.type) if raw is None else raw, self.schema, what
                )
                seg.data[at + member.offset : at + member.offset + len(data)] = data

    def write_array(
        self, owner: StructInfo, member: Member, raw, seg: _Segment, at: int, what: str, depth: int
    ) -> None:
        kind, element_ref = member.element
        items = raw if raw not in (None, "") else []
        if isinstance(items, str) and kind in (T_U32, T_S32, T_FLOAT, T_HASH):
            items = [v for v in re.split(r"[\s,]+", items.strip()) if v]
        if isinstance(items, str) and kind == T_VEC3:
            numbers = [v for v in re.split(r"[\s,]+", items.strip()) if v]
            if len(numbers) % 3:
                raise SpecError(f"{what}: Vector3 list needs a multiple of three numbers")
            items = [numbers[i : i + 3] for i in range(0, len(numbers), 3)]
        if not isinstance(items, list):
            raise SpecError(f"{what}: expected a list")
        if not items:
            self.array(seg, at, None, 0)
            return
        if kind == T_POINTER:
            pointers = self.segment(T_POINTER, 8 * len(items), PRIMITIVE_ALIGN[T_POINTER])
            self.array(seg, at, pointers, len(items))
            default = DEFAULT_TARGET.get((owner.name, member.name))
            for i, item in enumerate(items):
                if not isinstance(item, dict):
                    raise SpecError(f"{what}[{i}]: expected an object")
                type_name = item.get("type", default)
                if not type_name:
                    raise SpecError(f"{what}[{i}]: needs a `type`")
                info = self.schema.struct(type_name)
                target = self.segment(info.hash, info.size, info.align)
                self.fixups.append((pointers, 8 * i, target, 0))
                self.write_struct(info, item, target, 0, f"{what}[{i}]", depth + 1)
        elif kind == T_STRUCT:
            info = self.schema.struct(element_ref)
            target = self.segment(info.hash, info.size * len(items), info.align)
            self.array(seg, at, target, len(items))
            for i, item in enumerate(items):
                if isinstance(item, dict) and item.get("type") not in (None, info.name):
                    raise SpecError(f"{what}[{i}]: inline array of {info.name}, not {item['type']}")
                self.write_struct(info, item, target, info.size * i, f"{what}[{i}]", depth + 1)
        elif kind in SCALAR_SIZE:
            size = SCALAR_SIZE[kind]
            target = self.segment(kind, size * len(items), PRIMITIVE_ALIGN.get(kind, 4))
            self.array(seg, at, target, len(items))
            for i, item in enumerate(items):
                target.data[size * i : size * (i + 1)] = encode_scalar(
                    kind, element_ref, item, self.schema, f"{what}[{i}]"
                )
        else:
            raise SpecError(f"{what}: array element type {kind:#x} is not supported")


def _align(value: int, align: int) -> int:
    return (value + align - 1) // align * align


def _pow2(value: int) -> int:
    return 1 << max(0, (value - 1).bit_length())


def place(objects: list[tuple[str, int, int]]) -> tuple[dict[str, int], list[int]]:
    """Retail page placement: the header at 0, then largest first (stable), first fit over the open
    pages; a new page is the smallest power of two >= 8 KiB that holds the object opening it (the
    first page also holds the header). Returns {name: offset} and the page sizes in file order."""
    header_name, header_size, _align_unused = objects[0]
    pages: list[list[int]] = []  # [size, used]
    where: dict[str, tuple[int, int]] = {header_name: (0, 0)}
    for name, size, align in sorted(objects[1:], key=lambda item: -item[1]):
        for number, page in enumerate(pages):  # noqa: B007 (number is used after the loop)
            at = _align(page[1], align)
            if at + size <= page[0]:
                break
        else:
            at = _align(header_size if not pages else 0, align)
            pages.append([max(MIN_PAGE, _pow2(at + size)), 0])
            number = len(pages) - 1
        pages[number][1] = at + size
        where[name] = (number, at)
    if not pages:
        pages.append([MIN_PAGE, header_size])
    sizes = [page[0] for page in pages]
    if sizes != sorted(sizes, reverse=True):
        raise SpecError(f"page sizes {sizes} are not largest-first")
    starts = [sum(sizes[:i]) for i in range(len(sizes))]
    return {name: starts[number] + at for name, (number, at) in where.items()}, sizes


def build_payload(spec: dict, schema: Schema, template: bytes) -> tuple[bytes, list[int]]:
    """Inflated payload and page sizes for `spec`; `template` is the template's payload (header words,
    opaque +0x40 blob, struct/block order)."""
    if not isinstance(spec, dict):
        raise SpecError("spec must be a JSON object")
    if spec.get("format", FORMAT) != FORMAT:
        raise SpecError(f"spec format {spec.get('format')!r} is not {FORMAT}")
    body = {k: v for k, v in spec.items() if k != "format"}
    encoder = _Encoder(schema)
    root_info = schema.struct(MAP_TYPES)
    root = encoder.segment(MAP_TYPES, root_info.size, root_info.align)
    encoder.write_struct(root_info, body, root, 0, "spec", 0)

    template_meta = _meta(template)
    order: list[int] = []
    for struct_hash, _size, _at in template_meta.blocks:
        if struct_hash not in order:
            order.append(struct_hash)
    for seg in encoder.segments:
        if seg.pool not in order:
            order.append(seg.pool)
    blocks: list[tuple[int, list[_Segment]]] = []
    for pool in order:
        current: list[_Segment] = []
        used = 0
        for seg in (s for s in encoder.segments if s.pool == pool):
            if seg.size >= MAX_BLOCK:
                raise SpecError(f"one {_label(pool)} array needs {seg.size} bytes, more than a block holds")
            if current and used + seg.size >= MAX_BLOCK:
                blocks.append((pool, current))
                current, used = [], 0
            seg.offset = used
            current.append(seg)
            used += seg.size
        if current:
            blocks.append((pool, current))
    for number, (_pool, segs) in enumerate(blocks, 1):
        for seg in segs:
            seg.block = number
    for seg, at, target, offset in encoder.fixups:
        struct.pack_into("<Q", seg.data, at, (target.offset + offset) << 12 | target.block)

    used_structs = [h for h in (s for s, _i in template_order(template_meta, schema)) if h in order]
    used_structs += [pool for pool in order if pool in schema.structs and pool not in used_structs]
    infos = [schema.structs[h] for h in used_structs]
    enum_refs = {m.ref for info in infos for m in info.members if m.type == T_ENUM}
    enum_refs |= {m.element[1] for info in infos for m in info.members if m.element and m.element[0] == T_ENUM}
    template_enums = [
        struct.unpack_from("<I", template, template_meta.off(template_meta.enum_ptr) + 0x18 * i)[0]
        for i in range(template_meta.n_enum)
    ]
    enums = [schema.enums[h] for h in template_enums if h in enum_refs and h in schema.enums]
    enums += [schema.enums[h] for h in sorted(enum_refs) if h in schema.enums and schema.enums[h] not in enums]
    blob = b""
    blob_ptr = struct.unpack_from("<Q", template, 0x40)[0]
    if blob_ptr:
        blob_at = template_meta.off(blob_ptr)
        blob = template[blob_at : blob_at + 4 + struct.unpack_from(">I", template, blob_at)[0]]

    objects: list[tuple[str, int, int]] = [("header", HEADER_SIZE, 16)]
    for number, (_pool, segs) in enumerate(blocks, 1):
        align = max(seg.align for seg in segs)
        objects.append((f"block{number}", sum(seg.size for seg in segs), align))
    objects.append(("pageinfo", PAGE_INFO_HEADER + PAGE_INFO_RECORD, 16))
    objects.append(("structinfos", 0x20 * len(infos), 16))
    objects += [(f"members{i}", len(info.rows), 16) for i, info in enumerate(infos) if info.rows]
    if enums:
        objects.append(("enuminfos", 0x18 * len(enums), 8))
    objects += [(f"enumvalues{i}", len(info.values_raw), 8) for i, info in enumerate(enums) if info.values_raw]
    if blob:
        objects.append(("blob", len(blob), 16))
    objects.append(("blocktable", 16 * len(blocks), 16))
    for _ in range(8):
        offsets, sizes = place(objects)
        wanted = PAGE_INFO_HEADER + PAGE_INFO_RECORD * len(sizes)
        index = next(i for i, item in enumerate(objects) if item[0] == "pageinfo")
        if objects[index][1] == wanted:
            break
        objects[index] = ("pageinfo", wanted, 16)
    else:
        raise SpecError("page count does not settle")

    out = bytearray(sum(sizes))
    out[:HEADER_SIZE] = template[:HEADER_SIZE]
    ptr = lambda name: RESOURCE_BASE | offsets[name]  # noqa: E731
    struct.pack_into("<Q", out, 0x08, ptr("pageinfo"))
    root_block = next(i for i, (pool, _s) in enumerate(blocks, 1) if pool == MAP_TYPES)
    struct.pack_into("<I", out, 0x1C, root_block)
    struct.pack_into("<Q", out, 0x20, ptr("structinfos"))
    struct.pack_into("<Q", out, 0x28, ptr("enuminfos") if enums else 0)
    struct.pack_into("<Q", out, 0x30, ptr("blocktable"))
    struct.pack_into("<Q", out, 0x40, ptr("blob") if blob else 0)
    struct.pack_into("<HHHH", out, 0x48, len(infos), len(enums), len(blocks), 0)
    info_at = offsets["pageinfo"]
    out[info_at + 8] = len(sizes)
    for i, info in enumerate(infos):
        at = offsets["structinfos"] + 0x20 * i
        out[at : at + 0x20] = info.raw
        struct.pack_into("<Q", out, at + 0x10, ptr(f"members{i}") if info.rows else 0)
        if info.rows:
            out[offsets[f"members{i}"] : offsets[f"members{i}"] + len(info.rows)] = info.rows
    for i, info in enumerate(enums):
        at = offsets["enuminfos"] + 0x18 * i
        out[at : at + 0x18] = info.raw
        struct.pack_into("<Q", out, at + 8, ptr(f"enumvalues{i}") if info.values_raw else 0)
        if info.values_raw:
            out[offsets[f"enumvalues{i}"] : offsets[f"enumvalues{i}"] + len(info.values_raw)] = info.values_raw
    if blob:
        out[offsets["blob"] : offsets["blob"] + len(blob)] = blob
    for number, (pool, segs) in enumerate(blocks, 1):
        start = offsets[f"block{number}"]
        size = sum(seg.size for seg in segs)
        struct.pack_into("<IIQ", out, offsets["blocktable"] + 16 * (number - 1), pool, size, RESOURCE_BASE | start)
        for seg in segs:
            out[start + seg.offset : start + seg.offset + seg.size] = seg.data
    return bytes(out), sizes


def template_order(meta: Meta, schema: Schema) -> list[tuple[int, int]]:
    """(struct hash, index) of the template's struct infos, in file order."""
    return [
        (struct.unpack_from("<I", meta.b, meta.off(meta.struct_ptr) + 0x20 * i)[0], i) for i in range(meta.n_struct)
    ]


def build(spec: dict, template: bytes, schemas: list[bytes] | None = None) -> bytes:
    """Loose RSC7 v2 .ptyp (stored deflate) for `spec`, verified by verify() before it is returned.

    `template` and every `schemas` entry are loose RSC7 typs from the user's own game.
    """
    template_payload = read_payload(template)
    schema = Schema.from_payload(template_payload)
    for extra in schemas or []:
        schema.merge(Schema.from_payload(read_payload(extra)))
    payload, sizes = build_payload(spec, schema, template_payload)
    sys_flags = flags_for_pages(sizes) | (VERSION >> 4) << 28
    gfx_flags = (VERSION & 0xF) << 28
    resource = struct.pack("<4sIII", b"RSC7", VERSION, sys_flags, gfx_flags) + stored_deflate(payload)
    verify(resource, spec, template, schemas)
    return resource


# --------------------------------------------------------------------------- compare / verify


def canonical(tree, schema: Schema, info: StructInfo | None = None, path: str = "spec"):
    """Schema-typed canonical form: every scalar as the bytes it encodes to, defaults filled in."""
    info = info or schema.struct(MAP_TYPES)
    out = {}
    if isinstance(tree, dict) and tree.get("format") == FORMAT and info.hash == MAP_TYPES:
        tree = {k: v for k, v in tree.items() if k != "format"}
    for member in info.members:
        raw = tree.get(member.name) if isinstance(tree, dict) else None
        what = f"{path}.{member.name}"
        if member.type == T_ARRAY:
            kind, element_ref = member.element
            items = raw if raw not in (None, "") else []
            if isinstance(items, str):
                numbers = [v for v in re.split(r"[\s,]+", items.strip()) if v]
                items = [numbers[i : i + 3] for i in range(0, len(numbers), 3)] if kind == T_VEC3 else numbers
            if kind == T_POINTER:
                default = DEFAULT_TARGET.get((info.name, member.name))
                out[member.name] = [
                    (
                        schema.struct(item.get("type", default)).hash,
                        canonical(item, schema, schema.struct(item.get("type", default)), f"{what}[{i}]"),
                    )
                    for i, item in enumerate(items)
                ]
            elif kind == T_STRUCT:
                element = schema.struct(element_ref) if items else None
                out[member.name] = [canonical(item, schema, element, f"{what}[{i}]") for i, item in enumerate(items)]
            else:
                out[member.name] = [encode_scalar(kind, element_ref, v, schema, what) for v in items]
        elif member.type == T_CHARPTR:
            out[member.name] = raw or ""
        else:
            out[member.name] = encode_scalar(
                member.type, member.ref, _zero(member.type) if raw is None else raw, schema, what
            )
    return out


def differences(a, b, path: str = "", limit: int = 20) -> list[str]:
    """Paths where two canonical trees differ (at most `limit`)."""
    out: list[str] = []

    def walk(x, y, where):
        if len(out) >= limit:
            return
        if isinstance(x, dict) and isinstance(y, dict):
            for key in sorted(set(x) | set(y)):
                walk(x.get(key), y.get(key), f"{where}.{key}")
        elif isinstance(x, list) and isinstance(y, list):
            if len(x) != len(y):
                out.append(f"{where}: {len(x)} vs {len(y)} entries")
            for i, (u, v) in enumerate(zip(x, y, strict=False)):
                walk(u, v, f"{where}[{i}]")
        elif isinstance(x, tuple) and isinstance(y, tuple):
            if x[0] != y[0]:
                out.append(f"{where}: {_label(x[0])} vs {_label(y[0])}")
            walk(x[1], y[1], where)
        elif x != y:
            show = lambda v: v.hex() if isinstance(v, bytes) else repr(v)  # noqa: E731
            out.append(f"{where}: {show(x)} vs {show(y)}")

    walk(a, b, path)
    return out


def mlo_problems(spec: dict) -> list[str]:
    """Consistency of every MLO in a spec: indices in range, corners, set locations, portal counts."""
    try:
        return _mlo_problems(spec)
    except (AttributeError, TypeError, ValueError) as error:
        return [f"malformed MLO description: {error}"]


def _mlo_problems(spec: dict) -> list[str]:
    problems = []
    for a, arch in enumerate(spec.get("archetypes") or []):
        if arch.get("type") != "CMloArchetypeDef":
            continue
        where = f"archetypes[{a}]"
        entities = len(arch.get("entities") or [])
        rooms = arch.get("rooms") or []
        portals = arch.get("portals") or []
        if not rooms:
            problems.append(f"{where}: an MLO needs at least the limbo room")
        touching = [0] * len(rooms)
        for p, portal in enumerate(portals):
            ends = [int(portal.get("roomFrom", 0)), int(portal.get("roomTo", 0))]
            if any(not 0 <= r < len(rooms) for r in ends):
                problems.append(f"{where}.portals[{p}]: room {ends} out of range")
            else:
                for r in set(ends):
                    touching[r] += 1
            corners = portal.get("corners") or []
            if len(corners) != 4:
                problems.append(f"{where}.portals[{p}]: {len(corners)} corners (4 expected)")
            for obj in portal.get("attachedObjects") or []:
                if not 0 <= int(obj) < entities:
                    problems.append(f"{where}.portals[{p}]: attached object {obj} out of range")
        for r, room in enumerate(rooms):
            for obj in room.get("attachedObjects") or []:
                if not 0 <= int(obj) < entities:
                    problems.append(f"{where}.rooms[{r}]: attached object {obj} out of range")
            if int(room.get("portalCount", 0)) != touching[r]:
                count = room.get("portalCount", 0)
                problems.append(f"{where}.rooms[{r}]: portalCount {count} but {touching[r]} portals")
        for s, entity_set in enumerate(arch.get("entitySets") or []):
            locations = entity_set.get("locations") or []
            if len(locations) != len(entity_set.get("entities") or []):
                problems.append(f"{where}.entitySets[{s}]: {len(locations)} locations for its entities")
            if any(not 0 <= int(r) < len(rooms) for r in locations):
                problems.append(f"{where}.entitySets[{s}]: location out of range")
    return problems


def verify(resource: bytes, spec: dict, template: bytes, schemas: list[bytes] | None = None) -> dict:
    """Parse a built .ptyp back against its spec; returns a summary."""
    payload = read_payload(resource)
    _magic, _version, sys_flags, _gfx = struct.unpack_from("<4sIII", resource, 0)
    sizes = page_sizes(sys_flags & 0x0FFFFFFF)
    if len(payload) != sum(sizes):
        raise SpecError(f"payload {len(payload)} bytes does not match its pages {sizes}")
    schema = Schema.from_payload(read_payload(template))
    for extra in schemas or []:
        schema.merge(Schema.from_payload(read_payload(extra)))
    own = Schema.from_payload(payload)
    for key, info in own.structs.items():
        if (info.raw[:0x10], info.raw[0x18:], info.rows) != (
            schema.structs[key].raw[:0x10],
            schema.structs[key].raw[0x18:],
            schema.structs[key].rows,
        ):
            raise SpecError(f"struct info {info.name} differs from the schema source")
    decoded = decode_payload(payload)
    diff = differences(canonical(spec, schema), canonical(decoded, schema))
    if diff:
        raise SpecError("readback differs from the spec: " + "; ".join(diff))
    problems = mlo_problems(decoded)
    if problems:
        raise SpecError("MLO is inconsistent: " + "; ".join(problems[:5]))
    again, _sizes = build_payload(decoded, schema, read_payload(template))
    if again != payload:
        raise SpecError("re-encoding the readback gives different bytes")
    return summary(decoded) | {"pages": sizes, "bytes": len(payload)}


def summary(spec: dict) -> dict:
    archetypes = spec.get("archetypes") or []
    mlos = [a for a in archetypes if a.get("type") == "CMloArchetypeDef"]
    return {
        "name": spec.get("name"),
        "archetypes": len(archetypes),
        "mlos": [
            {
                "name": m.get("name"),
                "entities": len(m.get("entities") or []),
                "rooms": len(m.get("rooms") or []),
                "portals": len(m.get("portals") or []),
                "entitySets": len(m.get("entitySets") or []),
                "setEntities": sum(len(s.get("entities") or []) for s in m.get("entitySets") or []),
                "timeCycleModifiers": len(m.get("timeCycleModifiers") or []),
            }
            for m in mlos
        ],
    }


# --------------------------------------------------------------------------- edits


def the_mlo(spec: dict) -> dict:
    mlos = [a for a in spec.get("archetypes") or [] if a.get("type") == "CMloArchetypeDef"]
    if len(mlos) != 1:
        raise SpecError(f"expected exactly one CMloArchetypeDef, found {len(mlos)}")
    return mlos[0]


def remove_entities(mlo: dict, indices: set[int]) -> list[dict]:
    """Drop MLO entities by index; room/portal attachedObjects follow (dropped or renumbered)."""
    entities = mlo.get("entities") or []
    if any(not 0 <= i < len(entities) for i in indices):
        raise SpecError(f"entity index out of range (the MLO has {len(entities)})")
    removed = [entities[i] for i in sorted(indices)]
    remap, kept = {}, []
    for i, entity in enumerate(entities):
        if i not in indices:
            remap[i] = len(kept)
            kept.append(entity)
    mlo["entities"] = kept
    for holder in [*(mlo.get("rooms") or []), *(mlo.get("portals") or [])]:
        holder["attachedObjects"] = [remap[int(o)] for o in holder.get("attachedObjects") or [] if int(o) in remap]
    return removed


def select_entities(mlo: dict, selectors: list[str]) -> set[int]:
    """INDEX or ARCHETYPE (name or 0xHASH: every MLO entity of it) -> entity indices."""
    entities = mlo.get("entities") or []
    out: set[int] = set()
    for selector in selectors:
        if re.fullmatch(r"\d+", selector):
            out.add(int(selector))
            continue
        wanted = hash_value(selector)
        hits = {i for i, e in enumerate(entities) if hash_value(e.get("archetypeName", "")) == wanted}
        if not hits:
            raise SpecError(f"no MLO entity of archetype {selector}")
        out |= hits
    return out


def edit(
    spec: dict,
    *,
    typ_name: str | None = None,
    mlo_name: str | None = None,
    mlo_only: bool = False,
    remove: list[str] | None = None,
    move: dict[int, tuple[float, float, float]] | None = None,
) -> tuple[dict, list[dict]]:
    """A changed copy of `spec` and the removed entities."""
    spec = json.loads(json.dumps(spec))
    mlo = the_mlo(spec)
    if typ_name:
        spec["name"] = typ_name
    if mlo_name:  # assetName is kept, as make_addon_mlo.py keeps it (an MLO is assetless)
        mlo["name"] = mlo_name
    if mlo_only:
        spec["archetypes"] = [mlo]
    for index, delta in (move or {}).items():
        entities = mlo.get("entities") or []
        if not 0 <= index < len(entities):
            raise SpecError(f"entity {index} out of range")
        position = _vector(entities[index].get("position", [0, 0, 0]), 3, "position")
        entities[index]["position"] = [p + d for p, d in zip(position, delta, strict=True)]
    removed = remove_entities(mlo, select_entities(mlo, remove)) if remove else []
    return spec, removed


# --------------------------------------------------------------------------- XML


def _xml_value(node: ET.Element):
    """A CodeWalker XML element as spec data (types are resolved later by the writer's schema)."""
    if any(k.endswith("}nil") and v == "true" for k, v in node.attrib.items()):
        return ""
    attrs = {k: v for k, v in node.attrib.items() if k not in ("type", "content")}
    if "value" in attrs:
        return _xml_number(attrs["value"])
    if set(attrs) in ({"x", "y", "z"}, {"x", "y", "z", "w"}):
        return [_xml_number(attrs[a]) for a in "xyzw" if a in attrs]
    children = list(node)
    if children:
        if any(child.tag != "Item" for child in children):
            raise SpecError(f"<{node.tag}> mixes Item and other children")
        items = []
        for child in children:
            if len(child) or child.get("type"):
                item = {c.tag: _xml_value(c) for c in child}
                if child.get("type"):
                    item = {"type": child.get("type"), **item}
                items.append(item)
            else:
                items.append(_xml_value(child))
        return items
    text = (node.text or "").strip()
    content = node.get("content")
    if content in ("int_array", "short_array", "char_array", "float_array", "vector3_array"):
        numbers = [_xml_number(v) for v in re.split(r"[\s,]+", text) if v]
        if content == "vector3_array":
            if len(numbers) % 3:
                raise SpecError(f"<{node.tag}>: vector3_array needs a multiple of three numbers")
            return [numbers[i : i + 3] for i in range(0, len(numbers), 3)]
        return numbers
    return text


def _xml_number(text: str):
    text = text.strip()
    if re.fullmatch(r"[-+]?\d+", text):
        return int(text)
    try:
        return float(text)
    except ValueError:
        raise SpecError(f"not a number: {text!r}") from None


def import_xml(data: bytes) -> dict:
    """A CodeWalker .ytyp.xml (CMapTypes) as a spec. Parsed without DTDs or entities."""
    try:
        root = parse_xml(data, Limits(max_metadata_bytes=MAX_XML_BYTES))
    except (AssetError, ET.ParseError, UnicodeError) as error:
        raise SpecError(f"XML refused: {error}") from None
    if root.tag != "CMapTypes":
        raise SpecError(f"root element is <{root.tag}>, not <CMapTypes>")
    spec = {"format": FORMAT}
    for child in root:
        spec[child.tag] = _xml_value(child)
    return spec


def export_xml(spec: dict) -> str:
    """A spec as CodeWalker-style .ytyp.xml (the shape import_xml reads)."""
    lines = ['<?xml version="1.0" encoding="UTF-8"?>', "<CMapTypes>"]

    def emit(name: str, value, indent: int, item_type: str | None = None):
        pad = "  " * indent
        attr = f' type="{item_type}"' if item_type else ""
        if isinstance(value, dict):
            lines.append(f"{pad}<{name}{attr}>")
            for key, sub in value.items():
                if key != "type":
                    emit(key, sub, indent + 1)
            lines.append(f"{pad}</{name}>")
        elif (
            isinstance(value, list)
            and value
            and all(isinstance(v, (int, float)) for v in value)
            and name
            in (
                "position",
                "rotation",
                "bbMin",
                "bbMax",
                "bsCentre",
                "sphere",
            )
        ):
            axes = " ".join(f'{a}="{v!r}"' for a, v in zip("xyzw", value, strict=False))
            lines.append(f"{pad}<{name} {axes} />")
        elif isinstance(value, list):
            if not value:
                lines.append(f"{pad}<{name} />")
                return
            if all(isinstance(v, int) for v in value):
                lines.append(f'{pad}<{name} content="int_array">{" ".join(str(v) for v in value)}</{name}>')
                return
            lines.append(f"{pad}<{name}{attr}>")
            for item in value:
                if isinstance(item, dict):
                    emit("Item", item, indent + 1, item.get("type"))
                elif isinstance(item, list):
                    axes = " ".join(f'{a}="{v!r}"' for a, v in zip("xyzw", item, strict=False))
                    lines.append(f"{pad}  <Item {axes} />")
                else:
                    lines.append(f"{pad}  <Item>{_xml_escape(str(item))}</Item>")
            lines.append(f"{pad}</{name}>")
        elif isinstance(value, (int, float)) and not isinstance(value, bool):
            lines.append(f'{pad}<{name} value="{value!r}" />')
        elif value in ("", None):
            lines.append(f"{pad}<{name} />")
        else:
            lines.append(f"{pad}<{name}>{_xml_escape(str(value))}</{name}>")

    for key, value in spec.items():
        if key != "format":
            emit(key, value, 1)
    lines.append("</CMapTypes>")
    return "\n".join(lines) + "\n"


def _xml_escape(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


# --------------------------------------------------------------------------- CLI


def load_names(paths: list[Path]) -> dict[int, str]:
    """joaat -> text from name/listing files (every token, its basename stem, `+x`/`hi@` stripped)."""
    known: dict[int, str] = {}
    for path in paths:
        for line in path.read_text(encoding="utf-8", errors="replace").split():
            for part in line.split("/"):
                stem = part.split(".")[0].lower()
                for text in {stem, stem.split("+")[0], stem.removeprefix("hi@")}:
                    if text and text.isascii() and text.replace("_", "a").replace("@", "a").isalnum():
                        known.setdefault(joaat(text), text)
    return known


def _load_spec(path: Path, names: dict[int, str]) -> dict:
    data = path.read_bytes()
    if data[:4] == b"RSC7":
        return decode(data, names)
    if data.lstrip()[:1] == b"<":
        return import_xml(data)
    return json.loads(data)


def _write(path: Path, data: bytes | str) -> None:
    if path.exists():
        raise SystemExit(f"refusing to overwrite {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode())


def _describe(info: dict) -> str:
    mlos = ", ".join(
        f"{m['name']}: entities={m['entities']} rooms={m['rooms']} portals={m['portals']} "
        f"sets={m['entitySets']}({m['setEntities']}) tcms={m['timeCycleModifiers']}"
        for m in info["mlos"]
    )
    pages = info.get("pages")
    extra = f" pages=[{', '.join(f'{s // 1024} KiB' for s in pages)}]" if pages else ""
    return f"typ={info['name']} archetypes={info['archetypes']} [{mlos}]{extra}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    names_args = argparse.ArgumentParser(add_help=False)
    names_args.add_argument("--names", type=Path, action="append", default=[], help="name list (any text)")
    names_args.add_argument("--listings", type=Path, help="directory of *.txt name listings")
    p = sub.add_parser("decode", parents=[names_args], help="retail or built .ptyp -> spec JSON")
    p.add_argument("source", type=Path)
    p.add_argument("--output", type=Path, required=True)
    p = sub.add_parser("import-xml", help="CodeWalker .ytyp.xml -> spec JSON")
    p.add_argument("source", type=Path)
    p.add_argument("--output", type=Path, required=True)
    p = sub.add_parser("import-pc", parents=[names_args], help="PC binary .ytyp (RSC7 meta) -> spec JSON")
    p.add_argument("source", type=Path)
    p.add_argument("--output", type=Path, required=True)
    p = sub.add_parser("export-xml", help="spec JSON -> CodeWalker-style .ytyp.xml")
    p.add_argument("spec", type=Path)
    p.add_argument("--output", type=Path, required=True)
    p = sub.add_parser("edit", help="rename, keep only the MLO, remove or move entities")
    p.add_argument("spec", type=Path)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--typ-name")
    p.add_argument("--mlo-name")
    p.add_argument("--mlo-only", action="store_true", help="define only the MLO (room props from another typ)")
    p.add_argument("--remove-entity", action="append", default=[], metavar="INDEX|ARCHETYPE")
    p.add_argument("--move-entity", action="append", default=[], metavar="INDEX=DX,DY,DZ")
    p = sub.add_parser("build", help="spec JSON -> PS5 .ptyp")
    p.add_argument("spec", type=Path)
    p.add_argument("--template", type=Path, required=True, help="loose RSC7 interior .ptyp from the user's game")
    p.add_argument("--schema", type=Path, action="append", default=[], help="more retail typs for struct infos")
    p.add_argument("--output", type=Path, required=True)
    p = sub.add_parser("compare", parents=[names_args], help="semantic difference of two typs/specs")
    p.add_argument("a", type=Path)
    p.add_argument("b", type=Path)
    p.add_argument("--template", type=Path, help="schema for JSON/XML inputs (default: a .ptyp input's own)")
    p = sub.add_parser("check", help="decode a .ptyp and check its MLO")
    p.add_argument("source", type=Path)
    args = parser.parse_args(argv)
    names: dict[int, str] = {}
    if getattr(args, "names", None) is not None:
        files = [*args.names, *(sorted(args.listings.glob("*.txt")) if args.listings else [])]
        names = load_names(files)
    try:
        if args.command in ("decode", "import-pc"):
            spec = decode(args.source.read_bytes(), names)
            _write(args.output, json.dumps(spec, indent=1) + "\n")
            problems = mlo_problems(spec)
            print(f"wrote {args.output} {_describe(summary(spec))}")
            for problem in problems:
                print(f"  inconsistent: {problem}")
        elif args.command == "import-xml":
            spec = import_xml(args.source.read_bytes())
            _write(args.output, json.dumps(spec, indent=1) + "\n")
            print(f"wrote {args.output} {_describe(summary(spec))}")
            for problem in mlo_problems(spec):
                print(f"  inconsistent: {problem}")
        elif args.command == "export-xml":
            _write(args.output, export_xml(json.loads(args.spec.read_text())))
            print(f"wrote {args.output}")
        elif args.command == "edit":
            moves = {}
            for row in args.move_entity:
                index, sep, delta = row.partition("=")
                values = delta.split(",")
                if not sep or not index.isdigit() or len(values) != 3:
                    raise SpecError(f"--move-entity wants INDEX=DX,DY,DZ, got {row!r}")
                moves[int(index)] = tuple(float(v) for v in values)
            spec, removed = edit(
                json.loads(args.spec.read_text()),
                typ_name=args.typ_name,
                mlo_name=args.mlo_name,
                mlo_only=args.mlo_only,
                remove=args.remove_entity,
                move=moves,
            )
            _write(args.output, json.dumps(spec, indent=1) + "\n")
            print(f"wrote {args.output} {_describe(summary(spec))}")
            for entity in removed:
                print(f"  removed {entity.get('archetypeName')} at {entity.get('position')}")
        elif args.command == "build":
            spec = json.loads(args.spec.read_text())
            schemas = [path.read_bytes() for path in args.schema]
            template = args.template.read_bytes()
            resource = build(spec, template, schemas)
            report = verify(resource, spec, template, schemas)
            _write(args.output, resource)
            print(f"wrote {args.output} bytes={len(resource)} {_describe(report)}")
        elif args.command == "compare":
            inputs = [args.a.read_bytes(), args.b.read_bytes()]
            schema_source = (
                args.template.read_bytes()
                if args.template
                else next((blob for blob in inputs if blob[:4] == b"RSC7"), None)
            )
            if schema_source is None:
                raise SpecError("compare needs a .ptyp input or --template for the schema")
            schema = Schema.from_payload(read_payload(schema_source))
            trees = [canonical(_load_spec(path, names), schema) for path in (args.a, args.b)]
            diff = differences(*trees, limit=50)
            print(f"{args.a} vs {args.b}: " + ("semantically identical" if not diff else f"{len(diff)}+ differences"))
            for line in diff:
                print(f"  {line}")
            return 1 if diff else 0
        elif args.command == "check":
            spec = decode(args.source.read_bytes())
            problems = mlo_problems(spec)
            print(f"{args.source}: {_describe(summary(spec))}")
            for problem in problems:
                print(f"  inconsistent: {problem}")
            return 1 if problems else 0
    except SpecError as error:
        raise SystemExit(f"make_mlo_ptyp: {error}") from None
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

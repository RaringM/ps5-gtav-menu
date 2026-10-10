"""Bounded readers for PC map placements: binary .ymap (RSC7 CMapData meta) and CodeWalker .ymap.xml.

Both return the same `Ymap`: every CEntityDef / CMloInstanceDef with its stored fields (archetype
hash, position, the raw stored rotation quaternion, scale, lodDist, flags, parentIndex, lodLevel,
priority, ...) and the map-level data a PS5 pack map cannot carry yet (physics dictionaries, car
generators, timecycle modifiers, occluders, LOD lights, instanced grass/props, container LODs),
reported as counts so a converter can list what it drops.

Inputs are third-party files: every size, count, pointer and block reference is checked before it
is read, nothing is executed or resolved outside the buffer, and XML goes through
`asset_metadata.parse_xml` (no DTD, no entities, node and depth limits). Field layout follows
CodeWalker MetaTypes.cs (CMapData 0x200 bytes, CEntityDef 0x80, CMloInstanceDef 0xA0); a binary
file whose embedded schema places a read member elsewhere is refused.

The rotation stays exactly as stored. In a ymap the CEntityDef quaternion of an ordinary entity is
the inverse of its world orientation (CodeWalker YmapFile.cs); the PS5 CEntityDef has the same
meaning, so a converter copies it verbatim.
"""

from __future__ import annotations

import math
import re
import struct
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field, replace

from .asset_formats import (
    AssetError,
    Limits,
    RpfMember,
    decode_resource,
    read_rpf_member,
    rpf_header,
    rpf_members,
)
from .asset_metadata import parse_xml
from .hashes import joaat
from .meta_resource import RESOURCE_BASE, joaat_cs

MAX_FILE_BYTES = 32 * 1024 * 1024
MAX_PAYLOAD_BYTES = 64 * 1024 * 1024
MAX_ENTITIES = 100000
MAX_BLOCKS = 65535
MAX_STRUCTS = 512
MAX_MEMBERS = 512
XML_LIMITS = Limits(max_metadata_bytes=MAX_FILE_BYTES, max_xml_nodes=4_000_000, max_xml_depth=32)

LOD_LEVELS = {
    "LODTYPES_DEPTH_HD": 0,
    "LODTYPES_DEPTH_LOD": 1,
    "LODTYPES_DEPTH_SLOD1": 2,
    "LODTYPES_DEPTH_SLOD2": 3,
    "LODTYPES_DEPTH_SLOD3": 4,
    "LODTYPES_DEPTH_ORPHANHD": 5,
    "LODTYPES_DEPTH_SLOD4": 6,
}
PRIORITIES = {"PRI_REQUIRED": 0, "PRI_OPTIONAL_HIGH": 1, "PRI_OPTIONAL_MEDIUM": 2, "PRI_OPTIONAL_LOW": 3}
LOD_LEVEL_NAMES = {v: k for k, v in LOD_LEVELS.items()}

# CMapData members this reader uses: name -> (offset, meta type); 0x52 array, 0x05 embedded struct.
MAP_MEMBERS = {
    "name": (0x08, 0x4A),
    "parent": (0x0C, 0x4A),
    "flags": (0x10, 0x15),
    "contentFlags": (0x14, 0x15),
    "streamingExtentsMin": (0x20, 0x33),
    "streamingExtentsMax": (0x30, 0x33),
    "entitiesExtentsMin": (0x40, 0x33),
    "entitiesExtentsMax": (0x50, 0x33),
    "entities": (0x60, 0x52),
    "containerLods": (0x70, 0x52),
    "boxOccluders": (0x80, 0x52),
    "occludeModels": (0x90, 0x52),
    "physicsDictionaries": (0xA0, 0x52),
    "instancedData": (0xB0, 0x05),
    "timeCycleModifiers": (0xE0, 0x52),
    "carGenerators": (0xF0, 0x52),
    "LODLightsSOA": (0x100, 0x05),
    "DistantLODLightsSOA": (0x188, 0x05),
}
ENTITY_MEMBERS = {
    "archetypeName": (0x08, 0x4A),
    "flags": (0x0C, 0x15),
    "guid": (0x10, 0x15),
    "position": (0x20, 0x33),
    "rotation": (0x30, 0x34),
    "scaleXY": (0x40, 0x21),
    "scaleZ": (0x44, 0x21),
    "parentIndex": (0x48, 0x14),
    "lodDist": (0x4C, 0x21),
    "childLodDist": (0x50, 0x21),
    "lodLevel": (0x54, 0x62),
    "numChildren": (0x58, 0x15),
    "priorityLevel": (0x5C, 0x62),
    "extensions": (0x60, 0x52),
    "ambientOcclusionMultiplier": (0x70, 0x14),
    "artificialAmbientOcclusion": (0x74, 0x14),
    "tintValue": (0x78, 0x15),
}
# CMloInstanceDef = CEntityDef + these interior members (CodeWalker MetaTypes.cs; the same on PS5).
MLO_MEMBERS = {
    "groupId": (0x80, 0x15),
    "floorId": (0x84, 0x15),
    "defaultEntitySets": (0x88, 0x52),
    "numExitPortals": (0x98, 0x15),
    "MLOInstflags": (0x9C, 0x15),
}
MAP_SIZE, ENTITY_SIZE, MLO_SIZE, CARGEN_SIZE = 0x200, 0x80, 0xA0, 0x50
POINTER_BLOCK = 7
# Counted (not converted) arrays inside embedded structs: (label, offset of the array in CMapData).
NESTED_ARRAYS = (
    ("instanced props", 0xB0 + 0x10),
    ("instanced grass batches", 0xB0 + 0x20),
    ("LOD lights", 0x100 + 0x08),
    ("distant LOD lights", 0x188 + 0x08),
)
SECTION_LABELS = {
    "containerLods": "container LODs",
    "boxOccluders": "box occluders",
    "occludeModels": "occlude models",
    "timeCycleModifiers": "timecycle modifiers",
}


def _hashes(name: str) -> tuple[int, int]:
    return joaat_cs(name), joaat(name)


@dataclass(frozen=True)
class Entity:
    archetype: int
    name: str | None
    mlo: bool
    flags: int
    guid: int
    position: tuple[float, float, float]
    rotation: tuple[float, float, float, float]
    scale_xy: float
    scale_z: float
    parent_index: int
    lod_dist: float
    child_lod_dist: float
    lod_level: int
    num_children: int
    priority: int
    extensions: int
    tint: int
    # CMloInstanceDef only (0 for a CEntityDef): the interior instance members.
    group_id: int = 0
    floor_id: int = 0
    default_entity_sets: int = 0  # count; the set names are not read
    num_exit_portals: int = 0
    mlo_flags: int = 0


@dataclass(frozen=True)
class CarGen:
    model: int
    model_name: str | None
    position: tuple[float, float, float]


@dataclass
class Ymap:
    source: str  # "binary" or "xml"
    name: str | None
    name_hash: int
    parent: int
    flags: int
    content_flags: int
    streaming_extents: tuple[tuple[float, ...], tuple[float, ...]]
    entities_extents: tuple[tuple[float, ...], tuple[float, ...]]
    entities: list[Entity] = field(default_factory=list)
    physics_dictionaries: list[int] = field(default_factory=list)
    car_generators: list[CarGen] = field(default_factory=list)
    sections: dict[str, int] = field(default_factory=dict)  # other non-empty map data: label -> count
    notes: list[str] = field(default_factory=list)


def _finite(values: tuple[float, ...], label: str) -> tuple[float, ...]:
    if not all(math.isfinite(v) and abs(v) <= 1e7 for v in values):
        raise AssetError(f"{label} is not finite or out of range")
    return values


# --- binary -------------------------------------------------------------------------------------


class _Payload:
    def __init__(self, payload: bytes):
        self.b = payload
        if len(payload) < 0x70 or struct.unpack_from("<I", payload, 0x10)[0] != 0x50524430:
            raise AssetError("not a parser meta resource (no PRD0 header)")
        self.n_struct, self.n_enum, self.n_block = struct.unpack_from("<HHH", payload, 0x48)
        if self.n_struct > MAX_STRUCTS or not 0 < self.n_block <= MAX_BLOCKS:
            raise AssetError("meta header counts exceed the reader limits")
        table = self.pointer(struct.unpack_from("<Q", payload, 0x30)[0], 16 * self.n_block, "block table")
        self.blocks = []
        for i in range(self.n_block):
            struct_hash, size, ptr = struct.unpack_from("<IIQ", payload, table + 16 * i)
            self.blocks.append((struct_hash, size, self.pointer(ptr, size, f"block {i + 1}")))
        self.structs: dict[int, tuple[int, dict[int, tuple[int, int]]]] = {}
        if self.n_struct:
            infos = self.pointer(struct.unpack_from("<Q", payload, 0x20)[0], 0x20 * self.n_struct, "struct infos")
            for i in range(self.n_struct):
                at = infos + 0x20 * i
                name = struct.unpack_from("<I", payload, at)[0]
                members_ptr, size, _pad, count = struct.unpack_from("<QIHH", payload, at + 0x10)
                if count > MAX_MEMBERS:
                    raise AssetError("struct info member count exceeds the reader limit")
                rows = self.pointer(members_ptr, 16 * count, "member rows") if count else 0
                members = {}
                for j in range(count):
                    mname, offset, kind = struct.unpack_from("<IIB", payload, rows + 16 * j)
                    members.setdefault(mname, (offset, kind))
                self.structs[name] = (size, members)

    def pointer(self, value: int, size: int, label: str) -> int:
        if value & 0xF0000000 != RESOURCE_BASE or value >> 32:
            raise AssetError(f"{label}: not a resource pointer")
        at = value - RESOURCE_BASE
        if size < 0 or at + size > len(self.b):
            raise AssetError(f"{label}: outside the resource")
        return at

    def ref(self, value: int, size: int, label: str, kinds: tuple[int, ...] | None = None) -> tuple[int, int]:
        """Block reference (offset << 12) | block -> (payload offset, block struct hash)."""
        block, offset = value & 0xFFF, value >> 12
        if not 0 < block <= len(self.blocks):
            raise AssetError(f"{label}: block reference {value:#x} names no block")
        struct_hash, block_size, at = self.blocks[block - 1]
        if kinds is not None and struct_hash not in kinds:
            raise AssetError(f"{label}: references a block of another type")
        if offset + size > block_size:
            raise AssetError(f"{label}: runs past the end of its block")
        return at + offset, struct_hash

    def array(self, at: int, element: int, label: str) -> tuple[int, int]:
        ref, count, _capacity = struct.unpack_from("<QHH", self.b, at)
        if not count:
            return 0, 0
        return self.ref(ref, count * element, label)[0], count

    def check_schema(
        self, names: tuple[int, int], expected: dict[str, tuple[int, int]], size: int, *, partial: bool = False
    ) -> bool:
        """True when the struct has schema rows; refuses any read member at another offset/type
        (`partial`: members the schema does not list are not required)."""
        info = self.structs.get(names[0]) or self.structs.get(names[1])
        if info is None:
            return False
        struct_size, members = info
        if struct_size != size:
            raise AssetError(f"embedded schema size {struct_size:#x} differs from {size:#x}")
        for member, (offset, kind) in expected.items():
            row = next((members[h] for h in _hashes(member) if h in members), None)
            if row is None and partial:
                continue
            if row is None or row[0] != offset or row[1] != kind:
                raise AssetError(f"embedded schema places {member} differently ({row})")
        return True


def _read_entity(meta: _Payload, at: int, mlo: bool) -> Entity:
    b = meta.b
    archetype, flags, guid = struct.unpack_from("<III", b, at + 0x08)
    position = _finite(struct.unpack_from("<3f", b, at + 0x20), "entity position")
    rotation = _finite(struct.unpack_from("<4f", b, at + 0x30), "entity rotation")
    scale_xy, scale_z, parent, lod, child_lod = struct.unpack_from("<ffiff", b, at + 0x40)
    _finite((scale_xy, scale_z, lod, child_lod), "entity scale/lodDist")
    lod_level, children, priority = struct.unpack_from("<III", b, at + 0x54)
    extensions = struct.unpack_from("<H", b, at + 0x68)[0]
    tint = struct.unpack_from("<I", b, at + 0x78)[0]
    interior = {}
    if mlo:
        group_id, floor_id = struct.unpack_from("<II", b, at + 0x80)
        sets = struct.unpack_from("<H", b, at + 0x90)[0]
        exits, mlo_flags = struct.unpack_from("<II", b, at + 0x98)
        interior = {
            "group_id": group_id,
            "floor_id": floor_id,
            "default_entity_sets": sets,
            "num_exit_portals": exits,
            "mlo_flags": mlo_flags,
        }
    return Entity(
        archetype,
        None,
        mlo,
        flags,
        guid,
        position,
        rotation,
        scale_xy,
        scale_z,
        parent,
        lod,
        child_lod,
        lod_level,
        children,
        priority,
        extensions,
        tint,
        **interior,
    )


def read_binary(data: bytes) -> Ymap:
    """PC binary .ymap: RSC7 v2, system pages only, a CMapData root."""
    if len(data) > MAX_FILE_BYTES:
        raise AssetError("ymap exceeds the reader size limit")
    header, payload = decode_resource(data, MAX_PAYLOAD_BYTES)
    if header["version"] != 2 or header["graphicsBytes"]:
        raise AssetError("ymap must be an RSC7 version 2 resource without graphics pages")
    meta = _Payload(payload)
    map_hashes, entity_hashes, mlo_hashes = _hashes("CMapData"), _hashes("CEntityDef"), _hashes("CMloInstanceDef")
    root_index = struct.unpack_from("<i", payload, 0x1C)[0]
    if not 0 < root_index <= len(meta.blocks):
        raise AssetError("root block index is out of range")
    root_hash, root_size, root = meta.blocks[root_index - 1]
    if root_hash not in map_hashes or root_size < MAP_SIZE:
        raise AssetError("root block is not a CMapData")
    notes = []
    if not meta.check_schema(map_hashes, MAP_MEMBERS, MAP_SIZE):
        notes.append("no embedded CMapData schema; CodeWalker layout assumed")
    meta.check_schema(entity_hashes, ENTITY_MEMBERS, ENTITY_SIZE)
    meta.check_schema(mlo_hashes, ENTITY_MEMBERS | MLO_MEMBERS, MLO_SIZE, partial=True)

    name_hash, parent, flags, content = struct.unpack_from("<IIII", payload, root + 0x08)
    extents = [_finite(struct.unpack_from("<3f", payload, root + o), "map extents") for o in (0x20, 0x30, 0x40, 0x50)]
    ymap = Ymap("binary", None, name_hash, parent, flags, content, (extents[0], extents[1]), (extents[2], extents[3]))
    ymap.notes += notes

    pointers, count = meta.array(root + 0x60, 8, "entities")
    if count > MAX_ENTITIES:
        raise AssetError("entity count exceeds the reader limit")
    sizes = {**dict.fromkeys(entity_hashes, ENTITY_SIZE), **dict.fromkeys(mlo_hashes, MLO_SIZE)}
    for i in range(count):
        value = struct.unpack_from("<Q", payload, pointers + 8 * i)[0]
        block = value & 0xFFF
        kind = meta.blocks[block - 1][0] if 0 < block <= len(meta.blocks) else None
        if kind not in sizes:
            raise AssetError(f"entity {i} does not reference a CEntityDef or CMloInstanceDef block")
        at, _ = meta.ref(value, sizes[kind], f"entity {i}")
        ymap.entities.append(_read_entity(meta, at, kind in mlo_hashes))

    at, count = meta.array(root + 0xA0, 4, "physicsDictionaries")
    ymap.physics_dictionaries = list(struct.unpack_from(f"<{count}I", payload, at)) if count else []

    cargen_hashes = _hashes("CCarGen")
    info = meta.structs.get(cargen_hashes[0]) or meta.structs.get(cargen_hashes[1])
    stride = info[0] if info else CARGEN_SIZE
    if not 0x20 <= stride <= 0x100:
        raise AssetError("CCarGen schema size is out of range")
    at, count = meta.array(root + 0xF0, stride, "carGenerators")
    for i in range(count):
        position = _finite(struct.unpack_from("<3f", payload, at + stride * i), "car generator position")
        model = struct.unpack_from("<I", payload, at + stride * i + 0x1C)[0]
        ymap.car_generators.append(CarGen(model, None, position))

    for member, label in SECTION_LABELS.items():
        count = struct.unpack_from("<H", payload, root + MAP_MEMBERS[member][0] + 8)[0]
        if count:
            ymap.sections[label] = count
    for label, offset in NESTED_ARRAYS:
        count = struct.unpack_from("<H", payload, root + offset + 8)[0]
        if count:
            ymap.sections[label] = count
    if struct.unpack_from("<I", payload, root + 0xB0 + 0x08)[0]:
        ymap.sections["instanced data link"] = 1
    return ymap


# --- CodeWalker XML -----------------------------------------------------------------------------

_FLOAT = re.compile(r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?\Z")
_INT = re.compile(r"[+-]?[0-9]{1,12}\Z")
_HASH_TEXT = re.compile(r"(?:hash_|0x)([0-9A-Fa-f]{1,8})\Z")
_NAME_TEXT = re.compile(r"[A-Za-z0-9_.\-]{1,96}\Z")
_NIL = "{http://www.w3.org/2001/XMLSchema-instance}nil"


def _one(parent: ET.Element, tag: str) -> ET.Element | None:
    found = parent.findall(tag)
    if len(found) > 1:
        raise AssetError(f"duplicate <{tag}> in <{parent.tag}>")
    return found[0] if found else None


def _xml_float(element: ET.Element | None, attribute: str = "value", default: float = 0.0) -> float:
    if element is None:
        return default
    text = element.get(attribute)
    if text is None or len(text) > 48 or not _FLOAT.fullmatch(text.strip()):
        raise AssetError(f"<{element.tag}> needs a numeric {attribute}")
    value = float(text)
    if not math.isfinite(value) or abs(value) > 1e7:
        raise AssetError(f"<{element.tag}> {attribute} is out of range")
    return value


def _xml_int(element: ET.Element | None, default: int = 0, low: int = -(1 << 31), high: int = 0xFFFFFFFF) -> int:
    if element is None:
        return default
    text = (element.get("value") or "").strip()
    if not _INT.fullmatch(text) or not low <= int(text) <= high:
        raise AssetError(f"<{element.tag}> needs an integer value in range")
    return int(text)


def _xml_vector(element: ET.Element | None, axes: str) -> tuple[float, ...]:
    if element is None:
        return tuple(0.0 for _ in axes)
    return tuple(_xml_float(element, axis) for axis in axes)


def _xml_hash(element: ET.Element | None) -> tuple[int, str | None]:
    """(hash, name or None) of a CodeWalker MetaHash text: a name, hash_XXXXXXXX, or empty."""
    if element is None or element.get(_NIL) == "true":
        return 0, None
    text = (element.text or "").strip()
    if not text:
        return 0, None
    match = _HASH_TEXT.fullmatch(text)
    if match:
        return int(match.group(1), 16), None
    if _INT.fullmatch(text) and 0 <= int(text) <= 0xFFFFFFFF:
        return int(text), None
    if not _NAME_TEXT.fullmatch(text):
        raise AssetError(f"<{element.tag}> is not a name or hash: {text[:40]!r}")
    return joaat(text), text.lower()


def _xml_enum(element: ET.Element | None, table: dict[str, int], default: int = 0) -> int:
    if element is None:
        return default
    text = (element.text or "").strip()
    if text in table:
        return table[text]
    if _INT.fullmatch(text) and 0 <= int(text) <= 255:
        return int(text)
    raise AssetError(f"<{element.tag}> has an unknown value {text[:40]!r}")


def _items(parent: ET.Element | None) -> list[ET.Element]:
    if parent is None:
        return []
    items = list(parent)
    if any(item.tag != "Item" for item in items):
        raise AssetError(f"<{parent.tag}> holds something other than <Item>")
    return items


def _xml_entity(item: ET.Element) -> Entity:
    kind = item.get("type", "CEntityDef")
    if kind not in ("CEntityDef", "CMloInstanceDef"):
        raise AssetError(f"unknown entity type {kind[:40]!r}")
    archetype, name = _xml_hash(_one(item, "archetypeName"))
    rotation = _xml_vector(_one(item, "rotation"), "xyzw")
    if rotation == (0.0, 0.0, 0.0, 0.0):
        rotation = (0.0, 0.0, 0.0, 1.0)
    return Entity(
        archetype,
        name,
        kind == "CMloInstanceDef",
        _xml_int(_one(item, "flags"), low=0) & 0xFFFFFFFF,
        _xml_int(_one(item, "guid"), low=0),
        _xml_vector(_one(item, "position"), "xyz"),  # type: ignore[arg-type]
        rotation,  # type: ignore[arg-type]
        _xml_float(_one(item, "scaleXY"), default=1.0),
        _xml_float(_one(item, "scaleZ"), default=1.0),
        _xml_int(_one(item, "parentIndex"), default=-1),
        _xml_float(_one(item, "lodDist")),
        _xml_float(_one(item, "childLodDist")),
        _xml_enum(_one(item, "lodLevel"), LOD_LEVELS),
        _xml_int(_one(item, "numChildren"), low=0),
        _xml_enum(_one(item, "priorityLevel"), PRIORITIES),
        len(_items(_one(item, "extensions"))),
        _xml_int(_one(item, "tintValue"), low=0),
        **(_xml_interior(item) if kind == "CMloInstanceDef" else {}),
    )


def _xml_id(element: ET.Element | None) -> int:
    """groupId/floorId: CodeWalker writes value="N"; a name or hash_ text is accepted too."""
    if element is not None and element.get("value") is not None:
        return _xml_int(element, low=0)
    return _xml_hash(element)[0]


def _xml_interior(item: ET.Element) -> dict:
    """The CMloInstanceDef members of a CodeWalker <Item type="CMloInstanceDef">."""
    return {
        "group_id": _xml_id(_one(item, "groupId")),
        "floor_id": _xml_id(_one(item, "floorId")),
        "default_entity_sets": len(_items(_one(item, "defaultEntitySets"))),
        "num_exit_portals": _xml_int(_one(item, "numExitPortals"), low=0),
        "mlo_flags": _xml_int(_one(item, "MLOInstflags"), low=0),
    }


def read_xml(data: bytes) -> Ymap:
    """CodeWalker .ymap.xml (root <CMapData>)."""
    if len(data) > MAX_FILE_BYTES:
        raise AssetError("ymap XML exceeds the reader size limit")
    try:
        root = parse_xml(data, XML_LIMITS)
    except (UnicodeError, ET.ParseError) as exc:
        raise AssetError(f"invalid ymap XML: {exc}") from exc
    if root.tag != "CMapData":
        raise AssetError("ymap XML root must be <CMapData>")
    name_hash, name = _xml_hash(_one(root, "name"))
    parent, _ = _xml_hash(_one(root, "parent"))
    ymap = Ymap(
        "xml",
        name,
        name_hash,
        parent,
        _xml_int(_one(root, "flags"), low=0),
        _xml_int(_one(root, "contentFlags"), low=0),
        (_xml_vector(_one(root, "streamingExtentsMin"), "xyz"), _xml_vector(_one(root, "streamingExtentsMax"), "xyz")),
        (_xml_vector(_one(root, "entitiesExtentsMin"), "xyz"), _xml_vector(_one(root, "entitiesExtentsMax"), "xyz")),
    )
    entities = _items(_one(root, "entities"))
    if len(entities) > MAX_ENTITIES:
        raise AssetError("entity count exceeds the reader limit")
    ymap.entities = [_xml_entity(item) for item in entities]
    for item in _items(_one(root, "physicsDictionaries")):
        value, _ = _xml_hash(item)
        ymap.physics_dictionaries.append(value)
    for item in _items(_one(root, "carGenerators")):
        model, model_name = _xml_hash(_one(item, "carModel"))
        position = _xml_vector(_one(item, "position"), "xyz")
        ymap.car_generators.append(CarGen(model, model_name, position))  # type: ignore[arg-type]
    for member, label in SECTION_LABELS.items():
        if count := len(_items(_one(root, member))):
            ymap.sections[label] = count
    instanced = _one(root, "instancedData")
    if instanced is not None:
        for tag, label in (("PropInstanceList", "instanced props"), ("GrassInstanceList", "instanced grass batches")):
            if count := len(_items(_one(instanced, tag))):
                ymap.sections[label] = count
        if _xml_hash(_one(instanced, "ImapLink"))[0]:
            ymap.sections["instanced data link"] = 1
    for tag, inner, label in (
        ("LODLightsSOA", "direction", "LOD lights"),
        ("LodLightsSoa", "direction", "LOD lights"),
        ("DistantLODLightsSOA", "position", "distant LOD lights"),
        ("DistantLodLightsSoa", "position", "distant LOD lights"),
    ):
        holder = _one(root, tag)
        if holder is not None and (count := len(_items(_one(holder, inner)))):
            ymap.sections[label] = count
    known = {
        "name", "parent", "flags", "contentFlags", "streamingExtentsMin", "streamingExtentsMax",
        "entitiesExtentsMin", "entitiesExtentsMax", "entities", "physicsDictionaries", "carGenerators",
        "instancedData", "LODLightsSOA", "DistantLODLightsSOA", "LodLightsSoa", "DistantLodLightsSoa", "block",
        *SECTION_LABELS,
    }  # fmt: skip
    for child in root:
        if child.tag not in known:
            ymap.notes.append(f"unknown CMapData field <{child.tag[:40]}> ignored")
    return ymap


def read_ymap(data: bytes) -> Ymap:
    """Binary (starts with RSC7) or CodeWalker XML (starts with '<', optionally after a BOM)."""
    if data[:4] == b"RSC7":
        return read_binary(data)
    if data.lstrip(b"\xef\xbb\xbf \t\r\n")[:1] == b"<":
        return read_xml(data)
    raise AssetError("not a binary ymap (RSC7) or a ymap XML")


# --- PC mod archives (dlc.rpf) -------------------------------------------------------------------

# Container bounds only (a nested map archive may be GBs); what is actually read is bounded by
# MOD_READ_BUDGET (all .ymap/.ytyp members together) and MAX_FILE_BYTES (each).
MOD_LIMITS = Limits(max_file_bytes=16 * 1024 * 1024 * 1024, max_entries=200000, max_depth=16)
MOD_READ_BUDGET = 512 * 1024 * 1024
MODEL_SUFFIXES = (".ydr", ".yft", ".ydd")
ARCHETYPE_KINDS = ("CBaseArchetypeDef", "CTimeArchetypeDef", "CMloArchetypeDef")


def label_name(name: str) -> str:
    """Looser name policy for archives whose names are only shown and suffix-matched (mods use
    '[', ']', '&'): printable ASCII, no separators, not '.' or '..'."""
    if not 0 < len(name) <= 256 or any(not 32 <= ord(c) <= 126 for c in name) or "\\" in name or "/" in name:
        raise AssetError(f"unusable archive member name: {name[:60]!r}")
    if name in (".", ".."):
        raise AssetError("relative archive member name")
    return name


@dataclass
class ModArchive:
    """What a PC mod archive ships: its ymaps and typs (raw bytes), the archetypes its typs define,
    the model names of its drawables/fragments, and members it could not read."""

    ymaps: dict[str, bytes] = field(default_factory=dict)
    typs: dict[str, bytes] = field(default_factory=dict)
    typ_archetypes: dict[int, str] = field(default_factory=dict)  # hash -> typ member path
    models: set[str] = field(default_factory=set)
    unreadable: list[str] = field(default_factory=list)


def typ_archetypes(data: bytes) -> list[int]:
    """Archetype name hashes of a PC binary .ytyp (CMapTypes meta)."""
    _header, payload = decode_resource(data, MAX_PAYLOAD_BYTES)
    meta = _Payload(payload)
    kinds = {h for name in ARCHETYPE_KINDS for h in _hashes(name)}
    out = []
    for struct_hash, size, at in meta.blocks:
        if struct_hash not in kinds:
            continue
        info = meta.structs.get(struct_hash)
        stride = info[0] if info else 0
        if stride < 0x60:
            raise AssetError("archetype struct without a usable schema size")
        out += [struct.unpack_from("<I", payload, base + 0x58)[0] for base in range(at, at + size - stride + 1, stride)]
    return out


def scan_mod_archive(source, budget: int = MOD_READ_BUDGET) -> ModArchive:
    """Walk a PC RPF7 (OPEN/NONE tables only) and its uncompressed nested archives.

    `source` is bytes or a read-only mmap: only the tables and the .ymap/.ytyp members are read,
    so a multi-GB archive is not loaded whole.
    """
    found = ModArchive()
    pending: list[tuple[int, int, str, int]] = [(0, len(source), "", 0)]
    read = 0
    while pending:
        base, size, prefix, depth = pending.pop()

        def read_at(offset: int, count: int, base: int = base, size: int = size) -> bytes:
            if offset < 0 or count < 0 or offset + count > size:
                raise AssetError("archive read outside its bounds")
            return bytes(source[base + offset : base + offset + count])

        try:
            table_end = rpf_header(read_at(0, 16), size)["tableEnd"]
            members: list[RpfMember] = rpf_members(
                read_at(0, table_end), MOD_LIMITS, file_size=size, read_at=read_at, name_check=label_name
            )
        except AssetError as exc:
            if not prefix:
                raise
            found.unreadable.append(f"{prefix.rstrip('/')} ({exc})")
            continue
        for member in members:
            path = prefix + member.name
            lower = member.name.lower()
            if lower.endswith(".rpf") and not member.resource:
                if member.compressed or member.encrypted or depth >= 4:
                    found.unreadable.append(f"{path} (nested archive is compressed, encrypted or too deep)")
                else:
                    pending.append((base + member.offset, member.size, path + "/", depth + 1))
                continue
            if lower.endswith(MODEL_SUFFIXES):
                found.models.add(lower.rsplit("/", 1)[-1].rsplit(".", 1)[0])
            if not lower.endswith((".ymap", ".ytyp")):
                continue
            if member.size > MAX_FILE_BYTES:
                found.unreadable.append(f"{path} (larger than {MAX_FILE_BYTES} bytes)")
                continue
            read += member.size
            if read > budget:
                raise AssetError("mod archive maps and typs exceed the reader budget")
            try:
                raw = read_rpf_member(read_at(member.offset, member.size), replace(member, offset=0), MAX_PAYLOAD_BYTES)
                if lower.endswith(".ymap"):
                    found.ymaps[path] = raw
                else:
                    for value in typ_archetypes(raw):
                        found.typ_archetypes.setdefault(value, path)
                    found.typs[path] = raw
            except AssetError as exc:
                found.unreadable.append(f"{path} ({exc})")
    return found

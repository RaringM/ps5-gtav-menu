#!/usr/bin/env python3
"""Move and rename an interior placement map (`*_interior_<mlo>_milo_.pmap`) by patching fields.

The input is a loose RSC7 v2 one-entity CMapData whose entity is a CMloInstanceDef, taken from the
user's own game (e.g. kt1_11_interior_v_gun2_milo_.pmap inside prosperol.rpf, extracted with its
16-byte RSC7 header); nothing of it ships with this tool. Patched:

- CMapData name (+0x08) = joaat(--name), parent (+0x0c) = 0, flags (+0x10) = 0; contentFlags kept
  (must have bit 8, the MLO bit the proxy creator tests);
- both extents (streaming +0x20/+0x30, entities +0x40/+0x50) shifted by the same offset as the
  instance (with --heading: the retail boxes rotated about the instance both ways and united, so
  they cover the MLO whichever sense the game gives the quaternion);
- CMloInstanceDef archetypeName (+0x08) = joaat(--mlo-name), position (+0x20) = --position
  (the MLO-local origin; the v_gun2 floor is at local z ~0); rotation kept, or with --heading DEG
  the retail rotation pre-multiplied by (0, 0, sin(DEG/2), cos(DEG/2)) (the sense of a
  CMloInstanceDef quaternion is not verified on PS5; 0 is the tested value);
- parentIndex (+0x48) = -1: with parent 0 the retail index into the parent map's LOD entities
  dangles (--keep-parent-index keeps it for an A/B);
- guid (+0x10) = joaat("<name>:0") so it does not repeat the retail placement's (--keep-guid);
- physicsDictionaries entries equal to the old MLO name -> the new MLO name (retail convention: the
  milo map names its interior's static bounds file); --physics-dictionary NAME sets every entry,
  `keep` leaves them as retail.

Every other byte stays: groupId, floorId, defaultEntitySets, numExitPortals, MLOInstflags, lodDist,
lodLevel, flags, AO, the parser schema and the page layout.

A PC map mod's own interior placement (--instance-ymap PC.ymap, a CodeWalker-written binary .ymap or
.ymap.xml read as untrusted data by gtavmenu_tools.ymap): the retail milo map is only the container
(schema, page layout, AO bytes). Its one CMloInstanceDef of --mlo-name supplies position, rotation
(copied verbatim: a CMloInstanceDef quaternion is the world rotation itself, unlike a CEntityDef's,
which is the inverse; the MalibuMansion shells only meet their exterior collision this way),
flags, lodDist, childLodDist, lodLevel, priorityLevel, groupId, floorId, numExitPortals and
MLOInstflags; the PC map supplies both extents (the proxy box is the entitiesExtents). Refused:
defaultEntitySets (not carried), a scale other than 1, --position or --heading alongside. The output is parsed back (schema and
block table unchanged, only the planned fields differ, entity inside entitiesExtents inside
streamingExtents) and written as RSC7 with the input's flags.

  make_addon_mlo_pmap.py kt1_11_interior_v_gun2_milo_.pmap --name gmmap_gun2 --mlo-name gm_gun2 \
      --position -90 -1792 28.0 --output gmmap_gun2.pmap

Exterior shell (ground placement): an MLO has no outside faces; retail puts it inside a building
entity of the area's HD map (v_gun2 at kt1_11: building `kt1_11_shop` in kt1_11_strm_0.pmap). With
--exterior-map RETAIL.pmap --exterior NAME [...] --exterior-typ TYP.ptyp [...] --exterior-spec OUT.json
the tool writes a tools/make_pmap.py spec that places the instance of each NAME nearest the retail
MLO (within 100 m) at the same offset from the moved MLO: position = --position + (retail entity -
retail MLO position), heading and scale from the retail entity, lodDist from the entity (else the
archetype), radius = |bsCentre| + bsRadius of the archetype in the --exterior-typ that defines it.
When one typ defines them all it prints the exterior map's stock dependency
(`--map-dep MAP.pmap=TYP.ptyp:retail`: the worker binds it like a retail map's manifest dependency,
the engine streams the typ with the map and the map creates the entities); the `--retail-typ` rows
(the worker requests the typs itself) stay the fallback and the route for several typs. Archetype
names stay retail (drawables are found by archetype
name). The moved MLO keeps the retail instance rotation (times --heading), so the shell moves with
it rigidly: its world offset from the MLO is turned by --heading only, never by the retail MLO
rotation. Requirements: the retail MLO rotation is identity or a half turn about Z (v_gun2 at ch3_03
stores (0, 0, -1, 0)) and every carried entity rotates about Z only; with --exterior, --heading must
be a multiple of 180 (the CEntityDef and CMloInstanceDef quaternion senses are not verified to agree,
and only half turns are sense-free). No collision is
carried for the shell: retail building collision is the area's world .pbn (one BVH per block, e.g.
kt1_11_0.pbn covers the whole block), so the shell is visual and the MLO's own .pbn is the only
collision.

  make_addon_mlo_pmap.py kt1_11_interior_v_gun2_milo_.pmap --name gmmap_gun2 --mlo-name gm_gun2 \
      --position -266 -1658 31.05 --output gmmap_gun2.pmap --exterior-map kt1_11_strm_0.pmap \
      --exterior kt1_11_shop --exterior kt1_11_emm01_a --exterior-typ koreatown_metadata_005_strm.ptyp \
      --exterior-name gmmap_gun2x --exterior-spec gmmap_gun2x.json
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` (an --instance-ymap is untrusted input) drops the script directory
    sys.path.insert(0, str(_HERE))

import argparse  # noqa: E402
import json  # noqa: E402
import math  # noqa: E402
import struct  # noqa: E402

from gtavmenu_tools.asset_formats import AssetError  # noqa: E402
from gtavmenu_tools.hashes import joaat  # noqa: E402
from gtavmenu_tools.meta_resource import Meta, joaat_cs, stored_deflate  # noqa: E402
from gtavmenu_tools.ymap import read_ymap  # noqa: E402
from make_addon_mlo import ARCH_NAME, MAP_TYPES, TYPES_ARCHETYPES, MloError, _array, read_resource  # noqa: E402
from make_pmap import page_sizes  # noqa: E402

MAP_DATA, MLO_INSTANCE = joaat_cs("CMapData"), joaat_cs("CMloInstanceDef")
POINTER_BLOCK = 7
# CMapData: name +0x08, parent +0x0c, flags +0x10, contentFlags +0x14, streamingExtents +0x20/+0x30,
# entitiesExtents +0x40/+0x50, entities +0x60, physicsDictionaries +0xa0 (hash array).
MAP_NAME, MAP_PARENT, MAP_FLAGS, MAP_CONTENT = 0x08, 0x0C, 0x10, 0x14
EXTENTS = (0x20, 0x30, 0x40, 0x50)
MAP_ENTITIES, MAP_PHYSICS = 0x60, 0xA0
CONTENT_MLO = 8
# CMloInstanceDef (CEntityDef 0x80 + interior fields): archetypeName +0x08, guid +0x10, position +0x20,
# rotation +0x30, parentIndex +0x48.
INST_ARCHETYPE, INST_GUID, INST_POSITION, INST_ROTATION, INST_PARENT = 0x08, 0x10, 0x20, 0x30, 0x48
# The fields an --instance-ymap placement copies besides position and rotation: offset -> struct format.
INST_FLAGS, INST_LOD, INST_CHILD_LOD, INST_LOD_LEVEL, INST_PRIORITY = 0x0C, 0x4C, 0x50, 0x54, 0x5C
INST_GROUP, INST_FLOOR, INST_SETS_COUNT, INST_EXITS, INST_MLO_FLAGS = 0x80, 0x84, 0x90, 0x98, 0x9C
INSTANCE_FIELDS = {
    "flags": (INST_FLAGS, "<I"),
    "lod_dist": (INST_LOD, "<f"),
    "child_lod_dist": (INST_CHILD_LOD, "<f"),
    "lod_level": (INST_LOD_LEVEL, "<I"),
    "priority": (INST_PRIORITY, "<I"),
    "group_id": (INST_GROUP, "<I"),
    "floor_id": (INST_FLOOR, "<I"),
    "num_exit_portals": (INST_EXITS, "<I"),
    "mlo_flags": (INST_MLO_FLAGS, "<I"),
}
# CEntityDef: scaleXY +0x40, scaleZ +0x44, lodDist +0x4c. CBaseArchetypeDef: lodDist +0x08,
# bsCentre +0x40, bsRadius +0x50.
ENTITY_SCALE, ENTITY_LOD = 0x40, 0x4C
ARCH_LOD, ARCH_BS_CENTRE, ARCH_BS_RADIUS = 0x08, 0x40, 0x50
EXTERIOR_REACH = 100.0  # metres: a carried entity must stand this close to the retail MLO


def _f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _instance(meta: Meta) -> tuple[int, int]:
    """(CMapData offset, CMloInstanceDef offset) of a one-entity milo map."""
    if not meta.blocks or meta.blocks[0][0] != MAP_DATA:
        raise MloError("block 1 is not CMapData")
    root = meta.blocks[0][2]
    ref, count, _cap = struct.unpack_from("<QHH", meta.b, root + MAP_ENTITIES)
    array = meta.ref(ref)
    if count != 1 or array is None or meta.blocks[(ref & 0xFFF) - 1][0] != POINTER_BLOCK:
        raise MloError(f"expected one entity through a pointer block, found {count}")
    entity_ref = meta.u64(array)
    entity = meta.ref(entity_ref)
    if entity is None or meta.blocks[(entity_ref & 0xFFF) - 1][0] != MLO_INSTANCE:
        raise MloError("the entity is not a CMloInstanceDef")
    return root, entity


def _physics(meta: Meta, root: int) -> list[int]:
    """Offsets of the physicsDictionaries hash entries."""
    ref, count, _cap = struct.unpack_from("<QHH", meta.b, root + MAP_PHYSICS)
    if not count:
        return []
    base = meta.ref(ref)
    if base is None:
        raise MloError("physicsDictionaries does not name a block")
    return [base + 4 * i for i in range(count)]


def heading_quaternion(heading: float) -> tuple[float, float, float, float]:
    """(x, y, z, w) of a rotation by `heading` degrees about +Z (make_pmap.py's convention)."""
    half = math.radians(heading) / 2.0
    return 0.0, 0.0, math.sin(half), math.cos(half)


def _qmul(a: tuple[float, ...], b: tuple[float, ...]) -> tuple[float, float, float, float]:
    """Hamilton product a * b of (x, y, z, w) quaternions."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def _rotated_box(lo: tuple[float, ...], hi: tuple[float, ...], heading: float) -> tuple[list[float], list[float]]:
    """AABB of a box (relative to the pivot) turned by +heading and by -heading about Z, united."""
    xs, ys = [], []
    for sense in (1.0, -1.0):
        c, s = math.cos(math.radians(sense * heading)), math.sin(math.radians(sense * heading))
        for x in (lo[0], hi[0]):
            for y in (lo[1], hi[1]):
                xs.append(c * x - s * y)
                ys.append(s * x + c * y)
    return [min(xs), min(ys), lo[2]], [max(xs), max(ys), hi[2]]


def pc_instance(ymap_bytes: bytes, mlo_name: str) -> dict:
    """The placement of `mlo_name` in a PC map mod's .ymap (binary or CodeWalker XML; untrusted):
    position, rotation, the INSTANCE_FIELDS values and the map's extents, for build(instance=...)."""
    try:
        data = read_ymap(ymap_bytes)
    except AssetError as error:
        raise MloError(f"instance ymap: {error}") from None
    found = [e for e in data.entities if e.mlo and e.archetype == joaat(mlo_name)]
    if len(found) != 1:
        raise MloError(f"instance ymap: expected one CMloInstanceDef of {mlo_name}, found {len(found)}")
    e = found[0]
    if not (math.isclose(e.scale_xy, 1.0, abs_tol=1e-4) and math.isclose(e.scale_z, 1.0, abs_tol=1e-4)):
        raise MloError(f"instance ymap: scale ({e.scale_xy}, {e.scale_z}) is not 1")
    if e.default_entity_sets:
        raise MloError("instance ymap: defaultEntitySets are not carried")
    if not math.isclose(math.sqrt(sum(v * v for v in e.rotation)), 1.0, abs_tol=1e-3):
        raise MloError("instance ymap: rotation is not a unit quaternion")
    return {
        "position": tuple(e.position),
        "rotation": tuple(e.rotation),
        "flags": e.flags,
        "lod_dist": e.lod_dist,
        "child_lod_dist": e.child_lod_dist,
        "lod_level": e.lod_level,
        "priority": e.priority,
        "group_id": e.group_id,
        "floor_id": e.floor_id,
        "num_exit_portals": e.num_exit_portals,
        "mlo_flags": e.mlo_flags,
        "streaming_extents": tuple(tuple(v) for v in data.streaming_extents),
        "entities_extents": tuple(tuple(v) for v in data.entities_extents),
    }


def build(
    resource: bytes,
    name: str,
    mlo_name: str,
    position: tuple[float, float, float] | None,
    physics: str | None = None,
    keep_parent_index: bool = False,
    keep_guid: bool = False,
    heading: float = 0.0,
    instance: dict | None = None,
) -> bytes:
    """Patched loose RSC7 .pmap; parsed back by verify() before it is returned. With `instance`
    (pc_instance()) the placement and both extents come from it and `position` must be None."""
    header, payload = read_resource(resource)
    meta = Meta(payload)
    root, entity = _instance(meta)
    content = struct.unpack_from("<I", payload, root + MAP_CONTENT)[0]
    if not content & CONTENT_MLO:
        raise MloError(f"contentFlags {content:#x} lack the MLO bit 8")
    if not math.isfinite(heading):
        raise MloError("heading must be finite")
    if instance is not None:
        if position is not None or heading % 360.0:
            raise MloError("an instance placement takes neither a position nor a heading")
        position = instance["position"]
        if struct.unpack_from("<H", payload, entity + INST_SETS_COUNT)[0]:
            raise MloError("the template instance has defaultEntitySets")
    if position is None:
        raise MloError("a position is required")
    old_mlo = struct.unpack_from("<I", payload, entity + INST_ARCHETYPE)[0]
    old_position = struct.unpack_from("<3f", payload, entity + INST_POSITION)
    shift = [_f32(p) - o for p, o in zip(position, old_position, strict=True)]
    out = bytearray(payload)
    struct.pack_into("<III", out, root + MAP_NAME, joaat(name), 0, 0)
    for at in EXTENTS:
        values = struct.unpack_from("<3f", payload, root + at)
        struct.pack_into("<3f", out, root + at, *(v + d for v, d in zip(values, shift, strict=True)))
    if heading % 360.0:
        rotation = struct.unpack_from("<4f", payload, entity + INST_ROTATION)
        struct.pack_into("<4f", out, entity + INST_ROTATION, *_qmul(heading_quaternion(heading), rotation))
        for lo_at, hi_at in ((0x20, 0x30), (0x40, 0x50)):
            lo = [v - o for v, o in zip(struct.unpack_from("<3f", payload, root + lo_at), old_position, strict=True)]
            hi = [v - o for v, o in zip(struct.unpack_from("<3f", payload, root + hi_at), old_position, strict=True)]
            new_lo, new_hi = _rotated_box(lo, hi, heading)
            struct.pack_into("<3f", out, root + lo_at, *(p + v for p, v in zip(position, new_lo, strict=True)))
            struct.pack_into("<3f", out, root + hi_at, *(p + v for p, v in zip(position, new_hi, strict=True)))
    if instance is not None:
        for at, values in zip(EXTENTS, (*instance["streaming_extents"], *instance["entities_extents"]), strict=True):
            struct.pack_into("<3f", out, root + at, *values)
        struct.pack_into("<4f", out, entity + INST_ROTATION, *instance["rotation"])
        for key, (at, fmt) in INSTANCE_FIELDS.items():
            struct.pack_into(fmt, out, entity + at, instance[key])
    struct.pack_into("<I", out, entity + INST_ARCHETYPE, joaat(mlo_name))
    struct.pack_into("<3f", out, entity + INST_POSITION, *position)
    if not keep_guid:
        struct.pack_into("<I", out, entity + INST_GUID, joaat(f"{name}:0"))
    if not keep_parent_index:
        struct.pack_into("<i", out, entity + INST_PARENT, -1)
    if physics != "keep":
        for at in _physics(meta, root):
            old = struct.unpack_from("<I", payload, at)[0]
            if physics is not None:
                struct.pack_into("<I", out, at, joaat(physics))
            elif old == old_mlo:
                struct.pack_into("<I", out, at, joaat(mlo_name))
    version, sys_flags, gfx_flags = header
    blob = struct.pack("<4sIII", b"RSC7", version, sys_flags, gfx_flags) + stored_deflate(bytes(out))
    verify(blob, resource, name, mlo_name, position, heading, instance)
    return blob


def verify(
    blob: bytes,
    original: bytes,
    name: str,
    mlo_name: str,
    position: tuple[float, float, float],
    heading: float = 0.0,
    instance: dict | None = None,
) -> dict:
    """Parse the patched map back against the retail one."""
    (version, sys_flags, gfx_flags), payload = read_resource(blob)
    old_header, old = read_resource(original)
    if (version, sys_flags, gfx_flags) != old_header or len(payload) != len(old):
        raise MloError("header flags or payload size changed")
    if gfx_flags & 0x0FFFFFFF or len(payload) != sum(page_sizes(sys_flags & 0x0FFFFFFF)):
        raise MloError("payload does not fill its system pages")
    meta, old_meta = Meta(payload), Meta(old)
    if meta.structs != old_meta.structs or meta.blocks != old_meta.blocks:
        raise MloError("struct schema or block table changed")
    root, entity = _instance(meta)
    turned = bool(heading % 360.0)
    allowed = {root + MAP_NAME, root + MAP_PARENT, root + MAP_FLAGS, entity + INST_ARCHETYPE}
    allowed |= {root + at + 4 * i for at in EXTENTS for i in range(3)}
    allowed |= {entity + INST_POSITION + 4 * i for i in range(3)}
    allowed |= {entity + INST_GUID, entity + INST_PARENT, *_physics(meta, root)}
    if turned or instance is not None:
        allowed |= {entity + INST_ROTATION + 4 * i for i in range(4)}
    if instance is not None:
        allowed |= {entity + at for at, _fmt in INSTANCE_FIELDS.values()}
    changed = {at & ~3 for at in range(len(old)) if old[at] != payload[at]}
    if not changed <= allowed:
        raise MloError(f"bytes changed outside the planned fields: {sorted(changed - allowed)[:4]}")
    map_name, parent, flags, content = struct.unpack_from("<4I", payload, root + MAP_NAME)
    if (map_name, parent, flags) != (joaat(name), 0, 0) or not content & CONTENT_MLO:
        raise MloError("CMapData name/parent/flags/contentFlags readback differs")
    if struct.unpack_from("<I", payload, entity + INST_ARCHETYPE)[0] != joaat(mlo_name):
        raise MloError("CMloInstanceDef archetypeName readback differs")
    where = struct.unpack_from("<3f", payload, entity + INST_POSITION)
    if any(abs(a - b) > 1e-3 for a, b in zip(where, position, strict=True)):
        raise MloError("instance position readback differs")
    rotation = struct.unpack_from("<4f", payload, entity + INST_ROTATION)
    if not math.isclose(math.sqrt(sum(v * v for v in rotation)), 1.0, abs_tol=1e-3):
        raise MloError("instance rotation is not a unit quaternion")
    old_root, old_entity = _instance(old_meta)
    smin, smax, emin, emax = (struct.unpack_from("<3f", payload, root + at) for at in EXTENTS)
    if instance is not None:
        if any(abs(a - b) > 1e-5 for a, b in zip(rotation, instance["rotation"], strict=True)):
            raise MloError("instance rotation readback differs from the PC placement")
        for key, (at, fmt) in INSTANCE_FIELDS.items():
            value = struct.unpack_from(fmt, payload, entity + at)[0]
            if not math.isclose(value, instance[key], rel_tol=1e-6, abs_tol=1e-5):
                raise MloError(f"instance {key} readback differs from the PC placement")
        expected = (*instance["streaming_extents"], *instance["entities_extents"])
        for got, want in zip((smin, smax, emin, emax), expected, strict=True):
            if any(abs(a - b) > 1e-3 for a, b in zip(got, want, strict=True)):
                raise MloError("extents readback differs from the PC map")
        if not all(s <= a <= p <= b <= t for s, a, p, b, t in zip(smin, emin, where, emax, smax, strict=True)):
            raise MloError("instance outside entitiesExtents or entitiesExtents outside streamingExtents")
        return _report(payload, meta, root, entity, content, rotation, changed, (smin, smax, emin, emax))
    if turned:
        expected = _qmul(heading_quaternion(heading), struct.unpack_from("<4f", old, old_entity + INST_ROTATION))
        if any(abs(a - b) > 1e-5 for a, b in zip(rotation, expected, strict=True)):
            raise MloError("instance rotation readback differs from heading * retail rotation")
    if not all(s <= a <= p <= b <= t for s, a, p, b, t in zip(smin, emin, where, emax, smax, strict=True)):
        raise MloError("instance outside entitiesExtents or entitiesExtents outside streamingExtents")
    old_where = struct.unpack_from("<3f", old, old_entity + INST_POSITION)
    old_extents = [struct.unpack_from("<3f", old, old_root + at) for at in EXTENTS]
    pairs = []
    for lo_index in (0, 2):
        lo = [v - o for v, o in zip(old_extents[lo_index], old_where, strict=True)]
        hi = [v - o for v, o in zip(old_extents[lo_index + 1], old_where, strict=True)]
        pairs += list(_rotated_box(lo, hi, heading)) if turned else [lo, hi]
    for new_box, expected_box in zip((smin, smax, emin, emax), pairs, strict=True):
        for axis in range(3):
            if abs((new_box[axis] - where[axis]) - expected_box[axis]) > 1e-2:
                raise MloError("extents did not move (and turn) with the instance")
    return _report(payload, meta, root, entity, content, rotation, changed, (smin, smax, emin, emax))


def _report(payload: bytes, meta: Meta, root: int, entity: int, content: int, rotation, changed, extents) -> dict:
    smin, smax, emin, emax = extents
    return {
        "content_flags": content,
        "rotation": rotation,
        "entities_extents": (emin, emax),
        "streaming_extents": (smin, smax),
        "parent_index": struct.unpack_from("<i", payload, entity + INST_PARENT)[0],
        "physics": [struct.unpack_from("<I", payload, at)[0] for at in _physics(meta, root)],
        "num_exit_portals": struct.unpack_from("<I", payload, entity + INST_EXITS)[0],
        "changed_words": len(changed),
    }


def _map_entities(payload: bytes) -> list[int]:
    """CEntityDef (or derived) offsets of any CMapData's entities array."""
    meta = Meta(payload)
    if not meta.blocks or meta.blocks[0][0] != MAP_DATA:
        raise MloError("exterior map: block 1 is not CMapData")
    return _array(meta, meta.blocks[0][2] + MAP_ENTITIES)


def _typ_archetypes(resource: bytes) -> dict[int, int]:
    """name hash -> archetype offset for any .ptyp (CMapTypes archetypes array)."""
    _header, payload = read_resource(resource)
    meta = Meta(payload)
    roots = [start for h, _size, start in meta.blocks if h == MAP_TYPES]
    if len(roots) != 1:
        raise MloError(f"exterior typ: expected one CMapTypes block, found {len(roots)}")
    return {
        struct.unpack_from("<I", payload, at + ARCH_NAME)[0]: at for at in _array(meta, roots[0] + TYPES_ARCHETYPES)
    }


def exterior_spec(
    milo: bytes,
    exterior: bytes,
    names: list[str],
    typs: dict[str, bytes],
    position: tuple[float, float, float],
    map_name: str,
    heading: float = 0.0,
) -> tuple[dict, list[str]]:
    """(make_pmap.py spec placing the retail exterior entities around the moved MLO, stock typs they need)."""
    if not names:
        raise MloError("no --exterior archetype named")
    turn = heading % 360.0
    if turn not in (0.0, 180.0):
        raise MloError(
            "with an exterior shell the heading must be a multiple of 180: the CEntityDef and"
            " CMloInstanceDef quaternion senses are not verified to agree"
        )
    _header, milo_payload = read_resource(milo)
    _root, instance = _instance(Meta(milo_payload))
    origin = struct.unpack_from("<3f", milo_payload, instance + INST_POSITION)
    mlo_rotation = struct.unpack_from("<4f", milo_payload, instance + INST_ROTATION)
    # Identity or a half turn about Z: both read the same in either quaternion sense. Nothing below
    # turns by this rotation; the moved MLO keeps it, and the shell keeps its world offset from it.
    if any(abs(v) > 1e-4 for v in mlo_rotation[:2]) or min(abs(mlo_rotation[2]), abs(mlo_rotation[3])) > 1e-4:
        raise MloError(
            f"retail MLO rotation {mlo_rotation} is not identity or a half turn about Z; relative placement"
            " unsupported"
        )
    _header, payload = read_resource(exterior)
    entities = _map_entities(payload)
    defined = {name: _typ_archetypes(blob) for name, blob in typs.items()}
    rows, needed = [], []
    for text in names:
        target = joaat(text)
        candidates = []
        for at in entities:
            if struct.unpack_from("<I", payload, at + INST_ARCHETYPE)[0] != target:
                continue
            where = struct.unpack_from("<3f", payload, at + INST_POSITION)
            candidates.append((math.dist(where, origin), at, where))
        if not candidates:
            raise MloError(f"exterior archetype {text} is not placed in the exterior map")
        distance, at, where = min(candidates)
        if distance > EXTERIOR_REACH:
            raise MloError(f"nearest {text} stands {distance:.1f} m from the retail MLO (> {EXTERIOR_REACH:.0f} m)")
        qx, qy, qz, qw = struct.unpack_from("<4f", payload, at + INST_ROTATION)
        if abs(qx) > 1e-3 or abs(qy) > 1e-3:
            raise MloError(f"exterior {text} does not rotate about Z only ({qx:.4f}, {qy:.4f})")
        scale_xy, scale_z = struct.unpack_from("<ff", payload, at + ENTITY_SCALE)
        if not math.isclose(scale_xy, scale_z, rel_tol=1e-4):
            raise MloError(f"exterior {text} has scaleXY {scale_xy} != scaleZ {scale_z}")
        owner = [name for name, archetypes in defined.items() if target in archetypes]
        if not owner:
            raise MloError(f"exterior archetype {text} is defined by no --exterior-typ")
        arch_at = defined[owner[0]][target]
        _h, typ_payload = read_resource(typs[owner[0]])
        centre = struct.unpack_from("<3f", typ_payload, arch_at + ARCH_BS_CENTRE)
        radius = math.hypot(*centre) + struct.unpack_from("<f", typ_payload, arch_at + ARCH_BS_RADIUS)[0]
        lod = struct.unpack_from("<f", payload, at + ENTITY_LOD)[0]
        if lod <= 0:
            lod = struct.unpack_from("<f", typ_payload, arch_at + ARCH_LOD)[0]
        offset = [w - o for w, o in zip(where, origin, strict=True)]
        if turn:
            offset[0], offset[1] = -offset[0], -offset[1]
        yaw = math.degrees(2.0 * math.atan2(qz, qw)) + turn
        rows.append(
            {
                "archetype": text,
                "position": [round(p + d, 4) for p, d in zip(position, offset, strict=True)],
                "heading": round((yaw + 180.0) % 360.0 - 180.0, 4),
                "lod": round(lod, 3),
                "radius": round(math.ceil(radius * 10.0) / 10.0, 1),
                "scale": round(scale_xy, 4),
            }
        )
        if owner[0] not in needed:
            needed.append(owner[0])
    return {"name": map_name, "entities": rows}, needed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path, help="loose RSC7 *_milo_.pmap from the user's own game")
    parser.add_argument("--name", required=True, help="new map name (also the pack member stem)")
    parser.add_argument("--mlo-name", required=True, help="new MLO archetype name (make_addon_mlo.py --mlo-name)")
    parser.add_argument("--position", nargs=3, type=float, metavar=("X", "Y", "Z"))
    parser.add_argument(
        "--instance-ymap", type=Path, help="PC map mod .ymap placing --mlo-name (instead of --position)"
    )
    parser.add_argument(
        "--heading", type=float, default=0.0, help="turn the instance about +Z (degrees; sense unverified)"
    )
    parser.add_argument("--physics-dictionary", help="set every physicsDictionaries entry (or 'keep')")
    parser.add_argument("--keep-parent-index", action="store_true", help="keep the retail LOD parentIndex")
    parser.add_argument("--keep-guid", action="store_true", help="keep the retail instance guid")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--exterior-map", type=Path, help="retail map holding the exterior shell (e.g. kt1_11_strm_0)")
    parser.add_argument("--exterior", action="append", default=[], metavar="ARCHETYPE", help="exterior entity to carry")
    parser.add_argument("--exterior-typ", type=Path, action="append", default=[], help="typ defining the exterior")
    parser.add_argument("--exterior-name", help="exterior map name (default: NAME + 'x')")
    parser.add_argument("--exterior-spec", type=Path, help="write the exterior make_pmap.py spec here")
    args = parser.parse_args(argv)
    if (args.position is None) == (args.instance_ymap is None):
        raise SystemExit("make_addon_mlo_pmap: give exactly one of --position and --instance-ymap")
    if args.instance_ymap and (args.heading or args.exterior_map):
        raise SystemExit("make_addon_mlo_pmap: --instance-ymap takes no --heading and no --exterior-map")
    position = tuple(args.position) if args.position else None
    if not all(math.isfinite(v) for v in (*(position or ()), args.heading)):
        raise SystemExit("make_addon_mlo_pmap: position and heading must be finite")
    exterior_args = (args.exterior_map, args.exterior, args.exterior_typ, args.exterior_spec)
    if any(exterior_args) and not all(exterior_args):
        raise SystemExit(
            "make_addon_mlo_pmap: --exterior-map, --exterior, --exterior-typ and --exterior-spec go together"
        )
    source = args.source.read_bytes()
    spec, needed = None, []
    try:
        instance = pc_instance(args.instance_ymap.read_bytes(), args.mlo_name) if args.instance_ymap else None
        blob = build(
            source,
            args.name,
            args.mlo_name,
            position,
            args.physics_dictionary,
            args.keep_parent_index,
            args.keep_guid,
            args.heading,
            instance,
        )
        if instance is not None:
            position = instance["position"]
        report = verify(blob, source, args.name, args.mlo_name, position, args.heading, instance)
        if args.exterior_map:
            typs = {path.name: path.read_bytes() for path in args.exterior_typ}
            spec, needed = exterior_spec(
                source,
                args.exterior_map.read_bytes(),
                [name.lower() for name in args.exterior],
                typs,
                position,
                args.exterior_name or f"{args.name}x",
                args.heading,
            )
    except MloError as error:
        raise SystemExit(f"make_addon_mlo_pmap: {error}") from None
    for path in (args.output, args.exterior_spec):
        if path is not None and path.exists():
            raise SystemExit(f"refusing to overwrite {path}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    lo, hi = report["entities_extents"]
    print(
        f"wrote {args.output} map={args.name} mlo={args.mlo_name} at ({position[0]:.2f},{position[1]:.2f},"
        f"{position[2]:.2f}) heading={args.heading:g} contentFlags={report['content_flags']:#x} "
        f"parentIndex={report['parent_index']} numExitPortals={report['num_exit_portals']} "
        f"physicsDictionaries={[f'{h:#010x}' for h in report['physics']]} "
        f"changed words={report['changed_words']}\n"
        f"  entitiesExtents ({lo[0]:.2f},{lo[1]:.2f},{lo[2]:.2f})-({hi[0]:.2f},{hi[1]:.2f},{hi[2]:.2f})"
    )
    if spec is not None:
        args.exterior_spec.parent.mkdir(parents=True, exist_ok=True)
        args.exterior_spec.write_text(json.dumps(spec, indent=1) + "\n")
        print(f"wrote {args.exterior_spec} map={spec['name']} entities={len(spec['entities'])}")
        for row in spec["entities"]:
            x, y, z = row["position"]
            print(
                f"  {row['archetype']} at ({x:.2f},{y:.2f},{z:.2f}) heading={row['heading']:g} lod={row['lod']:g}"
                f" radius={row['radius']:g}"
            )
        if len(needed) == 1:  # one inline map dependency
            print(f"  build_runtime_pack.py --map-dep {spec['name']}.pmap={needed[0]}:retail")
        label = "fallback without map dependency binding: " if len(needed) == 1 else "build_runtime_pack.py "
        print("  " + label + " ".join(f"--retail-typ {name}" for name in needed))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

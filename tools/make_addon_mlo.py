"""Clone an interior .ptyp's MLO under a new name by hash and pointer-entry patching only.

The input is a loose RSC7 v2 meta resource (fwMapTypes) taken from the user's own game, e.g.
v_int_22.ptyp (Ammu-Nation, MLO v_gun2); nothing of it ships with this tool.

Default (MLO-only): the output defines just the MLO, renamed to --mlo-name (the archetypes array
keeps one entry, the MLO's pointer, count 1; the other definitions stay in the file unreferenced),
and the CMapTypes name becomes --typ-name. Room entities keep their retail archetypeNames, so they
are the source typ's own archetypes: the clone depends on the source typ the way a retail interior
typ depends on its itypDependencies. The pack declares that with one `typdep` row
(`build_runtime_pack.py --typ-dep NEW.ptyp=SOURCE.ptyp`, printed below; the mapping JSON's
"typ_dep"): the worker binds it typ->typ before the clone's request and the engine streams the
source typ with the clone. The typ itself is not edited for it: the engine never reads
CMapTypes.dependencies (retail interior typs leave it empty; their dependencies come from
_manifest itypDependencies). The game finds an archetype's drawable by the archetype *name*
(assetName is not used for the lookup), so a renamed room archetype is invisible unless the pack
ships a drawable under the new name.

--rename-room-archetypes: every archetype `name` (+0x58 of CBaseArchetypeDef / CTimeArchetypeDef /
CMloArchetypeDef) becomes joaat(PREFIX + original name); the MLO becomes --mlo-name; every
CEntityDef that the MLO reaches (its room entities and its entity sets' entities) whose
archetypeName is one of the renamed archetypes follows the mapping; the CMapTypes name (+0x28)
becomes --typ-name. assetName, textureDictionary, physicsDictionary and every other field are kept,
which keeps the textures and physics but finds no drawable unless the pack ships .pdr members named
like the new names.

Room entities whose archetype lives in another typ (the source's itypDependencies, e.g. int_retail
or v_storage) keep their names; the report lists them.

Original name strings come from --names files and from the *.txt files of --listings DIR (any
text: every token, its basename stem, with a `+hi`-style suffix or `hi@` prefix stripped). A name
whose string is unknown is renamed to joaat(PREFIX + "<8 hex digits>").

No size or page changes: only u32 hash values (and, MLO-only, one pointer entry and the count). The
output is parsed back (same struct schema and block table, only the planned u32 offsets differ, no
old archetype name left, every renamed reference resolved), written as RSC7 with the input's flags
and a stored-deflate payload, and inflated again. Refuses to overwrite.

  make_addon_mlo.py v_int_22.ptyp --typ-name gm_int_22 --mlo-name gm_gun2 --output gm_int_22.ptyp \
      [--prefix gm_] [--names FILE ...] [--listings DIR] [--mapping mapping.json] [--deps-dir DIR] \
      [--rename-room-archetypes] [--room-timecycle INDEX=NAME ...]

--room-timecycle INDEX=NAME: write room INDEX's timecycleName (CMloRoomDef +0x44) as joaat(NAME), a
0xHASH, or 0 for `none`. Only modifiers the game already has tint the room; the rest of the room
stays as in the source.

--deps-dir names a directory of the source typ's dependency typs (the itypDependencies of its
_manifest, each a loose RSC7 .ptyp from the user's game). Every external room-entity archetype is
looked up there and an archetype found in none of them fails the build. The typs that define one
are written to the mapping JSON as "retail_typs" and printed as the `--retail-typ` fallback (the
worker requests those stock typs itself; the route before typ->typ binding). With the `typdep` row
only the source typ needs binding when its own dependencies are permanent typs, as v_int_22's nine
are (the worker logs dep_deps and the dependency's flags): the room-entity check looks one
dependency level down from the map, so a non-permanent dependency of the source still needs its
`--retail-typ` row.
"""

from __future__ import annotations

import argparse
import json
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

from gtavmenu_tools.hashes import joaat
from gtavmenu_tools.meta_resource import Meta, joaat_cs, stored_deflate

ARCHETYPE_STRUCTS = {joaat_cs(n): n for n in ("CBaseArchetypeDef", "CTimeArchetypeDef", "CMloArchetypeDef")}
MAP_TYPES, ENTITY_DEF = joaat_cs("CMapTypes"), joaat_cs("CEntityDef")
MLO_DEF, ENTITY_SET = joaat_cs("CMloArchetypeDef"), joaat_cs("CMloEntitySet")
ROOM_DEF = joaat_cs("CMloRoomDef")
POINTER_BLOCK = 7
# CMapTypes: archetypes +0x18, name +0x28. Archetype defs: name +0x58, txd +0x5c, physics +0x68,
# assetName +0x70, extensions +0x78. CMloArchetypeDef: entities +0x98, entitySets +0xc8.
# CEntityDef: archetypeName +0x08, extensions +0x60. CMloEntitySet (0x30): entities +0x20.
TYPES_ARCHETYPES, TYPES_NAME = 0x18, 0x28
ARCH_NAME, ARCH_TXD, ARCH_PHYSICS, ARCH_ASSET, ARCH_EXTENSIONS = 0x58, 0x5C, 0x68, 0x70, 0x78
MLO_ENTITIES, MLO_ENTITY_SETS, SET_ENTITIES, SET_SIZE = 0x98, 0xC8, 0x20, 0x30
ENTITY_ARCHETYPE, ENTITY_EXTENSIONS = 0x08, 0x60
# CMloArchetypeDef rooms +0xa8 (inline CMloRoomDef array, 0x70 each); CMloRoomDef timecycleName +0x44.
MLO_ROOMS, ROOM_SIZE, ROOM_TIMECYCLE = 0xA8, 0x70, 0x44


class MloError(ValueError):
    """The input is not an interior typ this tool can clone, or a readback check failed."""


@dataclass
class Archetype:
    at: int
    kind: str
    name: int
    asset: int


@dataclass
class Layout:
    """What the clone touches inside one inflated .ptyp payload."""

    root: int
    archetypes: list[Archetype]
    entities: list[int]  # CEntityDef offsets reached through the MLO(s), each once
    extensions: int = 0  # archetype + entity extension entries (kept as they are)
    sets: int = 0


@dataclass
class Clone:
    resource: bytes
    mapping: dict[int, tuple[str, str]]  # old hash -> (old text, new text)
    unknown: list[int] = field(default_factory=list)
    entity_renames: int = 0
    external: dict[str, int] = field(default_factory=dict)  # archetype text (or hex) -> references


def load_known(paths: list[Path]) -> dict[int, str]:
    """joaat -> text from listing/name files: every token, its basename stem, `+x` and `hi@` stripped."""
    known: dict[int, str] = {}
    for path in paths:
        for line in path.read_text(encoding="utf-8", errors="replace").split():
            for part in line.split("/"):
                stem = part.split(".")[0].lower()
                for text in {stem, stem.split("+")[0], stem.removeprefix("hi@")}:
                    if text and text.isascii() and text.replace("_", "a").replace("@", "a").isalnum():
                        known.setdefault(joaat(text), text)
    return known


def read_resource(resource: bytes) -> tuple[tuple[int, int, int], bytes]:
    """(version, system flags, graphics flags) and the inflated payload of a loose RSC7 resource."""
    if len(resource) < 16:
        raise MloError("too short for an RSC7 resource")
    magic, version, sys_flags, gfx_flags = struct.unpack_from("<4sIII", resource, 0)
    if magic != b"RSC7":
        raise MloError("not a loose RSC7 resource (extract the member with its 16-byte RSC7 header, --rsc7)")
    if version != 2 or ((sys_flags >> 28) << 4 | gfx_flags >> 28) != version:
        raise MloError(f"meta resources are version 2; header says {version} flags {sys_flags:#x}/{gfx_flags:#x}")
    stream = zlib.decompressobj(-15)
    payload = stream.decompress(resource[16:])
    if not stream.eof or stream.unused_data:
        raise MloError("payload does not raw-inflate to its end from offset 16")
    return (version, sys_flags, gfx_flags), payload


def _array(meta: Meta, at: int) -> list[int]:
    """Targets of a {u64 ref, u16 count, u16 cap} pointer array (refs -> pointer block -> structs)."""
    ref, count, _cap = struct.unpack_from("<QHH", meta.b, at)
    if not count:
        return []
    base = meta.ref(ref)
    block = ref & 0xFFF
    if base is None or meta.blocks[block - 1][0] != POINTER_BLOCK:
        raise MloError(f"array at {at:#x} does not name a pointer block")
    out = []
    for i in range(count):
        target = meta.ref(meta.u64(base + 8 * i))
        if target is None:
            raise MloError(f"array at {at:#x} entry {i} is not a block reference")
        out.append(target)
    return out


def _block_of(meta: Meta, at: int) -> int:
    for struct_hash, size, start in meta.blocks:
        if start <= at < start + size:
            return struct_hash
    raise MloError(f"offset {at:#x} lies in no data block")


def _count(meta: Meta, at: int) -> int:
    return struct.unpack_from("<H", meta.b, at + 8)[0]


def layout(payload: bytes) -> Layout:
    """Find the CMapTypes root, its archetypes, and every CEntityDef its MLO archetypes reach."""
    meta = Meta(payload)
    roots = [start for h, _size, start in meta.blocks if h == MAP_TYPES]
    if len(roots) != 1:
        raise MloError(f"expected one CMapTypes block, found {len(roots)}")
    result = Layout(roots[0], [], [])
    seen: set[int] = set()
    for at in _array(meta, roots[0] + TYPES_ARCHETYPES):
        struct_hash = _block_of(meta, at)
        if struct_hash not in ARCHETYPE_STRUCTS:
            raise MloError(f"archetype at {at:#x} is in a {struct_hash:#010x} block")
        name, asset = (
            struct.unpack_from("<I", payload, at + ARCH_NAME)[0],
            struct.unpack_from("<I", payload, at + ARCH_ASSET)[0],
        )
        result.archetypes.append(Archetype(at, ARCHETYPE_STRUCTS[struct_hash], name, asset))
        result.extensions += _count(meta, at + ARCH_EXTENSIONS)
        if struct_hash != MLO_DEF:
            continue
        entities = _array(meta, at + MLO_ENTITIES)
        ref, sets, _cap = struct.unpack_from("<QHH", payload, at + MLO_ENTITY_SETS)
        result.sets += sets
        for i in range(sets):
            set_at = meta.ref(ref)
            if set_at is None or _block_of(meta, set_at) != ENTITY_SET:
                raise MloError("entitySets does not name a CMloEntitySet block")
            entities += _array(meta, set_at + i * SET_SIZE + SET_ENTITIES)
        for entity in entities:
            if _block_of(meta, entity) != ENTITY_DEF:
                raise MloError(f"room entity at {entity:#x} is not in a CEntityDef block")
            if entity not in seen:
                seen.add(entity)
                result.entities.append(entity)
                result.extensions += _count(meta, entity + ENTITY_EXTENSIONS)
    slots = sum(size // meta.structs[h][0] for h, size, _ in meta.blocks if h == ENTITY_DEF)
    if slots != len(result.entities):
        raise MloError(f"{slots} CEntityDef slots but the MLO reaches {len(result.entities)}")
    if sum(a.kind == "CMloArchetypeDef" for a in result.archetypes) != 1:
        raise MloError("expected exactly one CMloArchetypeDef")
    return result


def mlo_rooms(payload: bytes) -> list[int]:
    """CMloRoomDef offsets of the typ's MLO, in room-index order (rooms are an inline struct array)."""
    meta = Meta(payload)
    found = layout(payload)
    mlo = next(a for a in found.archetypes if a.kind == "CMloArchetypeDef")
    size, rows = meta.structs.get(ROOM_DEF, (0, []))
    if size != ROOM_SIZE or not any(row[0] == joaat_cs("timecycleName") and row[1] == ROOM_TIMECYCLE for row in rows):
        raise MloError("CMloRoomDef schema differs (size 0x70, timecycleName +0x44 expected)")
    ref, count, _cap = struct.unpack_from("<QHH", payload, mlo.at + MLO_ROOMS)
    base = meta.ref(ref) if count else None
    if count and (base is None or _block_of(meta, base) != ROOM_DEF):
        raise MloError("MLO rooms do not name a CMloRoomDef block")
    return [base + ROOM_SIZE * i for i in range(count)] if base is not None else []


def clone(
    resource: bytes,
    typ_name: str,
    mlo_name: str | None = None,
    prefix: str = "gm_",
    known: dict[int, str] | None = None,
    mlo_only: bool = False,
    room_timecycles: dict[int, int] | None = None,
) -> Clone:
    """Renamed copy of an interior typ; parsed back by verify() before it is returned.

    mlo_only: define only the (renamed) MLO. The archetypes array keeps one entry, the MLO's, and
    every room entity keeps its retail archetypeName, so the room props are the source typ's own
    archetypes: the pack requests the source typ as a stock (`retail`) typ. A renamed room archetype
    has no drawable, because an archetype finds its drawable by its *name*.

    room_timecycles: room index -> timecycle modifier hash written to that CMloRoomDef's
    timecycleName (+0x44; 0 = none). Only modifier names the game already has take effect (a new
    name needs a TIMECYCLEMOD_FILE row; an unknown one only means no tint).
    """
    known = known or {}
    header, payload = read_resource(resource)
    found = layout(payload)
    mapping: dict[int, tuple[str, str]] = {}
    unknown: list[int] = []
    for arch in found.archetypes:
        text = known.get(arch.name)
        if text is None:
            unknown.append(arch.name)
        old_text = text if text is not None else f"{arch.name:08x}"
        new_text = mlo_name if arch.kind == "CMloArchetypeDef" and mlo_name else prefix + old_text
        if arch.name in mapping:
            raise MloError(f"archetype {old_text} is defined twice")
        mapping[arch.name] = (old_text, new_text.lower())
    if mlo_only:
        mlo = next(a for a in found.archetypes if a.kind == "CMloArchetypeDef")
        mapping = {mlo.name: mapping[mlo.name]}
        unknown = [name for name in unknown if name in mapping]
    new_hashes = [joaat(new) for _old, new in mapping.values()]
    if len(set(new_hashes)) != len(new_hashes) or set(new_hashes) & set(mapping):
        raise MloError("new names collide with each other or with an original name")

    out = bytearray(payload)
    patches: dict[int, int] = {found.root + TYPES_NAME: joaat(typ_name)}
    for arch in found.archetypes:
        if arch.name in mapping:
            patches[arch.at + ARCH_NAME] = joaat(mapping[arch.name][1])
    if mlo_only:
        # archetypes {u64 ref, u16 count, u16 cap}: entry 0 becomes the MLO's pointer, count 1.
        meta = Meta(payload)
        array_at = found.root + TYPES_ARCHETYPES
        ref, _count_was, cap = struct.unpack_from("<QHH", payload, array_at)
        entries = meta.ref(ref)
        index = found.archetypes.index(mlo)
        low, high = struct.unpack_from("<II", payload, entries + 8 * index)
        patches[entries] = low
        patches[entries + 4] = high
        patches[array_at + 8] = 1 | cap << 16
    renames, external = 0, {}
    for entity in found.entities:
        old = struct.unpack_from("<I", payload, entity + ENTITY_ARCHETYPE)[0]
        if old in mapping:
            patches[entity + ENTITY_ARCHETYPE] = joaat(mapping[old][1])
            renames += 1
        else:
            label = known.get(old, f"{old:#010x}")
            external[label] = external.get(label, 0) + 1
    if room_timecycles:
        rooms = mlo_rooms(payload)
        for index, value in room_timecycles.items():
            if not 0 <= index < len(rooms):
                raise MloError(f"room {index} out of range (the MLO has {len(rooms)} rooms)")
            patches[rooms[index] + ROOM_TIMECYCLE] = value
    for at, value in patches.items():
        struct.pack_into("<I", out, at, value)
    version, sys_flags, gfx_flags = header
    blob = struct.pack("<4sIII", b"RSC7", version, sys_flags, gfx_flags) + stored_deflate(bytes(out))
    result = Clone(blob, mapping, unknown, renames, external)
    verify(resource, result, typ_name, set(patches))
    return result


def verify(original: bytes, result: Clone, typ_name: str, patched: set[int] | None = None) -> dict:
    """Parse the clone back against the original: same schema, blocks and kept fields; names renamed."""
    old_header, old = read_resource(original)
    new_header, new = read_resource(result.resource)
    if old_header != new_header or len(old) != len(new):
        raise MloError("header flags or payload size changed")
    old_meta, new_meta = Meta(old), Meta(new)
    if old_meta.structs != new_meta.structs or old_meta.blocks != new_meta.blocks:
        raise MloError("struct schema or block table changed")
    changed = {at & ~3 for at in range(len(old)) if old[at] != new[at]}
    if patched is not None and not changed <= patched:
        raise MloError(f"bytes changed outside the planned hash fields: {sorted(changed - patched)[:4]}")
    before, after = layout(old), layout(new)
    if struct.unpack_from("<I", new, after.root + TYPES_NAME)[0] != joaat(typ_name):
        raise MloError("CMapTypes name readback differs")
    renamed = {old_hash: joaat(new_text) for old_hash, (_t, new_text) in result.mapping.items()}
    by_at = {a.at: a for a in before.archetypes}
    if len(after.archetypes) != len(before.archetypes) and [b.kind for b in after.archetypes] != ["CMloArchetypeDef"]:
        raise MloError("archetype list changed (only an MLO-only clone keeps just the MLO)")
    for b in after.archetypes:
        a = by_at[b.at]
        if b.name != renamed[a.name] or b.asset != a.asset:
            raise MloError(f"archetype at {a.at:#x}: name not renamed or assetName changed")
        for field_at in (ARCH_TXD, ARCH_PHYSICS):
            if old[a.at + field_at : a.at + field_at + 4] != new[b.at + field_at : b.at + field_at + 4]:
                raise MloError(f"archetype at {a.at:#x}: txd/physicsDictionary changed")
    names = {b.name for b in after.archetypes}
    if names & set(renamed):
        raise MloError("an original archetype name is still defined")
    for a, b in zip(before.entities, after.entities, strict=True):
        was, now = (struct.unpack_from("<I", p, at + ENTITY_ARCHETYPE)[0] for p, at in ((old, a), (new, b)))
        if now != renamed.get(was, was) or now in renamed:
            raise MloError(f"room entity at {b:#x}: archetypeName readback differs")
    return {"archetypes": len(after.archetypes), "entities": len(after.entities), "changed_words": len(changed)}


def archetype_names(resource: bytes) -> list[int]:
    """Archetype name hashes defined by any .ptyp (CMapTypes archetypes, MLO or not)."""
    _header, payload = read_resource(resource)
    meta = Meta(payload)
    roots = [start for h, _size, start in meta.blocks if h == MAP_TYPES]
    if len(roots) != 1:
        raise MloError(f"expected one CMapTypes block, found {len(roots)}")
    return [struct.unpack_from("<I", payload, at + ARCH_NAME)[0] for at in _array(meta, roots[0] + TYPES_ARCHETYPES)]


def resolve_dependencies(external: dict[str, int], deps: dict[str, bytes]) -> tuple[list[str], list[str]]:
    """(dependency typ names that define an external archetype, external archetypes found nowhere).

    `external` keys are archetype texts (or "0x%08x" hashes) as clone() reports them; `deps` maps a
    typ file name to its loose RSC7 bytes.
    """
    wanted = {(int(k, 16) if k.startswith("0x") else joaat(k)): k for k in external}
    needed: list[str] = []
    found: set[int] = set()
    for name, blob in sorted(deps.items()):
        hits = wanted.keys() & set(archetype_names(blob))
        if hits:
            needed.append(name)
            found |= hits
    return needed, sorted(wanted[h] for h in wanted.keys() - found)


def parse_room_timecycles(rows: list[str]) -> dict[int, int]:
    """`INDEX=NAME` rows -> {room index: timecycle hash}; NAME is a modifier name, 0xHASH or none."""
    out: dict[int, int] = {}
    for row in rows:
        index, sep, name = row.partition("=")
        if not sep or not index.strip().isdigit() or not name.strip():
            raise ValueError(f"--room-timecycle wants INDEX=NAME, got {row!r}")
        text = name.strip()
        if text.lower() == "none":
            value = 0
        elif text.lower().startswith("0x"):
            value = int(text, 16)
        else:
            value = joaat(text)
        if not 0 <= value <= 0xFFFFFFFF or int(index) in out:
            raise ValueError(f"--room-timecycle {row!r}: hash out of range or room repeated")
        out[int(index)] = value
    return out


def main(argv: list[str] | None = None, *, default_listings: Path | None = None) -> int:
    """`default_listings` (a developer default, never set by this tool) is used when --listings is absent."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path, help="loose RSC7 interior .ptyp from the user's own game")
    parser.add_argument("--typ-name", required=True, help="new CMapTypes name (e.g. gm_int_22)")
    parser.add_argument("--mlo-name", help="new MLO archetype name (default: PREFIX + original)")
    parser.add_argument("--prefix", default="gm_", help="prefix for every other archetype name (default gm_)")
    parser.add_argument("--names", type=Path, action="append", default=[], help="extra name list (any text)")
    parser.add_argument(
        "--listings", type=Path, help="directory of *.txt name listings (e.g. of the user's own retail archives)"
    )
    parser.add_argument("--mapping", type=Path, help="write the old -> new name mapping as JSON")
    parser.add_argument("--deps-dir", type=Path, help="the source typ's dependency typs (*.ptyp, loose RSC7)")
    parser.add_argument(
        "--rename-room-archetypes",
        action="store_true",
        help="define every room archetype under PREFIX + name (they have no drawable unless the pack"
        " ships .pdr members named like them); default: define only the MLO and use the source typ's"
        " room archetypes",
    )
    parser.add_argument(
        "--room-timecycle",
        action="append",
        default=[],
        metavar="INDEX=NAME",
        help="set room INDEX's timecycleName (a retail modifier name, 0xHASH, or none)",
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    listings = args.listings or default_listings
    name_files = [*args.names, *(sorted(listings.glob("*.txt")) if listings else [])]
    try:
        timecycles = parse_room_timecycles(args.room_timecycle)
    except ValueError as error:
        raise SystemExit(f"make_addon_mlo: {error}") from None
    try:
        result = clone(
            args.source.read_bytes(),
            args.typ_name,
            args.mlo_name,
            args.prefix,
            load_known(name_files),
            mlo_only=not args.rename_room_archetypes,
            room_timecycles=timecycles,
        )
    except MloError as error:
        raise SystemExit(f"make_addon_mlo: {error}") from None
    retail: list[str] = []
    # The stock typ the clone depends on: the source file is named like its typ (<name>.ptyp).
    typ_dep = args.source.name if not args.rename_room_archetypes and args.source.suffix == ".ptyp" else None
    if args.deps_dir or not args.rename_room_archetypes:
        deps = {path.name: path.read_bytes() for path in sorted(args.deps_dir.glob("*.ptyp"))} if args.deps_dir else {}
        if not args.rename_room_archetypes:
            deps[args.source.name] = args.source.read_bytes()  # the room archetypes stay the source's
        try:
            retail, missing = resolve_dependencies(result.external, deps)
        except MloError as error:
            raise SystemExit(f"make_addon_mlo: dependency typ: {error}") from None
        if missing and args.deps_dir:
            raise SystemExit(f"make_addon_mlo: external archetypes in no --deps-dir typ: {', '.join(missing)}")
    if args.output.exists():
        raise SystemExit(f"refusing to overwrite {args.output}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(result.resource)
    summary = verify(args.source.read_bytes(), result, args.typ_name)
    if args.mapping:
        rows = {
            f"{h:#010x}": {"old": o, "new": n, "new_hash": f"{joaat(n):#010x}"} for h, (o, n) in result.mapping.items()
        }
        args.mapping.write_text(
            json.dumps(
                {
                    "typ": args.typ_name,
                    "archetypes": rows,
                    "external": result.external,
                    "typ_dep": typ_dep,
                    "retail_typs": retail,
                },
                indent=1,
            )
            + "\n"
        )
    print(
        f"wrote {args.output} typ={args.typ_name} archetypes renamed={len(result.mapping)} "
        f"(unknown strings {len(result.unknown)}) room entities={summary['entities']} "
        f"renamed refs={result.entity_renames} external refs={sum(result.external.values())} "
        f"({len(result.external)} archetypes) changed words={summary['changed_words']}"
    )
    for old_hash in result.unknown:
        print(f"  unknown name {old_hash:#010x} -> {result.mapping[old_hash][1]}")
    if result.external:
        print("  external archetypes (need their own typ resident): " + ", ".join(sorted(result.external)))
    if typ_dep:
        print(f"  build_runtime_pack.py --typ {args.typ_name}.ptyp --typ-dep {args.typ_name}.ptyp={typ_dep}")
    if retail:
        print("  fallback without typ->typ binding: " + " ".join(f"--retail-typ {name}" for name in retail))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

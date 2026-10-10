#!/usr/bin/env python3
"""Convert a PC map mod's placements into PS5 pack maps (and, with --pack-id, a whole pack).

For mods that place EXISTING game archetypes (props and buildings already in the game). Inputs, any
mix: a binary .ymap, a CodeWalker .ymap.xml, or the mod's PC archive (dlc.rpf: its .ymap members are
converted, its .ytyp archetypes and .ydr/.yft/.ydd names mark the mod's own models; the same files
loose, from an unpacked mod folder, mark them too). Every input is
read as untrusted data with fixed limits (gtavmenu_tools.ymap). Run it with `python3 -I`, never from
inside a mod directory; this script puts only its own directory on sys.path.

Each entity's archetype is classified with an archetype index of the user's own game (JSON written
by tools/index_game_archetypes.py from the retail .ptyp files; schema "gtavmenu-archetype-index-v1": "archetypes"
{"0x<hash>": [[typ, lodDist, bbMin, bbMax, flags, kind], ...]}, optional "names" {"0x<hash>": name}):
  retail   defined by a retail typ: converted;
  pack     one of the mod's own models already converted for this pack (--pack-archetypes, a table
           written by a developer model converter): converted, and the pack carries their
           .pdr/.ptd members and their typ (one `typ` row);
  custom   defined or modelled by the mod itself and not converted: dropped, listed;
  unknown  neither (another mod's model, or an index from another game build): dropped, listed.
--keep-unresolved keeps custom/unknown entities (they draw nothing unless another pack supplies
the archetype).

Per entity: archetype hash, position, the stored rotation quaternion, scaleXY/scaleZ, flags (minus
the LOD-hierarchy bits 0x8/0x10/0x40), tintValue and lodDist (a lodDist <= 0 takes the archetype's)
are kept; the archetype's bbMin/bbMax from the index sizes the map extents. Pack maps are ORPHANHD
only (tools/make_pmap.py), so an HD entity's LOD parent link is dropped (it stays, drawn to its own
lodDist), and LOD/SLOD entities, MLO instances (CMloInstanceDef and MLO archetypes), entity
extensions, priorities other than REQUIRED, car generators, physics dictionaries, timecycle
modifiers, occluders, LOD lights, instanced grass/props and container LODs are dropped and listed.

The retail typs that define the kept archetypes become `typ <name>.ptyp retail` rows, most used
first, at most --max-retail-typs (default 8, pack limit 16). The PS5 manifests that decide which
typs are permanent are encrypted, so the index classes typs by path (resident: base-game prop typs,
seen resident on hardware; streamed: map-area and interior typs; unknown: DLC typs). --typ-rows
needed (default) emits rows for the typs not classed resident; all / none. A row for a resident typ
costs nothing; a non-resident one is requested with 1024 archetypes of pool headroom; a typ the game
does not have stops the pack load ("stock typ"). Entities of typs without a row stay in the map and
appear only if their typ is resident (warned past the limit).

  convert_ymap.py mod/dlc.rpf --archetype-index INDEX.json --template TEMPLATE.pmap --out build/x \\
      --prefix gmymap_garage --pack-id gtavmenu-ymap-garage-v1 --label "Franklin garage"
  convert_ymap.py a.ymap b.ymap.xml --archetype-index INDEX.json --out build/x   (specs + report only)

--out receives one make_pmap spec per map (<map>.json), the .pmap files (with --template; the
one-entity .pmap from the user's own game that make_pmap needs) and report.json. With --pack-id the
pack is built by tools/build_runtime_pack.py under --output-root and checked by
tools/validate_runtime_pack.py. Each converted source map gets one Custom Packs teleport row on its
first map row (--place X,Y,Z[:TEXT] per source overrides the default: just above the kept entity
nearest the source's centre). A source above --max-entities is split by area into several maps.

--translate DX,DY,DZ moves every kept entity by that offset (a slice of a mod placed elsewhere, e.g.
out of an overlap with the stock map): the entity positions move, the archetype boxes are local to
the entity and stay, so the map's entity and streaming extents follow. --place points are given in
the moved coordinates. Rotations are unchanged (translation only).
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

import argparse  # noqa: E402
import json  # noqa: E402
import math  # noqa: E402
import mmap  # noqa: E402
import re  # noqa: E402
from collections import Counter  # noqa: E402
from dataclasses import dataclass, field  # noqa: E402

from gtavmenu_tools.asset_formats import AssetError  # noqa: E402
from gtavmenu_tools.hashes import joaat  # noqa: E402
from gtavmenu_tools.ymap import (  # noqa: E402
    LOD_LEVEL_NAMES,
    MODEL_SUFFIXES,
    Entity,
    ModArchive,
    Ymap,
    read_ymap,
    scan_mod_archive,
    typ_archetypes,
)

INDEX_SCHEMA = "gtavmenu-archetype-index-v1"
PACK_ARCHETYPES_SCHEMA = "gtavmenu-pack-archetypes-v1"
MAX_PACK_MEMBERS = 512
MAX_INDEX_BYTES = 256 * 1024 * 1024
MAX_INPUT_BYTES = 32 * 1024 * 1024
PACK_TYP_MAX = 16  # GTAV_CUSTOM_PACK_TYP_MAX
PACK_MAP_MAX = 8  # GTAV_CUSTOM_PACK_MAP_MAX
MAX_MAP_ENTITIES = 4096  # tools/make_pmap.py MAX_PAGED_ENTITIES
DROPPED_FLAGS = 0x8 | 0x10 | 0x40  # LOD_IN_PARENT_MAP, LOD_ADOPTME, IS_INTERIOR_LOD
DEFAULT_LOD = 150.0
MAX_LOD = 16000.0
MAX_TRANSLATE = 100000.0
_PREFIX = re.compile(r"[a-z][a-z0-9_]{0,40}\Z")
_NAME = re.compile(r"[a-z0-9_]{1,63}\Z")


class ConvertError(ValueError):
    """An input, the index or an option is outside what this converter supports."""


@dataclass
class Index:
    archetypes: dict[int, list[list]]
    names: dict[int, str]
    residency: dict[str, str] = field(default_factory=dict)  # typ -> resident|streamed|unknown

    @classmethod
    def load(cls, path: Path | None) -> Index:
        if path is None:
            return cls({}, {})
        if path.stat().st_size > MAX_INDEX_BYTES:
            raise ConvertError(f"{path}: archetype index exceeds {MAX_INDEX_BYTES} bytes")
        data = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(data, dict) or data.get("schema") != INDEX_SCHEMA:
            raise ConvertError(f"{path}: not a {INDEX_SCHEMA} archetype index")
        archetypes = {int(k, 16): v for k, v in data.get("archetypes", {}).items()}
        names = {int(k, 16): str(v) for k, v in data.get("names", {}).items() if joaat(str(v)) == int(k, 16)}
        residency = {str(k): str(v.get("residency", "unknown")) for k, v in data.get("typs", {}).items()}
        return cls(archetypes, names, residency)


@dataclass
class PackArchetypes:
    """The mod's own archetypes converted for this pack: index-shaped rows, the pack typ and the
    member files (name -> path), from a --pack-archetypes JSON file."""

    typ: str
    archetypes: dict[int, list[list]]
    names: dict[int, str]
    members: dict[str, Path]

    @classmethod
    def load(cls, path: Path) -> PackArchetypes:
        if path.stat().st_size > MAX_INDEX_BYTES:
            raise ConvertError(f"{path}: pack archetype table is too large")
        data = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(data, dict) or data.get("schema") != PACK_ARCHETYPES_SCHEMA:
            raise ConvertError(f"{path}: not a {PACK_ARCHETYPES_SCHEMA} table")
        typ = str(data.get("typ", ""))
        if not _NAME.fullmatch(typ):
            raise ConvertError(f"{path}: typ name {typ!r} is not a lowercase name")
        archetypes: dict[int, list[list]] = {}
        for key, rows in dict(data.get("archetypes", {})).items():
            if not isinstance(rows, list) or len(rows) != 1 or len(rows[0]) != 6 or rows[0][0] != typ:
                raise ConvertError(f"{path}: archetype {key} needs one [typ, lodDist, bbMin, bbMax, flags, kind] row")
            if _bbox(rows[0]) is None or not 0 <= float(rows[0][1]) <= MAX_LOD:
                raise ConvertError(f"{path}: archetype {key} has an unusable box or lodDist")
            archetypes[int(key, 16)] = rows
        names = {int(k, 16): str(v) for k, v in dict(data.get("names", {})).items() if joaat(str(v)) == int(k, 16)}
        root = path.resolve().parent
        members: dict[str, Path] = {}
        for name, rel in dict(data.get("members", {})).items():
            source = (root / str(rel)).resolve()
            if not re.fullmatch(r"[a-z0-9_]{1,63}\.(pdr|ptd|ptyp)", name) or source.parent != root:
                raise ConvertError(f"{path}: member {name!r} must be a .pdr/.ptd/.ptyp next to the table")
            if not source.is_file() or source.is_symlink():
                raise ConvertError(f"{path}: member file {rel!r} is missing")
            members[name] = source
        if f"{typ}.ptyp" not in members or len(members) > MAX_PACK_MEMBERS:
            raise ConvertError(f"{path}: needs its {typ}.ptyp member (and at most {MAX_PACK_MEMBERS} members)")
        missing = [names.get(h, hex(h)) for h in archetypes if f"{names.get(h, '')}.pdr" not in members]
        if missing:
            raise ConvertError(f"{path}: no .pdr member for {', '.join(missing[:8])}")
        return cls(typ, archetypes, names, members)


@dataclass
class MapPlan:
    name: str
    source: str
    entities: list[dict] = field(default_factory=list)
    positions: list[tuple[float, float, float]] = field(default_factory=list)


@dataclass
class Plan:
    maps: list[MapPlan] = field(default_factory=list)
    classes: dict[int, str] = field(default_factory=dict)  # archetype hash -> retail|custom|unknown
    typ_of: dict[int, str] = field(default_factory=dict)  # retail archetype -> chosen typ
    uses: Counter = field(default_factory=Counter)  # archetype hash -> source entities
    kept: Counter = field(default_factory=Counter)  # archetype hash -> converted entities
    dropped: Counter = field(default_factory=Counter)  # reason -> entities
    adjusted: Counter = field(default_factory=Counter)  # what changed -> entities
    map_data: list[str] = field(default_factory=list)  # dropped map-level data
    notes: list[str] = field(default_factory=list)


def read_names(paths: list[Path]) -> dict[int, str]:
    names: dict[int, str] = {}
    for path in paths:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            token = line.strip().split("\t", 1)[0].split(" ", 1)[0].lower()
            if token and not token.startswith("#") and _NAME.fullmatch(token):
                names.setdefault(joaat(token), token)
    return names


def load_inputs(paths: list[Path]) -> tuple[list[tuple[str, Ymap]], ModArchive]:
    """(label, ymap) per map found, and what the mod archives ship."""
    maps: list[tuple[str, Ymap]] = []
    shipped = ModArchive()
    for path in paths:
        if not path.is_file() or path.is_symlink():
            raise ConvertError(f"{path}: not a regular file")
        if path.suffix.lower() == ".rpf":
            with path.open("rb") as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as view:
                found = scan_mod_archive(view)
            shipped.typ_archetypes.update(found.typ_archetypes)
            shipped.models |= found.models
            shipped.unreadable += [f"{path.name}: {item}" for item in found.unreadable]
            for member, raw in sorted(found.ymaps.items()):
                maps.append((f"{path.name}/{member}", read_ymap(raw)))
            continue
        lower = path.name.lower()
        if lower.endswith(MODEL_SUFFIXES):  # a loose model of the mod: only its name is used
            shipped.models.add(lower.rsplit(".", 1)[0])
            continue
        if path.stat().st_size > MAX_INPUT_BYTES:
            raise ConvertError(f"{path}: larger than {MAX_INPUT_BYTES} bytes")
        if lower.endswith(".ytyp"):
            try:
                for value in typ_archetypes(path.read_bytes()):
                    shipped.typ_archetypes.setdefault(value, path.name)
            except AssetError as exc:
                shipped.unreadable.append(f"{path.name} ({exc})")
            continue
        maps.append((path.name, read_ymap(path.read_bytes())))
    return maps, shipped


def classify(archetype: int, index: Index, shipped: set[int], pack: set[int] = frozenset()) -> str:
    if archetype in pack:
        return "pack"
    if archetype in index.archetypes:
        return "retail"
    return "custom" if archetype in shipped else "unknown"


def _bbox(row: list) -> list[list[float]] | None:
    try:
        lo, hi = [float(v) for v in row[2]], [float(v) for v in row[3]]
    except (TypeError, ValueError, IndexError):
        return None
    if len(lo) != 3 or len(hi) != 3 or not all(math.isfinite(v) and abs(v) < 1e5 for v in lo + hi):
        return None
    if any(a > b for a, b in zip(lo, hi, strict=True)):
        return None
    return [lo, hi]


def parse_translate(text: str | None) -> tuple[float, float, float]:
    """--translate DX,DY,DZ as three finite offsets (metres)."""
    if not text:
        return 0.0, 0.0, 0.0
    try:
        values = [float(v) for v in text.split(",")]
    except ValueError:
        values = []
    if len(values) != 3 or not all(math.isfinite(v) and abs(v) <= MAX_TRANSLATE for v in values):
        raise ConvertError(f"--translate needs DX,DY,DZ (finite, at most {MAX_TRANSLATE:.0f} m each)")
    return values[0], values[1], values[2]


def moved(position: tuple[float, float, float], delta: tuple[float, float, float]) -> tuple[float, float, float]:
    return position[0] + delta[0], position[1] + delta[1], position[2] + delta[2]


def convert_entity(
    e: Entity,
    name: str,
    row: list | None,
    plan: Plan,
    default_lod: float,
    delta: tuple[float, float, float] = (0.0, 0.0, 0.0),
) -> dict:
    item: dict = {"archetype": name, "position": [round(v, 5) for v in moved(e.position, delta)]}
    if tuple(round(v, 7) for v in e.rotation) not in ((0.0, 0.0, 0.0, 1.0), (-0.0, -0.0, -0.0, 1.0)):
        item["rotation"] = [round(v, 7) for v in e.rotation]
    lod = e.lod_dist
    if not lod > 0:
        lod = float(row[1]) if row is not None and float(row[1]) > 0 else default_lod
        plan.adjusted["lodDist <= 0 replaced by the archetype's (or the default)"] += 1
    if lod > MAX_LOD:
        lod = MAX_LOD
        plan.adjusted[f"lodDist capped at {MAX_LOD:.0f}"] += 1
    item["lod"] = round(lod, 3)
    bbox = _bbox(row) if row is not None else None
    if bbox is not None:
        item["bbox"] = bbox
    flags = e.flags & ~DROPPED_FLAGS
    if flags != e.flags:
        plan.adjusted["LOD-hierarchy entity flags 0x58 cleared"] += 1
    if flags:
        item["flags"] = flags
    if e.scale_xy != 1.0 or e.scale_z != 1.0:
        if not (e.scale_xy > 0 and e.scale_z > 0):
            raise ConvertError(f"entity of {name} has a non-positive scale")
        item["scale"] = e.scale_xy
        if e.scale_z != e.scale_xy:
            item["scaleZ"] = e.scale_z
    if e.tint:
        item["tint"] = e.tint
    if e.parent_index >= 0:
        plan.adjusted["LOD parent link dropped (kept as ORPHANHD)"] += 1
    if e.priority:
        plan.adjusted["streaming priority set to REQUIRED"] += 1
    if e.extensions:
        plan.adjusted["entity extensions dropped (lights, particles, ladders, ...)"] += 1
    return item


def _split(items: list[tuple[dict, tuple[float, float, float]]], limit: int) -> list[list]:
    """Recursive median split along the longest horizontal axis until each part holds <= limit."""
    if len(items) <= limit:
        return [items]
    xs, ys = [p[0] for _, p in items], [p[1] for _, p in items]
    axis = 0 if max(xs) - min(xs) >= max(ys) - min(ys) else 1
    ordered = sorted(items, key=lambda it: it[1][axis])
    half = len(ordered) // 2
    return _split(ordered[:half], limit) + _split(ordered[half:], limit)


def plan_maps(
    maps: list[tuple[str, Ymap]],
    index: Index,
    shipped: ModArchive,
    names: dict[int, str],
    *,
    prefix: str,
    keep_unresolved: bool = False,
    default_lod: float = DEFAULT_LOD,
    max_entities: int = MAX_MAP_ENTITIES,
    pack: PackArchetypes | None = None,
    translate: tuple[float, float, float] = (0.0, 0.0, 0.0),
) -> Plan:
    """`pack` archetypes are converted like retail ones (their rows give box and lodDist) and get no
    retail typ row; they win over a retail archetype of the same name. `translate` moves every kept
    entity (positions only: archetype boxes are entity-local)."""
    plan = Plan()
    pack_rows = pack.archetypes if pack else {}
    shipped_hashes = set(shipped.typ_archetypes) | {joaat(m) for m in shipped.models}
    typ_votes: Counter = Counter()
    for _label, ymap in maps:
        for e in ymap.entities:
            for row in pack_rows.get(e.archetype) or index.archetypes.get(e.archetype, []):
                typ_votes[row[0]] += 1
    for label, ymap in maps:
        items: list[tuple[dict, tuple[float, float, float]]] = []
        for e in ymap.entities:
            if e.name:
                names.setdefault(e.archetype, e.name)
            plan.uses[e.archetype] += 1
            if e.archetype not in plan.classes:
                plan.classes[e.archetype] = classify(e.archetype, index, shipped_hashes, set(pack_rows))
                if plan.classes[e.archetype] == "retail" and e.archetype in shipped_hashes:
                    plan.notes.append(
                        f"the mod re-ships retail archetype {names.get(e.archetype) or hex(e.archetype)}; "
                        "the retail one is placed"
                    )
            cls = plan.classes[e.archetype]
            rows = pack_rows.get(e.archetype) or index.archetypes.get(e.archetype, [])
            row = max(rows, key=lambda r: typ_votes[r[0]]) if rows else None
            if e.mlo:
                plan.dropped["MLO instance (CMloInstanceDef)"] += 1
                continue
            if e.lod_level not in (0, 5):
                plan.dropped[f"{LOD_LEVEL_NAMES.get(e.lod_level, e.lod_level)} entity (LOD model)"] += 1
                continue
            if not e.archetype:
                plan.dropped["no archetype"] += 1
                continue
            if row is not None and row[5] == "mlo":
                plan.dropped["MLO archetype placed as an entity"] += 1
                continue
            if cls not in ("retail", "pack") and not keep_unresolved:
                plan.dropped[f"{cls} archetype"] += 1
                continue
            if row is not None and cls == "retail":
                plan.typ_of[e.archetype] = row[0]
            name = names.get(e.archetype) or f"hash_{e.archetype:08x}"
            items.append((convert_entity(e, name, row, plan, default_lod, translate), moved(e.position, translate)))
            plan.kept[e.archetype] += 1
        for what, count in ymap.sections.items():
            plan.map_data.append(f"{label}: {count} {what}")
        if ymap.car_generators:
            models = Counter(c.model_name or names.get(c.model) or f"hash_{c.model:08x}" for c in ymap.car_generators)
            plan.map_data.append(
                f"{label}: {len(ymap.car_generators)} car generators ({', '.join(f'{m} x{n}' for m, n in models.items())})"
            )
        if ymap.physics_dictionaries:
            plan.map_data.append(f"{label}: {len(ymap.physics_dictionaries)} physics dictionaries")
        plan.notes += [f"{label}: {note}" for note in ymap.notes]
        if not items:
            plan.notes.append(f"{label}: no convertible entity; no map written")
            continue
        for part in _split(items, max_entities):
            map_name = f"{prefix}_{len(plan.maps)}"
            plan.maps.append(MapPlan(map_name, label, [i for i, _ in part], [p for _, p in part]))
    return plan


def retail_typs(
    plan: Plan,
    limit: int,
    skip: frozenset[str] = frozenset(),
    residency: dict[str, str] | None = None,
    policy: str = "needed",
) -> tuple[list[str], list[str]]:
    """(requested typ names, warnings): typs ranked by the converted entities that need them.

    policy "needed" leaves out the typs the index classes as resident, "all" keeps them, "none"
    emits no row."""
    need: Counter = Counter()
    for archetype, typ in plan.typ_of.items():
        need[typ] += plan.kept[archetype]
    residency = residency or {}
    resident = sorted(t for t in need if residency.get(t) == "resident") if policy == "needed" else []
    if resident:
        count = sum(need[t] for t in resident)
        plan.notes.append(f"no row for {len(resident)} typs the index classes resident ({count} entities)")
    left_out = sorted((set(need) if policy == "none" else skip & set(need)) - set(resident))
    if left_out:
        count = sum(need[t] for t in left_out)
        plan.notes.append(f"no row (--skip-typ / --typ-rows none) for {', '.join(left_out)} ({count} entities)")
    ranked = [typ for typ, _ in need.most_common() if typ not in set(resident) | set(left_out)]
    chosen, rest = ranked[:limit], ranked[limit:]
    warnings = []
    if rest:
        missing = sum(need[t] for t in rest)
        warnings.append(
            f"{len(rest)} more retail typs ({missing} entities: {', '.join(rest[:12])}{' ...' if len(rest) > 12 else ''}) "
            f"get no `typ ... retail` row (limit {limit}); those entities appear only if their typ is resident"
        )
    return chosen, warnings


def default_place(positions: list[tuple[float, float, float]]) -> tuple[int, int, int]:
    cx = sum(p[0] for p in positions) / len(positions)
    cy = sum(p[1] for p in positions) / len(positions)
    near = min(positions, key=lambda p: (p[0] - cx) ** 2 + (p[1] - cy) ** 2)
    return round(near[0]), round(near[1]), math.ceil(near[2] + 1.0)


def place_problem(point: tuple[int, int, int], maps: list[list]) -> str | None:
    """Why a teleport point does not suit its maps: outside every map's streamingExtents (the map
    would not be streamed in there), or more than 200 m from every converted entity."""
    import make_pmap

    inside = False
    for entities in maps:
        _lo, _hi, slo, shi = make_pmap.extents(entities)
        inside |= all(a <= v <= b for a, v, b in zip(slo, point, shi, strict=True))
    if not inside:
        return "outside the streamingExtents of its maps"
    nearest = min(math.dist(point, e.position) for entities in maps for e in entities)
    if nearest > 200.0:
        return f"{nearest:.0f} m from the nearest converted entity"
    return None


def _listed(items: list[str], limit: int = 10) -> str:
    return ", ".join(items[:limit]) + (f", +{len(items) - limit} more" if len(items) > limit else "")


def report(plan: Plan, names: dict[int, str], typs: list[str], warnings: list[str]) -> dict:
    by_class: dict[str, list] = {"retail": [], "pack": [], "custom": [], "unknown": []}
    for archetype, cls in sorted(plan.classes.items(), key=lambda kv: -plan.uses[kv[0]]):
        by_class[cls].append(
            {
                "archetype": names.get(archetype) or f"hash_{archetype:08x}",
                "entities": plan.uses[archetype],
                "converted": plan.kept[archetype],
                **({"typ": plan.typ_of[archetype]} if archetype in plan.typ_of else {}),
            }
        )
    return {
        "maps": [{"name": m.name, "source": m.source, "entities": len(m.entities)} for m in plan.maps],
        "entities": {"source": sum(plan.uses.values()), "converted": sum(plan.kept.values())},
        "archetypes": {cls: len(rows) for cls, rows in by_class.items()},
        "retailTypRows": typs,
        "dropped": dict(plan.dropped),
        "adjusted": dict(plan.adjusted),
        "droppedMapData": plan.map_data,
        "warnings": warnings,
        "notes": plan.notes,
        "classification": by_class,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "inputs",
        type=Path,
        nargs="+",
        help=".ymap, .ymap.xml or a PC mod .rpf (.ytyp/.ydr/.yft/.ydd: the mod's models)",
    )
    parser.add_argument("--archetype-index", type=Path, help="archetype index JSON of the user's own game")
    parser.add_argument("--names", type=Path, action="append", default=[], help="extra archetype name list")
    parser.add_argument(
        "--pack-archetypes",
        type=Path,
        help="the mod's own models converted for this pack (a gtavmenu-pack-archetypes-v1 table): "
        "their entities are kept and the pack carries their members and typ",
    )
    parser.add_argument("--out", type=Path, required=True, help="new directory for specs, maps and report")
    parser.add_argument("--prefix", default="gmymap", help="map name prefix (maps are PREFIX_0, PREFIX_1, ...)")
    parser.add_argument("--template", type=Path, help="one-entity .pmap from the user's own game (writes .pmap)")
    parser.add_argument("--keep-unresolved", action="store_true", help="keep custom/unknown archetypes")
    parser.add_argument(
        "--refuse-custom",
        action="store_true",
        help="stop before writing anything when the mod places its own models (custom archetypes)",
    )
    parser.add_argument("--default-lod", type=float, default=DEFAULT_LOD)
    parser.add_argument("--max-entities", type=int, default=MAX_MAP_ENTITIES, help="split maps above this")
    parser.add_argument("--max-retail-typs", type=int, default=8)
    parser.add_argument("--skip-typ", action="append", default=[], metavar="NAME", help="never emit a row for typ NAME")
    parser.add_argument(
        "--typ-rows",
        choices=("needed", "all", "none"),
        default="needed",
        help="retail typ rows: for typs not classed resident by the index (default), for all, or none",
    )
    parser.add_argument("--pack-id", help="also build this pack (needs --template)")
    parser.add_argument("--archive", help="pack archive name (default: PREFIX.rpf)")
    parser.add_argument("--output-root", type=Path, default=_HERE.parent / "build/custom-assets")
    parser.add_argument("--label", help="teleport row text (default: the map name)")
    parser.add_argument(
        "--translate",
        metavar="DX,DY,DZ",
        help="move every kept entity by this offset (metres); --place points are in the moved coordinates",
    )
    parser.add_argument(
        "--place",
        action="append",
        default=[],
        metavar="X,Y,Z[:TEXT]",
        help="teleport point of each converted source map (on its first map row)",
    )
    args = parser.parse_args(argv)
    try:
        if not _PREFIX.fullmatch(args.prefix):
            raise ConvertError("--prefix must be lowercase letters, digits and '_' (start with a letter)")
        pack = PackArchetypes.load(args.pack_archetypes) if args.pack_archetypes else None
        typ_room = PACK_TYP_MAX - (1 if pack else 0)
        if not 1 <= args.max_retail_typs <= typ_room or not 1 <= args.max_entities <= MAX_MAP_ENTITIES:
            raise ConvertError(
                f"--max-retail-typs 1..{typ_room} (the pack typ takes one of {PACK_TYP_MAX} rows), "
                f"--max-entities 1..{MAX_MAP_ENTITIES}"
            )
        if not 0 < args.default_lod <= MAX_LOD:
            raise ConvertError(f"--default-lod must be in (0, {MAX_LOD:.0f}]")
        if args.pack_id and not args.template:
            raise ConvertError("--pack-id needs --template")
        translate = parse_translate(args.translate)
        if args.out.exists() and any(args.out.iterdir()):
            raise ConvertError(f"refusing to write into the non-empty {args.out}")
        index = Index.load(args.archetype_index)
        names = {**read_names(args.names), **index.names, **(pack.names if pack else {})}
        maps, shipped = load_inputs(args.inputs)
        names.update({joaat(m): m for m in shipped.models if _NAME.fullmatch(m)})
        plan = plan_maps(
            maps,
            index,
            shipped,
            names,
            prefix=args.prefix,
            keep_unresolved=args.keep_unresolved,
            default_lod=args.default_lod,
            max_entities=args.max_entities,
            pack=pack,
            translate=translate,
        )
        typs, warnings = retail_typs(
            plan,
            args.max_retail_typs,
            frozenset(t.lower() for t in args.skip_typ),
            index.residency,
            args.typ_rows,
        )
        custom = sorted(names.get(a) or f"hash_{a:08x}" for a, cls in plan.classes.items() if cls == "custom")
        if args.refuse_custom and custom:
            raise ConvertError(
                f"the mod places {len(custom)} of its own models ({_listed(custom)}); only mods that place "
                "stock objects and buildings convert here"
            )
        if not index.archetypes:
            warnings.append("no --archetype-index: every archetype is unknown")
        warnings += [f"unreadable: {item}" for item in shipped.unreadable]
        if args.pack_id and len(plan.maps) > PACK_MAP_MAX:
            raise ConvertError(f"{len(plan.maps)} maps exceed the pack limit {PACK_MAP_MAX}; raise --max-entities")
        sources = len({m.source for m in plan.maps})
        if args.place and len(args.place) != sources:
            raise ConvertError(f"--place needs one point per converted source map ({sources})")
    except (AssetError, ConvertError, OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"convert_ymap: {exc}") from exc

    args.out.mkdir(parents=True, exist_ok=True)
    summary = report(plan, names, typs, warnings)
    if any(translate):
        summary["translate"] = list(translate)
    if pack:
        summary["packTyp"] = f"{pack.typ}.ptyp"
        summary["packMembers"] = sorted(pack.members)
    pmaps = []
    import make_pmap

    if args.template:
        template = args.template.read_bytes()
    for m in plan.maps:
        spec = {"name": m.name, "entities": m.entities}
        (args.out / f"{m.name}.json").write_text(json.dumps(spec, indent=1) + "\n", encoding="utf-8")
        if args.template:
            try:
                resource = make_pmap.build(spec, template)
            except make_pmap.MapError as exc:
                raise SystemExit(f"convert_ymap: {m.name}: {exc}") from exc
            (args.out / f"{m.name}.pmap").write_bytes(resource)
            check = make_pmap.verify(resource)
            pmaps.append(m.name)
            print(f"wrote {args.out / (m.name + '.pmap')} {make_pmap._describe(check)}")
    sources = list(dict.fromkeys(m.source for m in plan.maps))
    places = []
    for i, source in enumerate(sources):
        parts = [m for m in plan.maps if m.source == source]
        if args.place:
            coords, _, text = args.place[i].partition(":")
            x, y, z = (round(float(v)) for v in coords.split(","))
        else:
            x, y, z = default_place([p for m in parts for p in m.positions])
            text = ""
        text = text or args.label or parts[0].name
        if len(sources) > 1 and not args.place:
            text = f"{text[:34]} {i + 1}"
        places.append(f"{parts[0].name}.pmap={x},{y},{z}:{text[:38]}")
        problem = place_problem((x, y, z), [make_pmap.entities_of({"entities": m.entities}) for m in parts])
        if problem:
            warnings.append(f"teleport {x},{y},{z} ({source}): {problem}")
            print(f"warning: teleport {x},{y},{z}: {problem}", file=sys.stderr)
    summary["places"] = places
    summary["warnings"] = warnings
    (args.out / "report.json").write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")
    counts = summary["archetypes"]
    print(
        f"{summary['entities']['converted']}/{summary['entities']['source']} entities converted into "
        f"{len(plan.maps)} map(s); archetypes retail={counts['retail']} pack={counts['pack']} custom={counts['custom']} "
        f"unknown={counts['unknown']}; retail typ rows: {', '.join(typs) or '-'}"
    )
    for cls in ("custom", "unknown"):
        rows = summary["classification"][cls]
        if rows:
            state = "kept" if args.keep_unresolved else "dropped"
            listed = _listed([f"{r['archetype']} x{r['entities']}" for r in rows])
            print(f"{cls} archetypes ({state}): {listed}")
    for reason, count in sorted(plan.dropped.items()):
        print(f"dropped: {count} x {reason}")
    for what, count in sorted(plan.adjusted.items()):
        print(f"adjusted: {count} x {what}")
    for item in plan.map_data:
        print(f"dropped map data: {item}")
    for warning in warnings:
        print(f"warning: {warning}", file=sys.stderr)
    if not args.pack_id:
        return 0
    if not plan.maps:
        raise SystemExit("convert_ymap: no map to pack")
    import build_runtime_pack
    import validate_runtime_pack

    pack_argv = ["--id", args.pack_id, "--archive", args.archive or f"{args.prefix}.rpf"]
    pack_argv += ["--output-root", str(args.output_root)]
    for name in pmaps:
        pack_argv += ["--member", f"{name}.pmap={args.out / (name + '.pmap')}", "--map", f"{name}.pmap"]
    if pack:
        for name, source in sorted(pack.members.items()):
            pack_argv += ["--member", f"{name}={source}"]
        pack_argv += ["--typ", f"{pack.typ}.ptyp"]
    for typ in typs:
        pack_argv += ["--retail-typ", f"{typ}.ptyp"]
    for place in places:
        pack_argv += ["--map-place", place]
    (args.out / "build_runtime_pack.args.json").write_text(json.dumps(pack_argv, indent=1) + "\n")
    build_runtime_pack.main(pack_argv)
    return validate_runtime_pack.main([str(args.output_root / args.pack_id)])


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""One PC map mod -> one or more runtime packs within the pack caps (`menu-ctl.sh convert-mapmod`).

A typical PC map mod is one dlc.rpf (or a folder of loose files) holding .ymap placements, .ytyp
definitions, its own models (.ydr + .ytd), static collision (.ybn) and sometimes interiors (an MLO
archetype of a .ytyp placed by a CMloInstanceDef). This tool inventories the mod, decides which of
the existing converters each part needs, and splits the result into packs within the caps of
include/gtavmenu/custom_pack_runtime.h (gtavmenu_tools.runtime_pack):

  world      the mod's placements (tools/convert_ymap.py) with the own models they place (the
             model converter, convert_pc_map_models.py) and the exterior collision rows (the bounds
             converter, tools/convert_pc_bounds.py with the bounds-templates/ of --templates: one row
             per .ybn, or --bounds merge); with
             --drawn-collision also rows made from the drawn geometry of own models whose PC
             collision lives inside the model (an embedded bound or physics dictionary, which the
             model route drops) or, with `all`, of every placed own model (drawable_collision.py);
  interior   the mod's placed MLOs (tools/convert_pc_mlo.py), their own models and their shell
             collision (<mlo>.ybn, MLO-local);
  collision  bounds-only companion packs for the rows past the world pack's 8;
  props      with --archetypes-from MOD: the models of ANOTHER mod (a Map Builder style prop pack) that
             these maps place, converted into a pack of their own (typ + models + textures, no maps);
             the world maps then place that pack's archetypes (cross-pack archetypes: both packs are
             active together). --archetypes-from PACK_DIR uses a props pack built before instead.

Caps applied per pack: 8 maps (each at most --max-entities entities; a world pack whose maps do not
fit at 300 is converted at 1024, and past that the mod's .ymap files are split over several world
packs), 16 typ rows (the pack typs first, then the retail typs the most entities need), 8 bounds
rows, 8 place rows, 4 archives of at most 64 MiB (members past the first archive move to further
archives, largest first; members of 16 MiB or more take the archive's oversize escape), and for
the whole set 8 active packs and 512 MiB of archives. A drawn-collision row holds at most 127
entities (one composite child each); a map with more gets several rows, split by archetype.
Interiors are not moved by --translate.

The converters do all the format work. This tool runs them (subprocesses: `python3 -I` for the
publishable tools/ it calls itself and --bounds-tool; `python3 -P`, with --model-pythonpath when given, for the
numpy model converters given with --models-tool and --collision-tool) on the copied mod and composes the
tools/build_runtime_pack.py arguments the way the per-pack recipes and the convert-* commands do. Without a
converter the parts that need it are left out and listed. The collision converter takes the class tags of the
cached retail bounds (bounds-templates/ of --templates), as the bounds converter does.

The plan (JSON, schema gtavmenu-mapmod-plan-v1) names every pack, archive, typ, map prefix, row and
teleport. --dry-run prints it with the cap use and writes nothing (--write-plan FILE saves it);
--plan FILE converts an edited plan (other names, rows moved between packs, rows left out). The
mod is untrusted data: run this tool with `python3 -I` on a copy, never from the mod's folder.

  convert_map_mod.py WORK/input/dlc.rpf --id gtavmenu-villa-v1 --work WORK --packs-root build/custom-assets \\
      --archetype-index INDEX.json --templates build/retail-templates --reference-dir build/assets \\
      [--models-tool MODEL_CONVERTER] --bounds-tool tools/convert_pc_bounds.py \\
      [--dry-run] [--plan FILE] [--translate DX,DY,DZ] [--bounds rows|merge] [--drawn-collision embedded|all]
      [--archetypes-from MOD|PACK_DIR [--archetypes-id ID]]
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
import os  # noqa: E402
import re  # noqa: E402
import shutil  # noqa: E402
import subprocess  # noqa: E402
from collections import Counter  # noqa: E402
from dataclasses import dataclass, field  # noqa: E402

import convert_pc_mlo  # noqa: E402
import convert_ymap  # noqa: E402
import make_mlo_ptyp  # noqa: E402
from gtavmenu_tools import pack_archetypes, rpf7  # noqa: E402
from gtavmenu_tools import runtime_pack as rp  # noqa: E402
from gtavmenu_tools.asset_formats import AssetError, Limits, rpf_header, rpf_members  # noqa: E402
from gtavmenu_tools.asset_metadata import parse_xml  # noqa: E402
from gtavmenu_tools.hashes import joaat  # noqa: E402
from gtavmenu_tools.ymap import MOD_LIMITS, ModArchive, label_name, read_ymap, scan_mod_archive  # noqa: E402

SCHEMA = "gtavmenu-mapmod-plan-v1"
ROLES = ("props", "world", "interior", "collision")  # build order: props tables before the maps placing them
DRAWN_CHILDREN = 127  # entities per drawn collision row (composites of up to 127 children proven: GTA6-DIST-run2)
EXTERNAL_TYP = "gm_external"  # dry-run typ of another mod's archetypes (the props pack's typ in the build)
INCLUDE = "0x07f3bec0"  # retail world collision include flags (convert-bounds' default)
ENTITIES_DEFAULT = 300  # proven per map (BIGMAP-run1, YMAP-run1)
ENTITIES_LARGE = 1024  # proven per map (GTA6-DIST-run1)
MERGE_SOURCE_BYTES = 4 * 1024 * 1024  # .ybn bytes per merged row (~1.5x as .pbn; rows stay < 8 MiB)
PBN_FACTOR = 1.6  # .pbn bytes per .ybn byte (MalibuMansion: 1.06 MB -> 1.61 MB)
BLOCK = 512  # RPF7 member alignment
ARCHIVE_BUDGET = rp.ARCHIVE_BYTES_MAX
SET_BUDGET = rp.SET_ARCHIVE_BYTES_MAX
LARGE_MEMBER = 0xFFFFFF  # members this large take the table's oversize escape
SUFFIXES = (".ymap", ".ytyp", ".ydr", ".yft", ".ydd", ".ytd", ".ybn")
_ID = re.compile(r"[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\Z")
_NAME = re.compile(r"[a-z0-9_]{1,59}\Z")
_ARCHIVE = re.compile(r"[a-z0-9_]{1,59}\.rpf\Z")
_PREFIX = re.compile(r"[a-z][a-z0-9_]{0,40}\Z")
_PLACE = re.compile(r"(-?\d+),(-?\d+),(-?\d+)(?::([\x20-\x7e]{1,38}))?\Z")
_VERSION = re.compile(r"(.*?)(-v\d+[a-z]?)?\Z")


class MapModError(ValueError):
    """The mod, the plan or an option is outside what this flow supports."""


class StepError(RuntimeError):
    """A converter step failed (its log names the reason)."""


# --------------------------------------------------------------------------------------------------
# inventory


@dataclass
class Inventory:
    """What the mod ships. Labels are paths inside the archive, or file names of a folder mod."""

    root: Path = Path(".")  # the mod as given (a dlc.rpf or a folder)
    archive: Path | None = None  # the mod's dlc.rpf (archive mode)
    loose: list[Path] = field(default_factory=list)  # a folder mod's map/typ/model files (convert_ymap inputs)
    ymaps: dict[str, bytes] = field(default_factory=dict)
    typs: dict[str, bytes] = field(default_factory=dict)
    files: dict[str, dict[str, tuple[str, int]]] = field(default_factory=dict)  # suffix -> stem -> (label, size)
    paths: dict[str, Path] = field(default_factory=dict)  # folder mode: label -> file
    others: Counter = field(default_factory=Counter)  # unsupported file types -> count
    editor: list[str] = field(default_factory=list)  # Map Editor / Menyoo object lists
    unreadable: list[str] = field(default_factory=list)

    def stems(self, suffix: str) -> dict[str, tuple[str, int]]:
        return self.files.get(suffix, {})

    def add(self, label: str, size: int) -> None:
        base = label.rsplit("/", 1)[-1].lower()
        for suffix in SUFFIXES:
            if base.endswith(suffix) and not base.endswith(".ymap.xml"):
                stem = base[: -len(suffix)]
                table = self.files.setdefault(suffix, {})
                if stem in table:
                    raise MapModError(f"two {suffix} files are named {stem} ({table[stem][0]}, {label})")
                table[stem] = (label, size)
                return
        if base.endswith(".ymap.xml"):
            self.files.setdefault(".ymap", {})[base[: -len(".ymap.xml")]] = (label, size)
        elif base.endswith(".rpf"):
            return
        else:
            self.others[os.path.splitext(base)[1] or base] += 1


def _archive_members(view, inv: Inventory) -> None:
    """Every member of the mod archive and its nested uncompressed archives: name and stored size."""
    pending = [(0, len(view), "", 0)]
    while pending:
        base, size, prefix, depth = pending.pop()

        def read_at(offset: int, count: int, base: int = base, size: int = size) -> bytes:
            if offset < 0 or count < 0 or offset + count > size:
                raise AssetError("archive read outside its bounds")
            return bytes(view[base + offset : base + offset + count])

        try:
            end = rpf_header(read_at(0, 16), size)["tableEnd"]
            members = rpf_members(read_at(0, end), MOD_LIMITS, file_size=size, read_at=read_at, name_check=label_name)
        except AssetError as exc:
            if not prefix:
                raise
            inv.unreadable.append(f"{prefix.rstrip('/')} ({exc})")
            continue
        for member in members:
            path = prefix + member.name
            if member.name.lower().endswith(".rpf") and not member.resource:
                if not (member.compressed or member.encrypted or depth >= 4):
                    pending.append((base + member.offset, member.size, path + "/", depth + 1))
                continue
            inv.add(path, member.size)


def editor_objects(blob: bytes) -> str | None:
    """'N Prop, M Vehicle, ...' for a Map Editor / Menyoo object list (<Map><Objects><MapObject>), else None."""
    try:
        root = parse_xml(blob, Limits(max_xml_nodes=200000, max_xml_depth=16))
    except (AssetError, UnicodeDecodeError, ValueError):
        return None
    if root.tag != "Map" or root.find("Objects") is None:
        return None
    kinds = Counter((item.findtext("Type") or "?").strip()[:20] for item in root.iter("MapObject"))
    kinds.update(f"marker {(item.findtext('Type') or '?').strip()[:20]}" for item in root.iter("Marker"))
    return ", ".join(f"{count} {kind}" for kind, count in sorted(kinds.items())) or "no objects"


def inventory(mod: Path) -> Inventory:
    """The mod's dlc.rpf, or a folder: its one dlc.rpf, or its loose .ymap/.ymap.xml/.ytyp/model/.ybn files."""
    inv = Inventory(root=mod)
    if mod.is_file():
        if not mod.name.lower().endswith(".rpf"):
            raise MapModError(f"{mod.name}: give the mod's dlc.rpf or its folder")
        inv.archive = mod
    elif mod.is_dir():
        files = sorted(p for p in mod.rglob("*") if p.is_file() and not p.is_symlink())
        archives = [p for p in files if p.name.lower().endswith(".rpf")]
        loose = [p for p in files if p.name.lower().endswith((".ymap", ".ymap.xml", ".ytyp"))]
        if archives and loose:
            raise MapModError("the folder holds both a .rpf and loose .ymap/.ytyp files: give one of them")
        if len(archives) > 1:
            raise MapModError(f"the folder holds {len(archives)} .rpf files: give the one dlc.rpf to convert")
        if archives:
            inv.archive = archives[0]
    else:
        raise MapModError(f"{mod}: not a file or folder")
    if inv.archive is not None:
        with inv.archive.open("rb") as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as view:
            found = scan_mod_archive(view)
            _archive_members(view, inv)
        inv.ymaps, inv.typs = dict(found.ymaps), dict(found.typs)
        inv.unreadable += found.unreadable
        return inv
    for path in files:
        label = path.relative_to(mod).as_posix()
        lower = path.name.lower()
        size = path.stat().st_size
        if lower.endswith(".xml") and not lower.endswith(".ymap.xml"):
            objects = editor_objects(path.read_bytes()) if size <= 4 * 1024 * 1024 else None
            if objects is not None:
                inv.editor.append(f"{label}: {objects}")
                continue
        inv.add(label, size)
        inv.paths[label] = path
        if lower.endswith((".ymap", ".ymap.xml")):
            inv.ymaps[label] = path.read_bytes()
        elif lower.endswith(".ytyp"):
            inv.typs[label] = path.read_bytes()
        if lower.endswith((".ymap", ".ymap.xml", ".ytyp", ".ydr", ".yft", ".ydd")):
            inv.loose.append(path)
    return inv


@dataclass
class External:
    """Archetypes that another source defines (--archetypes-from): a props mod (kind "mod": its .ytyp rows and
    .ydr models, converted here into a props pack) or a pack built before (kind "pack": its own typ rows)."""

    kind: str
    root: Path
    rows: dict[int, dict]  # archetype hash -> CBase/CTimeArchetypeDef row (name, lodDist, bbMin, bbMax, flags, ...)
    names: dict[int, str]
    inv: Inventory | None = None  # kind "mod"
    convertible: set[int] = field(default_factory=set)  # kind "mod": rows with a .ydr
    pack_id: str = ""  # kind "pack"
    typs: dict[int, str] = field(default_factory=dict)  # kind "pack": archetype hash -> its typ (no .ptyp)

    def label(self) -> str:
        return self.pack_id or self.root.name


def load_external(path: Path) -> External:
    """--archetypes-from: a pack directory (resources/pack.cfg; its own .ptyp rows) or a props mod (dlc.rpf or
    folder, untrusted: read like the map mod)."""
    if path.is_dir() and (path / "resources" / "pack.cfg").is_file():
        try:
            pack = rp.parse((path / "resources" / "pack.cfg").read_text(encoding="utf-8"))
        except (rp.RuntimePackError, UnicodeDecodeError) as exc:
            raise MapModError(f"--archetypes-from {path}: {exc}") from None
        payloads: dict[str, bytes] = {}
        names: dict[int, str] = {}
        for archive in [pack.archive, *(a for a, _, _ in pack.extra_archives)]:
            try:
                blob = (path / "resources" / archive).read_bytes()
                payloads.update(pack_archetypes.archive_payloads(blob, archive, (".ptyp",)))
                stems = [m.rsplit(".", 1)[0] for m in pack_archetypes.archive_member_names(blob, archive, (".pdr",))]
            except (OSError, rpf7.Rpf7Error, ValueError) as exc:
                raise MapModError(f"--archetypes-from {path}: {archive} is not readable ({exc})") from None
            names |= {joaat(stem): stem for stem in stems}
        rows: dict[int, dict] = {}
        typs: dict[int, str] = {}
        for typ in pack.typs:
            if typ in pack.retail_typs or typ not in payloads:
                continue
            spec = make_mlo_ptyp.decode_payload(payloads[typ], names)
            for row in spec.get("archetypes") or []:
                key = joaat(str(row.get("name", "")))
                if row.get("type") in ("CBaseArchetypeDef", "CTimeArchetypeDef") and key in names:
                    rows.setdefault(key, row)
                    typs.setdefault(key, typ.removesuffix(".ptyp"))
        if not rows:
            raise MapModError(f"--archetypes-from {path}: pack {pack.pack_id} defines no archetype with a model")
        return External("pack", path, rows, names, pack_id=pack.pack_id, typs=typs)
    inv = inventory(path)
    names = {joaat(stem): stem for stem in inv.stems(".ytd")}  # any name: a dictionary may be `<model>+hidr`
    for suffix in SUFFIXES:
        names |= {joaat(stem): stem for stem in inv.stems(suffix) if _NAME.fullmatch(stem)}
    try:
        _mlos, rows = convert_pc_mlo.mod_definitions(inv.typs, names)
    except Exception as exc:
        raise MapModError(f"--archetypes-from: the .ytyp files do not decode: {exc}") from None
    for key, row in rows.items():
        name = str(row.get("name", ""))
        if _NAME.fullmatch(name):
            names.setdefault(key, name)
    if not rows:
        raise MapModError(f"--archetypes-from {path.name}: no .ytyp archetypes (not a props mod)")
    convertible = {key for key in rows if names.get(key, "") in inv.stems(".ydr")}
    return External("mod", path, rows, names, inv=inv, convertible=convertible)


def external_txd(ext: External, key: int) -> str:
    """The texture dictionary name of an external archetype ("" when unknown)."""
    value = str(ext.rows[key].get("textureDictionary") or "")
    if value.startswith("0x"):
        value = ext.names.get(int(value, 16), "")
    return value.lower()


def txd_aliases(ext: External, keys: list[int]) -> list[str]:
    """--txd-alias HASH=NAME rows for the texture dictionaries of these archetypes whose names are not member
    names (Map Builder: `<model>+hidr`): the model converter reads the mod's .ytd of that hash and writes NAME."""
    out = {}
    for key in keys:
        txd = external_txd(ext, key)
        if txd and not _NAME.fullmatch(txd):
            out[joaat(txd)] = short(re.sub(r"[^a-z0-9_]", "_", txd), 59)
    return [f"0x{value:08x}={name}" for value, name in sorted(out.items(), key=lambda item: item[1])]


# --------------------------------------------------------------------------------------------------
# analysis


class SimTable:
    """convert_ymap's view of a pack-archetypes table, from the mod's own typ rows (the dry run)."""

    def __init__(self, typ: str, rows: dict[int, dict]) -> None:
        self.typ = typ
        self.archetypes = {}
        for key, row in rows.items():
            lo, hi = list(row.get("bbMin") or [0, 0, 0]), list(row.get("bbMax") or [0, 0, 0])
            box = [[min(a, b) for a, b in zip(lo, hi, strict=True)], [max(a, b) for a, b in zip(lo, hi, strict=True)]]
            self.archetypes[key] = [[typ, float(row.get("lodDist") or 0.0), *box, int(row.get("flags") or 0), "base"]]


@dataclass
class Analysis:
    inv: Inventory
    index: convert_ymap.Index
    names: dict[int, str]
    maps: list[tuple[str, object]]  # (label, Ymap)
    shipped: ModArchive
    own: dict[int, dict]  # own CBase/CTime archetype rows
    mlos: dict[int, tuple[str, dict]]  # MLO hash -> (typ label, row)
    mlo_names: dict[int, str]
    mlo_maps: dict[int, list[str]]  # MLO hash -> labels of the ymaps placing it
    convertible: set[int]  # own archetypes with a .ydr
    txd: dict[int, str]  # own archetype -> texture dictionary name (resolved, else "")
    ext: External | None = None  # --archetypes-from
    ext_keys: set[int] = field(default_factory=set)  # external archetypes neither stock nor the mod's own
    ext_index: convert_ymap.Index | None = None  # the index plus ext_keys (resident: another pack loads them)


def external_index(index: convert_ymap.Index, ext: External, keys: set[int], typ_of=None) -> convert_ymap.Index:
    """The archetype index plus these external archetypes, as rows of their typ (`typ_of`: hash -> typ name;
    default EXTERNAL_TYP), classed resident: their typ is another pack's typ row, requested by that pack."""
    archetypes = dict(index.archetypes)
    residency = dict(index.residency)
    for key in keys:
        row = ext.rows[key]
        lo, hi = list(row.get("bbMin") or [0, 0, 0]), list(row.get("bbMax") or [0, 0, 0])
        box = [[min(a, b) for a, b in zip(lo, hi, strict=True)], [max(a, b) for a, b in zip(lo, hi, strict=True)]]
        typ = (typ_of or {}).get(key, EXTERNAL_TYP)
        archetypes[key] = [[typ, float(row.get("lodDist") or 0.0), *box, int(row.get("flags") or 0), "drawable"]]
        residency[typ] = "resident"
    names = dict(index.names) | {k: ext.names[k] for k in keys if k in ext.names}
    return convert_ymap.Index(archetypes, names, residency)


def analyse(inv: Inventory, index: convert_ymap.Index, ext: External | None = None) -> Analysis:
    names = dict(index.names)
    for suffix in SUFFIXES:
        names |= {joaat(stem): stem for stem in inv.stems(suffix) if _NAME.fullmatch(stem)}
    shipped = ModArchive()
    for suffix in (".ydr", ".yft", ".ydd"):
        shipped.models |= set(inv.stems(suffix))
    try:
        mlos, own = convert_pc_mlo.mod_definitions(inv.typs, names)
    except Exception as exc:
        raise MapModError(f"the mod's .ytyp files do not decode: {exc}") from None
    for key, row in own.items():
        name = str(row.get("name", ""))
        if _NAME.fullmatch(name):
            names.setdefault(key, name)
    for typ_label in inv.typs:
        for key in list(own) + list(mlos):
            shipped.typ_archetypes.setdefault(key, typ_label)
    maps = []
    for label, raw in sorted(inv.ymaps.items()):
        try:
            maps.append((label, read_ymap(raw)))
        except AssetError as exc:
            inv.unreadable.append(f"{label} ({exc})")
    mlo_maps: dict[int, list[str]] = {}
    for label, ymap in maps:
        for e in ymap.entities:
            if e.mlo and e.archetype in mlos:
                mlo_maps.setdefault(e.archetype, []).append(label)
    mlo_names = {key: names[key] for key in mlos if key in names and _NAME.fullmatch(names[key])}
    convertible = {key for key in own if names.get(key, "") in inv.stems(".ydr")}
    txd = {}
    for key, row in own.items():
        value = str(row.get("textureDictionary") or "")
        if value.startswith("0x"):
            value = names.get(int(value, 16), "")
        txd[key] = value.lower() if _NAME.fullmatch(value.lower()) else ""
    a = Analysis(inv, index, names, maps, shipped, own, mlos, mlo_names, mlo_maps, convertible, txd)
    if ext is not None:
        a.ext = ext
        placed = {e.archetype for _label, ymap in maps for e in ymap.entities if not e.mlo}
        a.ext_keys = {k for k in placed & set(ext.rows) if k not in index.archetypes and k not in own}
        usable = a.ext_keys if ext.kind == "pack" else a.ext_keys & ext.convertible  # a props pack ships these
        a.ext_index = external_index(index, ext, usable, ext.typs)
        for key in a.ext_keys:
            if key in ext.names:
                a.names.setdefault(key, ext.names[key])
    return a


def simulate_maps(
    a: Analysis,
    sources: list[str],
    models: set[int],
    *,
    prefix: str,
    max_entities: int,
    translate: tuple[float, float, float],
    typ: str = "gm_sim",
    external: bool = False,
) -> convert_ymap.Plan:
    """convert_ymap's map plan for these source maps, the `models` archetypes kept as pack archetypes and, with
    `external`, the archetypes of --archetypes-from kept like stock ones (their typ is another pack's)."""
    chosen = [(label, ymap) for label, ymap in a.maps if label in sources]
    table = SimTable(typ, {k: a.own[k] for k in models}) if models else None
    return convert_ymap.plan_maps(
        chosen,
        a.ext_index if external and a.ext_index is not None else a.index,
        a.shipped,
        dict(a.names),
        prefix=prefix,
        keep_unresolved=not a.index.archetypes,
        max_entities=max_entities,
        pack=table,  # type: ignore[arg-type]
        translate=translate,
    )


def placed_own(a: Analysis, sources: list[str]) -> list[int]:
    """Own archetypes (with a .ydr) the CEntityDef rows of these maps place, most entities first."""
    uses: Counter = Counter()
    for label, ymap in a.maps:
        if label in sources:
            for e in ymap.entities:
                if not e.mlo and e.lod_level in (0, 5) and e.archetype in a.convertible:
                    uses[e.archetype] += 1
    return [k for k, _ in uses.most_common()]


def placed_external(a: Analysis, sources: list[str]) -> list[int]:
    """External archetypes (--archetypes-from) the CEntityDef rows of these maps place, most entities first."""
    uses: Counter = Counter()
    for label, ymap in a.maps:
        if label in sources:
            for e in ymap.entities:
                if not e.mlo and e.lod_level in (0, 5) and e.archetype in a.ext_keys:
                    uses[e.archetype] += 1
    return [k for k, _ in uses.most_common()]


# --------------------------------------------------------------------------------------------------
# naming and the default plan


def short(name: str, limit: int) -> str:
    return name[:limit].rstrip("_")


def role_id(base: str, role: str) -> str:
    stem, version = _VERSION.fullmatch(base).groups()
    return f"{stem}-{role}{version or ''}" if role else base


def slug(pack_id: str) -> str:
    return pack_id.removeprefix("gtavmenu-").replace("-", "_")


def bounds_member(pack_id: str, stem: str) -> str:
    """convert-bounds' row name, gmcol_<slug>_<stem> (at most 59 characters), with the prefix kept to 24
    characters; a longer prefix ends in a hash of the id, so two ids never share row names (the bounds store
    keys rows by name)."""
    prefix = "gmcol_" + slug(pack_id)
    if len(prefix) > 24:
        prefix = f"{short(prefix, 17)}_{joaat(pack_id) & 0xFFFFFF:06x}"
    return short(f"{prefix}_{re.sub(r'[^a-z0-9_]', '_', stem)}", 59) + ".pbn"


def place_text(text: str) -> str:
    return re.sub(r"[^\x20-\x7e]", "?", text).strip()[:38] or "map"


def format_place(point: tuple[int, int, int], text: str) -> str:
    return f"{point[0]},{point[1]},{point[2]}:{place_text(text)}"


def spawn_text(name: str) -> str:
    """Spawn > Objects row text of a props pack model: its name in words ("black_tiles" -> "Black Tiles")."""
    words = " ".join(w.capitalize() for w in name.split("_") if w)
    return words[: rp.SPAWN_TEXT_MAX].rstrip() or name[: rp.SPAWN_TEXT_MAX]


def props_spawns(table: dict) -> list[str]:
    """`--spawn object:NAME=TEXT` values for the converted models of a props pack table (pack_archetypes), by
    name, at most rp.SPAWN_MAX: the menu's Spawn > Objects (Custom) and Custom Packs > Objects rows, which
    Save Map stores by name with the pack id."""
    names = sorted(str(n) for k, n in (table.get("names") or {}).items() if k in (table.get("archetypes") or {}))
    return [f"object:{n}={spawn_text(n)}" for n in names[: rp.SPAWN_MAX]]


def mlo_place(a: Analysis, key: int, text: str) -> str:
    """Default teleport of a converted interior: 1 m above the origin of its CMloInstanceDef (convert_pc_mlo keeps
    the mod's placement); --map-teleport gmmlo_NAME=... overrides it."""
    label = a.mlo_maps[key][0]
    ymap = next(y for lab, y in a.maps if lab == label)
    e = next(e for e in ymap.entities if e.mlo and e.archetype == key)
    x, y, z = e.position
    return format_place((round(x), round(y), math.ceil(z + 1.0)), text)


@dataclass
class Options:
    pack_id: str
    name: str
    translate: tuple[float, float, float] = (0.0, 0.0, 0.0)
    bounds: str = "rows"
    drawn: str = "none"
    interiors: bool = True
    max_entities: int | None = None
    max_texture: int = 1024
    teleports: list[str] = field(default_factory=list)
    map_teleports: dict[str, str] = field(default_factory=dict)
    tools: dict[str, bool] = field(default_factory=dict)  # models / bounds / collision available
    props_id: str = ""  # --archetypes-id (default: ID with -props before the version)


def drawn_parts(a: Analysis, label: str, keys: list[int]) -> list[list[int]]:
    """The archetypes of one map's drawn collision, in rows of at most DRAWN_CHILDREN entities (one composite
    child each; first fit, most entities first). A map within the limit stays one row."""
    if not keys:
        return []
    uses: Counter = Counter()
    for source, ymap in a.maps:
        if source == label:
            uses.update(e.archetype for e in ymap.entities if not e.mlo and e.archetype in set(keys))
    parts: list[list[int]] = []
    sizes: list[int] = []
    for key in sorted(keys, key=lambda k: (-uses[k], a.names.get(k, ""))):
        for i, size in enumerate(sizes):
            if size + uses[key] <= DRAWN_CHILDREN:
                parts[i].append(key)
                sizes[i] += uses[key]
                break
        else:
            parts.append([key])
            sizes.append(uses[key])
    return parts if len(parts) > 1 else [keys]


def uses_external(a: Analysis, opts: Options) -> bool:
    """Whether the world maps keep --archetypes-from archetypes: a pack's always, a mod's with the model
    converter (it builds the props pack)."""
    return a.ext is not None and (a.ext.kind == "pack" or bool(opts.tools.get("models")))


def _world_groups(a: Analysis, sources: list[str], opts: Options) -> tuple[list[list[str]], int]:
    """Source maps per world pack and the entity limit per map: one pack when the maps fit at 300 (or at
    1024), else consecutive groups of whole .ymap files with at most 8 maps each."""
    limits = [opts.max_entities] if opts.max_entities else [ENTITIES_DEFAULT, ENTITIES_LARGE]
    own = set(placed_own(a, sources)) if opts.tools.get("models") else set()
    ext = uses_external(a, opts)
    for limit in limits:
        plan = simulate_maps(
            a, sources, own, prefix="gmsim", max_entities=limit, translate=opts.translate, external=ext
        )
        if len(plan.maps) <= rp.MAP_MAX:
            return [sources], limit
    limit = limits[-1]
    groups: list[list[str]] = [[]]
    for source in sources:
        count = len(
            simulate_maps(
                a, [source], own, prefix="gmsim", max_entities=limit, translate=opts.translate, external=ext
            ).maps
        )
        if count > rp.MAP_MAX:
            raise MapModError(f"{source} alone needs {count} maps at {limit} entities (a pack holds {rp.MAP_MAX})")
        current = groups[-1] + [source]
        if (
            groups[-1]
            and len(
                simulate_maps(
                    a, current, own, prefix="gmsim", max_entities=limit, translate=opts.translate, external=ext
                ).maps
            )
            > rp.MAP_MAX
        ):
            groups.append([source])
        else:
            groups[-1] = current
    seen: dict[int, int] = {}
    for g, group in enumerate(groups):
        for key in placed_own(a, group) if own else []:
            if seen.setdefault(key, g) != g:
                raise MapModError(
                    f"own model {a.names.get(key, hex(key))} is placed by maps of two world packs; "
                    "convert those .ymap files together with convert-model --maps"
                )
    return groups, limit


def default_plan(a: Analysis, opts: Options) -> dict:
    notes: list[str] = []
    packs: list[dict] = []
    translate = opts.translate
    props_id = opts.props_id or role_id(opts.pack_id, "props")
    # Interiors: placed, named MLOs of an archive mod.
    interior_keys = []
    for key in sorted(a.mlos, key=lambda k: a.mlo_names.get(k, "")):
        label = a.mlo_names.get(key) or f"0x{key:08x}"
        if key not in a.mlo_maps:
            notes.append(f"MLO {label}: defined but not placed by any .ymap (not converted)")
        elif len(a.mlo_maps[key]) != 1:
            notes.append(f"MLO {label}: placed by {len(a.mlo_maps[key])} .ymap files (convert_pc_mlo takes one)")
        elif key not in a.mlo_names:
            notes.append(f"MLO 0x{key:08x}: its name is not known (no file or index name hashes to it)")
        elif not opts.interiors:
            notes.append(f"MLO {label}: left out (--no-interiors)")
        elif a.inv.archive is None:
            notes.append(f"MLO {label}: interiors convert from a dlc.rpf only (convert_pc_mlo)")
        elif not a.index.archetypes:
            notes.append(f"MLO {label}: interiors need an archetype index (--archetype-index)")
        elif any(translate):
            raise MapModError(
                "--translate cannot move interiors (convert_pc_mlo keeps the mod's placement): add --no-interiors"
            )
        else:
            interior_keys.append(key)
    mlo_stems = set(a.mlo_names.values())
    # World: every map with a convertible CEntityDef.
    sources = []
    ext = uses_external(a, opts)
    for label, _ymap in a.maps:
        own = set(a.convertible) if opts.tools.get("models") else set()
        plan = simulate_maps(a, [label], own, prefix="gmsim", max_entities=4096, translate=translate, external=ext)
        if plan.maps:
            sources.append(label)
        else:
            what = ", ".join(item.split(": ", 1)[-1] for item in plan.map_data) or "no convertible entity"
            notes.append(f"{label.rsplit('/', 1)[-1]}: no world map ({what})")
    if sources:
        groups, limit = _world_groups(a, sources, opts)
        for g, group in enumerate(groups):
            pid = opts.pack_id if g == 0 else role_id(opts.pack_id, f"{g + 1}")
            s = slug(pid)
            own = placed_own(a, group)
            models = None
            if own and opts.tools.get("models"):
                models = {
                    "typ": short("gm_" + s, 40),
                    "select": "placed" if len(groups) == 1 else sorted(a.names[k] for k in own),
                    "renameTxd": [],
                    "lights": True,
                }
            elif own:
                notes.append(f"{len(own)} own models placed by the maps need the model converter (left out)")
            prefix = short("gmymap_" + s, 41)
            sim = simulate_maps(
                a,
                group,
                set(own) if models else set(),
                prefix=prefix,
                max_entities=limit,
                translate=translate,
                external=ext,
            )
            places = []
            for source in dict.fromkeys(m.source for m in sim.maps):
                parts = [m for m in sim.maps if m.source == source]
                point = convert_ymap.default_place([p for m in parts for p in m.positions])
                stem = source.rsplit("/", 1)[-1].rsplit(".ymap", 1)[0]
                text = opts.name if len(sim.maps) == len(parts) else f"{opts.name}: {stem}"
                places.append(opts.map_teleports.get(source_key(source)) or format_place(point, text))
            maps = {
                "sources": "all" if len(groups) == 1 else group,
                "prefix": prefix,
                "maxEntities": limit,
                "maxRetailTyps": rp.TYP_MAX - (1 if models else 0),
                "places": places,
            }
            if ext and (
                (a.ext.kind == "pack" and placed_external(a, group))
                or (opts.tools.get("models") and any(k in a.ext.convertible for k in placed_external(a, group)))
            ):
                maps["archetypesFrom"] = a.ext.pack_id or props_id
            packs.append(
                {
                    "id": pid,
                    "role": "world",
                    "archive": short("gmymap_" + s, 59) + ".rpf",
                    "description": f"{opts.name}: map" if len(groups) == 1 else f"{opts.name}: map part {g + 1}",
                    "maps": maps,
                    "models": models,
                    "bounds": [],
                    "drawn": [],
                    "places": [],
                }
            )
    # Interior packs: at most 8 MLOs each.
    world_own = {
        k
        for p in packs
        if p["models"]
        for k in placed_own(a, sources if p["maps"]["sources"] == "all" else p["maps"]["sources"])
    }
    world_txds = {a.txd[k] for k in world_own if a.txd.get(k)}
    for g in range(0, len(interior_keys), rp.MAP_MAX):
        keys = interior_keys[g : g + rp.MAP_MAX]
        pid = role_id(opts.pack_id, "int" if g == 0 else f"int{g // rp.MAP_MAX + 1}")
        s = slug(pid)
        typ = short("gm_" + s, 40)
        own_inside = mlo_own(a, keys) - world_own
        models = None
        if own_inside and opts.tools.get("models"):
            clash = sorted({a.txd[k] for k in own_inside if a.txd.get(k)} & world_txds)
            rename = [f"{t}={short(short('gm' + s, 20) + '_' + t, 59)}" for t in clash]
            models = {"typ": typ + "m", "renameTxd": rename, "lights": True}
        elif own_inside:
            notes.append(f"{len(own_inside)} own interior models need the model converter (their entities are dropped)")
        mlos = []
        for key in keys:
            name = a.mlo_names[key]
            place = opts.map_teleports.get(short("gmmlo_" + name, 59)) or mlo_place(
                a, key, f"{opts.name}: {name} (inside)"
            )
            mlos.append({"name": name, "map": short("gmmlo_" + name, 59), "place": place})
        bounds = []
        for key in keys:
            name = a.mlo_names[key]
            if name in a.inv.stems(".ybn"):
                if opts.tools.get("bounds"):
                    bounds.append({"member": f"{name}.pbn", "sources": [name]})
                else:
                    notes.append(f"{name}.ybn (interior collision) needs the bounds converter (left out)")
        packs.append(
            {
                "id": pid,
                "role": "interior",
                "archive": short("gmmlo_" + s, 59) + ".rpf",
                "description": f"{opts.name}: interiors",
                "typ": typ,
                "mlos": mlos,
                "models": models,
                "maxRetailTyps": rp.TYP_MAX - 1 - (1 if models else 0),
                "bounds": bounds,
                "places": [],
            }
        )
    # Exterior collision rows: every .ybn not named like an MLO (those are MLO-local).
    exterior = sorted(
        ((stem, size) for stem, (_label, size) in a.inv.stems(".ybn").items() if stem not in mlo_stems),
        key=lambda item: (-item[1], item[0]),
    )
    rows: list[dict] = []
    if exterior and not opts.tools.get("bounds"):
        notes.append(f"{len(exterior)} .ybn collision files need the bounds converter (left out)")
        exterior = []
    if opts.bounds == "merge" and len(exterior) > 1:
        group, size = [], 0
        for stem, nbytes in exterior:
            if group and size + nbytes > MERGE_SOURCE_BYTES:
                rows.append({"sources": [s for s, _ in group]})
                group, size = [], 0
            group.append((stem, nbytes))
            size += nbytes
        rows.append({"sources": [s for s, _ in group]})
    else:
        rows = [{"sources": [stem]} for stem, _ in exterior]
    # Drawn collision rows (opt-in): own models whose collision the model route drops, or all.
    drawn = []
    if opts.drawn != "none" and packs and packs[0]["role"] == "world":
        if not opts.tools.get("collision"):
            notes.append("--drawn-collision needs drawable_collision.py (left out)")
        else:
            from_mod = a.ext is not None and a.ext.kind == "mod"
            for p in [p for p in packs if p["role"] == "world" and (p["models"] or p["maps"].get("archetypesFrom"))]:
                group = sources if p["maps"]["sources"] == "all" else p["maps"]["sources"]
                for label in group:
                    keys = placed_own(a, [label]) if p["models"] else []
                    # Another mod's props: collision from their drawn shape too (a props pack has no rows).
                    extra = [k for k in placed_external(a, [label]) if k in a.ext.convertible] if from_mod else []
                    if opts.drawn == "embedded":
                        keys = [k for k in keys if a.own[k].get("physicsDictionary")]
                        extra = [k for k in extra if a.ext.rows[k].get("physicsDictionary")]
                    for part in drawn_parts(a, label, keys + extra):
                        drawn.append(
                            (
                                p["id"],
                                label,
                                sorted(a.names[k] for k in part if k not in a.ext_keys),
                                sorted(a.ext.names[k] for k in part if k in a.ext_keys),
                                len(drawn_parts(a, label, keys + extra)),
                            )
                        )
            if a.ext is not None and a.ext.kind == "pack" and a.ext_keys:
                notes.append(
                    f"drawn collision: the archetypes of pack {a.ext.pack_id} get none (their models are not here: "
                    "give their mod with --archetypes-from to build the props pack and the rows together)"
                )
    if opts.drawn != "none" and interior_keys:
        notes.append("drawn collision covers world maps only; interior models keep no collision of their own")
    world = next((p for p in packs if p["role"] == "world"), None)
    if world is None and rows:
        notes.append(f"{len(rows)} collision rows: no world map to attach them to; they go into collision packs")
    taken = 0
    leftover: list[dict] = []
    split: Counter = Counter()
    for pid, label, names, external, parts in drawn:
        target = next(p for p in packs if p["id"] == pid)
        stem = label.rsplit("/", 1)[-1].rsplit(".ymap", 1)[0]
        if parts > 1:
            split[label] += 1
            stem = f"{stem}_{split[label]}"
        row = {"member": bounds_member(pid, "drawn_" + stem), "map": label, "archetypes": names}
        if external:
            row["external"] = external
        if len(target["bounds"]) + len(target["drawn"]) < rp.BOUNDS_MAX:
            target["drawn"].append(row)
        else:
            notes.append(f"drawn collision of {label}: no bounds row left in {pid}")
    for row in rows:
        if world is not None and len(world["bounds"]) + len(world["drawn"]) < rp.BOUNDS_MAX:
            stem = row["sources"][0] if len(row["sources"]) == 1 else f"m{taken}"
            world["bounds"].append({"member": bounds_member(world["id"], stem), "sources": row["sources"]})
            taken += 1
        else:
            leftover.append(row)
    for g in range(0, len(leftover), rp.BOUNDS_MAX):
        pid = role_id(opts.pack_id, "col" if g == 0 else f"col{g // rp.BOUNDS_MAX + 1}")
        chunk = leftover[g : g + rp.BOUNDS_MAX]
        packs.append(
            {
                "id": pid,
                "role": "collision",
                "archive": short("gmcol_" + slug(pid), 59) + ".rpf",
                "description": f"{opts.name}: more collision",
                "bounds": [
                    {
                        "member": bounds_member(pid, r["sources"][0] if len(r["sources"]) == 1 else f"m{i}"),
                        "sources": r["sources"],
                    }
                    for i, r in enumerate(chunk)
                ],
                "places": [],
            }
        )
    # Another mod's props the maps place: one props pack (typ + models + textures), listed last.
    if a.ext is not None and a.ext.kind == "mod" and a.ext_keys:
        placed = placed_external(a, sources)
        keys = [k for k in placed if k in a.ext.convertible]
        missing = [a.ext.names.get(k) or f"hash_{k:08x}" for k in placed if k not in a.ext.convertible]
        if missing:
            notes.append(f"{len(missing)} archetypes of {a.ext.label()} have no .ydr there: {', '.join(missing[:8])}")
        if not opts.tools.get("models"):
            notes.append(f"{len(keys)} props of {a.ext.label()} need the model converter (their entities are dropped)")
        elif keys:
            s = slug(props_id)
            packs.append(
                {
                    "id": props_id,
                    "role": "props",
                    "archive": short("gmprop_" + s, 59) + ".rpf",
                    "description": f"{opts.name}: props",
                    "models": {
                        "typ": short("gm_" + s, 40),
                        "select": sorted(a.ext.names[k] for k in keys),
                        "renameTxd": [],
                        "txdAlias": txd_aliases(a.ext, keys),
                        "lights": True,
                        "fitBox": True,
                    },
                    "bounds": [],
                    "places": [],
                }
            )
    if a.ext is not None and not a.ext_keys:
        notes.append(f"--archetypes-from {a.ext.label()}: the maps place none of its archetypes")
    # --teleport rows: on the first pack.
    if opts.teleports:
        if not packs:
            raise MapModError("--teleport: the mod converts into no pack")
        packs[0]["places"] = list(opts.teleports)
    known = {m["map"] for p in packs if p["role"] == "interior" for m in p["mlos"]} | {source_key(s) for s in sources}
    unknown = sorted(set(opts.map_teleports) - known)
    if unknown:
        raise MapModError(f"--map-teleport {unknown[0]}: not a map of this plan (a .ymap name or an interior map)")
    return {
        "schema": SCHEMA,
        "id": opts.pack_id,
        "translate": list(translate),
        "maxTextureSize": opts.max_texture,
        "packs": packs,
        "notes": notes,
    }


def source_key(label: str) -> str:
    """A source map's --map-teleport key: its file name without .ymap / .ymap.xml, lower case."""
    return label.rsplit("/", 1)[-1].lower().removesuffix(".xml").removesuffix(".ymap")


def mlo_own(a: Analysis, keys: list[int]) -> set[int]:
    """Own archetypes (with a .ydr) the entities of these MLOs place."""
    out = set()
    for key in keys:
        for entity in a.mlos[key][1].get("entities") or []:
            value = convert_pc_mlo._hash(entity.get("archetypeName"))
            if value in a.convertible:
                out.add(value)
    return out


# --------------------------------------------------------------------------------------------------
# plan checks and cap use


def _check(condition: bool, message: str) -> None:
    if not condition:
        raise MapModError(message)


def _places(values, label: str) -> None:
    _check(isinstance(values, list), f"{label}: places must be a list")
    for value in values:
        match = _PLACE.fullmatch(str(value))
        _check(match is not None, f"{label}: place {value!r} is not X,Y,Z[:TEXT] (whole metres)")
        _check(all(abs(int(v)) <= rp.PLACE_COORD_MAX for v in match.groups()[:3]), f"{label}: place out of range")


def check_plan(plan: dict, a: Analysis, opts: Options) -> list[dict]:
    """Validate a (default or edited) plan against the mod and the caps; return the per-pack cap use."""
    _check(plan.get("schema") == SCHEMA, f"not a {SCHEMA} plan")
    packs = plan.get("packs")
    _check(isinstance(packs, list) and packs, "the plan has no packs")
    _check(len(packs) <= rp.ACTIVE_MAX, f"{len(packs)} packs: at most {rp.ACTIVE_MAX} can be active together")
    ids = [p.get("id") for p in packs]
    _check(all(isinstance(i, str) and _ID.fullmatch(i) and len(i) <= rp.ID_MAX for i in ids), "invalid pack id")
    _check(len(set(ids)) == len(ids), "pack ids must be distinct")
    archives = [p.get("archive") for p in packs]
    _check(all(isinstance(x, str) and _ARCHIVE.fullmatch(x) for x in archives), "archives are NAME.rpf ([a-z0-9_])")
    _check(len(set(archives)) == len(archives), "archive names must be distinct")
    translate = plan.get("translate") or [0, 0, 0]
    _check(len(translate) == 3 and all(isinstance(v, (int, float)) for v in translate), "translate is [dx, dy, dz]")
    mts = plan.get("maxTextureSize", 1024)
    _check(isinstance(mts, int) and 1 <= mts <= 16384, "maxTextureSize is a positive number")
    ybn = a.inv.stems(".ybn")
    members: Counter = Counter()
    used_ybn: Counter = Counter()
    sources_all = [label for label, _ in a.maps]
    usage = []
    props_ids = {p.get("id") for p in packs if isinstance(p, dict) and p.get("role") == "props"}
    for p in packs:
        _check(isinstance(p, dict), "every pack of the plan is an object")
        for key, default in (("bounds", []), ("drawn", []), ("places", [])):
            p.setdefault(key, default)
        role = p.get("role")
        _check(role in ROLES, f"{p['id']}: role is one of {', '.join(ROLES)}")
        _places(p.get("places", []), p["id"])
        desc = p.get("description", "")
        _check(isinstance(desc, str) and len(desc) <= rp.DESCRIPTION_MAX, f"{p['id']}: description too long")
        rows = list(p.get("bounds") or [])
        drawn = list(p.get("drawn") or [])
        for row in rows:
            _check(
                re.fullmatch(r"[a-z0-9_]{1,59}\.pbn", str(row.get("member", ""))) is not None, f"{p['id']}: bad member"
            )
            srcs = row.get("sources") or []
            _check(srcs and all(s in ybn for s in srcs), f"{p['id']}: bounds sources must be .ybn of the mod")
            used_ybn.update(srcs)
            members[row["member"]] += 1
        use = {
            "pack": p["id"],
            "role": role,
            "maps": 0,
            "entities": [],
            "typs": 0,
            "typsWanted": 0,
            "bounds": len(rows) + len(drawn),
            "places": len(p.get("places", [])),
            "bytes": 0,
        }
        if role == "world":
            maps = p.get("maps") or {}
            _check(isinstance(maps, dict), f"{p['id']}: maps is an object")
            p["maps"] = maps
            srcs = maps.get("sources")
            group = sources_all if srcs == "all" else srcs
            _check(
                isinstance(group, list) and group and all(s in sources_all for s in group),
                f"{p['id']}: maps.sources is \"all\" or a list of the mod's .ymap files",
            )
            _check(
                srcs == "all"
                or a.inv.archive is None
                or len({s.rsplit("/", 1)[-1].lower() for s in group}) == len(group),
                f"{p['id']}: two maps of the list share a file name",
            )
            _check(_PREFIX.fullmatch(str(maps.get("prefix", ""))) is not None, f"{p['id']}: bad map prefix")
            limit = maps.setdefault("maxEntities", ENTITIES_DEFAULT)
            _check(isinstance(limit, int) and 1 <= limit <= convert_ymap.MAX_MAP_ENTITIES, f"{p['id']}: maxEntities")
            models = p.get("models")
            own: set[int] = set()
            if models:
                _check(opts.tools.get("models"), f"{p['id']}: own models need the model converter")
                _check(_NAME.fullmatch(str(models.get("typ", ""))) is not None, f"{p['id']}: bad models typ")
                select = models.setdefault("select", "placed")
                if select == "placed":
                    own = set(placed_own(a, group))
                else:
                    _check(
                        isinstance(select, list) and all(joaat(n) in a.convertible for n in select),
                        f"{p['id']}: models.select names own models with a .ydr",
                    )
                    own = {joaat(n) for n in select}
                _renames(models, p["id"])
                use["bytes"] += model_bytes(a, own)
            rt = maps.setdefault("maxRetailTyps", rp.TYP_MAX - (1 if models else 0))
            _check(isinstance(rt, int) and 0 <= rt <= rp.TYP_MAX - (1 if models else 0), f"{p['id']}: maxRetailTyps")
            source = maps.get("archetypesFrom")
            if source is not None:
                _check(a.ext is not None, f"{p['id']}: maps.archetypesFrom needs --archetypes-from")
                _check(
                    source == a.ext.pack_id if a.ext.kind == "pack" else source in props_ids,
                    f"{p['id']}: maps.archetypesFrom names "
                    + (f"the pack {a.ext.pack_id}" if a.ext.kind == "pack" else "a props pack of this plan"),
                )
            sim = simulate_maps(
                a,
                group,
                own,
                prefix=maps["prefix"],
                max_entities=limit,
                translate=tuple(translate),
                external=source is not None,
            )
            residency = (a.ext_index if source is not None else a.index).residency
            typs, _warn = convert_ymap.retail_typs(sim, max(rt, 1), frozenset(), residency)
            wanted, _ = convert_ymap.retail_typs(sim, 10000, frozenset(), residency)
            places = maps.setdefault("places", [])
            _places(places, p["id"])
            sources_kept = len({m.source for m in sim.maps})
            _check(
                not places or len(places) == sources_kept,
                f"{p['id']}: maps.places needs one point per converted .ymap ({sources_kept})",
            )
            use.update(
                maps=len(sim.maps),
                entities=[len(m.entities) for m in sim.maps],
                typs=(1 if models else 0) + (len(typs) if rt else 0),
                typsWanted=(1 if models else 0) + len(wanted),
                dropped=dict(sim.dropped),
                unresolved=[
                    a.names.get(k) or f"hash_{k:08x}"
                    for k, _n in sim.uses.most_common()
                    if sim.classes.get(k) in ("custom", "unknown") and k not in a.mlos
                ],
                mapData=list(sim.map_data),
            )
            if source is not None:
                use["external"] = sorted(a.names.get(k) or f"hash_{k:08x}" for k in sim.kept if k in a.ext_keys)
                use["externalEntities"] = sum(n for k, n in sim.kept.items() if k in a.ext_keys)
            use["bytes"] += sum(16384 + 160 * len(m.entities) for m in sim.maps)
            _check(len(sim.maps) <= rp.MAP_MAX, f"{p['id']}: {len(sim.maps)} maps (a pack holds {rp.MAP_MAX})")
            for row in drawn:
                _check(opts.tools.get("collision"), f"{p['id']}: drawn rows need drawable_collision.py")
                _check(row.get("map") in group, f"{p['id']}: drawn.map must be one of the pack's maps")
                _check(
                    re.fullmatch(r"[a-z0-9_]{1,59}\.pbn", str(row.get("member", ""))) is not None,
                    f"{p['id']}: bad member",
                )
                external = list(row.get("external") or [])
                _check(
                    all(joaat(str(n)) in a.convertible for n in row.get("archetypes") or ([] if external else [""])),
                    f"{p['id']}: drawn.archetypes names own models with a .ydr",
                )
                _check(
                    not external
                    or (
                        source is not None
                        and a.ext.kind == "mod"
                        and all(joaat(str(n)) in a.ext.convertible for n in external)
                    ),
                    f"{p['id']}: drawn.external names --archetypes-from models with a .ydr (maps.archetypesFrom set)",
                )
                members[row["member"]] += 1
                use["bytes"] += 1024 * 1024
        elif role == "interior":
            _check(
                a.inv.archive is not None and a.index.archetypes, f"{p['id']}: interiors need a dlc.rpf and an index"
            )
            _check(not any(translate), f"{p['id']}: interiors cannot be translated")
            _check(_NAME.fullmatch(str(p.get("typ", ""))) is not None, f"{p['id']}: bad typ name")
            mlos = p.get("mlos") or []
            _check(0 < len(mlos) <= rp.MAP_MAX, f"{p['id']}: 1..{rp.MAP_MAX} MLOs")
            keys = []
            for m in mlos:
                key = joaat(str(m.get("name", "")))
                _check(
                    key in a.mlos and len(a.mlo_maps.get(key, [])) == 1, f"{p['id']}: {m.get('name')} is no placed MLO"
                )
                _check(_NAME.fullmatch(str(m.get("map", ""))) is not None, f"{p['id']}: bad map name")
                if m.get("place") is not None:
                    _places([m["place"]], p["id"])
                keys.append(key)
            models = p.get("models")
            rt = p.setdefault("maxRetailTyps", rp.TYP_MAX - 1 - (1 if models else 0))
            _check(
                isinstance(rt, int) and 0 <= rt <= rp.TYP_MAX - 1 - (1 if models else 0), f"{p['id']}: maxRetailTyps"
            )
            if models:
                _check(opts.tools.get("models"), f"{p['id']}: own models need the model converter")
                _check(_NAME.fullmatch(str(models.get("typ", ""))) is not None, f"{p['id']}: bad models typ")
                _renames(models, p["id"])
                use["bytes"] += model_bytes(a, mlo_own(a, keys))
            retail = interior_retail(a, keys, rt)
            use.update(
                maps=len(mlos),
                entities=[len(a.mlos[k][1].get("entities") or []) for k in keys],
                typs=1 + (1 if models else 0) + len(retail[0]),
                typsWanted=1 + (1 if models else 0) + retail[1],
            )
            use["bytes"] += 65536 * len(mlos)
        elif role == "props":
            _check(a.ext is not None and a.ext.kind == "mod", f"{p['id']}: a props pack needs --archetypes-from MOD")
            _check(opts.tools.get("models"), f"{p['id']}: props need the model converter")
            models = p.get("models")
            _check(isinstance(models, dict), f"{p['id']}: a props pack has models")
            _check(_NAME.fullmatch(str(models.get("typ", ""))) is not None, f"{p['id']}: bad models typ")
            select = models.get("select")
            _check(
                isinstance(select, list) and select and all(joaat(str(n)) in a.ext.convertible for n in select),
                f"{p['id']}: models.select names models of --archetypes-from with a .ydr",
            )
            _renames(models, p["id"])
            for item in models.get("txdAlias") or []:
                value, _, name = str(item).partition("=")
                _check(
                    re.fullmatch(r"0x[0-9a-f]{8}", value) and _NAME.fullmatch(name), f"{p['id']}: txdAlias HASH=NAME"
                )
            _check(not rows, f"{p['id']}: a props pack has no bounds rows (drawn rows go with the maps)")
            use.update(typs=1, typsWanted=1)
            use["bytes"] += external_bytes(a, {joaat(str(n)) for n in select})
        else:
            _check(rows, f"{p['id']}: a collision pack needs bounds rows")
        if rows and not opts.tools.get("bounds"):
            raise MapModError(f"{p['id']}: bounds rows need the bounds converter")
        _check(use["bounds"] <= rp.BOUNDS_MAX, f"{p['id']}: {use['bounds']} bounds rows (a pack holds {rp.BOUNDS_MAX})")
        _check(use["places"] <= rp.PLACE_MAX, f"{p['id']}: {use['places']} place rows (at most {rp.PLACE_MAX})")
        _check(use["typs"] <= rp.TYP_MAX, f"{p['id']}: {use['typs']} typ rows (at most {rp.TYP_MAX})")
        use["bytes"] += int(PBN_FACTOR * sum(ybn[s][1] for r in rows for s in r["sources"]))
        usage.append(use)
    twice = [m for m, n in members.items() if n > 1]
    _check(not twice, f"bounds member {twice[0] if twice else ''} is used twice")
    again = [s for s, n in used_ybn.items() if n > 1]
    _check(not again, f"{again[0] if again else ''}.ybn is converted into two rows")
    total = {k: sum(u[k] for u in usage) for k in ("maps", "typs", "bounds", "places", "bytes")}
    _check(total["maps"] <= rp.MAP_CAP, f"{total['maps']} maps in the set (merged cap {rp.MAP_CAP})")
    _check(total["typs"] <= rp.TYP_CAP, f"{total['typs']} typ rows in the set (merged cap {rp.TYP_CAP})")
    _check(total["bounds"] <= rp.BOUNDS_CAP, f"{total['bounds']} bounds rows in the set (merged cap {rp.BOUNDS_CAP})")
    return usage


def _renames(models: dict, pid: str) -> None:
    for item in models.get("renameTxd") or []:
        old, _, new = str(item).partition("=")
        _check(_NAME.fullmatch(old) is not None and _NAME.fullmatch(new) is not None, f"{pid}: renameTxd OLD=NEW")


def interior_retail(a: Analysis, keys: list[int], limit: int) -> tuple[list[str], int]:
    """(retail typ rows chosen, retail typs wanted) of these MLOs (convert_pc_mlo's rule)."""
    rows = []
    for key in keys:
        mlo = json.loads(json.dumps(a.mlos[key][1]))
        found, _drop = convert_pc_mlo.classify(mlo, a.own, a.index, dict.fromkeys(a.convertible, "sim"), a.names, False)
        rows += found
    chosen, rest = convert_pc_mlo.retail_rows(rows, min(limit, rp.TYP_MAX - 1))
    return chosen, len(chosen) + len(rest)


def external_bytes(a: Analysis, keys: set[int]) -> int:
    """model_bytes for --archetypes-from models."""
    ydr, ytd = a.ext.inv.stems(".ydr"), a.ext.inv.stems(".ytd")
    total = sum(ydr[a.ext.names[k]][1] for k in keys if a.ext.names.get(k) in ydr)
    return total + sum(ytd[t][1] for t in {external_txd(a.ext, k) for k in keys} if t in ytd)


def model_bytes(a: Analysis, keys: set[int]) -> int:
    """Source bytes of these models and their texture dictionaries (the converted members come out
    at roughly the same size: MalibuMansion 0.8-0.9x)."""
    ydr, ytd = a.inv.stems(".ydr"), a.inv.stems(".ytd")
    total = sum(ydr[a.names[k]][1] for k in keys if a.names.get(k) in ydr)
    return total + sum(ytd[t][1] for t in {a.txd.get(k) for k in keys} if t in ytd)


# --------------------------------------------------------------------------------------------------
# printing


def _mib(n: int) -> str:
    return f"{n / (1024 * 1024):.1f} MiB"


def describe(plan: dict, usage: list[dict], a: Analysis) -> list[str]:
    inv = a.inv
    lines = ["inventory:"]
    counts = [f"{len(inv.stems(s))} {s}" for s in SUFFIXES if inv.stems(s)]
    lines.append(
        f"  {', '.join(counts) or 'no map files'}" + (f"; other files: {dict(inv.others)}" if inv.others else "")
    )
    placed = sum(1 for _l, y in a.maps for e in y.entities if not e.mlo)
    lines.append(
        f"  {len(a.maps)} .ymap ({placed} entities), {len(a.own)} own archetypes ({len(a.convertible)} with a .ydr), "
        f"{len(a.mlos)} MLOs ({len(a.mlo_maps)} placed)"
    )
    for item in inv.editor:
        lines.append(f"  Map Editor object list (not converted): {item}")
    if a.ext is not None:
        ext = a.ext
        what = (
            f"pack {ext.pack_id}: {len(ext.rows)} archetypes"
            if ext.kind == "pack"
            else f"{ext.root.name}: {len(ext.rows)} archetypes ({len(ext.convertible)} with a .ydr)"
        )
        uses = Counter(e.archetype for _l, y in a.maps for e in y.entities if e.archetype in a.ext_keys)
        lines.append(
            f"  --archetypes-from {what}; the maps place {len(a.ext_keys)} of them ({sum(uses.values())} entities)"
        )
    for item in inv.unreadable:
        lines.append(f"  unreadable: {item}")
    lines.append(
        f"plan: {len(plan['packs'])} pack(s)" + (f", translate {plan['translate']}" if any(plan["translate"]) else "")
    )
    for p, u in zip(plan["packs"], usage, strict=True):
        lines.append(f"  {p['id']} ({p['role']}) archive {p['archive']}: {p.get('description', '')}")
        if p["role"] == "world":
            m = p["maps"]
            src = "all .ymap" if m["sources"] == "all" else ", ".join(s.rsplit("/", 1)[-1] for s in m["sources"])
            lines.append(
                f"    maps {m['prefix']}_N from {src}: {u['maps']} map(s) of {u['entities']} entities "
                f"(<= {m['maxEntities']} each)"
            )
            if p.get("models"):
                lines.append(
                    f"    own models -> {p['models']['typ']}.ptyp ({p['models']['select'] if isinstance(p['models']['select'], str) else len(p['models']['select'])})"
                )
            for place in m.get("places") or []:
                lines.append(f"    map teleport {place}")
            for reason, count in sorted((u.get("dropped") or {}).items()):
                lines.append(f"    dropped: {count} x {reason}")
            names = u.get("unresolved") or []
            if names:
                lines.append(
                    f"    {len(names)} archetypes are neither stock nor converted here (models of another mod or "
                    f"of a Map Builder style prop pack): {', '.join(names[:10])}{' ...' if len(names) > 10 else ''}"
                )
                if a.ext is None:
                    lines.append("    (give that mod with --archetypes-from to convert its props into a props pack)")
            if m.get("archetypesFrom"):
                names = u.get("external") or []
                lines.append(
                    f"    {u.get('externalEntities', 0)} entities of {len(names)} archetypes from {m['archetypesFrom']}"
                    f" (load together): {', '.join(names[:8])}{' ...' if len(names) > 8 else ''}"
                )
            for row in p.get("drawn") or []:
                every = [*row["archetypes"], *(row.get("external") or [])]
                lines.append(
                    f"    drawn collision {row['member']} <- {len(every)} models of "
                    f"{row['map'].rsplit('/', 1)[-1]}: {', '.join(every[:6])}"
                    f"{' ...' if len(every) > 6 else ''}"
                )
        elif p["role"] == "interior":
            lines.append(
                f"    MLOs -> {p['typ']}.ptyp: " + ", ".join(f"{m['name']} -> {m['map']}.pmap" for m in p["mlos"])
            )
            if p.get("models"):
                renames = p["models"].get("renameTxd") or []
                lines.append(
                    f"    own models -> {p['models']['typ']}.ptyp"
                    + (f", dictionaries renamed {renames}" if renames else "")
                )
            for m in p["mlos"]:
                if m.get("place"):
                    lines.append(f"    interior teleport {m['map']} {m['place']}")
        elif p["role"] == "props":
            models = p["models"]
            lines.append(
                f"    {len(models['select'])} models of {a.ext.label()} -> {models['typ']}.ptyp (no maps; the map "
                f"packs place them): {', '.join(models['select'][:8])}{' ...' if len(models['select']) > 8 else ''}"
            )
            if models.get("txdAlias"):
                lines.append(f"    {len(models['txdAlias'])} texture dictionaries renamed to member names")
        for row in p.get("bounds") or []:
            lines.append(f"    bounds {row['member']} <- {' + '.join(s + '.ybn' for s in row['sources'])}")
        for place in p.get("places") or []:
            lines.append(f"    place {place}")
    lines.append("cap use (per pack: used/limit):")
    for u in usage:
        typs = f"{u['typs']}/{rp.TYP_MAX}" + (f" (wanted {u['typsWanted']})" if u["typsWanted"] > u["typs"] else "")
        lines.append(
            f"  {u['pack']}: maps {u['maps']}/{rp.MAP_MAX}, entities/map max {max(u['entities'] or [0])}, "
            f"typ rows {typs}, bounds {u['bounds']}/{rp.BOUNDS_MAX}, places {u['places']}/{rp.PLACE_MAX}, "
            f"archives ~{_mib(u['bytes'])} (estimate; {rp.ARCHIVE_MAX} x {_mib(ARCHIVE_BUDGET)})"
        )
    total = {k: sum(u[k] for u in usage) for k in ("maps", "typs", "bounds", "places", "bytes")}
    lines.append(
        f"  set: packs {len(usage)}/{rp.ACTIVE_MAX}, maps {total['maps']}/{rp.MAP_CAP}, typ rows {total['typs']}/{rp.TYP_CAP}, "
        f"bounds {total['bounds']}/{rp.BOUNDS_CAP}, places {total['places']}/{rp.PLACE_CAP}, "
        f"archives ~{_mib(total['bytes'])}/{_mib(SET_BUDGET)}"
    )
    used = {s for p in plan["packs"] for row in p.get("bounds") or [] for s in row["sources"]}
    left = sorted(set(inv.stems(".ybn")) - used)
    if left:
        lines.append(f"note: collision files in no row: {', '.join(s + '.ybn' for s in left)}")
    for note in plan.get("notes") or []:
        lines.append(f"note: {note}")
    return lines


# --------------------------------------------------------------------------------------------------
# execution


@dataclass
class Context:
    a: Analysis
    plan: dict
    work: Path
    packs_root: Path
    index: Path | None
    templates: Path | None
    reference: Path | None
    tools: dict[str, Path]
    model_path: str | None
    props_tables: dict[str, Path] = field(default_factory=dict)  # props pack id -> its pack-archetypes table


def run(log: Path, argv: list, env: dict | None = None) -> None:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w", encoding="utf-8") as out:
        code = subprocess.run(
            [str(v) for v in argv], stdout=out, stderr=subprocess.STDOUT, env=env, check=False
        ).returncode
    if code:
        tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-20:]
        for line in tail:
            print(line, file=sys.stderr)
        raise StepError(f"{log.name} failed (log: {log})")


def py_tool(name: str) -> list:
    return [sys.executable, "-I", _HERE / f"{name}.py"]


def model_cmd(ctx: Context, tool: str) -> tuple[list, dict | None]:
    env = None
    if ctx.model_path:
        env = dict(os.environ, PYTHONPATH=ctx.model_path)
    return [sys.executable, "-P", ctx.tools[tool]], env


def template(ctx: Context, rel: str) -> Path:
    path = (ctx.templates or Path("-")) / rel
    if not path.is_file():
        raise MapModError(
            f"needs the retail template {rel} from your game: run ./menu-ctl.sh fetch-templates "
            f"(it fills {ctx.templates}; or pass --templates DIR)"
        )
    return path


def carriers(ctx: Context) -> list:
    out = []
    for folder in ("shader-carriers", "shader-carriers-interior"):
        directory = (ctx.templates or Path("-")) / folder
        files = sorted((p for p in directory.glob("*.pdr") if p.is_file()), key=lambda p: os.fsencode(p.name))
        out += [v for p in files for v in ("--shader-template", p)]
    if not out:
        raise MapModError("needs the retail shader carriers from your game: run ./menu-ctl.sh fetch-templates")
    return out


def mod_file(ctx: Context, suffix: str, stem: str) -> Path:
    """A mod file by type and base name: extracted from the archive once, or the folder's own file."""
    inv = ctx.a.inv
    label, _size = inv.stems(suffix)[stem]
    if inv.archive is None:
        return inv.paths[label]
    folder = ctx.work / "extract" / suffix.lstrip(".")
    target = folder / f"{stem}{suffix}"
    if not target.is_file():
        if folder.exists():
            shutil.rmtree(folder)
        run(
            ctx.work / f"extract-{suffix.lstrip('.')}.log",
            [*py_tool("extract_mod_files"), inv.archive, "--suffix", suffix, "--out", folder],
        )
    return target


def external_source(ctx: Context) -> Path:
    """A folder with the --archetypes-from files this plan needs (its models, their .ytd, its .ytyp files),
    extracted once: the props pack converts from it and drawn rows read its models. A whole Map Builder archive
    is 0.8 GB; the villa needs 15 of its 1,098 models."""
    folder = ctx.work / "props-source"
    if folder.is_dir():
        return folder
    ext = ctx.a.ext
    names = {n for p in ctx.plan["packs"] if p["role"] == "props" for n in p["models"]["select"]}
    names |= {n for p in ctx.plan["packs"] for row in p.get("drawn") or [] for n in row.get("external") or []}
    wanted = {
        ".ydr": sorted(names),
        ".ytd": sorted({t for n in names if (t := external_txd(ext, joaat(n))) in ext.inv.stems(".ytd")}),
        ".ytyp": sorted(ext.inv.stems(".ytyp")),
    }
    staging = ctx.work / "props-source.part"
    shutil.rmtree(staging, ignore_errors=True)
    for suffix, stems in wanted.items():
        if not stems:
            continue
        if ext.inv.archive is None:
            staging.mkdir(parents=True, exist_ok=True)
            for stem in stems:
                shutil.copyfile(ext.inv.paths[ext.inv.stems(suffix)[stem][0]], staging / f"{stem}{suffix}")
            continue
        only = [v for stem in stems for v in ("--only", stem)] if suffix != ".ytyp" else []
        run(
            ctx.work / f"props-extract-{suffix.lstrip('.')}.log",
            [*py_tool("extract_mod_files"), ext.inv.archive, "--suffix", suffix, "--out", staging, *only],
        )
    staging.rename(folder)
    return folder


def map_inputs(ctx: Context, sources) -> list:
    inv = ctx.a.inv
    if sources == "all":
        return [inv.archive] if inv.archive is not None else sorted(inv.loose, key=lambda p: os.fsencode(str(p)))
    out = [
        (
            mod_file(ctx, ".ymap", s.rsplit("/", 1)[-1].lower().removesuffix(".ymap"))
            if inv.archive is not None
            else inv.paths[s]
        )
        for s in sources
    ]
    models = [p for p in inv.loose if p.name.lower().endswith((".ytyp", ".ydr", ".yft", ".ydd"))]
    return out + models


def models_step(ctx: Context, out: Path, models: dict, select: list, log: Path, source: Path | None = None) -> Path:
    cmd, env = model_cmd(ctx, "models")
    copy = source or ctx.a.inv.archive or ctx.a.inv.root
    renames = [v for r in models.get("renameTxd") or [] for v in ("--rename-txd", r)]
    renames += [v for r in models.get("txdAlias") or [] for v in ("--txd-alias", r)]
    renames += ["--fit-bbox"] if models.get("fitBox") else []
    run(
        log,
        [
            *cmd,
            copy,
            *select,
            "--template",
            template(ctx, "shader-carriers/ch3_03_ss_cb.pdr"),
            *carriers(ctx),
            "--typ-template",
            template(ctx, "typ-templates/v_int_22.ptyp"),
            "--typ-name",
            models["typ"],
            *(["--reference-dir", ctx.reference] if ctx.reference else []),
            "--templates",
            ctx.templates,
            "--max-texture-size",
            str(ctx.plan.get("maxTextureSize", 1024)),
            *renames,
            *([] if models.get("lights", True) else ["--no-lights"]),
            "--out",
            out,
        ],
        env,
    )
    return out / "pack-archetypes.json"


def bounds_rows(ctx: Context, p: dict, folder: Path) -> list:
    args = []
    translate = [str(v) for v in ctx.plan.get("translate") or [0, 0, 0]]
    move = ["--translate", *translate] if any(float(v) for v in translate) else []
    for row in p.get("bounds") or []:
        member = row["member"]
        stem = member.removesuffix(".pbn")
        files = [mod_file(ctx, ".ybn", s) for s in row["sources"]]
        out = folder / member
        verb = ["convert", files[0]] if len(files) == 1 else ["merge", *files]
        if p["role"] == "interior":
            # convert-mlo --bounds: MLO-local, never translated.
            run(
                folder / f"{stem}.log",
                [
                    *py_tool_path(ctx, "bounds"),
                    "convert",
                    files[0],
                    "--include",
                    INCLUDE,
                    "--output",
                    out,
                    "--report",
                    folder / f"{stem}.json",
                    *bound_templates(ctx),
                ],
            )
        else:
            run(
                folder / f"{stem}.log",
                [
                    *py_tool_path(ctx, "bounds"),
                    *verb,
                    "--output",
                    out,
                    "--report",
                    folder / f"{stem}.json",
                    *move,
                    "--include",
                    INCLUDE,
                    *bound_templates(ctx),
                ],
            )
        args += ["--member", f"{member}={out}", "--bounds", member]
        print(f"  {member} <- {' + '.join(s + '.ybn' for s in row['sources'])}: {out.stat().st_size} bytes")
    return args


def py_tool_path(ctx: Context, tool: str) -> list:
    return [sys.executable, "-I", ctx.tools[tool]]


def bound_templates(ctx: Context) -> list:
    """--template rows for the bounds converter: the cached retail bounds (bounds-templates/), whose class
    tags the converted rows take."""
    directory = (ctx.templates or Path("-")) / "bounds-templates"
    files = sorted(
        (p for p in directory.glob("*") if p.is_file() and p.suffix in (".pbn", ".pdr")),
        key=lambda p: os.fsencode(p.name),
    )
    if not files:
        raise MapModError(
            "collision rows need the retail bounds templates (bounds-templates/) from your game: run ./menu-ctl.sh "
            f"fetch-templates (it fills {ctx.templates}; or pass --templates DIR)"
        )
    return [v for f in files for v in ("--template", f)]


def drawn_rows(ctx: Context, p: dict, folder: Path) -> list:
    args = []
    translate = [str(v) for v in ctx.plan.get("translate") or [0, 0, 0]]
    for row in p.get("drawn") or []:
        member = row["member"]
        stem = member.removesuffix(".pbn")
        ymap = (
            mod_file(ctx, ".ymap", row["map"].rsplit("/", 1)[-1].lower().removesuffix(".ymap"))
            if ctx.a.inv.archive is not None
            else ctx.a.inv.paths[row["map"]]
        )
        drawables = folder / f"{stem}-ydr"
        drawables.mkdir(parents=True, exist_ok=True)
        for name in row["archetypes"]:
            source = mod_file(ctx, ".ydr", name)
            shutil.copyfile(source, drawables / f"{name}.ydr")
        for name in row.get("external") or []:
            shutil.copyfile(external_source(ctx) / f"{name}.ydr", drawables / f"{name}.ydr")
        every = [*row["archetypes"], *(row.get("external") or [])]
        cmd, env = model_cmd(ctx, "collision")
        run(
            folder / f"{stem}.log",
            [
                *cmd,
                ymap,
                "--drawables",
                drawables,
                *[v for n in every for v in ("--archetype", n)],
                "--output",
                folder / member,
                "--report",
                folder / f"{stem}.json",
                *(["--translate", *translate] if any(float(v) for v in translate) else []),
                *bound_templates(ctx),  # the composite and BVH class tags, as for the .ybn rows
                *(["--reference-dir", ctx.reference] if ctx.reference else []),
            ],
            env,
        )
        args += ["--member", f"{member}={folder / member}", "--bounds", member]
        print(f"  {member} <- drawn geometry of {len(every)} models: {(folder / member).stat().st_size} bytes")
    return args


def _flag(args: list, name: str) -> str:
    return args[args.index(name) + 1]


def split_archives(args: list, archive: str) -> list:
    """Move the largest model members into further archives when the first would pass 64 MiB."""
    members = []
    i = 0
    while i < len(args):
        item = args[i]
        if item == "--member" or (isinstance(item, str) and item.startswith("--member=")):
            spec = args[i + 1] if item == "--member" else item.split("=", 1)[1]
            name, _, path = spec.partition("=")
            members.append((name, Path(path).stat().st_size))
            i += 2 if item == "--member" else 1
        else:
            i += 1

    def size(group: list) -> int:
        names = sum(len(n) + 1 for n, _ in group) + 1
        head = 16 + 16 * (len(group) + 1) + names + (-names % 16)
        return head + (-head % BLOCK) + sum(s + (-s % BLOCK) for _, s in group)

    if size(members) <= ARCHIVE_BUDGET:
        return []
    movable = sorted((m for m in members if m[0].endswith((".ptd", ".pdr"))), key=lambda m: (-m[1], m[0]))
    first = [m for m in members if m not in movable]
    extra: list[list] = []
    for member in movable:
        if size([*first, member]) <= ARCHIVE_BUDGET:
            first.append(member)
            continue
        for group in extra:
            if size([*group, member]) <= ARCHIVE_BUDGET:
                group.append(member)
                break
        else:
            extra.append([member])
    if len(extra) > rp.ARCHIVE_MAX - 1:
        raise MapModError(
            f"{archive}: the members need {len(extra) + 1} archives of 64 MiB (a pack holds "
            f"{rp.ARCHIVE_MAX}); lower --max-texture-size"
        )
    stem = archive.removesuffix(".rpf")
    out = []
    for k, group in enumerate(extra, 2):
        name = f"{short(stem, 57)}{k}.rpf"
        out += ["--extra-archive", f"{name}={','.join(sorted(n for n, _ in group))}"]
        print(f"  {name}: {len(group)} members ({_mib(size(group))})")
    return out


def build_world(ctx: Context, p: dict, tables: dict[str, Path]) -> None:
    folder = ctx.work / p["id"]
    m = p["maps"]
    inputs = map_inputs(ctx, m["sources"])
    index = ["--archetype-index", ctx.index] if ctx.index else []
    extra = []
    models = p.get("models")
    if models:
        if models.get("select", "placed") == "placed" and m["sources"] == "all":
            run(
                folder / "classify.log",
                [*py_tool("convert_ymap"), *inputs, *index, "--out", folder / "classify", "--prefix", m["prefix"]],
            )
            select = ["--report", folder / "classify" / "report.json"]
        else:
            names = (
                models["select"]
                if isinstance(models["select"], list)
                else sorted(ctx.a.names[k] for k in placed_own(ctx.a, m["sources"]))
            )
            select = [v for n in names for v in ("--archetype", n)]
        print(f"[{p['id']}] converting the own models (this can take minutes)")
        tables[p["id"]] = models_step(ctx, folder / "models", models, select, folder / "models.log")
        extra = ["--pack-archetypes", tables[p["id"]]]
        for line in (folder / "models.log").read_text(encoding="utf-8").splitlines():
            if line.startswith(("refused", "note:")) or "archetypes converted" in line:
                print(f"  {line}")
    translate = ctx.plan.get("translate") or [0, 0, 0]
    move = ["--translate", ",".join(str(v) for v in translate)] if any(translate) else []
    places = [v for place in m.get("places") or [] for v in ("--place", place)]
    if m.get("archetypesFrom"):
        index = ["--archetype-index", external_index_file(ctx, m["archetypesFrom"], folder)]
        index += [] if ctx.index else ["--keep-unresolved"]
    run(
        folder / "maps.log",
        [
            *py_tool("convert_ymap"),
            *inputs,
            *(index or ["--keep-unresolved"]),
            *extra,
            "--template",
            template(ctx, "map-templates/hei_dt1_02_impexpemproxy_c.pmap"),
            "--out",
            folder / "maps",
            "--prefix",
            m["prefix"],
            "--max-entities",
            str(m["maxEntities"]),
            "--max-retail-typs",
            str(m["maxRetailTyps"]),
            *move,
            *places,
            "--pack-id",
            p["id"],
            "--archive",
            p["archive"],
            "--output-root",
            folder / "stage",
        ],
    )
    shutil.rmtree(folder / "stage")
    for line in (folder / "maps.log").read_text(encoding="utf-8").splitlines():
        if "entities converted" in line or line.startswith(("warning", "dropped")):
            print(f"  {line}")
    args = json.loads((folder / "maps" / "build_runtime_pack.args.json").read_text(encoding="utf-8"))
    args[args.index("--output-root") + 1] = str(ctx.packs_root)
    args += bounds_rows(ctx, p, folder / "bounds") + drawn_rows(ctx, p, folder / "drawn")
    args += [v for place in p.get("places") or [] for v in ("--place", place)]
    finish(ctx, p, args)


def external_index_file(ctx: Context, source: str, folder: Path) -> Path:
    """The archetype index for maps that place --archetypes-from archetypes: the given index (if any) plus their
    rows, from the props pack's table (converted boxes) or the earlier pack's typ rows, classed resident (the
    other pack's typ row requests them; no retail row here)."""
    ext = ctx.a.ext
    data = json.loads(ctx.index.read_text(encoding="utf-8")) if ctx.index else {"schema": convert_ymap.INDEX_SCHEMA}
    rows: dict[str, list] = {}
    names: dict[str, str] = {}
    if source in ctx.props_tables:
        table = json.loads(ctx.props_tables[source].read_text(encoding="utf-8"))
        rows, names = dict(table["archetypes"]), dict(table.get("names") or {})
    else:
        index = external_index(convert_ymap.Index({}, {}), ext, ctx.a.ext_keys, ext.typs)
        rows = {f"0x{k:08x}": v for k, v in index.archetypes.items()}
        names = {f"0x{k:08x}": v for k, v in index.names.items()}
    for key in rows:
        data.setdefault("archetypes", {})[key] = rows[key]
        for row in rows[key]:
            data.setdefault("typs", {})[row[0]] = {"residency": "resident"}
    data.setdefault("names", {}).update(names)
    path = folder / "archetype-index.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, separators=(",", ":")) + "\n", encoding="utf-8")
    return path


def build_props(ctx: Context, p: dict, _tables: dict[str, Path]) -> None:
    """A props pack: --archetypes-from models (typ + .pdr + .ptd), no maps; its table feeds the map packs."""
    folder = ctx.work / p["id"]
    models = p["models"]
    print(f"[{p['id']}] converting {len(models['select'])} models of {ctx.a.ext.label()} (this can take minutes)")
    select = [v for n in models["select"] for v in ("--archetype", n)]
    table = models_step(ctx, folder / "models", models, select, folder / "models.log", external_source(ctx))
    for line in (folder / "models.log").read_text(encoding="utf-8").splitlines():
        if line.startswith(("refused", "note:")) or "archetypes converted" in line:
            print(f"  {line}")
    ctx.props_tables[p["id"]] = table
    data = json.loads(table.read_text(encoding="utf-8"))
    args = ["--id", p["id"], "--archive", p["archive"], "--output-root", str(ctx.packs_root)]
    for member, path in sorted(data["members"].items()):
        args += ["--member", f"{member}={folder / 'models' / path}"]
    args += ["--typ", f"{data['typ']}.ptyp"]
    args += [v for spec in props_spawns(data) for v in ("--spawn", spec)]
    args += [v for place in p.get("places") or [] for v in ("--place", place)]
    finish(ctx, p, args)


def build_interior(ctx: Context, p: dict, tables: dict[str, Path]) -> None:
    folder = ctx.work / p["id"]
    models = p.get("models")
    base = [
        *py_tool("convert_pc_mlo"),
        ctx.a.inv.archive,
        "--archetype-index",
        ctx.index,
        "--typ-template",
        template(ctx, "typ-templates/v_int_22.ptyp"),
        "--milo-template",
        template(ctx, "map-templates/ch3_03_interior_v_gun2_milo_.pmap"),
        "--typ-name",
        p["typ"],
        *[v for m in p["mlos"] for v in ("--mlo", f"{m['name']}={m['map']}")],
        "--max-retail-typs",
        str(p["maxRetailTyps"]),
    ]
    extra, mloargs = [], []
    world = list(tables.values())
    if models:
        run(
            folder / "plan.log",
            [*base, *[v for t in world for v in ("--pack-archetypes", t)], "--out", folder / "plan"],
        )
        own = json.loads((folder / "plan" / "report.json").read_text(encoding="utf-8"))["ownToConvert"]
        if own:
            print(f"[{p['id']}] converting the interiors' {len(own)} own models (this can take minutes)")
            table = models_step(
                ctx, folder / "models", models, [v for n in own for v in ("--archetype", n)], folder / "models.log"
            )
            mloargs = ["--pack-archetypes", table]
            data = json.loads(table.read_text(encoding="utf-8"))
            for member, path in sorted(data["members"].items()):
                extra += ["--member", f"{member}={folder / 'models' / path}"]
            extra += ["--typ", f"{data['typ']}.ptyp"]
        else:
            print(f"[{p['id']}] the interiors place none of the mod's own models")
    mloargs += [v for t in world for v in ("--pack-archetypes", t)]
    run(folder / "mlo.log", [*base, *mloargs, "--out", folder / "mlo"])
    for line in (folder / "mlo.log").read_text(encoding="utf-8").splitlines():
        print(f"  {line}")
    rows = json.loads((folder / "mlo" / "build_runtime_pack.args.json").read_text(encoding="utf-8"))
    args = extra + rows + ["--id", p["id"], "--archive", p["archive"], "--output-root", str(ctx.packs_root)]
    args += bounds_rows(ctx, p, folder / "bounds")
    args += [v for m in p["mlos"] if m.get("place") for v in ("--map-place", f"{m['map']}.pmap={m['place']}")]
    args += [v for place in p.get("places") or [] for v in ("--place", place)]
    finish(ctx, p, args)


def build_collision(ctx: Context, p: dict, _tables: dict[str, Path]) -> None:
    args = ["--id", p["id"], "--archive", p["archive"], "--output-root", str(ctx.packs_root)]
    args += bounds_rows(ctx, p, ctx.work / p["id"] / "bounds")
    args += [v for place in p.get("places") or [] for v in ("--place", place)]
    finish(ctx, p, args)


def finish(ctx: Context, p: dict, args: list) -> None:
    args = [str(v) for v in args]
    args += split_archives(args, p["archive"])
    if p.get("description"):
        args += ["--description", p["description"]]
    run(ctx.work / p["id"] / "build.log", [sys.executable, _HERE / "build_runtime_pack.py", *args])
    resources = ctx.packs_root / p["id"] / "resources"
    for item in sorted(resources.iterdir()):
        print(f"  {item.name}  {item.stat().st_size} bytes")


def execute(ctx: Context, owned: set[str]) -> list[str]:
    ids = [p["id"] for p in ctx.plan["packs"]]
    for pid in ids:
        target = ctx.packs_root / pid
        if target.exists() and pid not in owned:
            raise MapModError(f"{target} exists and was not made by convert-mapmod: pick another --id or remove it")
    (ctx.work / "packs-built").write_text("".join(f"{i}\n" for i in ids), encoding="utf-8")
    for pid in ids:
        shutil.rmtree(ctx.packs_root / pid, ignore_errors=True)
    tables: dict[str, Path] = {}
    order = sorted(ctx.plan["packs"], key=lambda p: ROLES.index(p["role"]))  # world models before interiors
    for p in order:
        print(f"[{p['id']}] building the {p['role']} pack")
        builders = {"props": build_props, "world": build_world, "interior": build_interior}
        builders.get(p["role"], build_collision)(ctx, p, tables)
    outside = [ctx.a.ext.root] if ctx.a.ext is not None and ctx.a.ext.kind == "pack" else []
    for pid in ids:
        others = [v for o in ids if o != pid for v in ("--against", ctx.packs_root / o)]
        others += [v for o in outside for v in ("--against", o)]
        run(
            ctx.work / pid / "validate.log",
            [sys.executable, _HERE / "validate_runtime_pack.py", ctx.packs_root / pid, *others],
        )
        for line in (ctx.work / pid / "validate.log").read_text(encoding="utf-8").splitlines():
            if " OK pack=" in line or line.startswith(("warning", "error", "note: Manage Packs")):
                print(line)
    return ids


# --------------------------------------------------------------------------------------------------
# command line


def parse_point(text: str) -> str:
    """X,Y,Z[,TEXT] or X,Y,Z[:TEXT] (whole metres) -> X,Y,Z:TEXT."""
    match = re.fullmatch(r"(-?\d+),(-?\d+),(-?\d+)(?:[,:](.*))?", text)
    if not match:
        raise MapModError(f"teleport {text!r}: X,Y,Z or X,Y,Z,TEXT in whole metres")
    x, y, z, label = match.groups()
    return format_place((int(x), int(y), int(z)), label or "")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mod", type=Path, help="the copied mod: its dlc.rpf, or a folder (untrusted data)")
    parser.add_argument("--id", required=True, help="the world pack id; the others derive from it (-int-, -col-)")
    parser.add_argument("--work", type=Path, required=True, help="work directory (logs, converter outputs)")
    parser.add_argument("--packs-root", type=Path, required=True, help="where the packs are written")
    parser.add_argument("--name", help="teleport and description text (default: the mod's name)")
    parser.add_argument("--dry-run", action="store_true", help="print the plan and the cap use; write nothing")
    parser.add_argument("--write-plan", type=Path, help="also save the plan (JSON) to this new file")
    parser.add_argument("--plan", type=Path, help="convert this (edited) plan instead of the default one")
    parser.add_argument("--archetype-index", type=Path)
    parser.add_argument("--translate", help="DX,DY,DZ in metres (maps and collision; not interiors)")
    parser.add_argument(
        "--bounds",
        choices=("rows", "merge"),
        default="rows",
        help="one row per .ybn (default; extra rows go to collision packs) or merged rows",
    )
    parser.add_argument(
        "--drawn-collision",
        choices=("none", "embedded", "all"),
        default="none",
        help="collision rows from the drawn geometry of own models whose PC collision is in the "
        "model (embedded), or of every placed own model (all)",
    )
    parser.add_argument("--no-interiors", action="store_true")
    parser.add_argument("--max-entities", type=int)
    parser.add_argument("--max-texture-size", type=int, default=1024)
    parser.add_argument("--teleport", action="append", default=[], metavar="X,Y,Z[,TEXT]")
    parser.add_argument("--map-teleport", action="append", default=[], metavar="MAP=X,Y,Z[,TEXT]")
    parser.add_argument(
        "--archetypes-from",
        type=Path,
        metavar="MOD|PACK_DIR",
        help="another mod whose props the maps place (its dlc.rpf or folder, e.g. Map Builder's): its placed models "
        "become a props pack; or a pack directory built before whose archetypes the maps place",
    )
    parser.add_argument("--archetypes-id", help="id of the props pack (default: --id with -props before the version)")
    parser.add_argument("--templates", type=Path, help="the fetch-templates cache")
    parser.add_argument("--reference-dir", type=Path, help="passed on to the model converters (which do not read it)")
    parser.add_argument("--models-tool", type=Path)
    parser.add_argument("--bounds-tool", type=Path)
    parser.add_argument("--collision-tool", type=Path)
    parser.add_argument("--model-pythonpath", help="PYTHONPATH for the numpy model converters")
    parser.add_argument("--owned", action="append", default=[], help="pack ids an earlier run of this id built")
    args = parser.parse_args(argv)
    sys.stdout.reconfigure(line_buffering=True)  # progress lines while the converters run
    try:
        if not (_ID.fullmatch(args.id) and len(args.id) <= rp.ID_MAX):
            raise MapModError("--id must be lowercase letters, digits and '-' (at most 64)")
        if args.max_entities is not None and not 1 <= args.max_entities <= convert_ymap.MAX_MAP_ENTITIES:
            raise MapModError(f"--max-entities 1..{convert_ymap.MAX_MAP_ENTITIES}")
        if not 1 <= args.max_texture_size <= 16384:
            raise MapModError("--max-texture-size must be a positive number")
        if args.archetypes_id is not None and (
            args.archetypes_from is None
            or not (_ID.fullmatch(args.archetypes_id) and len(args.archetypes_id) <= rp.ID_MAX)
        ):
            raise MapModError("--archetypes-id needs --archetypes-from and lowercase letters, digits and '-'")
        translate = convert_ymap.parse_translate(args.translate)
        tools = {"models": args.models_tool, "bounds": args.bounds_tool, "collision": args.collision_tool}
        available = {k: bool(v and v.is_file()) for k, v in tools.items()}
        map_teleports = {}
        for item in args.map_teleport:
            name, _, point = item.partition("=")
            map_teleports[name.lower().removesuffix(".pmap").removesuffix(".ymap")] = parse_point(point)
        opts = Options(
            args.id,
            args.name or (args.mod.stem if args.mod.is_file() else args.mod.name),
            translate,
            args.bounds,
            args.drawn_collision,
            not args.no_interiors,
            args.max_entities,
            args.max_texture_size,
            [parse_point(t) for t in args.teleport],
            map_teleports,
            available,
            args.archetypes_id or "",
        )
        opts.name = place_text(opts.name)[:30]
        if args.archetype_index is not None and not args.archetype_index.is_file():
            raise MapModError(f"no archetype index at {args.archetype_index}")
        index = convert_ymap.Index.load(args.archetype_index)
        inv = inventory(args.mod)
        ext = load_external(args.archetypes_from) if args.archetypes_from is not None else None
        a = analyse(inv, index, ext)
        if not a.maps:
            raise MapModError("the mod has no readable .ymap: not a map mod (convert-model / convert-bounds)")
        plan = json.loads(args.plan.read_text(encoding="utf-8")) if args.plan else default_plan(a, opts)
        usage = check_plan(plan, a, opts)
        if not index.archetypes:
            plan.setdefault("notes", []).append(
                "no archetype index: stock entities are kept unchecked, no retail typ rows"
            )
    except (MapModError, AssetError, convert_ymap.ConvertError, OSError, ValueError, KeyError, TypeError) as exc:
        raise SystemExit(f"convert_map_mod: {exc}") from None
    for line in describe(plan, usage, a):
        print(line)
    if args.write_plan:
        if args.write_plan.exists():
            raise SystemExit(f"convert_map_mod: refusing to overwrite {args.write_plan}")
        args.write_plan.write_text(json.dumps(plan, indent=1) + "\n", encoding="utf-8")
        print(f"plan written to {args.write_plan} (edit it, then --plan {args.write_plan})")
    if args.dry_run:
        return 0
    ctx = Context(
        a,
        plan,
        args.work,
        args.packs_root,
        args.archetype_index,
        args.templates,
        args.reference_dir,
        {k: v for k, v in tools.items() if v},
        args.model_pythonpath,
    )
    try:
        args.work.mkdir(parents=True, exist_ok=True)
        args.packs_root.mkdir(parents=True, exist_ok=True)
        ids = execute(ctx, set(args.owned))
    except (MapModError, StepError, OSError, json.JSONDecodeError, KeyError) as exc:
        raise SystemExit(f"convert_map_mod: {exc}") from None
    (args.work / "packs.txt").write_text("".join(f"{i}\n" for i in ids), encoding="utf-8")
    print(f"packs: {' '.join(ids)}" + (" (activate them together)" if len(ids) > 1 else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

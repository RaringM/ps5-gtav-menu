#!/usr/bin/env python3
"""Convert a PC map mod's own interiors (MLO archetype + its CMloInstanceDef placement) into pack members.

For a PC map mod whose .ytyp defines a CMloArchetypeDef and whose .ymap places it (MalibuMansion:
malibu_mansion_shell_col / mansion_garage_shell_col). The mod archive is untrusted data: it is read
with the bounded readers of gtavmenu_tools (ymap.scan_mod_archive, make_mlo_ptyp.decode,
gtavmenu_tools.ymap.read_ymap) and this script must run with `python3 -I`, never from a mod directory.

Per --mlo NAME=MAP (archetype name kept: the interior bounds binder pairs the proxy with a bounds slot
named like the MLO archetype, and drawables are found by archetype name):

- the MLO definition from the mod typ that defines it (make_mlo_ptyp.decode of the PC meta), every
  room, portal and timecycle as the mod has it; its entities are classified:
    own       the mod's own archetype (a CBase/CTimeArchetypeDef of the mod's typs): kept when a
              --pack-archetypes table (convert_pc_map_models.py output, this pack's or another
              active pack's) lists it, else dropped ("own model not converted");
    retail    defined by a retail typ (--archetype-index): kept. Its typ, when the index does not
              class it resident, competes for one of --max-retail-typs `typ <name>.ptyp retail` rows
              (most entities first); entities of typs past the limit stay and appear only if their
              typ is resident or another active pack requests it (convert_ymap.py's rule);
    unknown   neither: dropped.
  --no-entities drops every entity (the bisection variant: the MLO's rooms, portals and collision
  alone). Room/portal attachedObjects are renumbered (make_mlo_ptyp.remove_entities).
- the placement: the mod .ymap's CMloInstanceDef of NAME on a retail milo map container
  (tools/make_addon_mlo_pmap.py --instance-ymap: position, rotation verbatim, lodDist, lodLevel,
  numExitPortals, ... and the PC map's extents).

All MLOs go into ONE MLO-only typ (--typ-name; tools/make_mlo_ptyp.build with --typ-template's
schema, read back); each map gets a `mapdep MAP.pmap TYP.ptyp interior` row. Interior collision is
not made here: the mod's <mlo>.ybn becomes a bounds row named <mlo>.pbn (MLO-local coordinates, room
ids kept; `menu-ctl.sh convert-mlo --bounds`). The interior's own models come from a model converter's
--pack-archetypes table (`menu-ctl.sh convert-mlo --models`).

Writes into --out (new or empty): <typ>.ptyp, <typ>.json (spec), <map>.pmap per MLO, report.json
(per MLO: entity classes, dropped and kept entities with archetype, room and reason; the retail typ
rows; the own archetypes a model step must convert) and build_runtime_pack.args.json (member, typ,
map and mapdep arguments; the caller adds bounds rows and places).

  convert_pc_mlo.py MOD/dlc.rpf --archetype-index INDEX.json --typ-template v_int_22.ptyp \\
      --milo-template ch3_03_interior_v_gun2_milo_.pmap --typ-name gm_malibu_int \\
      --mlo malibu_mansion_shell_col=gmmlo_mm --mlo mansion_garage_shell_col=gmmlo_mmg \\
      [--pack-archetypes MODELS/pack-archetypes.json ...] [--max-retail-typs 15] [--no-entities] --out WORK
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

import argparse  # noqa: E402
import json  # noqa: E402
import mmap  # noqa: E402
import re  # noqa: E402
from collections import Counter  # noqa: E402

import make_addon_mlo_pmap  # noqa: E402
import make_mlo_ptyp  # noqa: E402
from convert_ymap import Index, PackArchetypes  # noqa: E402
from gtavmenu_tools.asset_formats import AssetError  # noqa: E402
from gtavmenu_tools.hashes import joaat  # noqa: E402
from gtavmenu_tools.ymap import scan_mod_archive  # noqa: E402
from make_addon_mlo import MloError  # noqa: E402

_NAME = re.compile(r"[a-z0-9_]{1,59}\Z")  # member stems: <name>.ptyp / .pmap / .pbn within 64 bytes
TYP_ROWS_MAX = 16  # GTAV_CUSTOM_PACK_TYP_MAX


def _hash(value) -> int:
    return make_mlo_ptyp.hash_value(value) if value not in ("", None) else 0


def mod_definitions(typs: dict[str, bytes], names: dict[int, str]) -> tuple[dict[int, tuple[str, dict]], dict]:
    """(MLO archetype hash -> (typ path, CMloArchetypeDef spec row), own CBase/CTime archetype hash -> row)."""
    mlos: dict[int, tuple[str, dict]] = {}
    own: dict[int, dict] = {}
    for path, blob in sorted(typs.items()):
        spec = make_mlo_ptyp.decode(blob, names)
        for row in spec.get("archetypes") or []:
            key = _hash(row.get("name"))
            if row.get("type") == "CMloArchetypeDef":
                if key in mlos:
                    raise AssetError(f"MLO {row.get('name')} is defined twice in the mod")
                mlos[key] = (path, row)
            elif row.get("type") in ("CBaseArchetypeDef", "CTimeArchetypeDef"):
                own.setdefault(key, row)
    return mlos, own


def classify(
    mlo: dict,
    own: dict[int, dict],
    index: Index,
    converted: dict[int, str],
    names: dict[int, str],
    no_entities: bool,
) -> tuple[list[dict], set[int]]:
    """(one row per entity: index, archetype, class, typ, room, keep/reason; indices to drop)."""
    rooms: dict[int, str] = {}
    for r, room in enumerate(mlo.get("rooms") or []):
        for o in room.get("attachedObjects") or []:
            rooms.setdefault(int(o), f"{r}:{room.get('name', '')}")
    rows, drop = [], set()
    for i, entity in enumerate(mlo.get("entities") or []):
        key = _hash(entity.get("archetypeName"))
        name = names.get(key) or f"hash_{key:08x}"
        row = {"index": i, "archetype": name, "room": rooms.get(i, "-")}
        if key in own:
            row["class"] = "own"
            if key in converted:
                row["typ"] = converted[key]
            else:
                row["reason"] = "own model not converted"
        elif key in index.archetypes:
            row["class"] = "retail"
            candidates = [r[0] for r in index.archetypes[key]]
            resident = [t for t in candidates if index.residency.get(t) == "resident"]
            row["typ"] = (resident or candidates)[0]
            row["residency"] = index.residency.get(row["typ"], "unknown")
        else:
            row["class"] = "unknown"
            row["reason"] = "archetype in neither the mod nor the retail index"
        if no_entities and "reason" not in row:
            row["reason"] = "--no-entities"
        if "reason" in row:
            drop.add(i)
        rows.append(row)
    return rows, drop


def retail_rows(rows: list[dict], limit: int) -> tuple[list[str], list[str]]:
    """(stock typs that get a `retail` row, most kept entities first; the non-resident ones left out)."""
    need = Counter(
        r["typ"] for r in rows if r["class"] == "retail" and "reason" not in r and r["residency"] != "resident"
    )
    ranked = [typ for typ, _count in sorted(need.items(), key=lambda kv: (-kv[1], kv[0]))]
    return ranked[:limit], ranked[limit:]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mod", type=Path, help="the mod's PC archive (dlc.rpf), untrusted")
    parser.add_argument("--archetype-index", type=Path, required=True, help="gtavmenu-archetype-index-v1 JSON")
    parser.add_argument("--typ-template", type=Path, required=True, help="retail interior .ptyp (meta schema)")
    parser.add_argument("--milo-template", type=Path, required=True, help="retail one-entity *_milo_.pmap")
    parser.add_argument("--typ-name", required=True, help="the pack's MLO typ (member <name>.ptyp)")
    parser.add_argument("--mlo", action="append", required=True, metavar="ARCHETYPE=MAP", help="MLO -> map name")
    parser.add_argument("--pack-archetypes", type=Path, action="append", default=[], help="converted own models")
    parser.add_argument("--max-retail-typs", type=int, default=15)
    parser.add_argument("--no-entities", action="store_true", help="drop every MLO entity (bisection)")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    if not _NAME.fullmatch(args.typ_name):
        raise SystemExit("--typ-name must be lowercase letters, digits and '_' (at most 59)")
    wanted: list[tuple[str, str]] = []
    for item in args.mlo:
        mlo_name, _, map_name = item.lower().partition("=")
        if not (_NAME.fullmatch(mlo_name) and _NAME.fullmatch(map_name)):
            raise SystemExit(f"--mlo {item!r}: expected ARCHETYPE=MAP with lowercase [a-z0-9_] names")
        wanted.append((mlo_name, map_name))
    if len({m for m, _ in wanted}) != len(wanted) or len({p for _, p in wanted}) != len(wanted):
        raise SystemExit("--mlo names and maps must be distinct")
    if args.out.exists() and any(args.out.iterdir()):
        raise SystemExit(f"refusing to write into the non-empty {args.out}")

    try:
        index = Index.load(args.archetype_index)
        converted: dict[int, str] = {}
        for path in args.pack_archetypes:
            table = PackArchetypes.load(path)
            converted |= dict.fromkeys(table.archetypes, table.typ)
        with args.mod.open("rb") as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as view:
            archive = scan_mod_archive(view)
        names = dict(index.names)
        names |= {joaat(m): m for m in archive.models}
        names |= {joaat(m): m for m, _ in wanted}
        mlos, own = mod_definitions(archive.typs, names)
        names |= {k: str(v["name"]) for k, v in own.items() if not str(v["name"]).startswith(("0x", "hash_"))}
        template = args.typ_template.read_bytes()
        milo = args.milo_template.read_bytes()
        spec = {"format": make_mlo_ptyp.FORMAT, "name": args.typ_name, "archetypes": []}
        spec |= {"extensions": [], "dependencies": [], "compositeEntityTypes": []}
        report: dict = {"kind": "gtavmenu-pc-mlo", "typ": args.typ_name, "mlos": [], "notes": archive.unreadable}
        all_rows: list[dict] = []
        maps: list[tuple[str, bytes, dict]] = []
        for mlo_name, map_name in wanted:
            entry = mlos.get(joaat(mlo_name))
            if entry is None:
                raise AssetError(f"the mod defines no MLO {mlo_name}")
            typ_path, row = entry
            mlo = json.loads(json.dumps(row))
            mlo["name"] = mlo_name
            mlo["assetName"] = mlo_name if _hash(mlo.get("assetName")) == joaat(mlo_name) else mlo.get("assetName")
            if _hash(mlo.get("physicsDictionary")) == joaat(mlo_name):
                mlo["physicsDictionary"] = mlo_name
            rows, drop = classify(mlo, own, index, converted, names, args.no_entities)
            make_mlo_ptyp.remove_entities(mlo, drop)
            spec["archetypes"].append(mlo)
            placed = sorted(archive.ymaps.items())
            instances = []
            for path, blob in placed:
                try:
                    instances.append((path, make_addon_mlo_pmap.pc_instance(blob, mlo_name)))
                except MloError:
                    continue
            if len(instances) != 1:
                raise AssetError(f"expected one mod .ymap placing {mlo_name}, found {len(instances)}")
            ymap_path, instance = instances[0]
            pmap = make_addon_mlo_pmap.build(milo, map_name, mlo_name, None, instance=instance)
            maps.append((map_name, pmap, instance))
            all_rows += rows
            classes = Counter(r["class"] for r in rows)
            report["mlos"].append(
                {
                    "mlo": mlo_name,
                    "map": map_name,
                    "typ": typ_path,
                    "ymap": ymap_path,
                    "instance": dict(instance),
                    "rooms": [
                        {"name": r.get("name"), "attachedObjects": len(r.get("attachedObjects") or [])}
                        for r in mlo.get("rooms") or []
                    ],
                    "portals": len(mlo.get("portals") or []),
                    "entities": {"source": len(rows), "kept": len(rows) - len(drop), "classes": dict(classes)},
                    "dropped": [r for r in rows if "reason" in r],
                    "kept": [r for r in rows if "reason" not in r],
                }
            )
        chosen, left_out = retail_rows(all_rows, min(args.max_retail_typs, TYP_ROWS_MAX - 1))
        typ = make_mlo_ptyp.build(spec, template)
    except (AssetError, MloError, make_mlo_ptyp.SpecError, OSError, ValueError, KeyError) as error:
        raise SystemExit(f"convert_pc_mlo: {error}") from None

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / f"{args.typ_name}.json").write_text(json.dumps(spec, indent=1) + "\n", encoding="utf-8")
    (args.out / f"{args.typ_name}.ptyp").write_bytes(typ)
    for map_name, pmap, _instance in maps:
        (args.out / f"{map_name}.pmap").write_bytes(pmap)
    need = Counter(r["typ"] for r in all_rows if r["class"] == "retail" and "reason" not in r)
    report["retailTypRows"] = [{"typ": t, "entities": need[t]} for t in chosen]
    report["retailTypsWithoutRow"] = [
        {"typ": t, "entities": need[t], "residency": index.residency.get(t, "unknown")}
        for t in sorted(need, key=lambda t: (-need[t], t))
        if t not in chosen
    ]
    report["ownToConvert"] = sorted({r["archetype"] for r in all_rows if r.get("reason") == "own model not converted"})
    (args.out / "report.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    argv_out = [f"--member={args.typ_name}.ptyp={args.out / (args.typ_name + '.ptyp')}", f"--typ={args.typ_name}.ptyp"]
    argv_out += [f"--retail-typ={t}.ptyp" for t in chosen]
    for map_name, _pmap, _instance in maps:
        argv_out += [f"--member={map_name}.pmap={args.out / (map_name + '.pmap')}", f"--map={map_name}.pmap"]
        argv_out += [f"--map-dep={map_name}.pmap={args.typ_name}.ptyp:interior"]
    (args.out / "build_runtime_pack.args.json").write_text(json.dumps(argv_out, indent=1) + "\n", encoding="utf-8")

    print(f"{args.typ_name}.ptyp: {len(typ)} B, {len(spec['archetypes'])} MLOs")
    for item in report["mlos"]:
        e = item["entities"]
        reasons = Counter(r["reason"] for r in item["dropped"])
        print(
            f"  {item['mlo']} -> {item['map']}.pmap: entities {e['source']} -> {e['kept']} "
            f"({', '.join(f'{k} {v}' for k, v in sorted(e['classes'].items()))}); dropped: "
            f"{', '.join(f'{k} {v}' for k, v in sorted(reasons.items())) or '-'}; rooms {len(item['rooms'])}, "
            f"portals {item['portals']}, numExitPortals {item['instance']['num_exit_portals']}"
        )
    print(f"retail typ rows ({len(chosen)}): {' '.join(chosen) or '-'}")
    if left_out:
        print(
            f"no row ({len(left_out)} non-resident typs; their entities appear only if resident): {' '.join(left_out)}"
        )
    if report["ownToConvert"]:
        print(f"own models not converted ({len(report['ownToConvert'])}): {' '.join(report['ownToConvert'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

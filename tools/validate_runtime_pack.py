#!/usr/bin/env python3
"""Check a runtime pack directory before uploading it.

  validate_runtime_pack.py PACK_DIR [--against OTHER_PACK_DIR ...] [--target STEM]

PACK_DIR/resources/pack.cfg must parse within the descriptor row limits, and every file it names
must exist with the declared size and sha256. With --against, the pack must also load together
with those packs the way the worker merges active packs: at most 16 packs, the merged row
capacities and archive/data byte totals, and no archive, data file, typ, map, bounds, label, spawn model, override or card name
repeated across them, and every `mapdep` row naming another pack's typ must find that typ as an own
(non-retail) row of one of them. Files in resources/ that pack.cfg does not name are reported as warnings.

Notes (stdout) give the line Custom Packs > Manage Packs shows for the pack (its optional
description, author and version rows and what it adds), and say, for each map row, how many of the archetypes it places the pack's own typs
define, how many come from each --against pack (a map may place another active pack's archetypes:
the worker requests every typ row of every active pack before it loads any map), and how many are
stock or from no pack given; archetypes two packs both define are warnings. Only archives with a
plain table (every pack built since TOC-run1) can be read for this.
A pack copied back from the console after the menu's uninstall holds pack.cfg.uninstalled instead
of pack.cfg: it is checked as the descriptor (with a warning) and an upload restores it as pack.cfg.

Carcols ids (gtavmenu_tools.carcols_ids): every mod kit, light and siren id of the pack's CARCOLS_FILE
rows must lie above the retail ranges (kits 0-613 and 999, lights 0-211, sirens 0-20) and below the
append value, and appear once across the pack and every --against pack (the first loaded item with an
id wins, silently); every wheelName once; and every VEHICLE_VARIATION_FILE lightSettings/sirenSettings
must name a retail id or an item of those packs (a dangling siren id makes the game read past its siren
array when that car's siren is switched on).

Peds: every ped a PED_METADATA_FILE row declares needs <name>.pft, .pdd, .ptd and .pmt in the pack's
(plain-table) archives (the game looks the .pft and .pmt up by name only: a missing one gives a ped
without skeleton or variation data), and a PropsName other than null needs both <props>.pdd and
<props>.ptd there (neither: a warning, the game then uses a retail props dictionary of that name).

Wheels: every <wheelName> of the pack's CARCOLS_FILE <Wheels> needs <wheelName>.pdr in the plain-table
archives of the pack or of an --against pack (the engine requests the wheel drawable by that name: no
member, no wheel drawn); an archive with a keyed or unreadable table makes this a warning.

Engine tables and stores (gtavmenu_tools.engine_caps), per game version: --target (default
$GTAV_TARGET; without either, each table's and store's tightest known baseline, naming its version).
The pack and every --against pack together must fit the fixed-size tables their data rows append to,
counted from the retail baseline of a fresh process: weapon components, weapon info files (one per
WEAPONINFO_FILE row), weapon models, vehicle models and ped models (one per <InitDatas> child of
their model rows; 01.011.001 has room for 3 add-on vehicles and 5 add-on peds, 01.010.002 for 7 and
4). They must also fit the asset stores with a recorded fresh-process census: the members of every
archive per store (gtavmenu_tools.rpf7.store_member_counts; a keyed overlay archive by its override
rows) plus the slots model rows add by name, plus the store's margin. Notes give each pack's per-store needs. A set past a table or
store is refused by the worker, so it is an error here, worded as in the game ("Vehicle slots full
(3 free, need 4)"). Archives with a keyed table cannot be counted here (a warning).

PS4 resources (gtavmenu_tools.ps4_resource_checks), for archives with .oft or .otd members: every
skinned vertex declaration a fragment's drawables reach must encode BlendIndices as UByte4 (8), as
stock PS4 fragments do; the PC Colour encoding (9) is an error (the console renders the vehicle
without most of its body), and another enabled component that differs from the stock declaration is a
warning. Every BC1/BC3/BC5 texture of a dictionary must be stored as GCN Thin1DThin, as
gtavmenu_tools.ps4_texture_writer writes it (tile word and index, allocation size, mip views and the
packed size; another format is a warning). An unreadable member is an error; an archive with a keyed
table is not read for this (a warning when its override rows name an .oft or .otd).

Exit status: 0 when every check passes, 1 otherwise. tools/upload_runtime_pack.py runs part of these
checks before an upload: the descriptor and file checks, the PS4 resource checks, and the engine
tables of the pack alone or, with --activate-add, of the pack together with the packs active on the
console (then also the merge rules and carcols ids). Stores, peds, wheels and map dependencies, and
carcols ids without --activate-add, are checked only here.
"""

from __future__ import annotations

import argparse
import struct
import sys
import xml.etree.ElementTree as ET
from collections.abc import Sequence
from pathlib import Path

from gtavmenu_tools import carcols_ids, engine_caps, pack_archetypes, ps4_resource_checks, rpf7, runtime_pack

PED_MEMBERS = (".pft", ".pdd", ".ptd", ".pmt")
PROPS_MEMBERS = (".pdd", ".ptd")


def check(
    pack_dir: Path, against: Sequence[Path] = (), target: str | None = None
) -> tuple[runtime_pack.RuntimePack | None, list[str], list[str]]:
    """The parsed pack, errors and warnings for `pack_dir`, alone and merged with every `against`
    pack; engine tables and stores are checked against `target`'s baseline (None: the strictest)."""
    pack, errors, warnings = runtime_pack.check_directory(pack_dir)
    errors = [f"{pack_dir}: {e}" for e in errors]
    warnings = [f"{pack_dir}: {w}" for w in warnings]
    others: list[runtime_pack.RuntimePack] = []
    dirs: list[tuple[Path, runtime_pack.RuntimePack]] = []
    for other_dir in against:
        other, other_errors, _ = runtime_pack.check_directory(other_dir)
        errors += [f"{other_dir}: {e}" for e in other_errors]
        if other is not None:
            others.append(other)
            dirs.append((other_dir, other))
    if pack is not None and not errors:
        errors += carcols_problems([*dirs, (pack_dir, pack)])
        errors += engine_problems([*dirs, (pack_dir, pack)], target)
        store_errors, store_warnings, _ = store_report([*dirs, (pack_dir, pack)], target)
        errors += store_errors
        warnings += store_warnings
        ped_errors, ped_warnings = ped_problems(pack_dir, pack)
        errors += [f"{pack_dir}: {e}" for e in ped_errors]
        warnings += [f"{pack_dir}: {w}" for w in ped_warnings]
        wheel_errors, wheel_warnings = wheel_problems(pack_dir, pack, dirs)
        errors += [f"{pack_dir}: {e}" for e in wheel_errors]
        warnings += [f"{pack_dir}: {w}" for w in wheel_warnings]
        resource_errors, resource_warnings = ps4_resource_checks.pack_problems(pack_dir / "resources", pack)
        errors += [f"{pack_dir}: {e}" for e in resource_errors]
        warnings += [f"{pack_dir}: {w}" for w in resource_warnings]
        errors += [f"{pack_dir}: {e}" for e in runtime_pack.stock_name_problems(pack_dir / "resources", pack)]
    if pack is not None and against:
        errors += [f"together: {p}" for p in runtime_pack.merge_conflicts([*others, pack])]
        errors += [f"together: {p}" for p in runtime_pack.unbound_map_deps([*others, pack])]
    elif pack is not None:
        warnings += [
            f"{pack_dir}: map {name} depends on typ {typ} of another pack; validate with --against that pack "
            "and keep both active (the worker refuses the map otherwise)"
            for name, typ in runtime_pack.external_map_deps(pack)
        ]
    return pack, errors, warnings


def carcols_problems(packs: Sequence[tuple[Path, runtime_pack.RuntimePack]]) -> list[str]:
    """Carcols id problems of the (directory, pack) list loaded together (carcols_ids.problems)."""
    rows, errors = [], []
    for directory, pack in packs:
        data = []
        for row in pack.data:
            if row.type in (carcols_ids.CARCOLS, carcols_ids.VARIATION):
                try:
                    data.append((row.type, row.file, (directory / "resources" / row.file).read_bytes()))
                except OSError as exc:
                    errors.append(f"{directory}: {row.file}: {exc.strerror or exc}")
        pack_rows, pack_errors = carcols_ids.pack_rows(pack.pack_id, data)
        rows += pack_rows
        errors += pack_errors
    return errors + [f"carcols: {p}" for p in carcols_ids.problems(rows)]


def pack_tables(directory: Path, pack: runtime_pack.RuntimePack) -> tuple[dict[str, int], list[str]]:
    """({engine table: slots}, errors) the pack's data rows take (engine_caps.pack_usage)."""
    data, errors = [], []
    for row in pack.data:
        if row.type not in engine_caps.ROW_TYPES:
            continue
        path = directory / "resources" / row.file
        try:
            blob = path.read_bytes() if row.type in engine_caps.COUNTED_TYPES else None
        except OSError as exc:
            errors.append(f"{directory}: {row.file}: {exc.strerror or exc}")
            continue
        data.append((row.type, row.file, blob))
    usage, usage_errors = engine_caps.pack_usage(pack.pack_id, data)
    return usage, errors + usage_errors


def engine_problems(packs: Sequence[tuple[Path, runtime_pack.RuntimePack]], target: str | None = None) -> list[str]:
    """Fixed engine tables the (directory, pack) list loaded together would overfill (engine_caps)."""
    usages, errors = [], []
    for directory, pack in packs:
        usage, pack_errors = pack_tables(directory, pack)
        usages.append((pack.pack_id, usage))
        errors += pack_errors
    return errors + [f"engine table: {p}" for p in engine_caps.problems(usages, target)]


def archive_members(directory: Path, pack: runtime_pack.RuntimePack) -> tuple[dict[str, int], list[str]]:
    """({store: members}, warnings) of the pack's archives, as the worker counts them. An `overlay`
    archive counts too: the worker checks its entry count, not its member names, before LoadImage,
    so it counts every member (a keyed overlay table by its override rows' extensions). Any other
    keyed or unreadable table is not counted (a warning)."""
    totals = dict.fromkeys(rpf7.STORES, 0)
    warnings = []
    for archive in [pack.archive, *(a for a, _, _ in pack.extra_archives)]:
        try:
            blob = (directory / "resources" / archive).read_bytes()
            entries, _ = rpf7.read_table(blob, archive)
        except (OSError, rpf7.Rpf7Error, ValueError, struct.error, IndexError):
            if archive in pack.overlay_archives:
                for row_archive, member in pack.overrides:
                    store = rpf7.member_store(member) if row_archive == archive else None
                    if store in totals:
                        totals[store] += 1
                continue
            warnings.append(f"store needs not counted for {archive}: keyed or unreadable table (a lower bound)")
            continue
        for store, count in rpf7.store_member_counts(entries).items():
            if store in totals:
                totals[store] += count
    return totals, warnings


def store_report(
    packs: Sequence[tuple[Path, runtime_pack.RuntimePack]], target: str | None = None
) -> tuple[list[str], list[str], list[str]]:
    """(errors, warnings, notes): the asset stores the (directory, pack) list loaded together would
    need more fresh-process free slots of than `target` has (engine_caps.store_problems); a note per
    pack gives its per-store needs."""
    needs, warnings, notes = [], [], []
    for directory, pack in packs:
        members, member_warnings = archive_members(directory, pack)
        usage, _ = pack_tables(directory, pack)  # engine_problems reports the row errors
        need = engine_caps.store_needs(members, usage)
        needs.append((pack.pack_id, need))
        warnings += [f"{directory}: {w}" for w in member_warnings]
        listed = ", ".join(f"{n} {store}" for store, n in need.items() if n)
        notes.append(f"{pack.pack_id} store needs: {listed or 'none'} (the set adds each store's margin)")
    errors = [f"engine store: {p}" for p in engine_caps.store_problems(needs, target)]
    return errors, warnings, notes


def ped_init_datas(text: bytes) -> list[tuple[str, str]]:
    """(name, PropsName) of every InitData of a CPedModelInfo__InitDataList, lower case."""
    if b"<!DOCTYPE" in text or b"<!ENTITY" in text:
        raise ValueError("declares a DTD or entities")
    try:
        root = ET.fromstring(text)
    except ET.ParseError as error:
        raise ValueError(f"not well-formed XML: {error}") from None
    if root.tag != "CPedModelInfo__InitDataList":
        raise ValueError("not a CPedModelInfo__InitDataList")
    return [
        ((item.findtext("Name") or "").strip().lower(), (item.findtext("PropsName") or "null").strip().lower())
        for item in root.iterfind("InitDatas/Item")
    ]


def ped_problems(pack_dir: Path, pack: runtime_pack.RuntimePack) -> tuple[list[str], list[str]]:
    """(errors, warnings) on the members the pack's PED_METADATA_FILE peds need (module docstring)."""
    rows = [row for row in pack.data if row.type == "PED_METADATA_FILE"]
    members: set[str] = set()
    for archive in [pack.archive, *(a for a, _, _ in pack.extra_archives)] if rows else []:
        try:
            blob = (pack_dir / "resources" / archive).read_bytes()
            members.update(pack_archetypes.archive_member_names(blob, archive, PED_MEMBERS))
        except (OSError, rpf7.Rpf7Error, ValueError, struct.error, IndexError):
            return [], [f"ped members not checked: {archive} has a keyed or unreadable table"]
    errors, warnings = [], []
    for row in rows:
        try:
            peds = ped_init_datas((pack_dir / "resources" / row.file).read_bytes())
        except (OSError, ValueError) as error:
            errors.append(f"{row.file}: {error}")
            continue
        for name, props in peds:
            missing = [name + ext for ext in PED_MEMBERS if name + ext not in members]
            if missing:
                errors.append(f"{row.file}: ped {name} has no {', '.join(missing)} in the pack's archives")
            have = [props + ext for ext in PROPS_MEMBERS if props + ext in members]
            if props != "null" and len(have) == 1:
                other = ({props + ext for ext in PROPS_MEMBERS} - set(have)).pop()
                errors.append(f"{row.file}: ped {name} props {props}: {have[0]} is in the pack but {other} is not")
            elif props != "null" and not have:
                warnings.append(
                    f"{row.file}: ped {name} props {props} are not in the pack (a retail props dictionary of that "
                    "name is used if the game has one)"
                )
    return errors, warnings


def wheel_problems(
    pack_dir: Path, pack: runtime_pack.RuntimePack, others: Sequence[tuple[Path, runtime_pack.RuntimePack]] = ()
) -> tuple[list[str], list[str]]:
    """(errors, warnings): a carcols <Wheels> wheelName of `pack` without <wheelName>.pdr in the
    archives of `pack` or of the packs loaded with it (module docstring)."""
    wheels: list[tuple[str, str]] = []
    errors: list[str] = []
    for row in pack.data:
        if row.type != carcols_ids.CARCOLS:
            continue
        try:
            keys = carcols_ids.read_keys(row.type, (pack_dir / "resources" / row.file).read_bytes())
        except (OSError, ValueError):
            continue  # carcols_problems names the unreadable file
        wheels += [(row.file, name) for name in keys.wheels if name]
    if not wheels:
        return errors, []
    members: set[str] = set()
    for directory, owner in [(pack_dir, pack), *others]:
        for archive in [owner.archive, *(a for a, _, _ in owner.extra_archives)]:
            try:
                blob = (directory / "resources" / archive).read_bytes()
                members.update(pack_archetypes.archive_member_names(blob, archive, (".pdr",)))
            except (OSError, rpf7.Rpf7Error, ValueError, struct.error, IndexError):
                return errors, [f"wheel models not checked: {archive} has a keyed or unreadable table"]
    for file, name in wheels:
        if f"{name.lower()}.pdr" not in members:
            errors.append(f"{file}: wheel {name} has no {name.lower()}.pdr in the archives (the game draws no wheel)")
    return errors, []


def archetype_report(pack_dir: Path, against: Sequence[Path] = ()) -> tuple[list[str], list[str]]:
    """(notes, warnings) on the archetypes the pack's maps place, with the --against packs' typs."""
    rows = []
    for directory in [*against, pack_dir]:
        pack, errors, _ = runtime_pack.check_directory(directory)
        if pack is not None and not errors:
            rows.append((pack, pack_archetypes.read_pack(directory, pack)))
    return pack_archetypes.cross_check(rows)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pack", type=Path, help="pack directory (contains resources/pack.cfg)")
    parser.add_argument(
        "--against",
        type=Path,
        action="append",
        default=[],
        metavar="PACK_DIR",
        help="another pack that will be active together with this one (repeatable)",
    )
    parser.add_argument(
        "--target",
        default=engine_caps.target_from_env(),
        metavar="STEM",
        help="game version whose engine baseline to check against (default: $GTAV_TARGET, else the strictest known)",
    )
    args = parser.parse_args(argv)
    try:
        target = engine_caps.canonical_target(args.target)
    except ValueError as exc:
        parser.error(str(exc))
    pack, errors, warnings = check(args.pack, args.against, target)
    if not errors and pack is not None:
        print(f"note: Manage Packs shows: {runtime_pack.about_line(pack)}")
        print(f"note: engine baseline: {target or engine_caps.STRICTEST + ' known per table and store'}")
        others = [(d, p) for d in args.against for p in [runtime_pack.check_directory(d)[0]] if p is not None]
        for note in store_report([*others, (args.pack, pack)], target)[2]:
            print(f"note: {note}")
        notes, archetype_warnings = archetype_report(args.pack, args.against)
        warnings += archetype_warnings
        for note in notes:
            print(f"note: {note}")
    for warning in warnings:
        print(f"warning: {warning}", file=sys.stderr)
    for error in errors:
        print(f"error: {error}", file=sys.stderr)
    if errors or pack is None:
        print(f"{args.pack}: INVALID ({len(errors)} error{'s' if len(errors) != 1 else ''})", file=sys.stderr)
        return 1
    together = f", merges with {len(args.against)} other pack(s)" if args.against else ""
    print(
        f"{args.pack}: OK pack={pack.pack_id} archives={1 + len(pack.extra_archives)} data={len(pack.data)} "
        f"typs={len(pack.typs)} maps={len(pack.maps)} spawns={len(pack.spawns)}{together}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

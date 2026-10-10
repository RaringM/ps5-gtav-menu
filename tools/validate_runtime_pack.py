#!/usr/bin/env python3
"""Check a runtime pack directory before uploading it.

  validate_runtime_pack.py PACK_DIR [--against OTHER_PACK_DIR ...]

PACK_DIR/resources/pack.cfg must parse within the descriptor row limits, and every file it names
must exist with the declared size and sha256. With --against, the pack must also load together
with those packs the way the worker merges active packs: at most 8 packs, the merged row
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

Engine tables (gtavmenu_tools.engine_caps): the pack and every --against pack together must fit the
fixed-size 01.010.002 tables their data rows append to, counted from the retail baseline of a fresh
process: weapon components (465 of 470, one per <Infos> child of a WEAPONCOMPONENTSINFO_FILE), weapon
info files (154 of 160, one per WEAPONINFO_FILE row), weapon models (547 of 610), vehicle models
(623 of 630) and ped models (426 of 430), one per <InitDatas> child of their model rows. A set past a
cap is refused at Load by the worker, so it is an error here. The ped store is the tightest: four add-on
peds per process (S2 2026-10-08: `store=429/430`, then `models 429->430/430`); the worker also refuses a
selection with more PED_METADATA_FILE rows than free slots before Load starts, naming the pack.

Exit status: 0 when every check passes, 1 otherwise. tools/upload_runtime_pack.py runs the same
checks before an upload.
"""

from __future__ import annotations

import argparse
import struct
import sys
import xml.etree.ElementTree as ET
from collections.abc import Sequence
from pathlib import Path

from gtavmenu_tools import carcols_ids, engine_caps, pack_archetypes, rpf7, runtime_pack

PED_MEMBERS = (".pft", ".pdd", ".ptd", ".pmt")
PROPS_MEMBERS = (".pdd", ".ptd")


def check(pack_dir: Path, against: Sequence[Path] = ()) -> tuple[runtime_pack.RuntimePack | None, list[str], list[str]]:
    """The parsed pack, errors and warnings for `pack_dir`, alone and merged with every `against` pack."""
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
        errors += engine_problems([*dirs, (pack_dir, pack)])
        ped_errors, ped_warnings = ped_problems(pack_dir, pack)
        errors += [f"{pack_dir}: {e}" for e in ped_errors]
        warnings += [f"{pack_dir}: {w}" for w in ped_warnings]
        wheel_errors, wheel_warnings = wheel_problems(pack_dir, pack, dirs)
        errors += [f"{pack_dir}: {e}" for e in wheel_errors]
        warnings += [f"{pack_dir}: {w}" for w in wheel_warnings]
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


def engine_problems(packs: Sequence[tuple[Path, runtime_pack.RuntimePack]]) -> list[str]:
    """Fixed engine tables the (directory, pack) list loaded together would overfill (engine_caps)."""
    usages, errors = [], []
    for directory, pack in packs:
        data = []
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
        usage, pack_errors = engine_caps.pack_usage(pack.pack_id, data)
        usages.append((pack.pack_id, usage))
        errors += pack_errors
    return errors + [f"engine table: {p}" for p in engine_caps.problems(usages)]


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
    args = parser.parse_args(argv)
    pack, errors, warnings = check(args.pack, args.against)
    if not errors and pack is not None:
        print(f"note: Manage Packs shows: {runtime_pack.about_line(pack)}")
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

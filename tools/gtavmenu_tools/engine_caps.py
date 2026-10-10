"""Fixed-size engine tables the data rows of a set of active packs fill (01.010.002).

The worker gates each data row by the live headroom of the engine table it appends to
(src/module/features/custom_pack_data.inc), so a selection whose rows add up past a table's
capacity passes the descriptor checks and is then refused at Load. Where the table has a fixed
capacity and its retail count is the same in every fresh single-player process on 01.010.002, the
host can count what the selected packs add and refuse the set up front. Tables that grow (mod kits,
lights, sirens, wheels, timecycle modifiers, the weapon info by-hash array) and those whose free
count varies (archetype pool, streaming stores, the ped MetaDataStore) are left to the worker.

Counting follows the worker: a WEAPONINFO_FILE row takes one weapon-info blob record whatever it
holds; a model or components row takes one slot per element child of its <InitDatas> / <Infos>
list (any child name, the structural bound the worker checks, not only the names it recognises).
"""

from __future__ import annotations

import xml.etree.ElementTree as ET
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass


@dataclass(frozen=True)
class EngineTable:
    name: str
    data_type: str  # the pack.cfg data row type that appends to it
    list_name: str | None  # element whose children each take a slot; None = one slot per row
    retail: int  # entries in a fresh 01.010.002 single-player process
    cap: int
    evidence: str


# One row per fixed table; `evidence` is the line the retail count and capacity come from (a
# pack-notes line of a recorded hardware run, or a snapshot of the table in a fresh process).
TABLES: tuple[EngineTable, ...] = (
    EngineTable(
        "weapon components",
        "WEAPONCOMPONENTSINFO_FILE",
        "Infos",
        465,
        470,
        "WCOMP-run1 pack-notes `pack data 1 components names=2 items=2 infos=465/470` (RE 3.3: fixed "
        "CWeaponComponentInfo*[470], refilled with no capacity check)",
    ),
    EngineTable(
        "weapon info files",
        "WEAPONINFO_FILE",
        None,
        154,
        160,
        "peds-weapons-datafiles-re.md 3.1 snapshot `154/160` (weapon-info blob list, one record per "
        "weapons.meta, count++ with no capacity check; worker gate count < cap)",
    ),
    EngineTable(
        "weapon models",
        "WEAPON_METADATA_FILE",
        "InitDatas",
        547,
        610,
        "WMODEL-run1 pack-notes `pack data models type=72 names=1 store=547/610`",
    ),
    EngineTable(
        "vehicle models",
        "VEHICLE_METADATA_FILE",
        "InitDatas",
        623,
        630,
        "PROTO-run1 pack-notes `pack data models type=73 names=1 entries=1 store=623/630`",
    ),
    EngineTable(
        "ped models",
        "PED_METADATA_FILE",
        "InitDatas",
        426,
        430,
        "PEDDD-run1, SPIDEY-run1 pack-notes `pack data 0 sync type=71 models 426->427/430`",
    ),
)
COUNTED_TYPES = frozenset(t.data_type for t in TABLES if t.list_name is not None)
ROW_TYPES = frozenset(t.data_type for t in TABLES)


def list_items(blob: bytes, list_name: str) -> int:
    """Element children of every `list_name` element of an XML data file (ValueError if not XML)."""
    try:
        text = blob.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise ValueError(f"not XML: {exc}") from exc
    if "<!" in text.replace("<!--", ""):  # DOCTYPE/ENTITY/CDATA: the worker refuses them too
        raise ValueError("not plain XML (declarations or CDATA)")
    try:
        root = ET.fromstring(text)
    except ET.ParseError as exc:
        raise ValueError(f"not XML: {exc}") from exc
    return sum(len(element) for element in root.iter(list_name))


def pack_usage(pack_id: str, data: Iterable[tuple[str, str, bytes | None]]) -> tuple[dict[str, int], list[str]]:
    """({table name: slots}, errors) one pack's (type, file, bytes) data rows take.

    Bytes are needed only for COUNTED_TYPES rows (None is fine for the others).
    """
    tables = {t.data_type: t for t in TABLES}
    usage: dict[str, int] = {}
    errors: list[str] = []
    for type_name, name, blob in data:
        table = tables.get(type_name)
        if table is None:
            continue
        if table.list_name is None:
            count = 1
        else:
            try:
                count = list_items(blob or b"", table.list_name)
            except ValueError as exc:
                errors.append(f"{pack_id} {name}: {exc}; {table.name} not counted")
                continue
        usage[table.name] = usage.get(table.name, 0) + count
    return usage, errors


def problems(usages: Sequence[tuple[str, Mapping[str, int]]]) -> list[str]:
    """One line per fixed table the (pack id, usage) set would overfill, e.g.
    `weapon components: 471 > 470 (465 retail + 6 in these packs: a 4, b 2)`."""
    found = []
    for table in TABLES:
        parts = [(pack_id, usage.get(table.name, 0)) for pack_id, usage in usages]
        added = sum(n for _, n in parts)
        if table.retail + added <= table.cap:
            continue
        where = "this pack" if len(usages) == 1 else "these packs"
        detail = ", ".join(f"{pack_id} {n}" for pack_id, n in parts if n)
        found.append(
            f"{table.name}: {table.retail + added} > {table.cap} ({table.retail} retail + {added} in {where}: "
            f"{detail}); the worker refuses the row that does not fit at Load"
        )
    return found

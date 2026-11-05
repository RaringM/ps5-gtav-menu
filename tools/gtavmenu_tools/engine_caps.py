"""Fixed engine tables and asset stores a set of active packs fills, per game version.

The worker gates a load by the live headroom of the engine tables and stores it adds to
(src/module/features/custom_pack_select.inc before Load, custom_rpf.inc / custom_pack_data.inc at
registration and per data row), so a selection past a table's capacity is refused in the game. Where
a table has a fixed capacity and its retail count is the same in every fresh single-player process
of a game version, the host can count what the selected packs add and refuse the set up front, with
the in-game wording ("Vehicle slots full (3 free, need 4)").

Baselines are per target (`baseline(target)`); the stem comes from `--target` or $GTAV_TARGET, and
without one the strictest known value of each table and store is used (its game version is named).
PPSA04263 01.010.002 runs PPSA04264 01.010.002's code and shares its baseline.

Counting follows the worker: a WEAPONINFO_FILE row takes one weapon-info blob record whatever it
holds; a model or components row takes one slot per element child of its <InitDatas> / <Infos>
list (any child name, the structural bound the worker checks, not only the names it recognises).

Stores (gtavmenu_tools.rpf7.STORES): a set needs, per store, the members its archives add
(rpf7.store_member_counts, an upper bound: a name the store already holds adds no slot) plus the
slots its data rows add by name (each vehicle model a txd, each ped model a txd and a dwd for
itself and one each for its props), plus the store's margin when the set adds anything to it
(src/common/custom_pack_checks.c gtav_custom_pack_store_need). Store free counts are those of a
fresh process at the recorded census; the world streams into some of them, so the worker's live
gate stays the authority and a store without a census is not checked here.

Boot-time pool growth (data/boot_pools): where a target's loader grows pools at boot
(build.features.bootPools, or BOOT_POOLS=1 in the environment), its model caps and store sizes
include the shared policy's deltas (boot_pool_growth).
"""

from __future__ import annotations

import json
import os
import xml.etree.ElementTree as ET
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, replace
from pathlib import Path

from gtavmenu_tools.rpf7 import STORES

TARGET_010 = "ppsa04264-01.010.002"
TARGET_011 = "ppsa04264-01.011.001"
TARGET_PS4 = "cusa00411-01.57"
# Targets that run another target's code (same eboot build, same engine tables).
TARGET_ALIASES = {"ppsa04263-01.010.002": TARGET_010}
STRICTEST = "strictest"


@dataclass(frozen=True)
class EngineTable:
    name: str
    data_type: str  # the pack.cfg data row type that appends to it
    list_name: str | None  # element whose children each take a slot; None = one slot per row
    retail: int  # entries in a fresh single-player process of `target`
    cap: int
    evidence: str
    label: str = ""  # the worker's store label: "<label> slots full (...)"
    target: str = ""  # the game version the baseline was measured on
    measured: bool = True  # False: assumed from another version until measured


@dataclass(frozen=True)
class StoreBaseline:
    free: int  # free slots in a fresh single-player process
    size: int
    evidence: str
    target: str = ""


# name, data type, list, worker label (custom_pack_checks.c kStores model rows).
_SHAPES: tuple[tuple[str, str, str | None, str], ...] = (
    ("weapon components", "WEAPONCOMPONENTSINFO_FILE", "Infos", "Component"),
    ("weapon info files", "WEAPONINFO_FILE", None, "Weapon file"),
    ("weapon models", "WEAPON_METADATA_FILE", "InitDatas", "Weapon model"),
    ("vehicle models", "VEHICLE_METADATA_FILE", "InitDatas", "Vehicle"),
    ("ped models", "PED_METADATA_FILE", "InitDatas", "Ped model"),
)
_HW_011 = "01.011.001 hardware run"
# {target: {table: (retail, cap, evidence, measured)}}: `evidence` is the line the retail count and
# capacity come from (a pack-notes line of a recorded hardware run, or a snapshot in a fresh process).
_TABLE_BASELINES: dict[str, dict[str, tuple[int, int, str, bool]]] = {
    TARGET_010: {
        "weapon components": (
            465,
            470,
            "WCOMP-run1 pack-notes `pack data 1 components names=2 items=2 infos=465/470` (RE 3.3: fixed "
            "CWeaponComponentInfo*[470], refilled with no capacity check)",
            True,
        ),
        "weapon info files": (
            154,
            160,
            "peds-weapons-datafiles-re.md 3.1 snapshot `154/160` (weapon-info blob list, one record per "
            "weapons.meta, count++ with no capacity check; worker gate count < cap)",
            True,
        ),
        "weapon models": (547, 610, "WMODEL-run1 pack-notes `pack data models type=72 names=1 store=547/610`", True),
        "vehicle models": (
            623,
            630,
            "PROTO-run1 pack-notes `pack data models type=73 names=1 entries=1 store=623/630`",
            True,
        ),
        "ped models": (
            426,
            430,
            "PEDDD-run1, SPIDEY-run1 pack-notes `pack data 0 sync type=71 models 426->427/430`",
            True,
        ),
    },
    TARGET_011: {
        "weapon components": (
            465,
            470,
            f"{_HW_011} quality-20261101 klog `pack data 14 components names=4 items=4 infos=465/470`",
            True,
        ),
        "weapon info files": (
            154,
            160,
            "not measured on 01.011.001: 01.010.002's `154/160` (peds-weapons-datafiles-re.md 3.1) assumed",
            False,
        ),
        "weapon models": (
            547,
            610,
            f"{_HW_011} quality-20261101 klog `pack data 13 sync type=72 models 547->552/610`",
            True,
        ),
        "vehicle models": (
            637,
            640,
            f"{_HW_011} quality-20261101 klog `pack data models type=73 names=1 entries=1 store=637/640`; "
            "gameconfig cfg+0x31c = 640 (crashrepro-20261101 config_pools.json)",
            True,
        ),
        "ped models": (
            435,
            440,
            f"{_HW_011} crashrepro-20261101 factory_census.txt `Ped ... block 0xf000: 435/440 used`; "
            "gameconfig cfg+0x318 = 440",
            True,
        ),
    },
    TARGET_PS4: {
        "weapon components": (
            465,
            470,
            "not measured on CUSA00411 01.57: the shared Legacy table's 465/470 baseline is assumed",
            False,
        ),
        "weapon info files": (
            154,
            160,
            "not measured on CUSA00411 01.57: the shared Legacy table's 154/160 baseline is assumed",
            False,
        ),
        "weapon models": (
            547,
            610,
            "CUSA00411 01.57 HW-2 pack-live-checks.json `weapon 547/610`",
            True,
        ),
        "vehicle models": (637, 640, "CUSA00411 01.57 HW-2 pack-live-checks.json `vehicle 637/640`", True),
        "ped models": (435, 440, "CUSA00411 01.57 HW-2 pack-live-checks.json `ped 435/440`", True),
    },
}
# {target: {store: (free, size, evidence)}}: free slots in a fresh process (store +0x50 - used).
_STORE_BASELINES: dict[str, dict[str, tuple[int, int, str]]] = {
    TARGET_010: {
        "frag": (564, 0, "custom-assets-roadmap.md pack-register `store free frag=564 drawable=14458 txd=1598`"),
        "drawable": (14458, 0, "custom-assets-roadmap.md pack-register `store free frag=564 drawable=14458`"),
        "txd": (1598, 0, "custom-assets-roadmap.md pack-register `store free ... txd=1598`"),
        "typ": (302, 1700, "runtime-map-loading-re.md 1 snapshot 1398/1700"),
        "metadata": (20, 3200, "peds-weapons-datafiles-re.md 2.1 snapshot `Meta 20 (3180/3200)`"),
        "dwd": (1218, 0, "peds-weapons-datafiles-re.md 2.1 snapshot `Dwd 1218`"),
        "ptfx": (36, 400, "custom_rpf.inc PtFxAssetStore snapshot 364/400"),
    },
    TARGET_011: {
        store: (free, size, f"{_HW_011} quality-20261101 census_pid220_preinject.json (fresh Story Mode, pid 220)")
        for store, (free, size) in {
            "frag": (284, 42000),
            "drawable": (15499, 81000),
            "txd": (1532, 80200),
            "typ": (288, 1700),
            "mapdata": (5427, 10020),
            "bounds": (5168, 14700),
            "metadata": (1, 3200),
            "dwd": (1078, 20500),
            "ptfx": (32, 400),
        }.items()
    },
}
# The worker's store labels (custom_pack_checks.c kStores) and margins (kMemberMargins): a set that
# adds to a store needs this many slots more than it adds.
STORE_LABELS = {
    "frag": "Fragment",
    "drawable": "Drawable",
    "txd": "Texture",
    "typ": "Archetype",
    "mapdata": "Map data",
    "bounds": "Collision",
    "metadata": "Ped metadata",
    "dwd": "Ped drawable",
    "ptfx": "Particle",
}
STORE_MARGINS = {
    "frag": 16,
    "drawable": 16,
    "txd": 16,
    "typ": 8,
    "mapdata": 8,
    "bounds": 8,
    "metadata": 1,
    "dwd": 16,
    "ptfx": 2,
}
TARGETS = tuple(_TABLE_BASELINES)

REPO_ROOT = Path(__file__).resolve().parents[2]
BOOT_POOL_POLICY = REPO_ROOT / "data/boot_pools/policy.json"
# Boot-pool policy entry -> the table or store it grows (data/boot_pools/policy.json).
_BOOT_POOL_NAMES = {"ped": "ped models", "vehicle": "vehicle models", "weapon": "weapon models"}


def boot_pool_growth(target: str | None, environ: Mapping[str, str] | None = None) -> dict[str, int]:
    """Slots the loader's boot-time pool growth adds for `target` ({table or store name: slots}).

    Empty without a target, for a target without boot-pool pins, or when the target's manifest has
    not enabled it (build.features.bootPools). BOOT_POOLS=1/0 in the environment forces it on/off,
    like the make variable of the same name.
    """
    if not target:
        return {}
    manifest_path = REPO_ROOT / "data/targets" / f"{target}.json"
    try:
        build = json.loads(manifest_path.read_text(encoding="utf-8")).get("build", {})
        policy = json.loads(BOOT_POOL_POLICY.read_text(encoding="utf-8"))["entries"]
    except (OSError, ValueError, KeyError):
        return {}
    forced = (environ if environ is not None else os.environ).get("BOOT_POOLS", "")
    enabled = forced == "1" or (forced != "0" and bool(build.get("features", {}).get("bootPools", False)))
    if not enabled or "bootPools" not in build:
        return {}
    return {_BOOT_POOL_NAMES.get(entry["name"], entry["name"]): int(entry["delta"]) for entry in policy}


def canonical_target(target: str | None) -> str | None:
    """The baseline stem for `target` (an alias maps to the target whose code it runs); None or ""
    for the strictest baseline. ValueError for a target without a baseline."""
    if not target:
        return None
    stem = TARGET_ALIASES.get(target, target)
    if stem not in _TABLE_BASELINES:
        known = ", ".join(sorted([*_TABLE_BASELINES, *TARGET_ALIASES]))
        raise ValueError(f"no engine baseline for target {target} (known: {known})")
    return stem


def target_from_env(environ: Mapping[str, str] | None = None) -> str | None:
    """$GTAV_TARGET, or None (strictest) when unset or empty."""
    return (environ if environ is not None else os.environ).get("GTAV_TARGET") or None


def tables(target: str | None = None) -> tuple[EngineTable, ...]:
    """The fixed engine tables of `target`; without one, each table's tightest known baseline
    (fewest free slots; its game version is in EngineTable.target)."""
    stem = canonical_target(target)
    growth = boot_pool_growth(target)
    rows = []
    for name, data_type, list_name, label in _SHAPES:
        candidates = [stem] if stem else list(_TABLE_BASELINES)
        pick = min(candidates, key=lambda t: _TABLE_BASELINES[t][name][1] - _TABLE_BASELINES[t][name][0])
        retail, cap, evidence, measured = _TABLE_BASELINES[pick][name]
        if growth.get(name):
            cap += growth[name]
            evidence += f"; +{growth[name]} boot-time pool growth (data/boot_pools/policy.json)"
        rows.append(EngineTable(name, data_type, list_name, retail, cap, evidence, label, pick, measured))
    return tuple(rows)


def store_baselines(target: str | None = None) -> dict[str, StoreBaseline]:
    """Fresh-process free slots per store of `target` (stores without a census are absent); without
    a target, each store's lowest free count across the versions that measured it."""
    stem = canonical_target(target)
    found: dict[str, StoreBaseline] = {}
    for candidate in [stem] if stem else list(_STORE_BASELINES):
        for store, (free, size, evidence) in _STORE_BASELINES.get(candidate, {}).items():
            if store not in found or free < found[store].free:
                found[store] = StoreBaseline(free, size, evidence, candidate)
    growth = boot_pool_growth(target)
    for store, slots in growth.items():
        if store in found:
            base = found[store]
            found[store] = replace(
                base,
                free=base.free + slots,
                size=base.size + slots if base.size else 0,
                evidence=f"{base.evidence}; +{slots} boot-time pool growth (data/boot_pools/policy.json)",
            )
    return {store: found[store] for store in STORES if store in found}


TABLES = tables()  # the strictest baseline (no target)
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
    shapes = {data_type: (name, list_name) for name, data_type, list_name, _ in _SHAPES}
    usage: dict[str, int] = {}
    errors: list[str] = []
    for type_name, file, blob in data:
        shape = shapes.get(type_name)
        if shape is None:
            continue
        name, list_name = shape
        if list_name is None:
            count = 1
        else:
            try:
                count = list_items(blob or b"", list_name)
            except ValueError as exc:
                errors.append(f"{pack_id} {file}: {exc}; {name} not counted")
                continue
        usage[name] = usage.get(name, 0) + count
    return usage, errors


def _baseline_note(target: str | None, picked: str, numbers: str) -> str:
    if target:
        return f"{picked} baseline {numbers}"
    return f"strictest baseline, {picked} {numbers}; pass --target for another game version"


def _detail(parts: Sequence[tuple[str, int]]) -> str:
    return ", ".join(f"{pack_id} {n}" for pack_id, n in parts if n)


def problems(usages: Sequence[tuple[str, Mapping[str, int]]], target: str | None = None) -> list[str]:
    """One line per fixed table the (pack id, usage) set would overfill, in the worker's words, e.g.
    `Vehicle slots full (3 free, need 4): a 3, b 1 (ppsa04264-01.011.001 baseline 637/640 used)`."""
    found = []
    for table in tables(target):
        parts = [(pack_id, usage.get(table.name, 0)) for pack_id, usage in usages]
        added = sum(n for _, n in parts)
        free = table.cap - table.retail
        if added <= free:
            continue
        numbers = f"{table.retail}/{table.cap} used" + ("" if table.measured else ", assumed")
        found.append(
            f"{table.label} slots full ({free} free, need {added}): {_detail(parts)} "
            f"({_baseline_note(target, table.target, numbers)}); the worker refuses this set at Load"
        )
    return found


def store_needs(members: Mapping[str, int], usage: Mapping[str, int]) -> dict[str, int]:
    """Slots one pack adds per store: its archives' members (rpf7.store_member_counts) plus the
    slots its model rows add by name (txd: one per vehicle and two per ped, dwd: two per ped,
    metadata: one per ped, its pmt; an archive .pmt of that name is found, not added: the worker
    over-counts it the same way)."""
    vehicles = usage.get("vehicle models", 0)
    peds = usage.get("ped models", 0)
    needs = {store: members.get(store, 0) for store in STORES}
    needs["txd"] += vehicles + 2 * peds
    needs["dwd"] += 2 * peds
    needs["metadata"] += peds
    return needs


def store_need(store: str, adds: int) -> int:
    """Free slots a set adding `adds` to `store` needs (gtav_custom_pack_store_need)."""
    return adds + STORE_MARGINS[store] if adds else 0


def store_problems(needs: Sequence[tuple[str, Mapping[str, int]]], target: str | None = None) -> list[str]:
    """One line per store the (pack id, store_needs) set needs more free slots of than a fresh
    process of `target` has, e.g. `Ped metadata slots full (1 free, need 2): p 1 (...)`."""
    found = []
    for store, baseline in store_baselines(target).items():
        parts = [(pack_id, need.get(store, 0)) for pack_id, need in needs]
        need = store_need(store, sum(n for _, n in parts))
        if need <= baseline.free:
            continue
        numbers = f"{baseline.free} free in a fresh process"
        found.append(
            f"{STORE_LABELS[store]} slots full ({baseline.free} free, need {need}): {_detail(parts)} "
            f"({_baseline_note(target, baseline.target, numbers)}); the worker refuses this set at Load"
        )
    return found

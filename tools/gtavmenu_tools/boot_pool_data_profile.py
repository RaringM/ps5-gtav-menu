"""Data-method boot-pool profiles: grow the pools with plain data writes in the boot window.

The PS5 loader grows the pools with a call-site stub (``boot_pool_profile``). A data-method target
instead rewrites each PoolSizes node of the parsed gameconfig after the parse and before the first
store is sized, and the parsed model caps before the model-info factories exist. Its manifest's
``build.bootPools`` names the reviewed pins (``data/boot_pools/<stem>.json``) and the generated header
``gtavmenu/boot_pool_data_<stem>_generated.h``; how much each entry grows comes from the shared policy.
The host watcher and the in-process plugin read the same row, so a revision is a data change.
"""

from __future__ import annotations

import json
from pathlib import Path

from gtavmenu_tools.boot_pool_profile import (
    CFG_OFFSET_MAX,
    POLICY_PATH,
    REPO_ROOT,
    BootPoolProfileError,
    _sha256,
    load_policy,
    parse_pins,
    render_overrides,
)

# Every address and layout offset the data method reads, in the order the header lists them.
LAYOUT = (
    ("kFsmState", "FSM_STATE", "u16 boot state machine: 0 config parse, 1 streaming init, 2 running"),
    ("kConfigManager", "CONFIG_MANAGER", "u64 gameconfig manager, published before the parse"),
    ("kConfigMgrBuckets", "CONFIG_MGR_BUCKETS", "manager PoolSizes map: node** buckets"),
    ("kConfigMgrSlots", "CONFIG_MGR_SLOTS", "manager PoolSizes map: u16 bucket count"),
    ("kConfigMgrCfg", "CONFIG_MGR_CFG", "manager: parsed config pointer (model caps)"),
    ("kConfigNodeValue", "CONFIG_NODE_VALUE", "PoolSizes node: i32 value"),
    ("kConfigNodeNext", "CONFIG_NODE_NEXT", "PoolSizes node: next node"),
    ("kStoreInitObject", "STORE_INIT_OBJECT", "u64 set immediately before the stores are sized"),
    ("kFirstStoreSized", "FIRST_STORE_SIZED", "u8 set once the first store is sized"),
    ("kStoreSizeOffset", "STORE_SIZE_OFFSET", "store object: u32 pool size after sizing"),
    ("kStoreInitializedOffset", "STORE_INITIALIZED_OFFSET", "store object: u8 set once its Init sized it"),
    ("kPedModelFactory", "PED_MODEL_FACTORY", "ped model-info factory"),
    ("kVehicleModelFactory", "VEHICLE_MODEL_FACTORY", "vehicle model-info factory"),
    ("kWeaponModelFactory", "WEAPON_MODEL_FACTORY", "weapon model-info factory"),
    ("kModelFactoryBuckets", "MODEL_FACTORY_BUCKETS", "factory block map: node** buckets"),
    ("kModelFactorySlots", "MODEL_FACTORY_SLOTS", "factory block map: u16 bucket count"),
    ("kModelFactoryBlockKey", "MODEL_FACTORY_BLOCK_KEY", "storage-block key"),
    ("kModelFactoryBlockValue", "MODEL_FACTORY_BLOCK_VALUE", "factory block node: storage-block pointer"),
    ("kModelFactoryNodeNext", "MODEL_FACTORY_NODE_NEXT", "factory block node: next node"),
    ("kModelBlockCount", "MODEL_BLOCK_COUNT", "storage block: u16 used count"),
    ("kModelBlockCapacity", "MODEL_BLOCK_CAPACITY", "storage block: u16 capacity"),
)
REQUIRED_CONSTS = tuple(pin for pin, _, _ in LAYOUT)
# The object whose size or capacity shows that a grown entry took effect.
VERIFY_OBJECTS = {
    "metadata": "kMetaDataStoreObj",
    "frag": "kFragmentStoreObj",
    "txd": "kTxdStoreObj",
    "ptfx": "kPtFxAssetStoreObj",
    "typ": "kMapTypesStoreObj",
    "dwd": "kDwdStoreObj",
    "ped": "kPedModelFactory",
    "vehicle": "kVehicleModelFactory",
}
CHAIN_MAX = 64  # PoolSizes and factory node chains walked before giving up


def header_name(stem: str) -> str:
    return f"gtavmenu/boot_pool_data_{stem}_generated.h"


def commit_order(pins: dict, policy: list[dict], source: Path) -> list[str]:
    """The row order of the commit: the pins' ``commitOrder`` (the order the game reads the entries,
    so the earliest-read node is written first), or the policy order when the pins carry none."""
    names = [entry["name"] for entry in policy]
    order = pins.get("commitOrder")
    if order is None:
        return names
    if not isinstance(order, list) or not all(isinstance(name, str) for name in order):
        raise BootPoolProfileError(f"{source.name}: commitOrder must be a list of policy entry names")
    if sorted(order) != sorted(names) or len(set(order)) != len(order):
        raise BootPoolProfileError(f"{source.name}: commitOrder must name each of {', '.join(names)} exactly once")
    return list(order)


def data_row(manifest: dict, stem: str, policy: list[dict], *, root: Path = REPO_ROOT) -> dict | None:
    """The data-method row of one manifest, or None when it carries no ``build.bootPools``.

    Refuses stale pins, a missing layout pin and any entry that would not grow within its limit."""
    build = manifest.get("build", {})
    item = build.get("bootPools")
    if item is None:
        return None
    header = header_name(stem)
    if item.get("header") != header:
        raise BootPoolProfileError(f"{stem}: build.bootPools.header must be {header}")
    pins_path = root / item["json"]
    if _sha256(pins_path) != item.get("sha256"):
        raise BootPoolProfileError(f"{stem}: build.bootPools.sha256 is stale for {item['json']}")
    pins = json.loads(pins_path.read_text(encoding="utf-8"))
    consts, _ = parse_pins(pins, pins_path)
    order = commit_order(pins, policy, pins_path)
    missing = [name for name in REQUIRED_CONSTS if name not in consts]
    missing += [entry[key] for entry in policy for key in ("key", "stock") if entry[key] not in consts]
    if missing:
        raise BootPoolProfileError(f"{pins_path.name}: missing pins {', '.join(sorted(set(missing)))}")
    overrides = []
    for entry in policy:
        stock, key = consts[entry["stock"]], consts[entry["key"]]
        new = stock + entry["delta"]
        if stock <= 0 or new > entry["max"]:
            raise BootPoolProfileError(
                f"{pins_path.name}: {entry['name']} {stock}+{entry['delta']} exceeds {entry['max']}"
            )
        if entry["kind"] == "cfg" and (key % 4 or key >= CFG_OFFSET_MAX):
            raise BootPoolProfileError(f"{pins_path.name}: {entry['name']} cfg offset {key:#x} is not aligned")
        verify = VERIFY_OBJECTS.get(entry["name"])
        overrides.append(
            {
                "name": entry["name"],
                "kind": entry["kind"],
                "key": key,
                "stock": stock,
                "new": new,
                "verify": consts.get(verify, 0) if verify else 0,
            }
        )
    by_name = {o["name"]: o for o in overrides}
    return {
        "stem": stem,
        "header": header,
        "targetId": manifest["targetId"],
        "enabled": bool(build.get("features", {}).get("bootPools", False)),
        "consts": consts,
        "overrides": [by_name[name] for name in order],
    }


def load_data_row(manifest_path: Path, *, policy_path: Path = POLICY_PATH, root: Path = REPO_ROOT) -> dict:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    row = data_row(manifest, manifest_path.stem, load_policy(policy_path), root=root)
    if row is None:
        raise BootPoolProfileError(f"{manifest_path.name} carries no build.bootPools")
    return row


def render_data_header(row: dict) -> str:
    """The layout macros, the growth-only overrides and the per-entry verify objects."""
    consts = row["consts"]
    lines = [
        f"/* Generated by devtools/generators/generate_boot_pool_data_header.py for {row['stem']}. */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        '#include "gtavmenu/boot_pool.h"',
        "",
        f'#define GTAV_BOOT_POOL_DATA_TARGET_ID "{row["targetId"]}"',
        f"#define GTAV_BOOT_POOL_DATA_ENABLED {int(row['enabled'])}",
        f"#define GTAV_BOOT_POOL_DATA_CHAIN_MAX {CHAIN_MAX}u",
    ]
    for pin, macro, comment in LAYOUT:
        lines.append(f"#define GTAV_BOOT_POOL_DATA_{macro} UINT64_C(0x{consts[pin]:x}) /* {comment} */")
    lines += [
        f"#define GTAV_BOOT_POOL_DATA_OVERRIDE_COUNT {len(row['overrides'])}u",
        "",
        *render_overrides(row, "gtav_boot_pool_data_overrides"),
        "",
        "/* Store object (pool size at STORE_SIZE_OFFSET) or model factory that shows each entry took effect. */",
        "static const uint64_t gtav_boot_pool_data_verify[] = {",
        *(f"    UINT64_C(0x{o['verify']:x}), /* {o['name']} */" for o in row["overrides"]),
        "};",
        "",
    ]
    return "\n".join(lines)


def data_profile(row: dict) -> dict:
    """The plain values a data-method applier needs (host watcher and tests)."""
    consts = row["consts"]
    return {
        "targetId": row["targetId"],
        "enabled": row["enabled"],
        "chainMax": CHAIN_MAX,
        "layout": {pin: consts[pin] for pin in REQUIRED_CONSTS},
        "stores": {name: consts[obj] for name, obj in VERIFY_OBJECTS.items() if obj in consts and obj.endswith("Obj")},
        "factories": {
            "ped": consts["kPedModelFactory"],
            "vehicle": consts["kVehicleModelFactory"],
            "weapon": consts["kWeaponModelFactory"],
        },
        "overrides": [dict(o) for o in row["overrides"]],
    }


def data_contract(manifest_path: Path, *, root: Path = REPO_ROOT) -> dict:
    """The data profile of one manifest, refusing stale pins and a stale committed header."""
    row = load_data_row(manifest_path, root=root)
    header_path = root / "include" / row["header"]
    if not header_path.is_file() or header_path.read_text(encoding="utf-8") != render_data_header(row):
        raise BootPoolProfileError(f"{row['stem']}: include/{row['header']} is stale for its pins and policy")
    return data_profile(row)


__all__ = [
    "CHAIN_MAX",
    "LAYOUT",
    "REQUIRED_CONSTS",
    "VERIFY_OBJECTS",
    "commit_order",
    "data_contract",
    "data_profile",
    "data_row",
    "header_name",
    "load_data_row",
    "render_data_header",
]

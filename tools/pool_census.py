#!/usr/bin/env python3
"""Read-only census of the engine pools that bound custom packs, from the running GTA (ps5debug reads).

  pool_census.py --host H [--port 744] [--target STEM] [--pid N] [--json OUT] [--no-config]

Reports, for the selected target (GTAV_TARGET or --target):
  * the nine stores the pack gates check (size / used / free from +0x50/+0x58/+0x60, and whether the
    object still holds its pinned vtable);
  * the vehicle, ped and weapon model-info blocks (count / capacity of the 0xf000 block);
  * weapon components (count / 470) and the weapon-info file list (count / capacity);
  * the parsed gameconfig PoolSizes map and the ConfigModelInfo fields (cfg+0x300..0x340), for targets
    with boot-pool pins (pins_for_target);
  * each data/boot_pools/policy.json entry's live value beside its pinned stock value (kStock*), and, for
    stock values the pins still lack, a ready-to-paste kStock* fragment for those pins.

Store and model addresses come from the target's custom-lane JSON (data/targets/<stem>.json
build.customLane); the config manager, its map layout and the model-cap offsets from the boot-pool pins.
Nothing is written. GTA runs two eboot.bin processes; the census reads the one whose store objects hold their
pinned vtables (or --pid).
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import struct
import sys
import textwrap
from collections.abc import Sequence
from pathlib import Path
from typing import Protocol

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from gtavmenu_tools.hashes import joaat  # noqa: E402

# kPackStoreGates order (src/module/features/custom_rpf.inc): gate name -> lane object.
STORES = (
    ("frag", "kFragmentStore"),
    ("drawable", "kDrawableStore"),
    ("txd", "kTextureStore"),
    ("typ", "kTypStore"),
    ("mapdata", "kMapStore"),
    ("bounds", "kBoundsStore"),
    ("metadata", "kMetaDataStore"),
    ("dwd", "kDwdStore"),
    ("ptfx", "kPtfxStore"),
)
FACTORIES = (("vehicle", "kVehicleModelFactory"), ("ped", "kPedModelFactory"), ("weapon", "kWeaponModelFactory"))
COMPONENT_INFO_MAX = 470  # static CWeaponComponentInfo*[470] (custom_pack_data.inc kComponentInfoMax)
# The gameconfig layout pins (data/boot_pools/<stem>.json): [g_ConfigManager], manager +buckets/+slots/+cfg,
# node +value/+next. A target without pins skips the config section.
CONFIG_PINS = (
    "kConfigManager",
    "kConfigMgrBuckets",
    "kConfigMgrSlots",
    "kConfigMgrCfg",
    "kConfigNodeValue",
    "kConfigNodeNext",
)
MODEL_CAP_PINS = (("ped", "kCfgPedCap"), ("vehicle", "kCfgVehicleCap"), ("weapon", "kCfgWeaponCap"))
CFG_WINDOW = (0x300, 0x340)  # ConfigModelInfo fields dumped whole
POLICY = "data/boot_pools/policy.json"
# Stock values pinned beside the growth policy's although the stub never grows them.
STOCK_EXTRAS = ({"name": "weapon", "kind": "cfg", "key": "kCfgWeaponCap", "stock": "kStockCfgWeaponCap"},)
POOL_NAMES = (
    "MetaDataStore",
    "DwdStore",
    "DrawableStore",
    "TxdStore",
    "FragmentStore",
    "ClothStore",
    "fwClothStore",
    "PtFxAssetStore",
    "ScaleformStore",
    "StreamedScripts",
    "ScriptStore",
    "CutSceneStore",
    "CutsceneStore",
    "AnimStore",
    "ClipStore",
    "fwClipDictionaryStore",
    "ExprDictStore",
    "FrameFilterStore",
    "NetworkDefStore",
    "PoseMatcherStore",
    "PMStore",
    "BlendshapeStore",
    "IplStore",
    "MapDataStore",
    "fwMapDataStore",
    "MapTypesStore",
    "fwMapTypesStore",
    "StaticBounds",
    "StaticBoundsStore",
    "fwStaticBoundsStore",
    "PhysicsBounds",
    "NavMeshes",
    "HeightMeshes",
    "WaypointRecordingRoutes",
    "Peds",
    "Vehicles",
    "Object",
    "CWeapon",
    "Building",
    "AnimatedBuilding",
    "CompEntity",
    "InteriorInst",
    "InteriorProxy",
    "PortalInst",
    "EntityBatch",
    "GrassBatch",
    "InstanceBuffer",
    "fwArchetypePooledMap",
    "fwLodNode",
    "QuadTreeNodes",
    "CTask",
    "CEvent",
    "ObjectIntelligence",
    "VehicleStruct",
    "VehicleStreamRequest",
    "CPickup",
    "CPickupPlacement",
    "MaxLoadedInfo",
    "MaxLoadRequestedInfo",
    "ActiveLoadedInfo",
    "ActiveLoadRequestedInfo",
    "MaxManagedRequests",
    "MaxUnguardedRequests",
    "MaxReleaseRefs",
    "CGameScriptResource",
    "CScriptEntityExtension",
    "DecoratorExtension",
    "AttachmentExtension",
    "SyncedScenes",
    "AnimScenes",
    "ScenarioPoint",
    "TrafficLightInfos",
    "CWeaponComponentInfo",
    "CVehicleModelInfo",
    "CPedModelInfo",
    "CWeaponModelInfo",
    "CBaseModelInfo",
)
POOL_LABELS = {joaat(name): name for name in POOL_NAMES}
NODE_DEPTH_MAX = 64
BUCKETS_MAX = 4096


class Reader(Protocol):
    def read(self, pid: int, address: int, length: int) -> bytes: ...


class CensusError(RuntimeError):
    pass


def lane_for_target(stem: str, root: Path = ROOT) -> dict[str, int]:
    """The target's custom-lane constants (name -> integer value)."""
    manifest_path = root / "data/targets" / f"{stem}.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise CensusError(f"no target manifest {manifest_path}") from exc
    lane = ((manifest.get("build") or {}).get("customLane") or {}).get("json")
    if not lane:
        raise CensusError(f"{stem} has no custom lane (build.customLane): no pack pools to census")
    payload = json.loads((root / lane).read_text(encoding="utf-8"))
    return {row["name"]: int(str(row["value"]), 0) for row in payload["consts"]}


def pins_for_target(stem: str, root: Path = ROOT) -> Path | None:
    """The target's boot-pool pins: the manifest's build.bootPools.json, else data/boot_pools/<stem of
    build.customLane.json>.json when it exists (a regional build runs its lane's code), else None."""
    try:
        manifest = json.loads((root / "data/targets" / f"{stem}.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    build = manifest.get("build") or {}
    pins = (build.get("bootPools") or {}).get("json")
    if pins:
        return root / pins
    lane = (build.get("customLane") or {}).get("json")
    if not lane:
        return None
    candidate = root / "data/boot_pools" / f"{Path(lane).stem}.json"
    return candidate if candidate.is_file() else None


def load_pins(path: Path) -> dict[str, dict]:
    """name -> pin const row ({name, file, value, method}) with the value parsed to an integer."""
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
        return {row["name"]: {**row, "value": int(str(row["value"]), 0)} for row in payload["consts"]}
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise CensusError(f"unreadable boot-pool pins {path}: {exc}") from exc


def stock_entries(root: Path = ROOT) -> list[dict]:
    """The growth policy's entries (name, kind, key, stock, ...) plus STOCK_EXTRAS it does not list."""
    try:
        policy = json.loads((root / POLICY).read_text(encoding="utf-8"))["entries"]
    except (OSError, ValueError, KeyError) as exc:
        raise CensusError(f"unreadable growth policy {POLICY}: {exc}") from exc
    listed = {entry["stock"] for entry in policy}
    return [*policy, *(entry for entry in STOCK_EXTRAS if entry["stock"] not in listed)]


def pool_label(key: int) -> str:
    """A PoolSizes key's pool name, or its hash when POOL_NAMES does not know it."""
    return POOL_LABELS.get(key, f"0x{key:08x}")


def _u16(raw: bytes, offset: int) -> int:
    return struct.unpack_from("<H", raw, offset)[0]


def _u32(raw: bytes, offset: int) -> int:
    return struct.unpack_from("<I", raw, offset)[0]


def _u64(raw: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", raw, offset)[0]


def store_census(reader: Reader, pid: int, lane: dict[str, int]) -> list[dict[str, object]]:
    rows = []
    for gate, name in STORES:
        address = lane[name]
        raw = reader.read(pid, address, 0x68)
        vtable = _u64(raw, 0)
        size, first_free, used = _u32(raw, 0x50), _u32(raw, 0x58), _u32(raw, 0x60) & 0x3FFFFFFF
        base, index, module_size = _u32(raw, 8), _u32(raw, 0xC), _u32(raw, 0x10)
        expected = lane.get(f"{name}Vtable")
        full = first_free == 0xFFFFFFFF
        rows.append(
            {
                "store": gate,
                "object": name,
                "address": hex(address),
                "vtable": hex(vtable),
                "vtableOk": expected is None or vtable == expected,
                "size": size,
                "used": used,
                "free": 0 if full or used > size else size - used,
                "full": full,
                "base": base,
                "index": index,
                "moduleSize": module_size,
            }
        )
    return rows


def txd_base_chain(stores: list[dict[str, object]]) -> dict[str, object]:
    """AddModule starts each streaming range where the previous module's ends; TxdStore follows
    DrawableStore. The worker's texture gate checks this chain (custom_engine.inc), so a boot-time
    Dwd/Drawable growth that moves the Txd base keeps texture actions working."""
    rows = {row["store"]: row for row in stores}
    drawable, txd = rows["drawable"], rows["txd"]
    expected = int(drawable["base"]) + int(drawable["moduleSize"])
    return {
        "txdBase": txd["base"],
        "drawableEnd": expected,
        "ok": bool(drawable["vtableOk"] and txd["vtableOk"])
        and txd["base"] == expected
        and txd["index"] == int(drawable["index"]) + 1,
    }


def factory_block(reader: Reader, pid: int, factory: int, key: int) -> tuple[int, int] | None:
    """(count, capacity) of the factory's `key` storage block (custom_pack_data.inc pack_model_factory_block)."""
    head = reader.read(pid, factory, 0x18)
    buckets, bucket_count = _u64(head, 8), _u16(head, 0x10)
    if not buckets or not bucket_count or bucket_count > BUCKETS_MAX:
        return None
    table = reader.read(pid, buckets, 8 * bucket_count)
    for b in range(bucket_count):
        node = _u64(table, 8 * b)
        for _ in range(256):
            if not node:
                break
            raw = reader.read(pid, node, 0x18)
            if _u32(raw, 0) == key:
                array = _u64(raw, 8)
                block = reader.read(pid, array, 0x10)
                return _u16(block, 8), _u16(block, 0xA)
            node = _u64(raw, 0x10)
    return None


def model_census(reader: Reader, pid: int, lane: dict[str, int]) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    key = lane.get("kVehicleModelMapTypeDef", 0xF000)
    for kind, name in FACTORIES:
        factory = lane[name]
        vtable = _u64(reader.read(pid, factory, 8), 0)
        expected = lane.get(f"{name}Vtable")
        block = factory_block(reader, pid, factory, key) if expected is None or vtable == expected else None
        row: dict[str, object] = {"table": kind, "vtableOk": expected is None or vtable == expected}
        if block:
            row.update(count=block[0], capacity=block[1], free=max(0, block[1] - block[0]))
        rows.append(row)
    if "kComponentInfoCount" in lane:
        count = struct.unpack("<i", reader.read(pid, lane["kComponentInfoCount"], 4))[0]
        rows.append(
            {"table": "wcomp", "count": count, "capacity": COMPONENT_INFO_MAX, "free": COMPONENT_INFO_MAX - count}
        )
    if "kWeaponInfoBlobs" in lane:
        raw = reader.read(pid, lane["kWeaponInfoBlobs"], 0x10)
        count, capacity = _u16(raw, 8), _u16(raw, 0xA)
        rows.append({"table": "winfo", "count": count, "capacity": capacity, "free": max(0, capacity - count)})
    return rows


def config_census(reader: Reader, pid: int, pins: dict[str, int], cfg_offsets: Sequence[int] = ()) -> dict[str, object]:
    """The parsed gameconfig, laid out as the pins say: PoolSizes {joaat(name): size}, the ConfigModelInfo
    fields cfg+0x300..0x340 and the pinned model caps, plus any further cfg offsets asked for."""
    missing = [name for name in CONFIG_PINS if name not in pins]
    if missing:
        raise CensusError(f"the boot-pool pins lack {', '.join(missing)}")
    manager = _u64(reader.read(pid, pins["kConfigManager"], 8), 0)
    if not manager:
        raise CensusError("the config manager is not published yet")
    at_buckets, at_slots, at_cfg = pins["kConfigMgrBuckets"], pins["kConfigMgrSlots"], pins["kConfigMgrCfg"]
    head = reader.read(pid, manager, max(at_buckets + 8, at_slots + 2, at_cfg + 8))
    buckets, bucket_count, cfg = _u64(head, at_buckets), _u16(head, at_slots), _u64(head, at_cfg)
    if not buckets or not 0 < bucket_count <= BUCKETS_MAX:
        raise CensusError(f"implausible config map (buckets={bucket_count})")
    table = reader.read(pid, buckets, 8 * bucket_count)
    at_value, at_next = pins["kConfigNodeValue"], pins["kConfigNodeNext"]
    node_size = max(4, at_value + 4, at_next + 8)  # the u32 hash is at +0
    pools: dict[int, int] = {}
    for b in range(bucket_count):
        node = _u64(table, 8 * b)
        for _ in range(NODE_DEPTH_MAX):
            if not node:
                break
            raw = reader.read(pid, node, node_size)
            pools[_u32(raw, 0)] = struct.unpack_from("<i", raw, at_value)[0]
            node = _u64(raw, at_next)
    caps = {kind: pins[name] for kind, name in MODEL_CAP_PINS if name in pins}
    offsets = sorted({*range(*CFG_WINDOW, 4), *caps.values(), *cfg_offsets})
    fields = reader.read(pid, cfg, offsets[-1] + 4) if cfg else b""
    return {
        "manager": hex(manager),
        "buckets": bucket_count,
        "entries": len(pools),
        "pools": {pool_label(h): v for h, v in sorted(pools.items())},
        "modelInfo": {kind: _u32(fields, o) for kind, o in caps.items()} if fields else {},
        "modelInfoFields": {f"cfg+0x{o:x}": _u32(fields, o) for o in offsets} if fields else {},
    }


def stock_census(config: dict, pins: dict[str, dict], entries: Sequence[dict]) -> list[dict[str, object]]:
    """Each stock entry's live gameconfig value beside its pinned kStock* value (missing/unread/match/differs)."""
    pools, fields = config["pools"], config["modelInfoFields"]
    rows: list[dict[str, object]] = []
    for entry in entries:
        key_pin, stock_pin = pins.get(entry["key"]), pins.get(entry["stock"])
        label = live = None
        if key_pin is not None:
            key = key_pin["value"]
            label = pool_label(key) if entry["kind"] == "node" else f"cfg+0x{key:x}"
            live = (pools if entry["kind"] == "node" else fields).get(label)
        pinned = None if stock_pin is None else stock_pin["value"]
        state = "missing" if pinned is None else "unread" if live is None else "match" if live == pinned else "differs"
        row: dict[str, object] = {
            "name": entry["name"],
            "kind": entry["kind"],
            "key": entry["key"],
            "stock": entry["stock"],
            "label": label,
            "live": live,
            "pinned": pinned,
            "state": state,
        }
        if state == "differs":
            row.update(delta=live - pinned, policyDelta=entry.get("delta"))
        rows.append(row)
    return rows


def stock_fragment(rows: Sequence[dict], pins: dict[str, dict], date: str) -> list[dict[str, str]]:
    """kStock* consts in the pins' shape (name, file, value, method) for every stock value the pins lack and
    the census read. Only a fresh Story Mode census (no pack, no boot-pool growth) reads stock values."""
    fallback = (pins.get("kConfigManager") or {}).get("file") or "pool_census.py"
    fragment: list[dict[str, str]] = []
    for row in rows:
        live = row["live"]
        if row["state"] != "missing" or not isinstance(live, int) or live <= 0:
            continue
        if row["kind"] == "node":
            source = f"PoolSizes {row['label']}"
        else:
            source = f"ConfigModelInfo {row['label']} ({row['name']})"
        fragment.append(
            {
                "name": row["stock"],
                "file": (pins.get(row["key"]) or {}).get("file") or fallback,
                "value": hex(live),
                "method": f"stock {source} = {live}, read live (fresh Story Mode, {date}) by pool_census",
            }
        )
    return fragment


def _relative(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return str(path)


def choose_pid(reader, lane: dict[str, int], pid: int | None) -> int:
    """--pid, else the GTA process whose store objects hold their pinned vtables (the decoy's do not)."""
    if pid is not None:
        return pid
    candidates: list[int] = []
    foreground = reader.foreground()
    if int(foreground.get("pid") or 0) > 0:
        candidates.append(int(foreground["pid"]))
    for row in reader.processes():
        if row.get("name") == "eboot.bin" and int(row["pid"]) not in candidates:
            candidates.append(int(row["pid"]))
    name = STORES[6][1]  # MetaDataStore
    for candidate in candidates:
        try:
            if _u64(reader.read(candidate, lane[name], 8), 0) == lane[f"{name}Vtable"]:
                return candidate
        except Exception:  # an unreadable candidate is just not the game process
            continue
    raise CensusError(f"no eboot.bin process holds the {name} vtable (pids tried: {candidates})")


def census(
    reader, stem: str, *, pid: int | None = None, config: bool = True, root: Path = ROOT, date: str | None = None
) -> dict[str, object]:
    lane = lane_for_target(stem, root)
    chosen = choose_pid(reader, lane, pid)
    result: dict[str, object] = {
        "target": stem,
        "pid": chosen,
        "stores": store_census(reader, chosen, lane),
        "models": model_census(reader, chosen, lane),
    }
    result["txdChain"] = txd_base_chain(result["stores"])  # type: ignore[arg-type]
    if not config:
        return result
    pins_path = pins_for_target(stem, root)
    if pins_path is None:
        result["config"] = {"error": f"no config-manager address recorded for {stem} (no boot-pool pins)"}
        return result
    result["pins"] = _relative(pins_path, root)
    try:
        pins = load_pins(pins_path)
        entries = stock_entries(root)
        values = {name: row["value"] for name, row in pins.items()}
        cfg_offsets = [values[e["key"]] for e in entries if e["kind"] == "cfg" and e["key"] in values]
        result["config"] = config_census(reader, chosen, values, cfg_offsets)
    except CensusError as exc:
        result["config"] = {"error": str(exc)}
        return result
    rows = stock_census(result["config"], pins, entries)
    result["stock"] = rows
    fragment = stock_fragment(rows, pins, date or datetime.date.today().isoformat())
    if fragment:
        result["stockFragment"] = fragment
    return result


def render_stock(result: dict[str, object]) -> list[str]:
    rows = result.get("stock")
    if not isinstance(rows, list) or not rows:
        return []
    lines = [f"  stock values vs pins {result.get('pins')}:"]
    lines.append(f"    {'entry':<8} {'gameconfig':<16} {'live':>8} {'pinned':>8}")
    for row in rows:
        verdict = str(row["state"])
        if verdict == "differs":
            grown = ", the policy delta" if row["delta"] == row.get("policyDelta") else ""
            verdict += f" ({row['delta']:+d}{grown})"
        live = "-" if row["live"] is None else row["live"]
        pinned = "-" if row["pinned"] is None else row["pinned"]
        lines.append(f"    {row['name']:<8} {row['label'] or '-':<16} {live:>8} {pinned:>8}  {verdict}")
    fragment = result.get("stockFragment")
    if isinstance(fragment, list) and fragment:
        lines.append(
            f"  {len(fragment)} kStock* pin(s) missing. Valid only from a fresh Story Mode census (no pack loaded, "
            f"no boot-pool growth); append to the consts of {result.get('pins')}:"
        )
        lines.append(textwrap.indent(",\n".join(json.dumps(row, indent=2) for row in fragment), "    "))
    return lines


def render(result: dict[str, object]) -> str:
    lines = [f"pool census {result['target']} pid {result['pid']}"]
    lines.append("  store     size    used    free  vtable")
    for row in result["stores"]:
        flag = "ok" if row["vtableOk"] else "MISMATCH"
        full = " FULL" if row["full"] else ""
        lines.append(f"  {row['store']:<8} {row['size']:>6} {row['used']:>7} {row['free']:>7}  {flag}{full}")
    chain = result.get("txdChain")
    if isinstance(chain, dict):
        verdict = "ok" if chain["ok"] else "MISMATCH"
        lines.append(f"  txd base {chain['txdBase']} = drawable end {chain['drawableEnd']}: {verdict}")
    lines.append("  table     count/capacity   free")
    for row in result["models"]:
        if "count" in row:
            lines.append(f"  {row['table']:<8} {row['count']:>6}/{row['capacity']:<8} {row['free']:>5}")
        else:
            lines.append(f"  {row['table']:<8} unreadable (vtable {'ok' if row.get('vtableOk') else 'MISMATCH'})")
    config = result.get("config")
    if isinstance(config, dict) and "error" in config:
        lines.append(f"  gameconfig: {config['error']}")
    elif isinstance(config, dict):
        named = sum(1 for key in config["pools"] if not key.startswith("0x"))
        caps = " ".join(f"{k}={v}" for k, v in config["modelInfo"].items())
        lines.append(f"  gameconfig: {config['entries']} PoolSizes entries ({named} named); model caps {caps}")
        stores = ("MetaDataStore", "DwdStore", "TxdStore", "FragmentStore", "DrawableStore", "PtFxAssetStore")
        for name in (*stores, "MapTypesStore"):
            if name in config["pools"]:
                lines.append(f"    {name}: {config['pools'][name]}")
    lines += render_stock(result)
    return "\n".join(lines)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=os.environ.get("PS5_HOST", ""))
    parser.add_argument("--port", type=int, default=int(os.environ.get("PS5DEBUG_PORT") or 744))
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--target", default=os.environ.get("GTAV_TARGET") or "")
    parser.add_argument("--pid", type=int)
    parser.add_argument("--json", type=Path, help="also write the full census as JSON")
    parser.add_argument("--no-config", action="store_true", help="skip the gameconfig dump")
    args = parser.parse_args(argv)
    if not args.host.strip():
        print("[census] set PS5_HOST=<console-ip> for console commands", file=sys.stderr)
        return 2
    if not args.target:
        print("[census] set GTAV_TARGET (or --target) to the running build", file=sys.stderr)
        return 2
    from menu_client import Ps5DebugNg

    client = Ps5DebugNg(args.host.strip(), args.port, args.timeout)
    try:
        result = census(client, args.target, pid=args.pid, config=not args.no_config)
    except CensusError as exc:
        print(f"[census] {exc}", file=sys.stderr)
        return 1
    finally:
        client.close()
    print(render(result))
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
        print(f"[census] json -> {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

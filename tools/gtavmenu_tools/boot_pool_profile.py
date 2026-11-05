"""Boot-time pool growth profiles: one target's reviewed pins plus the shared growth policy.

A target manifest with ``build.bootPools`` names a pins table (``data/boot_pools/<target>.json``: call
site, cave, config manager, stock values) and a generated header. The shared policy
(``data/boot_pools/policy.json``) says how much each entry grows. This module turns the two into a
profile row, renders the C the loader compiles, and gives the universal registry a contract bound to
the reviewed pins, policy and committed header. The private generator adds the full lane-schema
review on top and writes the per-target header.
"""

from __future__ import annotations

import hashlib
import json
import re
import struct
from collections.abc import Callable
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
POLICY_PATH = REPO_ROOT / "data/boot_pools/policy.json"
KINDS = {"node": "GTAV_BOOT_POOL_KIND_NODE", "cfg": "GTAV_BOOT_POOL_KIND_CFG"}
ENTRIES_MAX = 16
CFG_OFFSET_MAX = 0x1000
REQUIRED_CONSTS = (
    "kCaveAddress",
    "kCaveCapacity",
    "kCavePageMask",
    "kLoadGameConfigCallSite",
    "kLoadGameConfigTarget",
    "kConfigManager",
    "kConfigDoneFlag",
)
# Confidence labels a relocation draft may carry that must never reach a build.
REFUSED_CONFIDENCE = frozenset({"low", "ambiguous", "missing", "conflict"})
NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
HEX_RE = re.compile(r"(?:[0-9a-f]{2})+")


class BootPoolProfileError(ValueError):
    pass


def _int(value: str | int) -> int:
    return value if isinstance(value, int) else int(value, 0)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_policy(path: Path = POLICY_PATH) -> list[dict]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schemaVersion") != 1 or not isinstance(payload.get("entries"), list):
        raise BootPoolProfileError(f"{path}: schemaVersion 1 with an entries list is required")
    entries = payload["entries"]
    if not 0 < len(entries) <= ENTRIES_MAX:
        raise BootPoolProfileError(f"{path}: 1..{ENTRIES_MAX} entries are required")
    names = set()
    for entry in entries:
        if set(entry) != {"name", "kind", "key", "stock", "delta", "max"}:
            raise BootPoolProfileError(f"{path}: entry {entry!r} needs exactly name, kind, key, stock, delta, max")
        if entry["kind"] not in KINDS or entry["name"] in names:
            raise BootPoolProfileError(f"{path}: entry {entry['name']!r} has an unknown kind or a duplicate name")
        if not isinstance(entry["delta"], int) or entry["delta"] <= 0:
            raise BootPoolProfileError(f"{path}: entry {entry['name']!r} must grow (delta > 0)")
        names.add(entry["name"])
    return entries


def parse_pins(pins: object, source: Path) -> tuple[dict[str, int], dict[str, bytes]]:
    """The named integers and byte strings of a reviewed pins table."""
    if not isinstance(pins, dict) or pins.get("target") != source.stem:
        raise BootPoolProfileError(f"{source.name}: target must equal the file stem {source.stem!r}")
    consts, arrays = pins.get("consts"), pins.get("arrays")
    if not isinstance(consts, list) or not isinstance(arrays, list):
        raise BootPoolProfileError(f"{source.name}: consts and arrays must be lists")
    values: dict[str, int] = {}
    data: dict[str, bytes] = {}
    for kind, entries in (("consts", consts), ("arrays", arrays)):
        for entry in entries:
            name = entry.get("name") if isinstance(entry, dict) else None
            if not isinstance(name, str) or not NAME_RE.fullmatch(name) or name in values or name in data:
                raise BootPoolProfileError(f"{source.name}: {kind} entry {name!r} is unnamed or duplicated")
            confidence = entry.get("confidence")
            if isinstance(confidence, str) and confidence in REFUSED_CONFIDENCE:
                raise BootPoolProfileError(f"{source.name}: {name} needs review before it can be built")
            if kind == "consts":
                value = entry.get("value")
                try:
                    number = _int(value) if isinstance(value, (str, int)) and not isinstance(value, bool) else -1
                except ValueError:
                    number = -1
                if not 0 <= number < 1 << 64:
                    raise BootPoolProfileError(f"{source.name}: {name} value {value!r} is not a 64-bit integer")
                values[name] = number
            else:
                text = entry.get("hex")
                if not isinstance(text, str) or not HEX_RE.fullmatch(text):
                    raise BootPoolProfileError(f"{source.name}: {name} hex must be lowercase byte pairs")
                data[name] = bytes.fromhex(text)
    return values, data


Review = Callable[[dict, Path], None]


def manifest_row(
    manifest: dict, stem: str, policy: list[dict], *, review: Review | None = None, root: Path = REPO_ROOT
) -> dict | None:
    """The profile row for one manifest, or None when it carries no ``build.bootPools``."""
    build = manifest.get("build", {})
    item = build.get("bootPools")
    if item is None:
        return None
    header = f"gtavmenu/boot_pool_profile_{stem}_generated.h"
    if item.get("header") != header:
        raise BootPoolProfileError(f"{stem}: build.bootPools.header must be {header}")
    pins_path = root / item["json"]
    pins = json.loads(pins_path.read_text(encoding="utf-8"))
    if review is not None:
        review(pins, pins_path)
    consts, arrays = parse_pins(pins, pins_path)
    missing = [name for name in REQUIRED_CONSTS if name not in consts]
    missing += [entry[key] for entry in policy for key in ("key", "stock") if entry[key] not in consts]
    if missing:
        raise BootPoolProfileError(f"{pins_path.name}: missing pins {', '.join(sorted(set(missing)))}")
    call, context = arrays.get("kLoadGameConfigCallBytes", b""), arrays.get("kLoadGameConfigCallContext", b"")
    if len(call) != 5 or len(context) != 16 or context[:5] != call or call[0] != 0xE8:
        raise BootPoolProfileError(
            f"{pins_path.name}: the call bytes must be one E8 call that opens the 16-byte context"
        )
    site = consts["kLoadGameConfigCallSite"]
    if site + 5 + struct.unpack_from("<i", call, 1)[0] != consts["kLoadGameConfigTarget"]:
        raise BootPoolProfileError(f"{pins_path.name}: the call bytes do not reach kLoadGameConfigTarget")
    if consts["kCaveAddress"] != _int(manifest["loader"]["caveAddress"]):
        raise BootPoolProfileError(f"{stem}: boot-pool cave differs from loader.caveAddress")
    overrides = []
    for entry in policy:
        stock, key = consts[entry["stock"]], consts[entry["key"]]
        new = stock + entry["delta"]
        if stock <= 0 or new > entry["max"]:
            raise BootPoolProfileError(
                f"{pins_path.name}: {entry['name']} {stock}+{entry['delta']} exceeds {entry['max']}"
            )
        if entry["kind"] == "cfg" and (key % 4 or key >= CFG_OFFSET_MAX):
            raise BootPoolProfileError(f"{pins_path.name}: {entry['name']} cfg offset {key:#x} is not a stub offset")
        overrides.append({"name": entry["name"], "kind": entry["kind"], "key": key, "stock": stock, "new": new})
    return {
        "stem": stem,
        "header": header,
        "targetId": manifest["targetId"],
        "enabled": bool(build.get("features", {}).get("bootPools", False)),
        "consts": consts,
        "call": call,
        "context": context,
        "overrides": overrides,
    }


def load_row(
    manifest_path: Path, policy: list[dict], *, review: Review | None = None, root: Path = REPO_ROOT
) -> dict | None:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    return manifest_row(manifest, manifest_path.stem, policy, review=review, root=root)


def profile(row: dict) -> dict:
    """The loader-facing values of a row, as plain JSON (the universal registry's contract body)."""
    c = row["consts"]
    return {
        "targetId": row["targetId"],
        "enabled": bool(row["enabled"]),
        "caveAddress": c["kCaveAddress"],
        "caveCapacity": c["kCaveCapacity"],
        "cavePageMask": c["kCavePageMask"],
        "callSite": c["kLoadGameConfigCallSite"],
        "loadGameConfig": c["kLoadGameConfigTarget"],
        "configManager": c["kConfigManager"],
        "configDoneFlag": c["kConfigDoneFlag"],
        "callBytes": row["call"].hex(),
        "callContext": row["context"].hex(),
        "overrides": [dict(o) for o in row["overrides"]],
    }


def _bytes(data: bytes) -> str:
    return "{" + ", ".join(f"0x{byte:02x}" for byte in data) + "}"


def render_overrides(values: dict, symbol: str) -> list[str]:
    lines = [f"static const GtavBootPoolOverride {symbol}[] = {{"]
    for o in values["overrides"]:
        lines.append(f'    {{"{o["name"]}", {KINDS[o["kind"]]}, 0x{o["key"]:x}u, {o["stock"]}, {o["new"]}}},')
    lines.append("};")
    return lines


def profile_fields(values: dict, overrides_symbol: str) -> list[str]:
    return [
        f'.target_id = "{values["targetId"]}"',
        f".enabled = {int(values['enabled'])}",
        f".cave_address = 0x{values['caveAddress']:x}ull",
        f".cave_capacity = 0x{values['caveCapacity']:x}u",
        f".cave_page_mask = 0x{values['cavePageMask']:x}u",
        f".call_site = 0x{values['callSite']:x}ull",
        f".load_game_config = 0x{values['loadGameConfig']:x}ull",
        f".config_manager = 0x{values['configManager']:x}ull",
        f".config_done_flag = 0x{values['configDoneFlag']:x}ull",
        f".call_bytes = {_bytes(bytes.fromhex(values['callBytes']))}",
        f".call_context = {_bytes(bytes.fromhex(values['callContext']))}",
        f".overrides = {overrides_symbol}",
        f".override_count = {len(values['overrides'])}u",
    ]


def render_target_header(row: dict) -> str:
    """The single-target header: ``gtav_boot_pool_overrides`` and the ``GTAV_BOOT_POOL_PROFILE`` initializer."""
    values = profile(row)
    lines = [
        f"/* Generated by devtools/generators/generate_boot_pool_profiles.py for {row['stem']}. */",
        "#pragma once",
        "",
        '#include "gtavmenu/boot_pool.h"',
        "",
        *render_overrides(values, "gtav_boot_pool_overrides"),
        "",
        "#define GTAV_BOOT_POOL_PROFILE \\",
        "  { \\",
        *(f"    {field}, \\" for field in profile_fields(values, "gtav_boot_pool_overrides")),
        "  }",
        "",
    ]
    return "\n".join(lines)


def contract(manifest: dict, *, root: Path = REPO_ROOT, policy_path: Path | None = None) -> dict | None:
    """The reviewed boot-pool profile of one manifest, bound to its pins, policy and committed header.

    Returns None for a target without ``build.bootPools``. Refuses stale pins (``build.bootPools.sha256``)
    and a committed header that no longer matches the pins and policy.
    """
    item = manifest.get("build", {}).get("bootPools")
    if item is None:
        return None
    title, version = manifest.get("titleId"), manifest.get("contentVersion")
    if not isinstance(title, str) or not isinstance(version, str):
        raise BootPoolProfileError("boot-pool contract needs titleId and contentVersion")
    stem = f"{title.lower()}-{version}"
    policy_path = policy_path or root / "data/boot_pools/policy.json"
    pins_path = root / item["json"]
    pins_sha256 = _sha256(pins_path)
    if pins_sha256 != item.get("sha256"):
        raise BootPoolProfileError(f"{stem}: build.bootPools.sha256 is stale for {item['json']}")
    row = manifest_row(manifest, stem, load_policy(policy_path), root=root)
    assert row is not None
    header_path = root / "include" / row["header"]
    if not header_path.is_file() or header_path.read_text(encoding="utf-8") != render_target_header(row):
        raise BootPoolProfileError(f"{stem}: include/{row['header']} is stale for its pins and policy")
    return {
        **profile(row),
        "header": row["header"],
        "pinsSha256": pins_sha256,
        "policySha256": _sha256(policy_path),
    }

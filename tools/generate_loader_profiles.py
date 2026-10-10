#!/usr/bin/env python3
"""Generate a universal loader registry from reviewed manifests and built workers."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path

from gtavmenu_tools.target_profile import expected_build_config, integer, load_manifest, sha256_file
from gtavmenu_tools.universal_package import TARGETS as SUPPORTED_TARGETS
from gtavmenu_tools.universal_package import expected_contract, inventory_sha256

try:
    from tools.check_worker_span import image_span
except ModuleNotFoundError:
    from check_worker_span import image_span

ROOT = Path(__file__).resolve().parents[1]
CYCLE_KEYS = ("epoch", "reset", "producer", "consumer", "count0", "count1", "text", "counter")
FINGERPRINT_KEYS = ("leafInvoke", "groupInvoke", "group2Dispatch", "registration")


def write_changed(path: Path, data: str) -> None:
    if path.is_file() and path.read_text(encoding="utf-8") == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(data, encoding="utf-8")
    temporary.replace(path)


def source_commit(root: Path) -> str:
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()


def validate_worker_config(manifest: dict, config: dict, manifest_path: Path) -> None:
    if not isinstance(config, dict):
        raise ValueError(f"{manifest_path.stem}: worker build configuration must be an object")
    expected = {
        **expected_build_config(manifest, "standalone"),
        "target": manifest_path.stem,
        "profile": "production",
        "delivery": "standalone",
        "require_context": 1,
        "target_manifest_sha256": sha256_file(manifest_path),
    }
    for key, value in expected.items():
        actual = config.get(key)
        if isinstance(value, int):
            try:
                actual = integer(actual, key)
            except ValueError:
                actual = None
        if actual != value:
            raise ValueError(f"{manifest_path.stem}: worker build setting {key} must be {value!r}")


def registry_row(root: Path, build_dir: Path, target: str) -> dict:
    manifest_path = root / "data/targets" / f"{target}.json"
    manifest, profile = load_manifest(manifest_path)
    if profile["injection"]["lane"] != "ptrace-free-cave" or not profile["features"]["phaseIntercept"]:
        raise ValueError(f"{target}: universal delivery requires the reviewed cave and render-phase lane")
    directory = build_dir / target / "production/standalone"
    worker = directory / "gtav-menu-feature-menu.elf"
    config_path = directory / "build-config.json"
    config = json.loads(config_path.read_text(encoding="utf-8"))
    validate_worker_config(manifest, config, manifest_path)
    if worker.stat().st_size > 16 * 1024 * 1024:
        raise ValueError(f"{target}: worker size exceeds the loader limit")
    data = worker.read_bytes()
    span = image_span(data)
    if struct.unpack_from("<HH", data, 16) != (3, 62):
        raise ValueError(f"{target}: worker must be an x86-64 ET_DYN ELF")
    phoff = struct.unpack_from("<Q", data, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 54)
    for index in range(phnum):
        fields = struct.unpack_from("<IIQQQQQQ", data, phoff + index * phentsize)
        kind, _flags, offset, address, _physical, file_size, memory_size, _alignment = fields
        if kind == 1 and (file_size > memory_size or offset + file_size > len(data) or address + memory_size >= 2**64):
            raise ValueError(f"{target}: worker loadable segment is invalid")
    allocation = integer(manifest["loader"]["caveAllocationBytes"], "caveAllocationBytes")
    if span > allocation:
        raise ValueError(f"{target}: worker size/span exceeds the loader limits")
    return {
        "target": target,
        "targetId": manifest["targetId"],
        "titleId": manifest["titleId"],
        "contentId": manifest["contentId"],
        "contentVersion": manifest["contentVersion"],
        "targetManifestSha256": sha256_file(manifest_path),
        "customPacks": bool(profile["features"]["customPacks"]),
        "worker": {
            "path": str(worker.resolve()),
            "sha256": hashlib.sha256(data).hexdigest(),
            "size": len(data),
            "mappedSpan": span,
        },
        "workerBuildConfig": {"path": str(config_path.resolve()), "sha256": sha256_file(config_path)},
        "contract": expected_contract(manifest),
    }


def validate_identities(rows: list[dict]) -> None:
    identities: set[tuple[str, str]] = set()
    signatures: set[tuple[str, int, bytes]] = set()
    ids: set[str] = set()
    for row in rows:
        identity = (row["titleId"], row["contentVersion"])
        loader = row["contract"]["loader"]
        signature = (
            row["titleId"],
            integer(loader["versionSignatureAddr"], "versionSignatureAddr"),
            bytes.fromhex(loader["versionSignatureExpectedCompact"]),
        )
        if identity in identities or row["targetId"] in ids or signature in signatures:
            raise ValueError("duplicate or ambiguous title/build profile")
        identities.add(identity)
        signatures.add(signature)
        ids.add(row["targetId"])


def number(value: str | int) -> str:
    return f"0x{integer(value, 'profile value'):x}ull"


def byte_array(value: str) -> str:
    return "{" + ", ".join(f"0x{byte:02x}" for byte in bytes.fromhex(value)) + "}"


def render_header(rows: list[dict]) -> str:
    lines = [
        "/* Generated by tools/generate_loader_profiles.py. */",
        "#pragma once",
        '#include "gtavmenu/loader_target_profile.h"',
    ]
    for index in range(len(rows)):
        lines.append(
            f"extern const uint8_t gtav_universal_worker_{index}_start[], gtav_universal_worker_{index}_end[];"
        )
    lines.append("static const GtavLoaderTargetProfile gtav_loader_profiles[] = {")
    for index, row in enumerate(rows):
        loader = row["contract"]["loader"]
        phase = row["contract"]["renderPhase"]
        text = row["contract"]["text"]
        lines.extend(["  {", f'    .title_id = {json.dumps(row["titleId"])},', "    .pin = {"])
        pin = {
            "target_id": json.dumps(row["targetId"]),
            "version_signature_addr": number(loader["versionSignatureAddr"]),
            "version_signature_expected": byte_array(loader["versionSignatureExpectedCompact"]),
            "broker_target": number(loader["frameHookTarget"]),
            "broker_continuation": number(loader["brokerContinuation"]),
            "broker_patch_len": number(loader["brokerPatchLen"]),
            "broker_stolen_len": number(loader["brokerStolenLen"]),
            "broker_expected": byte_array(loader["brokerExpectedBytesCompact"]),
            "text_live_start": number(text["liveStart"]),
            "text_live_end": number(text["liveEnd"]),
            "sp_ready_addr": number(loader["playerPedAnchor"]),
            "sp_ready_deref_offset": number(loader["playerPedOffset"]),
        }
        lines.extend(f"      .{key} = {value}," for key, value in pin.items())
        lines.extend(
            [
                "    },",
                f'    .cave_address = {number(loader["caveAddress"])},',
                f'    .cave_allocation = {number(loader["caveAllocationBytes"])},',
                f'    .custom_packs = {int(row["customPacks"])},',
                "    .render = {",
            ]
        )
        for field, key in (
            ("root", "root"),
            ("leaf_vtable", "leafVtable"),
            ("group_vtable", "groupVtable"),
            ("task_id", "taskId"),
            ("original", "original"),
        ):
            lines.append(f"      .{field} = {number(phase[key])},")
        lines.append("      .fingerprints = {")
        for key in FINGERPRINT_KEYS:
            item = phase["fingerprints"][key]
            lines.append(
                f'        {{.address = {number(item["address"])}, .bytes = {byte_array(item["bytes"])}, .size = {len(bytes.fromhex(item["bytes"]))}u}},'
            )
        lines.extend(
            [
                "      },",
                "    },",
                "    .cycle_addresses = {" + ", ".join(number(phase["cycle"][key]) for key in CYCLE_KEYS) + "},",
                f"    .worker_start = gtav_universal_worker_{index}_start,",
                f"    .worker_end = gtav_universal_worker_{index}_end,",
                f'    .worker_sha256 = "{row["worker"]["sha256"]}",',
                "  },",
            ]
        )
    lines.extend(
        ["};", "#define GTAV_LOADER_PROFILE_COUNT (sizeof(gtav_loader_profiles) / sizeof(gtav_loader_profiles[0]))", ""]
    )
    return "\n".join(lines)


def render_assembly(rows: list[dict]) -> str:
    lines = ['    .section .rodata.gtav_workers,"a",@progbits']
    for index, row in enumerate(rows):
        path = str(Path(row["worker"]["path"]).resolve())
        if any(char in path for char in ('"', "\\", "\n", "\r")):
            raise ValueError("worker path cannot be represented safely in assembly")
        symbol = f"gtav_universal_worker_{index}"
        lines.extend(
            [
                "    .balign 16",
                f"    .global {symbol}_start, {symbol}_end",
                f"{symbol}_start:",
                f'    .incbin "{path}"',
                f"{symbol}_end:",
            ]
        )
    lines.extend(['    .section .note.GNU-stack,"",@progbits', ""])
    return "\n".join(lines)


def generate(root: Path, build_dir: Path, output: Path) -> dict:
    rows = [registry_row(root, build_dir, target) for target in SUPPORTED_TARGETS]
    validate_identities(rows)
    registry = {
        "schemaVersion": 1,
        "kind": "gtavmenu-universal-registry",
        "sourceCommit": source_commit(root),
        "supportedTargets": rows,
    }
    registry["inventorySha256"] = inventory_sha256(registry["sourceCommit"], rows)
    header, assembly = render_header(rows), render_assembly(rows)
    write_changed(output / "loader_profiles.h", header)
    write_changed(output / "embedded_workers.S", assembly)
    write_changed(output / "registry.json", json.dumps(registry, indent=2, sort_keys=True) + "\n")
    return registry


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/ps5")
    parser.add_argument("--output", type=Path, default=ROOT / "build/ps5/universal/production")
    args = parser.parse_args()
    try:
        generate(ROOT, args.build_dir, args.output)
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as exc:
        print(f"generate_loader_profiles: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

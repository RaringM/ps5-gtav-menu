#!/usr/bin/env python3
"""Inspect PC vehicle materials with public layouts and verified retail template inputs."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path, PurePosixPath

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from gtavmenu_tools import (  # noqa: E402
    asset_formats,
    asset_textures,
    drawable_contracts,
    vehicle_materials,
    vehicle_templates,
)
from gtavmenu_tools.asset_formats import AssetError, Limits  # noqa: E402
from gtavmenu_tools.assets import Budget, emit, read_file  # noqa: E402
from gtavmenu_tools.host_paths import build_dir  # noqa: E402
from gtavmenu_tools.io import sha256_file  # noqa: E402
from gtavmenu_tools.vehicle_repair_inputs import validate_fixture  # noqa: E402


def add_pc_source_arguments(parser: argparse.ArgumentParser) -> None:
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--pc-archive", type=Path)
    group.add_argument("--pc-fixture", type=Path)


def open_pc_source(args: argparse.Namespace, budget: Budget) -> dict:
    archive_path = getattr(args, "pc_archive", None)
    fixture_path = getattr(args, "pc_fixture", None)
    if bool(archive_path) == bool(fixture_path):
        raise AssetError("select exactly one PC archive or imported fixture")
    if archive_path:
        blob = read_file(archive_path, budget)
        return {
            "kind": "archive",
            "path": archive_path,
            "archive": blob,
            "archiveSha256": hashlib.sha256(blob).hexdigest(),
            "observed": [],
        }
    if fixture_path.is_symlink() or not fixture_path.is_dir():
        raise AssetError("PC fixture must be a real directory")
    validated_manifest = validate_fixture(fixture_path, budget.limits)
    manifest_path = fixture_path / "manifest.json"
    manifest_blob = read_file(manifest_path, budget)
    if len(manifest_blob) > budget.limits.max_metadata_bytes:
        raise AssetError("PC fixture manifest exceeds metadata budget")
    manifest = json.loads(manifest_blob)
    if manifest != validated_manifest:
        raise AssetError("PC fixture manifest changed during validation")
    qualification = manifest.get("qualification") if isinstance(manifest, dict) else None
    source_identity = manifest.get("source") if isinstance(manifest, dict) else None
    if (
        not isinstance(manifest, dict)
        or manifest.get("kind") != "gtavmenu-pc-asset-import"
        or manifest.get("operation") != "vehicle"
        or not isinstance(qualification, dict)
        or qualification.get("selectedMemberIdentityVerified") is not True
        or not isinstance(source_identity, dict)
        or not isinstance(manifest.get("resources"), list)
    ):
        raise AssetError("PC fixture manifest is not a verified vehicle import")
    if len(manifest["resources"]) > budget.limits.max_entries:
        raise AssetError("PC fixture resource list exceeds entry budget")
    resources = {}
    aliases = set()
    for row in manifest["resources"]:
        if not isinstance(row, dict):
            raise AssetError("PC fixture resource record is not an object")
        member = row.get("member")
        if not isinstance(member, str):
            raise AssetError("PC fixture resource member is missing")
        folded = member.casefold()
        if folded in aliases:
            raise AssetError("PC fixture has duplicate or case-colliding resource members")
        aliases.add(folded)
        resources[member] = row
    archive_sha = source_identity.get("sha256")
    if (
        not isinstance(archive_sha, str)
        or len(archive_sha) != 64
        or any(character not in "0123456789abcdef" for character in archive_sha)
    ):
        raise AssetError("PC fixture lacks its source archive identity")
    return {
        "kind": "fixture",
        "path": fixture_path,
        "manifestPath": manifest_path,
        "manifestBlob": manifest_blob,
        "manifestSha256": hashlib.sha256(manifest_blob).hexdigest(),
        "archiveSha256": archive_sha,
        "resources": resources,
        "observed": [],
    }


def source_resource(source: dict, member: str, budget: Budget, suffix: str) -> bytes:
    if source["kind"] == "archive":
        return vehicle_materials.selected_resource(source["archive"], member, budget, suffix)
    matches = [(name, row) for name, row in source["resources"].items() if name.casefold() == member.casefold()]
    if len(matches) != 1 or matches[0][0] != member:
        raise AssetError(f"fixture member is missing, duplicated, or a case alias: {member}")
    name, row = matches[0]
    if not name.lower().endswith(suffix):
        raise AssetError(f"fixture member must have suffix {suffix}: {name}")
    parts = []
    for segment in name.split("!/"):
        parts.extend(PurePosixPath(segment).parts)
    path = source["path"].joinpath("resources", *parts)
    if not path.resolve().is_relative_to(source["path"].resolve()):
        raise AssetError("fixture resource path escapes its root")
    if any(parent.is_symlink() for parent in (path, *path.parents) if parent.is_relative_to(source["path"])):
        raise AssetError("fixture resource traverses a symbolic link")
    blob = read_file(path, budget)
    if row.get("bytes") != len(blob) or row.get("sha256") != hashlib.sha256(blob).hexdigest():
        raise AssetError(f"fixture resource identity differs from its manifest: {name}")
    source["observed"].append((path, hashlib.sha256(blob).hexdigest()))
    return blob


def source_evidence(source: dict) -> dict:
    result = {"pcArchiveSha256": source["archiveSha256"]}
    if source["kind"] == "fixture":
        result.update(
            pcSource="verified-import-fixture",
            pcFixtureManifestSha256=source["manifestSha256"],
        )
    else:
        result["pcSource"] = "archive"
    return result


def source_matches_report(source: dict, report_sources: dict) -> bool:
    if report_sources.get("pcArchiveSha256") != source["archiveSha256"]:
        return False
    if source["kind"] == "archive":
        # Reports created before fixture support did not carry an explicit source kind.
        return report_sources.get("pcSource") in (None, "archive") and "pcFixtureManifestSha256" not in report_sources
    return (
        report_sources.get("pcSource") == "verified-import-fixture"
        and report_sources.get("pcFixtureManifestSha256") == source["manifestSha256"]
    )


def verify_pc_source(source: dict) -> None:
    if source["kind"] == "archive":
        if sha256_file(source["path"]) != source["archiveSha256"]:
            raise AssetError("PC archive changed during reproduction")
        return
    if sha256_file(source["manifestPath"]) != source["manifestSha256"]:
        raise AssetError("PC fixture manifest changed during reproduction")
    for path, expected in source["observed"]:
        if sha256_file(path) != expected:
            raise AssetError("PC fixture resource changed during reproduction")


def tool_hashes() -> dict[str, str]:
    return {
        "inspectorSha256": sha256_file(Path(__file__)),
        "materialReaderSha256": sha256_file(Path(vehicle_materials.__file__)),
        "templateReaderSha256": sha256_file(Path(vehicle_templates.__file__)),
        "containerReaderSha256": sha256_file(Path(asset_formats.__file__)),
        "textureReaderSha256": sha256_file(Path(asset_textures.__file__)),
        "drawableContractsSha256": sha256_file(drawable_contracts.DEFAULT),
    }


def inspect(args: argparse.Namespace) -> dict:
    before = tool_hashes()
    budget = Budget(Limits())
    source = open_pc_source(args, budget)
    contract = vehicle_materials.material_contract()
    templates = getattr(args, "templates", None) or build_dir(Path(__file__).resolve().parents[1]) / "retail-templates"
    stock_path = getattr(args, "stock_native_resource", None) or templates / "corpus/vehshare-a.ptd"
    stock, usage = vehicle_templates.stock_observations(
        stock_path, dictionary_name=getattr(args, "stock_dictionary_name", "vehshare"), budget=budget
    )
    members = args.resource_member
    if not members or len(members) > budget.limits.max_entries or len(members) != len(set(members)):
        raise AssetError("drawable member list is empty, duplicated or over budget")
    resources, identities = [], []
    for member in members:
        blob = source_resource(source, member, budget, ".yft")
        header, payload = asset_formats.decode_resource(blob, budget.limits.max_file_bytes)
        budget.charge(len(payload))
        row = vehicle_materials.inspect_legacy_yft(payload, header, contract, budget.limits)
        resources.append(row | {"member": member})
        identities.append(
            {
                "kind": "fragment",
                "member": member,
                "resourceBytes": len(blob),
                "resourceSha256": hashlib.sha256(blob).hexdigest(),
                "payloadBytes": len(payload),
                "payloadSha256": hashlib.sha256(payload).hexdigest(),
            }
        )
    blob = source_resource(source, args.texture_member, budget, ".ytd")
    header, payload = asset_formats.decode_resource(blob, budget.limits.max_file_bytes)
    budget.charge(len(payload))
    dictionary = asset_textures.inspect_legacy_dictionary(payload, header, budget.limits)
    identities.append(
        {
            "kind": "textureDictionary",
            "member": args.texture_member,
            "resourceBytes": len(blob),
            "resourceSha256": hashlib.sha256(blob).hexdigest(),
            "payloadBytes": len(payload),
            "payloadSha256": hashlib.sha256(payload).hexdigest(),
        }
    )
    summary = vehicle_materials.summarize(resources, dictionary, stock, usage)
    verify_pc_source(source)
    if sha256_file(stock_path) != stock["resourceSha256"] or tool_hashes() != before:
        raise AssetError("material input or tool changed during inspection")
    return {
        "schemaVersion": 2,
        "kind": "gtavmenu-pc-legacy-drawable-material-binding-observation",
        "sources": {
            **source_evidence(source),
            "resources": identities,
            "selectedNativeStockDictionary": stock,
            "selectedNativeUsageObservation": usage,
            "drawableContractsSha256": before["drawableContractsSha256"],
            "parameterNameSource": "published-shader-mappings",
        },
        "tools": before,
        "pcLegacyContract": {name: value for name, value in contract.items() if name != "parameterNames"},
        "textureDictionary": dictionary,
        "resources": resources,
        "summary": summary,
        "qualification": {
            "pcLegacyMainDrawableMaterialBindingsVerified": True,
            "allTextureParameterNamesResolved": summary["allTextureParameterNamesResolved"],
            "selectedStockTextureNameClosureVerified": summary["selectedStockTextureNameClosureVerified"],
            "selectedStockSamplerRoleCorrelationVerified": True,
            "nativeMaterialContractValidated": False,
            "nativeUsageMappingValidated": False,
            "nativeGpuLayoutValidated": False,
            "conversionAvailable": False,
            "runtimeEnabled": False,
        },
        "limitations": [
            "Legacy YFT v162 main drawable materials only; no physics-child material inspection",
            "Parameter names are diagnostic hints from published shader mappings; unknown hashes remain unknown",
            "Stock dictionary names and usage words are read from the verified user template, not frozen reports",
            "Sampler-role correlations do not define native usage mapping, pixel layout or runtime compatibility",
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_pc_source_arguments(parser)
    parser.add_argument("--resource-member", action="append", required=True)
    parser.add_argument("--texture-member", required=True)
    parser.add_argument("--templates", type=Path)
    parser.add_argument("--stock-native-resource", type=Path)
    parser.add_argument("--stock-dictionary-name", default="vehshare")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        report = inspect(args)
        emit(report, args.output, args.pc_archive or args.pc_fixture / "manifest.json")
        print(json.dumps(report["summary"], sort_keys=True))
        return 0
    except (AssetError, OSError, ValueError, KeyError, TypeError) as error:
        print(f"inspect_pc_vehicle_materials: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

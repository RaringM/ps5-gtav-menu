#!/usr/bin/env python3
"""Inspect, convert and verify experimental PC Legacy BC1/BC3/BC5/BGRA8 packages.

Zero exit status means the requested offline operation succeeded, not that its
output is release-ready. Conversion requires explicit experimental policies;
unsupported entries reject the entire dictionary. No console is contacted.
"""

from __future__ import annotations

import argparse
import json
import os
import tempfile
from collections.abc import Sequence
from pathlib import Path

from gtavmenu_tools.asset_formats import AssetError
from gtavmenu_tools.custom_pack_schema import (
    CustomPackSchemaError,
    LooseResource,
    build_manifest,
    canonical_json,
    expected_package,
    load_json,
    publish_package,
    read_regular,
    target_identity,
    verify_package,
)
from gtavmenu_tools.texture_candidates import (
    AUTHORING_POLICY,
    BGRA_REPORT_VERSION,
    CHILD_MATERIAL_SCOPE,
    CONVERSION_LIMITS,
    LEGACY_NAME_POLICY,
    MAIN_MATERIAL_SCOPE,
    MATERIAL_REPORT_VERSION,
    METADATA_POLICY,
    NAME_REPORT_VERSION,
    NO_MATERIAL_SCOPE,
    OBSERVED_SOURCE_POLICY,
    SOURCE_NAME_POLICY,
    STATIC_REPORT_VERSION,
    STATIC_SOURCE_POLICY,
    SUPPORTED_REPORT_VERSIONS,
    build_texture_authoring_template,
    check_texture_authoring_profile,
    convert_texture_candidate,
    inspect_texture_source,
    verify_texture_candidate,
)
from gtavmenu_tools.texture_choices import apply_texture_choices, build_texture_choices

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TARGET = ROOT / "data/targets/ppsa04264-01.010.002.json"
_EVIDENCE_TARGET = {
    "targetId": "PPSA04264_01.010.002_DISC",
    "titleId": "PPSA04264",
    "contentVersion": "01.010.002",
    "normalizedElfSha256": "2a3419b404a5d10f2d36d8056916e65afad6352b78489365e5f1003f8e953c0d",
}


def _require_evidence_target(target: dict) -> None:
    # Restrict the experimental format evidence, not a claim of runtime support.
    if not isinstance(target, dict) or any(target.get(key) != value for key, value in _EVIDENCE_TARGET.items()):
        raise CustomPackSchemaError(
            "candidate target identity mismatch: only the 01.010.002 evidence profile is admitted"
        )


def prepare(
    source_blob: bytes,
    target: dict,
    *,
    pack_id: str,
    dictionary: str,
    tile_mode: int,
    metadata_policy: str,
    experimental: bool,
    output: Path,
    authoring_profile: dict | None = None,
    material_blobs: Sequence[bytes] = (),
    name_policy: str = LEGACY_NAME_POLICY,
) -> dict:
    _require_evidence_target(target)
    if output.exists() or output.is_symlink():
        raise CustomPackSchemaError(f"refusing to replace an existing package: {output}")
    blob, report = convert_texture_candidate(
        source_blob,
        tile_mode=tile_mode,
        metadata_policy=metadata_policy,
        experimental=experimental,
        authoring_profile=authoring_profile,
        material_blobs=material_blobs,
        name_policy=name_policy,
    )
    path = f"resources/{dictionary}.ptd"
    manifest = build_manifest(
        pack_id,
        target,
        [
            LooseResource(
                path=path,
                resource_class="texture-dictionary",
                container="RSC7",
                logical_name=dictionary,
                data=blob,
                source=report["source"],
                oracle=report,
            )
        ],
    )
    files = expected_package(manifest, {path: blob})
    # Verify readers' manifest limits before publishing a package they cannot read.
    if any(len(files[name]) > 1024 * 1024 for name in ("manifest.json", "catalog-entry.json")):
        raise CustomPackSchemaError("candidate package metadata exceeds the verification budget")
    # Publication validates the staged tree within these reader bounds before
    # its atomic create-new commit. No fallible readback is deferred until after
    # the output directory becomes visible.
    publish_package(
        output,
        files,
        max_resources=1,
        max_resource_bytes=CONVERSION_LIMITS.max_file_bytes,
    )
    return report


def verify(
    source_blob: bytes,
    target: dict,
    package: Path,
    *,
    authoring_profile: dict | None = None,
    material_blobs: Sequence[bytes] = (),
) -> dict:
    _require_evidence_target(target)
    manifest = verify_package(
        package,
        expected_target=target,
        max_resources=1,
        max_resource_bytes=CONVERSION_LIMITS.max_file_bytes,
    )
    if len(manifest["resources"]) != 1:
        raise CustomPackSchemaError("texture conversion package must contain exactly one complete dictionary")
    resource = manifest["resources"][0]
    if (
        resource["class"] != "texture-dictionary"
        or resource["container"] != "RSC7"
        or resource["path"] != f"resources/{resource['logicalName']}.ptd"
    ):
        raise CustomPackSchemaError("texture conversion package resource class/container differs")
    policies = resource["oracle"].get("policies", {})
    version = resource["oracle"].get("schemaVersion")
    if type(version) is not int or version not in SUPPORTED_REPORT_VERSIONS:
        raise CustomPackSchemaError("unsupported texture conversion report version")
    policy_keys = {"metadata", "layout", "tileMode"} | ({"view"} if version >= 2 else set())
    if version >= BGRA_REPORT_VERSION:
        policy_keys.add("pixels")
    if version >= NAME_REPORT_VERSION:
        policy_keys.add("names")
    if version >= MATERIAL_REPORT_VERSION:
        policy_keys.add("materials")
    if version >= STATIC_REPORT_VERSION:
        policy_keys.add("source")
    if not isinstance(policies, dict) or set(policies) != policy_keys:
        raise CustomPackSchemaError("texture conversion package needs a complete policy object")
    report = verify_texture_candidate(
        source_blob,
        read_regular(package / resource["path"], maximum=CONVERSION_LIMITS.max_file_bytes),
        tile_mode=policies.get("tileMode"),
        metadata_policy=policies.get("metadata"),
        report_version=version,
        authoring_profile=authoring_profile,
        material_blobs=material_blobs,
        name_policy=policies.get("names", LEGACY_NAME_POLICY),
    )
    if canonical_json(report) != canonical_json(resource["oracle"]) or canonical_json(
        report["source"]
    ) != canonical_json(resource["source"]):
        raise CustomPackSchemaError("stored conversion evidence differs from fresh independent readback")
    return report


def _read_materials(paths: list[Path], source_bytes: int) -> list[bytes]:
    if len(paths) > 8:
        raise AssetError("at most eight original material sources may be supplied")
    blobs = []
    remaining = CONVERSION_LIMITS.max_total_bytes - source_bytes
    for path in paths:
        blob = read_regular(path, maximum=min(CONVERSION_LIMITS.max_file_bytes, remaining))
        remaining -= len(blob)
        if remaining <= 0:
            raise AssetError("material sources exceed the cumulative byte budget")
        blobs.append(blob)
    return blobs


def _publish_profile(path: Path, profile: dict) -> None:
    """Publish a complete create-new file; never replace an existing selection."""
    data = canonical_json(profile)
    if path.exists() or path.is_symlink():
        raise CustomPackSchemaError(f"refusing to replace an existing profile: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix=".texture-profile-", dir=path.parent) as temporary:
        temporary.write(data)
        temporary.flush()
        os.fsync(temporary.fileno())
        # Atomic complete-file publication with O_EXCL-like destination semantics.
        os.link(temporary.name, path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    inspect_parser = commands.add_parser("inspect", help="read every source mip and list experimental policy issues")
    profile_parser = commands.add_parser("profile", help="record material bindings with unresolved metadata choices")
    choices_parser = commands.add_parser(
        "choices", help="create compact unresolved choices bound to an evidence profile"
    )
    check_parser = commands.add_parser(
        "profile-check", help="check evidence, all choices and budgets without producing a candidate"
    )
    convert_parser = commands.add_parser("convert", help="create a new inactive experimental PTD package")
    verify_parser = commands.add_parser(
        "verify", help="recheck package structure and every mip against the original YTD"
    )
    for command in (inspect_parser, profile_parser, check_parser, convert_parser, verify_parser):
        command.add_argument("source", type=Path, help="original PC Legacy v13 YTD")
    for command in (profile_parser, check_parser, convert_parser, verify_parser):
        command.add_argument(
            "--material-source",
            type=Path,
            action="append",
            default=[],
            help="original Legacy v162 YFT; repeatable, at most eight",
        )
    for command in (convert_parser, verify_parser):
        command.add_argument("--target", type=Path, default=DEFAULT_TARGET)
    for command in (check_parser, convert_parser, verify_parser):
        command.add_argument(
            "--profile",
            type=Path,
            required=command is check_parser,
            help="original authoring evidence profile; declared YFT evidence is required too",
        )
        command.add_argument("--choices", type=Path, help="compact exact-name choices bound to the original profile")
    for command in (inspect_parser, check_parser, convert_parser):
        command.add_argument(
            "--name-policy",
            choices=(LEGACY_NAME_POLICY, SOURCE_NAME_POLICY),
            default=LEGACY_NAME_POLICY,
            help="preserve original spelling; expanded ASCII identifiers require explicit opt-in",
        )
    profile_parser.add_argument(
        "--output", type=Path, required=True, help="new JSON template; existing files are never replaced"
    )
    profile_parser.add_argument(
        "--material-scope",
        choices=(MAIN_MATERIAL_SCOPE, CHILD_MATERIAL_SCOPE, NO_MATERIAL_SCOPE),
        default=MAIN_MATERIAL_SCOPE,
        help="material evidence scope; no-material-evidence-v1 requires explicit-static-pixels-v1",
    )
    profile_parser.add_argument(
        "--source-policy",
        choices=(OBSERVED_SOURCE_POLICY, STATIC_SOURCE_POLICY),
        default=OBSERVED_SOURCE_POLICY,
        help="explicit-static-pixels-v1 authors static images, discarding source usage semantics after acknowledgment",
    )
    convert_parser.add_argument(
        "--experimental", action="store_true", help="acknowledge unresolved metadata and GPU policies"
    )
    convert_parser.add_argument("--metadata-policy", choices=(METADATA_POLICY, AUTHORING_POLICY), required=True)
    convert_parser.add_argument("--tile-mode", type=int, choices=(1, 5, 9), required=True)
    check_parser.add_argument("--tile-mode", type=int, choices=(1, 5, 9), required=True)
    choices_parser.add_argument("profile", type=Path, help="original immutable evidence profile")
    choices_parser.add_argument("--output", type=Path, required=True, help="new compact choices JSON")
    convert_parser.add_argument("--pack-id", required=True)
    convert_parser.add_argument(
        "--dictionary", required=True, help="external package label; internal texture names are preserved"
    )
    convert_parser.add_argument("--output-dir", type=Path, required=True)
    verify_parser.add_argument("--package", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "choices":
            profile = load_json(read_regular(args.profile, maximum=1024 * 1024), "texture authoring profile")
            result = build_texture_choices(profile)
            _publish_profile(args.output, result)
            print(json.dumps(result, sort_keys=True, indent=2))
            return 0
        source = read_regular(args.source, maximum=CONVERSION_LIMITS.max_file_bytes)
        if args.command == "inspect":
            result = inspect_texture_source(source, name_policy=args.name_policy)
        else:
            materials = _read_materials(args.material_source, len(source))
            if args.command == "profile":
                result = build_texture_authoring_template(
                    source, materials, material_scope=args.material_scope, source_policy=args.source_policy
                )
                _publish_profile(args.output, result)
            else:
                profile = (
                    load_json(read_regular(args.profile, maximum=1024 * 1024), "texture authoring profile")
                    if args.profile is not None
                    else None
                )
                if args.choices is not None:
                    if profile is None:
                        raise AssetError("texture choices require the original evidence profile")
                    choices = load_json(read_regular(args.choices, maximum=1024 * 1024), "texture authoring choices")
                    profile = apply_texture_choices(profile, choices)
                if args.command == "profile-check":
                    result = check_texture_authoring_profile(
                        source,
                        profile,
                        material_blobs=materials,
                        tile_mode=args.tile_mode,
                        name_policy=args.name_policy,
                    )
                    print(json.dumps(result, sort_keys=True, indent=2))
                    return 0 if result["readyForExperimentalConversion"] else 1
                target = target_identity(read_regular(args.target, maximum=1024 * 1024))
                if args.command == "convert":
                    result = prepare(
                        source,
                        target,
                        pack_id=args.pack_id,
                        dictionary=args.dictionary,
                        tile_mode=args.tile_mode,
                        metadata_policy=args.metadata_policy,
                        experimental=args.experimental,
                        output=args.output_dir,
                        authoring_profile=profile,
                        material_blobs=materials,
                        name_policy=args.name_policy,
                    )
                else:
                    result = verify(source, target, args.package, authoring_profile=profile, material_blobs=materials)
    except KeyboardInterrupt as exc:
        parser.exit(130, "\n".join([f"texture {args.command} cancelled", *getattr(exc, "__notes__", ()), ""]))
    except (AssetError, CustomPackSchemaError, OSError) as exc:
        parser.exit(1, "\n".join([f"texture {args.command} failed: {exc}", *getattr(exc, "__notes__", ()), ""]))
    print(json.dumps(result, sort_keys=True, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

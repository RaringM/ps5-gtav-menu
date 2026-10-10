"""Experimental complete-dictionary PC Legacy to PS5 texture conversion.

The default source policy admits only the observed metadata profile. An explicit
static-image re-authoring profile can instead discard the entire source usage
word after acknowledgment and independently chosen target metadata. Every source
scalar is retained; discarded flags are never described as mapped. Public AMD
swizzle identifiers are provisionally serialized unchanged.
That hypothesis and the incoming view-class placeholder require validation.
The ordinary 2D view fields are established; that is not GPU qualification.
No executable, private sidecar, console service or external helper is used.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import replace

from .asset_formats import AssetError, Limits
from .asset_materials import CHILD_MATERIAL_SCOPE, MAIN_MATERIAL_SCOPE
from .custom_pack_schema import CustomPackSchemaError, canonical_json
from .ps5_texture_reader import parse_ps5_texture_dictionary
from .ps5_texture_writer import (
    Ps5TextureInput,
    Ps5TextureMetadata,
    Ps5TextureStorage,
    plan_ps5_texture_dictionary,
    write_ps5_texture_dictionary,
)
from .texture_authoring import (
    AUTHORING_POLICY,
    NO_MATERIAL_SCOPE,
    OBSERVED_SOURCE_POLICY,
    STATIC_SOURCE_POLICY,
    build_authoring_template,
    check_authoring_profile,
    profile_material_scope,
    profile_source_policy,
    resolve_authoring_profile,
)
from .texture_conversion import digest, read_pc_texture_dictionary
from .texture_layout import MAX_BLOCKS, surface_layout, tile_mips, untile_mips
from .texture_names import LEGACY_NAME_POLICY, SOURCE_NAME_POLICY, texture_name_issue, validate_name_policy
from .texture_usage import describe_legacy_usage

METADATA_POLICY = "legacy-default-low5-experimental-v1"
LAYOUT_POLICY = "amd-gfx10-standard-explicit-v1"
REPORT_VERSION = 2
AUTHORING_REPORT_VERSION = 3
BGRA_REPORT_VERSION = 4
NAME_REPORT_VERSION = 5
MATERIAL_REPORT_VERSION = 6
STATIC_REPORT_VERSION = 7
STANDALONE_REPORT_VERSION = 8
SUPPORTED_REPORT_VERSIONS = (1, 2, 3, 4, 5, 6, 7, 8)
VIEW_POLICY = "ordinary2d-serialized-v1"
PIXEL_POLICY = "source-byte-preserving-v1"
CONVERSION_LIMITS = Limits(
    max_file_bytes=64 * 1024 * 1024,
    max_total_bytes=256 * 1024 * 1024,
    max_entries=64,
    max_metadata_bytes=1024 * 1024,
)
_USAGE_PROFILE = frozenset((0x20001001, 0x20008001, 0x2000B001, 0x20016001))
_FORMAT_CODES = {"BC1": 71, "BC3": 77, "BC5": 83, "BGRA8": 87}
_ELEMENT_BYTES = {"BC1": 8, "BC3": 16, "BC5": 16, "BGRA8": 4}
_VIEW_HEX = {
    1: "000000000000000000000000000000001400ffffffffffff0000000000000000",
    2: "000000000000000000000000000000004100ffffffffffff0000000000000000",
    3: "000000000000000000000000000000004100ffffffffffff0000000000000000",
    4: "000000000000000000000000000000004100ffffffffffff0000000000000000",
    5: "000000000000000000000000000000004100ffffffffffff0000000000000000",
    6: "000000000000000000000000000000004100ffffffffffff0000000000000000",
    7: "000000000000000000000000000000004100ffffffffffff0000000000000000",
    8: "000000000000000000000000000000004100ffffffffffff0000000000000000",
}


def _remaining_limits(limits: Limits, retained_bytes: int) -> Limits:
    remaining = limits.max_total_bytes - retained_bytes
    if remaining <= 0:
        raise AssetError("candidate pipeline exceeds the cumulative byte budget")
    return replace(limits, max_total_bytes=remaining)


def _read_source(blob: bytes, limits: Limits) -> dict:
    # Intake's decompressed payload and mip copies are additional to the input.
    return read_pc_texture_dictionary(blob, _remaining_limits(limits, len(blob)))


def _material_bytes(material_blobs: Sequence[bytes], limits: Limits) -> int:
    if not isinstance(material_blobs, (tuple, list)) or len(material_blobs) > 8:
        raise AssetError("material evidence must contain at most eight original YFT byte strings")
    if any(type(blob) is not bytes or not 16 <= len(blob) <= limits.max_file_bytes for blob in material_blobs):
        raise AssetError("material evidence contains invalid or oversized YFT bytes")
    total = sum(map(len, material_blobs))
    if total >= limits.max_total_bytes:
        raise AssetError("material evidence exceeds the cumulative byte budget")
    return total


def _profile_bytes(profile: dict | None, limits: Limits) -> int:
    if profile is None:
        return 0
    try:
        size = len(canonical_json(profile))
    except CustomPackSchemaError as exc:
        raise AssetError(f"invalid authoring profile: {exc}") from exc
    if size > min(limits.max_metadata_bytes, limits.max_total_bytes):
        raise AssetError("authoring profile exceeds the metadata byte budget")
    return size


def _authoring_limits(limits: Limits, retained_bytes: int) -> Limits:
    # Texture entries stay capped separately. Shader parameter slots require a
    # larger, explicitly bounded domain than the dictionary's 64-texture cap.
    return replace(_remaining_limits(limits, retained_bytes), max_entries=4096)


def build_texture_authoring_template(
    source_blob: bytes,
    material_blobs: Sequence[bytes],
    *,
    limits: Limits = CONVERSION_LIMITS,
    material_scope: str = MAIN_MATERIAL_SCOPE,
    source_policy: str = OBSERVED_SOURCE_POLICY,
) -> dict:
    """Record material evidence and unresolved choices without guessing metadata."""
    encoded_bytes = _material_bytes(material_blobs, limits)
    source = _read_source(source_blob, _remaining_limits(limits, encoded_bytes))
    return build_authoring_template(
        source,
        material_blobs,
        _authoring_limits(limits, len(source_blob) + source["linearBytes"]),
        material_scope=material_scope,
        source_policy=source_policy,
    )


def _resolve_authoring(
    source: dict,
    metadata_policy: str,
    authoring_profile: dict | None,
    material_blobs: Sequence[bytes],
    limits: Limits,
    retained_bytes: int,
) -> dict | None:
    if metadata_policy != AUTHORING_POLICY:
        if authoring_profile is not None or material_blobs:
            raise AssetError("authoring profile/material evidence requires the explicit authoring policy")
        return None
    if authoring_profile is None:
        raise AssetError("explicit authoring policy requires an authoring profile and original material evidence")
    return resolve_authoring_profile(
        source, authoring_profile, material_blobs, _authoring_limits(limits, retained_bytes)
    )


def _target_metadata(row: dict, authoring: dict | None) -> int:
    return authoring["targetMetadataByName"][row["name"]] if authoring else row["metadata"]["UsageData"] & 31


def _policy_issues(
    row: dict, name_policy: str = LEGACY_NAME_POLICY, *, source_policy: str = OBSERVED_SOURCE_POLICY
) -> list[str]:
    issues = []
    if row["format"] not in _FORMAT_CODES:
        issues.append("candidate conversion supports only BC1, BC3, BC5 and ordinary BGRA8")
    if issue := texture_name_issue(row["name"], name_policy):
        issues.append(issue)
    for key, value in row["metadata"].items():
        if key == "UsageData":
            if source_policy != STATIC_SOURCE_POLICY and value not in _USAGE_PROFILE:
                issues.append(f"UsageData 0x{value:08x} is outside the observed source profile")
        elif value != {"Unknown_30h": 1, "Unknown_32h": 128}.get(key, 0):
            issues.append(f"{key}={value} differs from the observed source profile")
    return issues


def _dictionary_policy_issues(source: dict) -> list[str]:
    metadata = source["dictionaryMetadata"]
    issues = [
        f"dictionary {key}={value} differs from the observed source profile"
        for key, value in metadata["scalars"].items()
        if value != (1 if key == "Unknown_18h" else 0)
    ]
    issues.extend(
        f"page information {key}={value} differs from the observed source profile"
        for key, value in metadata["pageInfo"]["scalars"].items()
        if value
    )
    return issues


def _source_summary(
    source: dict,
    report_version: int = REPORT_VERSION,
    name_policy: str = LEGACY_NAME_POLICY,
    source_policy: str = OBSERVED_SOURCE_POLICY,
) -> dict:
    summary = {
        "sha256": source["sourceSha256"],
        "header": source["header"],
        "textureCount": source["textureCount"],
        "mipCount": source["mipCount"],
        "linearBytes": source["linearBytes"],
        "textures": [
            {key: row[key] for key in ("name", "nameHash", "width", "height", "format", "mipLevels")}
            | {
                "allMetadata": row["metadata"],
                "sourceClassWords": row["sourceClassWords"],
                "headerSha256": row["headerSha256"],
                "mipSha256": [digest(mip) for mip in row["mips"]],
                "experimentalPolicyIssues": _policy_issues(row, name_policy, source_policy=source_policy),
            }
            | ({"rawHeaderHex": row["rawHeaderHex"]} if report_version >= 2 else {})
            | (
                {"observedMetadataPolicyIssues": _policy_issues(row, name_policy)}
                if report_version >= STATIC_REPORT_VERSION
                else {}
            )
            for row in source["textures"]
        ],
    }
    if report_version >= 2:
        summary["dictionaryMetadata"] = source["dictionaryMetadata"]
        summary["dictionaryPolicyIssues"] = _dictionary_policy_issues(source)
    return summary


def inspect_texture_source(
    blob: bytes, *, limits: Limits = CONVERSION_LIMITS, name_policy: str = LEGACY_NAME_POLICY
) -> dict:
    validate_name_policy(name_policy)
    source = _read_source(blob, limits)
    summary = _source_summary(source, name_policy=name_policy)
    return {
        "source": summary,
        "metadataPolicy": METADATA_POLICY,
        "namePolicy": name_policy,
        "sourceUsageDiagnostics": [
            {
                "name": row["name"],
                "admittedByObservedProfile": row["metadata"]["UsageData"] in _USAGE_PROFILE,
                **describe_legacy_usage(row["metadata"]["UsageData"], sum(map(len, row["mips"]))),
            }
            for row in source["textures"]
        ],
        "experimentalProfileAdmitted": not summary["dictionaryPolicyIssues"]
        and not any(row["experimentalPolicyIssues"] for row in summary["textures"]),
        "releaseReady": False,
    }


def _admit(
    source: dict,
    tile_mode: int,
    metadata_policy: str,
    limits: Limits,
    name_policy: str = LEGACY_NAME_POLICY,
    *,
    source_policy: str = OBSERVED_SOURCE_POLICY,
) -> list:
    validate_name_policy(name_policy)
    if type(metadata_policy) is not str or metadata_policy not in (METADATA_POLICY, AUTHORING_POLICY):
        raise AssetError("an explicit supported experimental metadata policy is required")
    if source_policy not in (OBSERVED_SOURCE_POLICY, STATIC_SOURCE_POLICY) or (
        source_policy == STATIC_SOURCE_POLICY and metadata_policy != AUTHORING_POLICY
    ):
        raise AssetError("static source re-authoring requires a validated explicit metadata profile")
    if type(tile_mode) is not int or tile_mode not in (1, 5, 9):
        raise AssetError("an explicit tile mode 1, 5 or 9 is required; automatic mode is unresolved")
    failures = _dictionary_policy_issues(source)
    failures.extend(
        f"{row['name']}: {issue}"
        for row in source["textures"]
        for issue in _policy_issues(row, name_policy, source_policy=source_policy)
    )
    if failures:
        raise AssetError("candidate source policy rejected the complete dictionary: " + "; ".join(failures))
    elements = sum(len(mip) // _ELEMENT_BYTES[row["format"]] for row in source["textures"] for mip in row["mips"])
    if elements > MAX_BLOCKS:
        raise AssetError("candidate dictionary exceeds the cumulative storage-element work budget")
    layouts = [
        surface_layout(
            row["width"],
            row["height"],
            row["mipLevels"],
            row["format"],
            tile_mode,
            max_storage_bytes=min(limits.max_file_bytes, 64 * 1024 * 1024),
        )
        for row in source["textures"]
    ]
    if sum(layout.storage_bytes for layout in layouts) > limits.max_file_bytes:
        raise AssetError("candidate dictionary exceeds the cumulative storage budget")
    return layouts


def _verify(
    source: dict,
    candidate_blob: bytes,
    tile_mode: int,
    metadata_policy: str,
    limits: Limits,
    retained_bytes: int,
    report_version: int = REPORT_VERSION,
    authoring: dict | None = None,
    name_policy: str = LEGACY_NAME_POLICY,
) -> dict:
    if type(report_version) is not int or report_version not in SUPPORTED_REPORT_VERSIONS:
        raise AssetError("unsupported texture conversion report version")
    validate_name_policy(name_policy)
    source_policy = profile_source_policy(authoring["profile"]) if authoring is not None else OBSERVED_SOURCE_POLICY
    no_materials = authoring is not None and profile_material_scope(authoring["profile"]) == NO_MATERIAL_SCOPE
    if (report_version == STANDALONE_REPORT_VERSION) != no_materials:
        raise AssetError("texture conversion report version and no-material evidence scope disagree")
    if (report_version >= STATIC_REPORT_VERSION) != (source_policy == STATIC_SOURCE_POLICY):
        raise AssetError("texture conversion report version and source policy disagree")
    has_child_materials = authoring is not None and profile_material_scope(authoring["profile"]) == CHILD_MATERIAL_SCOPE
    if report_version < STATIC_REPORT_VERSION and (report_version == MATERIAL_REPORT_VERSION) != has_child_materials:
        raise AssetError("texture conversion report version and material scope disagree")
    if report_version < MATERIAL_REPORT_VERSION and (report_version == NAME_REPORT_VERSION) != (
        name_policy == SOURCE_NAME_POLICY
    ):
        raise AssetError("texture conversion report version and name policy disagree")
    if report_version < BGRA_REPORT_VERSION and (report_version == AUTHORING_REPORT_VERSION) != (
        metadata_policy == AUTHORING_POLICY
    ):
        raise AssetError("texture conversion report version and metadata policy disagree")
    if report_version == 1 and any(row["format"] not in ("BC1", "BC3") for row in source["textures"]):
        raise AssetError("historical conversion reports admit only BC1 and BC3")
    has_bgra = any(row["format"] == "BGRA8" for row in source["textures"])
    if report_version < NAME_REPORT_VERSION and has_bgra != (report_version == BGRA_REPORT_VERSION):
        raise AssetError("ordinary BGRA8 requires conversion report version 4; historical versions exclude it")
    layouts = _admit(source, tile_mode, metadata_policy, limits, name_policy, source_policy=source_policy)
    # Account for caller-owned source data, target input, and the largest mip
    # recovery + zero-padding check while the reader's allocations are live.
    readback_scratch = max(
        2 * sum(len(mip) for mip in row["mips"]) + 2 * layout.storage_bytes
        for row, layout in zip(source["textures"], layouts, strict=True)
    )
    candidate = parse_ps5_texture_dictionary(
        candidate_blob,
        _remaining_limits(limits, retained_bytes + len(candidate_blob) + readback_scratch),
    )
    if [row["name"] for row in candidate["textures"]] != [row["name"] for row in source["textures"]]:
        raise AssetError("candidate names/count differ from the complete source dictionary")
    root = candidate["root"]
    raw_root = bytes.fromhex(root["rawHex"])
    if (
        root["classWord"] != 0
        or root["referenceCount"] != 1
        or root["keyCapacity"] != source["textureCount"]
        or root["valueCapacity"] != source["textureCount"]
        or any(raw_root[a:b].strip(b"\0") for a, b in ((28, 32), (44, 48), (60, 64)))
        or candidate["pageInfo"]["headerHex"] != "00000000000000000101000000000000"
        or not all(candidate["unownedBytesZero"].values())
    ):
        raise AssetError("candidate root, page information or padding differs from the constructor policy")
    rows = []
    for original, target, layout in zip(source["textures"], candidate["textures"], layouts, strict=True):
        name = original["name"]
        if any(original[key] != target[key] for key in ("nameHash", "width", "height", "depth", "format", "mipLevels")):
            raise AssetError(f"candidate shape or format differs from the source: {name}")
        if (
            target["tileMode"] != tile_mode
            or target["storageBytes"] != layout.storage_bytes
            or target["graphicsOffset"] % layout.alignment_bytes
        ):
            raise AssetError(f"candidate storage differs from the explicit CPU layout: {name}")
        metadata = target["metadata"]
        if (
            metadata["flags"] != 0x260208
            or metadata["metadataCandidate"] != _target_metadata(original, authoring)
            or metadata["referenceCount"] != 1
            or metadata["viewType"] != 2
            or metadata["rawViewHex"] != _VIEW_HEX[report_version]
            or metadata["packedStrideWord"] != layout.block_bytes
        ):
            raise AssetError(f"candidate metadata differs from the explicit experimental policy: {name}")
        raw_object = bytes.fromhex(metadata["rawObjectHex"])
        if any(raw_object[a:b].strip(b"\0") for a, b in ((0, 8), (20, 24), (35, 38), (69, 88))):
            raise AssetError(f"candidate reserved object fields differ from the constructor policy: {name}")
        recovered = untile_mips(target["allocation"], layout)
        if recovered != original["mips"]:
            raise AssetError(f"candidate mip payload differs from the source: {name}")
        # Canonical padding is checked too; accepting only visible pixels would
        # leave arbitrary data in allocations that later writers might reuse.
        if tile_mips(recovered, layout) != target["allocation"]:
            raise AssetError(f"candidate allocation padding differs from the zero-padding policy: {name}")
        rows.append(
            {
                "name": name,
                "mipCount": len(recovered),
                "storageBytes": layout.storage_bytes,
                "alignmentBytes": layout.alignment_bytes,
                "storageSha256": target["storageSha256"],
                "targetMetadata40": metadata["metadataCandidate"],
                "sourceUsageFlagsNotTransferred": original["metadata"]["UsageData"] >> 5,
                "allMipPayloadsMatch": True,
            }
        )
        if report_version >= BGRA_REPORT_VERSION:
            rows[-1].update(
                format=original["format"],
                sourceFormatCode=original["formatCode"],
                targetFormatCode=target["formatCode"],
                elementBytes=layout.block_bytes,
            )
        if report_version >= STATIC_REPORT_VERSION:
            rows[-1]["sourceUsageDataNotTransferred"] = original["metadata"]["UsageData"]
        del recovered
    report = {
        "schemaVersion": report_version,
        "kind": "gtavmenu-experimental-texture-conversion",
        "policies": {"metadata": metadata_policy, "layout": LAYOUT_POLICY, "tileMode": tile_mode},
        "source": _source_summary(source, report_version, name_policy, source_policy),
        "candidate": {
            "sha256": candidate["resourceSha256"],
            "bytes": len(candidate_blob),
            "header": candidate["header"],
            "textures": rows,
        },
        "qualification": {
            "completeSourceEntrySetPreserved": True,
            "independentTargetStructureParsed": True,
            "allMipPayloadsMatchCpuModel": True,
            "sourceLegacyMetadataMapped": False,
            "serializedTileModeMappingQualified": False,
            "gpuInterpretationQualified": False,
            "runtimeQualified": False,
            "releaseReady": False,
        },
        "unresolved": [
            "Legacy usage high bits and remaining metadata have no qualified PS5 transfer policy",
            "target +0x40 low-five-bit copy is a PC Gen9-inspired hypothesis, not a native mapping",
            "serialized tile IDs are provisionally equated to public AMD standard swizzles",
            "constructor view initializer, engine loading and GPU interpretation need hardware validation",
        ],
    }
    if report_version >= 2:
        report["policies"]["view"] = VIEW_POLICY
        report["qualification"].update(
            sourceDictionaryMetadataRecorded=True,
            ordinary2DViewFieldsEstablished=True,
            incomingViewClassPlaceholderQualified=False,
        )
        report["unresolved"] = [
            "paired mod/native textures disprove a general UsageData-only or low-five-bit metadata mapping",
            "target +0x40 low-five-bit copy remains an explicit experimental fallback, not metadata equivalence",
            "serialized tile IDs are provisionally equated to public AMD standard swizzles",
            "zero incoming view-class placeholder, engine loading and GPU interpretation remain unqualified",
        ]
    if metadata_policy == AUTHORING_POLICY:
        report["authoring"] = authoring
        report["qualification"].update(
            explicitMetadataChoicesValidated=True,
            materialEvidenceRecomputed=True,
            fullMaterialGraphCovered=False,
        )
        report["unresolved"] = [
            "target +0x40 is explicitly authored; observed words are not universal role presets or a source mapping",
            "sampler evidence covers only supplied main-drawable shader groups, not the full material graph",
            "serialized tile IDs are provisionally equated to public AMD standard swizzles",
            "zero incoming view-class placeholder, engine loading and GPU interpretation remain unqualified",
        ]
    if report_version >= BGRA_REPORT_VERSION:
        report["policies"]["pixels"] = PIXEL_POLICY
        report["qualification"].update(
            sourcePixelBytesPreserved=True,
            pixelChannelInterpretationQualified=False,
        )
        if has_bgra:
            report["unresolved"].append(
                "BGRA8 pixels are copied without channel, alpha or color-space transforms; target interpretation is unqualified"
            )
    if report_version >= NAME_REPORT_VERSION:
        report["policies"]["names"] = name_policy
        report["qualification"].update(sourceTextureNamesPreserved=True, runtimeNameResolutionQualified=False)
        report["unresolved"].append(
            "original ASCII texture names and hashes are preserved; target runtime name resolution is unqualified"
        )
    if has_child_materials:
        report["policies"]["materials"] = CHILD_MATERIAL_SCOPE
        report["qualification"]["selectedChildMaterialEvidenceRecomputed"] = True
        report["unresolved"][1] = (
            "sampler evidence covers supplied main groups and selected LOD1 child shader mappings, "
            "not the full material graph"
        )
    if report_version >= STATIC_REPORT_VERSION:
        report["policies"].update(source=source_policy, materials=profile_material_scope(authoring["profile"]))
        report["qualification"].update(
            sourceUsageDiscardAcknowledged=True,
            sourceUsageDataSemanticsTransferred=False,
            sourceRuntimeBehaviorPreserved=False,
        )
        report["unresolved"].insert(
            0,
            "the entire source UsageData word is deliberately discarded; this authors static images, "
            "not equivalent source runtime behavior",
        )
    if no_materials:
        report["qualification"].update(materialEvidenceRecomputed=False, sourceMaterialEvidencePresent=False)
        report["unresolved"][
            2
        ] = "no material evidence was supplied; all texture bindings and intended roles remain unknown"
    return report


def verify_texture_candidate(
    source_blob: bytes,
    candidate_blob: bytes,
    *,
    tile_mode: int,
    metadata_policy: str,
    limits: Limits = CONVERSION_LIMITS,
    report_version: int = REPORT_VERSION,
    authoring_profile: dict | None = None,
    material_blobs: Sequence[bytes] = (),
    name_policy: str = LEGACY_NAME_POLICY,
) -> dict:
    """Parse and recover every target mip without trusting a writer sidecar."""
    material_bytes = _material_bytes(material_blobs, limits)
    profile_bytes = _profile_bytes(authoring_profile, limits)
    source = _read_source(source_blob, _remaining_limits(limits, len(candidate_blob) + material_bytes + profile_bytes))
    retained_bytes = len(source_blob) + source["linearBytes"] + profile_bytes
    authoring = _resolve_authoring(
        source, metadata_policy, authoring_profile, material_blobs, limits, retained_bytes + len(candidate_blob)
    )
    return _verify(
        source,
        candidate_blob,
        tile_mode,
        metadata_policy,
        limits,
        retained_bytes + material_bytes + _profile_bytes(authoring, limits),
        report_version,
        authoring,
        name_policy,
    )


def convert_texture_candidate(
    source_blob: bytes,
    *,
    tile_mode: int,
    metadata_policy: str,
    experimental: bool = False,
    limits: Limits = CONVERSION_LIMITS,
    authoring_profile: dict | None = None,
    material_blobs: Sequence[bytes] = (),
    name_policy: str = LEGACY_NAME_POLICY,
) -> tuple[bytes, dict]:
    """Generate a complete, independently checked candidate, never a qualified asset."""
    if experimental is not True:
        raise AssetError("conversion is experimental; explicitly acknowledge the unresolved metadata/GPU policies")
    material_bytes = _material_bytes(material_blobs, limits)
    profile_bytes = _profile_bytes(authoring_profile, limits)
    source = _read_source(source_blob, _remaining_limits(limits, material_bytes + profile_bytes))
    retained_bytes = len(source_blob) + source["linearBytes"] + profile_bytes
    authoring = _resolve_authoring(source, metadata_policy, authoring_profile, material_blobs, limits, retained_bytes)
    # Only freshly validated choices and per-texture discard acknowledgments can
    # select the re-authoring path. There is no convert/verify policy override.
    source_policy = profile_source_policy(authoring["profile"]) if authoring is not None else OBSERVED_SOURCE_POLICY
    layouts = _admit(source, tile_mode, metadata_policy, limits, name_policy, source_policy=source_policy)
    retained_bytes += material_bytes + _profile_bytes(authoring, limits)
    writer_limits = _remaining_limits(limits, retained_bytes)
    # Check tiling's output plus its mutable scratch before allocating either.
    if 2 * sum(layout.storage_bytes for layout in layouts) > writer_limits.max_total_bytes:
        raise AssetError("candidate tiling exceeds the cumulative byte budget")
    inputs = [
        Ps5TextureInput(
            name=row["name"],
            width=row["width"],
            height=row["height"],
            format_code=_FORMAT_CODES[row["format"]],
            mip_count=row["mipLevels"],
            tile_mode=tile_mode,
            allocation=tile_mips(row["mips"], layout),
            storage_alignment=layout.alignment_bytes,
            metadata=Ps5TextureMetadata(0x260208, _target_metadata(row, authoring), 1, 2, VIEW_POLICY),
        )
        for row, layout in zip(source["textures"], layouts, strict=True)
    ]
    blob = write_ps5_texture_dictionary(inputs, writer_limits, name_policy=name_policy)
    # Keep only source pixels and the serialized output during independent readback.
    del inputs
    version = AUTHORING_REPORT_VERSION if authoring else REPORT_VERSION
    if any(row["format"] == "BGRA8" for row in source["textures"]):
        version = BGRA_REPORT_VERSION
    if name_policy == SOURCE_NAME_POLICY:
        version = NAME_REPORT_VERSION
    if authoring is not None and profile_material_scope(authoring["profile"]) == CHILD_MATERIAL_SCOPE:
        version = MATERIAL_REPORT_VERSION
    if source_policy == STATIC_SOURCE_POLICY:
        version = STATIC_REPORT_VERSION
    if authoring is not None and profile_material_scope(authoring["profile"]) == NO_MATERIAL_SCOPE:
        version = STANDALONE_REPORT_VERSION
    return blob, _verify(
        source, blob, tile_mode, metadata_policy, limits, retained_bytes, version, authoring, name_policy
    )


def check_texture_authoring_profile(
    source_blob: bytes,
    authoring_profile: dict,
    *,
    material_blobs: Sequence[bytes] = (),
    tile_mode: int,
    name_policy: str = LEGACY_NAME_POLICY,
    limits: Limits = CONVERSION_LIMITS,
) -> dict:
    """Aggregate authoring/admission/budget issues without tiling or writing.

    Choice issues do not suppress independent format or storage checks. Invalid
    source/evidence stops dependent checks instead of trusting supplied claims.
    Writer reservation uses the same pure page planner as actual serialization.
    Readiness is preflight-only: package manifest-size/publication checks still
    run during prepare, and no GPU or runtime qualification is established.
    """
    result = {
        "schemaVersion": 1,
        "kind": "gtavmenu-texture-authoring-check",
        "issues": [],
        "checks": {"sourceIntake": False, "immutableEvidence": False, "sourceAdmission": False, "storagePlan": False},
        "readyForExperimentalConversion": False,
        "candidateGenerated": False,
        "packagePublicationChecked": False,
        "releaseReady": False,
    }

    def issue(category: str, message: str, **details: object) -> None:
        result["issues"].append({"category": category, "message": message, **details})

    try:
        material_bytes = _material_bytes(material_blobs, limits)
        profile_bytes = _profile_bytes(authoring_profile, limits)
        source = _read_source(source_blob, _remaining_limits(limits, material_bytes + profile_bytes))
        result["checks"]["sourceIntake"] = True
        result.update(
            sourceSha256=source["sourceSha256"], textureCount=source["textureCount"], mipCount=source["mipCount"]
        )
    except AssetError as exc:
        issue("source-or-budget", str(exc))
        return result
    retained = len(source_blob) + source["linearBytes"] + profile_bytes
    try:
        evidence = check_authoring_profile(
            source, authoring_profile, material_blobs, _authoring_limits(limits, retained)
        )
        result["checks"]["immutableEvidence"] = True
        result.update(
            profileSha256=evidence["profileSha256"],
            sourcePolicy=evidence["sourcePolicy"],
            materialScope=evidence["materialScope"],
        )
        for row in evidence["issues"]:
            issue("authoring", row["message"], field=row["field"], texture=row["texture"])
    except AssetError as exc:
        issue("evidence-or-budget", str(exc))
        return result
    try:
        layouts = _admit(
            source, tile_mode, AUTHORING_POLICY, limits, name_policy, source_policy=evidence["sourcePolicy"]
        )
        result["checks"]["sourceAdmission"] = True
    except AssetError as exc:
        issue("source-admission", str(exc))
        return result
    # Reserve the complete resolved profile plus bounded name->uint32 map and
    # scalar/hash overhead even while choices remain unresolved. No placeholder
    # target metadata is selected or passed to the writer.
    resolved_reservation = profile_bytes + 8192 + sum(6 * len(row["name"]) for row in source["textures"])
    retained += material_bytes + resolved_reservation
    try:
        writer_limits = _remaining_limits(limits, retained)
        storage = [
            Ps5TextureStorage(row["name"], layout.storage_bytes, layout.alignment_bytes)
            for row, layout in zip(source["textures"], layouts, strict=True)
        ]
        plan = plan_ps5_texture_dictionary(storage, writer_limits, name_policy=name_policy)
        result["checks"]["storagePlan"] = True
        result["budget"] = {
            key: plan[key]
            for key in ("systemBytes", "graphicsBytes", "payloadBytes", "encodedBytes", "writerBufferBytes")
        }
        readback_scratch = max(
            2 * sum(map(len, row["mips"])) + 2 * layout.storage_bytes
            for row, layout in zip(source["textures"], layouts, strict=True)
        )
        readback_reservation = plan["encodedBytes"] + readback_scratch + plan["payloadBytes"] + plan["graphicsBytes"]
        result["budget"].update(
            retainedBytesUpperBound=retained,
            readbackBufferBytes=readback_reservation,
            totalBufferBytesUpperBound=retained + max(plan["writerBufferBytes"], readback_reservation),
            limitBytes=limits.max_total_bytes,
        )
        for message in plan["issues"]:
            issue("budget", message)
        if 2 * sum(layout.storage_bytes for layout in layouts) > writer_limits.max_total_bytes:
            issue("budget", "candidate tiling exceeds the cumulative byte budget")
        if readback_reservation > writer_limits.max_total_bytes:
            issue(
                "budget",
                f"candidate readback buffer reservation {readback_reservation} exceeds limit {writer_limits.max_total_bytes}",
            )
    except AssetError as exc:
        issue("budget", str(exc))
    result["readyForExperimentalConversion"] = not result["issues"] and all(result["checks"].values())
    return result

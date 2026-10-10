"""Build and verify a pixel-only candidate from a strict stock PS5 v5 PTD.

This lane does not author target metadata.  It preserves the complete decoded
system domain and RSC7 header from an already selected stock resource, replaces
one complete graphics allocation with caller-supplied same-size bytes, and
serializes the result with deterministic raw-DEFLATE stored blocks.

Passing these checks establishes a byte-level container contract only.  It does
not establish that the replacement bytes use the GPU layout implied by the
preserved stock metadata, or that the candidate can be admitted or rendered.
"""

from __future__ import annotations

import hashlib
import struct
from pathlib import Path

from . import custom_pack_schema as package_schema
from .asset_formats import AssetError, Limits, decode_resource, resource_header
from .ps5_texture_reader import parse_ps5_texture_dictionary
from .texture_conversion import read_pc_texture_dictionary
from .texture_layout import surface_layout, tile_mips, untile_mips
from .texture_visual_oracle import bc3_quadrant_mip, verify_bc3_quadrant_mip

POLICY = "stock-ps5-v5-pixel-allocation-replacement-v1"
PC_SOURCE_POLICY = "exact-pc-mips-to-preserved-stock-envelope-v1"
PACKAGE_ORACLE_KIND = "stock-texture-envelope-differential-proof"
PACKAGE_RESULT_KIND = "gtavmenu-source-bound-stock-texture-envelope-package"
VISUAL_ORACLE_POLICY = "exact-stock-ps5-v5-bc3-quadrant-replacement-v1"
VISUAL_ORACLE_PACKAGE_KIND = "stock-texture-envelope-bc3-visual-oracle-proof"
VISUAL_ORACLE_RESULT_KIND = "gtavmenu-stock-texture-envelope-bc3-visual-oracle-package"


def _sha256(data: bytes | memoryview) -> str:
    return hashlib.sha256(data).hexdigest()


def _stored_deflate(payload: bytes | bytearray) -> bytes:
    """Return the one canonical raw-DEFLATE stored-block representation."""
    output = bytearray()
    for start in range(0, len(payload), 65535):
        stop = min(start + 65535, len(payload))
        count = stop - start
        output.extend(struct.pack("<BHH", int(stop == len(payload)), count, count ^ 0xFFFF))
        output.extend(payload[start:stop])
    return bytes(output)


def _canonical_resource(header: bytes, payload: bytes | bytearray) -> bytes:
    if len(header) != 16:
        raise AssetError("stock texture envelope requires an exact 16-byte RSC7 header")
    return header + _stored_deflate(payload)


def _target(parsed: dict, texture_name: str) -> dict:
    if type(texture_name) is not str or not texture_name:
        raise AssetError("stock texture envelope requires one exact texture name")
    rows = [row for row in parsed["textures"] if row["name"] == texture_name]
    if len(rows) != 1:
        raise AssetError("stock texture envelope texture name does not select exactly one entry")
    return rows[0]


def _work_budget(source_bytes: int, replacement_bytes: int, payload_bytes: int, encoded_bytes: int) -> int:
    # Conservative retained-buffer model covering strict source/candidate
    # parsing, decoded payload comparison and canonical encoding.  This is not
    # a Python heap estimate; it prevents an attacker from multiplying the
    # public file bounds through this workflow.
    return 2 * source_bytes + 2 * replacement_bytes + 6 * payload_bytes + 3 * encoded_bytes


def _preflight_budget(
    source_bytes: int,
    replacement_bytes: int,
    payload_bytes: int,
    encoded_bytes: int,
    limits: Limits,
) -> None:
    if encoded_bytes > limits.max_file_bytes:
        raise AssetError("canonical stock texture candidate exceeds the output file limit")
    if _work_budget(source_bytes, replacement_bytes, payload_bytes, encoded_bytes) > limits.max_total_bytes:
        raise AssetError("stock texture candidate exceeds the cumulative work budget")


def _source_bound_preflight(pc_blob: bytes, stock_blob: bytes, limits: Limits) -> None:
    if type(pc_blob) is not bytes or type(stock_blob) is not bytes:
        raise AssetError("source-bound texture envelope inputs must be immutable complete bytes")
    pc_header = resource_header(pc_blob)
    stock_header = resource_header(stock_blob)
    pc_payload = pc_header["systemBytes"] + pc_header["graphicsBytes"]
    stock_payload = stock_header["systemBytes"] + stock_header["graphicsBytes"]
    encoded = 16 + stock_payload + 5 * ((stock_payload + 65534) // 65535)
    # The stock graphics size deliberately overestimates the selected
    # replacement until strict parsing identifies its exact allocation.
    estimate = (
        _work_budget(len(stock_blob), stock_header["graphicsBytes"], stock_payload, encoded)
        + 2 * len(pc_blob)
        + 3 * pc_payload
    )
    if estimate > limits.max_total_bytes:
        raise AssetError("source-bound stock texture candidate exceeds the cumulative work budget")


def _pc_replacement(
    pc_blob: bytes,
    stock_blob: bytes,
    *,
    pc_texture_name: str,
    stock_texture_name: str,
    limits: Limits,
) -> tuple[bytes, tuple[bytes, ...], dict]:
    _source_bound_preflight(pc_blob, stock_blob, limits)
    stock = parse_ps5_texture_dictionary(stock_blob, limits)
    stock_target = _target(stock, stock_texture_name)
    source = read_pc_texture_dictionary(pc_blob, limits)
    source_rows = [row for row in source["textures"] if row["name"] == pc_texture_name]
    if type(pc_texture_name) is not str or not pc_texture_name or len(source_rows) != 1:
        raise AssetError("PC source texture name does not select exactly one entry")
    source_target = source_rows[0]
    fields = ("width", "height", "format", "mipLevels")
    if any(source_target[key] != stock_target[key] for key in fields):
        raise AssetError("PC source shape, format or mip count differs from the selected stock texture")
    layout = surface_layout(
        stock_target["width"],
        stock_target["height"],
        stock_target["mipLevels"],
        stock_target["format"],
        stock_target["tileMode"],
        max_storage_bytes=min(stock_target["storageBytes"], limits.max_file_bytes),
    )
    if layout.storage_bytes != stock_target["storageBytes"]:
        raise AssetError("CPU layout storage size differs from the selected stock allocation")
    replacement = tile_mips(source_target["mips"], layout)
    if untile_mips(replacement, layout) != source_target["mips"]:
        raise AssetError("CPU layout did not round-trip every original PC source mip")
    binding = {
        "policy": PC_SOURCE_POLICY,
        "sourceResourceSha256": source["sourceSha256"],
        "sourceTexture": pc_texture_name,
        "stockTexture": stock_texture_name,
        "format": stock_target["format"],
        "width": stock_target["width"],
        "height": stock_target["height"],
        "mipLevels": stock_target["mipLevels"],
        "tileMode": stock_target["tileMode"],
        "linearMipSha256": [_sha256(mip) for mip in source_target["mips"]],
        "tiledAllocationSha256": _sha256(replacement),
        "allocationBytes": len(replacement),
    }
    return replacement, source_target["mips"], binding


def verify_stock_texture_envelope_candidate(
    source_blob: bytes,
    candidate_blob: bytes,
    *,
    texture_name: str,
    replacement: bytes,
    limits: Limits | None = None,
) -> dict:
    """Independently prove the candidate differs only in one graphics allocation."""
    limits = Limits() if limits is None else limits
    if type(source_blob) is not bytes or type(candidate_blob) is not bytes or type(replacement) is not bytes:
        raise AssetError("stock texture envelope inputs must be immutable complete bytes")

    source_declared = resource_header(source_blob)
    candidate_declared = resource_header(candidate_blob)
    source_payload_bytes = source_declared["systemBytes"] + source_declared["graphicsBytes"]
    candidate_payload_bytes = candidate_declared["systemBytes"] + candidate_declared["graphicsBytes"]
    encoded_bytes = 16 + candidate_payload_bytes + 5 * ((candidate_payload_bytes + 65534) // 65535)
    _preflight_budget(
        len(source_blob),
        len(replacement),
        max(source_payload_bytes, candidate_payload_bytes),
        encoded_bytes,
        limits,
    )

    source = parse_ps5_texture_dictionary(source_blob, limits)
    candidate = parse_ps5_texture_dictionary(candidate_blob, limits)
    source_target = _target(source, texture_name)
    candidate_target = _target(candidate, texture_name)
    if len(replacement) != source_target["storageBytes"] or replacement == source_target["allocation"]:
        raise AssetError("replacement must be different and exactly cover the selected stock allocation")
    if candidate_target["storageBytes"] != source_target["storageBytes"]:
        raise AssetError("candidate selected allocation size differs from stock")

    source_header, source_payload = decode_resource(source_blob, limits.max_total_bytes)
    candidate_header, candidate_payload = decode_resource(candidate_blob, limits.max_total_bytes)
    if source_blob[:16] != candidate_blob[:16] or source_header != candidate_header:
        raise AssetError("candidate RSC7 header or page fields differ from stock")
    if len(source_payload) != len(candidate_payload):
        raise AssetError("candidate decoded resource size differs from stock")
    if candidate_blob != _canonical_resource(candidate_blob[:16], candidate_payload):
        raise AssetError("candidate RSC7 payload is not in the canonical stored-DEFLATE representation")

    system_bytes = source_header["systemBytes"]
    if source_payload[:system_bytes] != candidate_payload[:system_bytes]:
        raise AssetError("candidate system metadata differs from stock")
    if source_target["graphicsOffset"] != candidate_target["graphicsOffset"]:
        raise AssetError("candidate selected graphics allocation moved")
    start = system_bytes + source_target["graphicsOffset"]
    stop = start + source_target["storageBytes"]
    if candidate_payload[start:stop] != replacement:
        raise AssetError("candidate selected graphics allocation differs from the explicit replacement")
    if (
        source_payload[system_bytes:start] != candidate_payload[system_bytes:start]
        or source_payload[stop:] != candidate_payload[stop:]
    ):
        raise AssetError("candidate changed bytes outside the selected graphics allocation")
    changed_bytes = sum(left != right for left, right in zip(source_payload[start:stop], replacement, strict=True))
    if not changed_bytes:
        raise AssetError("candidate does not change any selected graphics byte")

    return {
        "schemaVersion": 1,
        "policy": POLICY,
        "sourceResourceSha256": _sha256(source_blob),
        "candidateResourceSha256": _sha256(candidate_blob),
        "texture": {
            "name": texture_name,
            "format": source_target["format"],
            "width": source_target["width"],
            "height": source_target["height"],
            "mipLevels": source_target["mipLevels"],
            "tileMode": source_target["tileMode"],
            "graphicsOffset": source_target["graphicsOffset"],
            "allocationBytes": source_target["storageBytes"],
            "sourceAllocationSha256": source_target["storageSha256"],
            "replacementAllocationSha256": _sha256(replacement),
            "changedBytes": changed_bytes,
        },
        "proof": {
            "rsc7HeaderBitExact": True,
            "systemDomainBitExact": True,
            "nonTargetGraphicsBitExact": True,
            "selectedAllocationReadbackExact": True,
            "canonicalStoredDeflate": True,
            "strictStructuralReadback": True,
        },
        "qualification": {
            "gpuLayoutQualified": False,
            "runtimeQualified": False,
            "releaseReady": False,
        },
    }


def build_stock_texture_envelope_candidate(
    source_blob: bytes,
    *,
    texture_name: str,
    replacement: bytes,
    limits: Limits | None = None,
) -> tuple[bytes, dict]:
    """Replace one complete allocation and return canonical bytes plus fresh proof."""
    limits = Limits() if limits is None else limits
    if type(source_blob) is not bytes or type(replacement) is not bytes:
        raise AssetError("stock texture envelope inputs must be immutable complete bytes")
    declared = resource_header(source_blob)
    payload_bytes = declared["systemBytes"] + declared["graphicsBytes"]
    encoded_bytes = 16 + payload_bytes + 5 * ((payload_bytes + 65534) // 65535)
    _preflight_budget(len(source_blob), len(replacement), payload_bytes, encoded_bytes, limits)
    parsed = parse_ps5_texture_dictionary(source_blob, limits)
    target = _target(parsed, texture_name)
    if len(replacement) != target["storageBytes"] or replacement == target["allocation"]:
        raise AssetError("replacement must be different and exactly cover the selected stock allocation")
    header, payload = decode_resource(source_blob, limits.max_total_bytes)
    start = header["systemBytes"] + target["graphicsOffset"]
    stop = start + target["storageBytes"]
    changed = bytearray(payload)
    changed[start:stop] = replacement
    candidate = _canonical_resource(source_blob[:16], changed)
    report = verify_stock_texture_envelope_candidate(
        source_blob,
        candidate,
        texture_name=texture_name,
        replacement=replacement,
        limits=limits,
    )
    return candidate, report


def verify_pc_stock_texture_envelope_candidate(
    pc_blob: bytes,
    stock_blob: bytes,
    candidate_blob: bytes,
    *,
    pc_texture_name: str,
    stock_texture_name: str,
    limits: Limits | None = None,
) -> dict:
    """Rebuild the tiled allocation and prove candidate pixels match the PC mips."""
    limits = Limits() if limits is None else limits
    replacement, source_mips, binding = _pc_replacement(
        pc_blob,
        stock_blob,
        pc_texture_name=pc_texture_name,
        stock_texture_name=stock_texture_name,
        limits=limits,
    )
    report = verify_stock_texture_envelope_candidate(
        stock_blob,
        candidate_blob,
        texture_name=stock_texture_name,
        replacement=replacement,
        limits=limits,
    )
    candidate = parse_ps5_texture_dictionary(candidate_blob, limits)
    target = _target(candidate, stock_texture_name)
    layout = surface_layout(
        target["width"],
        target["height"],
        target["mipLevels"],
        target["format"],
        target["tileMode"],
        max_storage_bytes=min(target["storageBytes"], limits.max_file_bytes),
    )
    if untile_mips(target["allocation"], layout) != source_mips:
        raise AssetError("candidate CPU untile readback differs from the original PC source mips")
    report["sourceBinding"] = binding
    report["proof"].update(pcSourceMipReadbackExact=True, cpuTileUntileRoundTripExact=True)
    report["qualification"].update(sourceLegacyMetadataMapped=False)
    return report


def build_pc_stock_texture_envelope_candidate(
    pc_blob: bytes,
    stock_blob: bytes,
    *,
    pc_texture_name: str,
    stock_texture_name: str,
    limits: Limits | None = None,
) -> tuple[bytes, dict]:
    """Tile one exact PC texture into a metadata-preserving stock envelope."""
    limits = Limits() if limits is None else limits
    replacement, _source_mips, _binding = _pc_replacement(
        pc_blob,
        stock_blob,
        pc_texture_name=pc_texture_name,
        stock_texture_name=stock_texture_name,
        limits=limits,
    )
    candidate, _report = build_stock_texture_envelope_candidate(
        stock_blob,
        texture_name=stock_texture_name,
        replacement=replacement,
        limits=limits,
    )
    report = verify_pc_stock_texture_envelope_candidate(
        pc_blob,
        stock_blob,
        candidate,
        pc_texture_name=pc_texture_name,
        stock_texture_name=stock_texture_name,
        limits=limits,
    )
    return candidate, report


def _package_evidence(
    report: dict,
    *,
    pc_texture_name: str,
    stock_texture_name: str,
) -> tuple[dict, dict]:
    """Bind both originals and the full differential proof into one resource record."""

    binding = report["sourceBinding"]
    source = {
        "bindingPolicy": PC_SOURCE_POLICY,
        "pcResource": {
            "container": "YTD",
            "sha256": binding["sourceResourceSha256"],
            "texture": pc_texture_name,
        },
        "stockEnvelope": {
            "container": "PTD",
            "sha256": report["sourceResourceSha256"],
            "texture": stock_texture_name,
        },
    }
    oracle = {
        "schemaVersion": 1,
        "kind": PACKAGE_ORACLE_KIND,
        "differentialProof": report,
        "qualification": {
            "activationEnabled": False,
            "gpuBehaviorQualified": False,
            "runtimeQualified": False,
            "releaseReady": False,
        },
    }
    return source, oracle


def build_pc_stock_texture_envelope_package(
    pc_blob: bytes,
    stock_blob: bytes,
    target_manifest_blob: bytes,
    *,
    pack_id: str,
    resource_path: str,
    logical_name: str,
    pc_texture_name: str,
    stock_texture_name: str,
    limits: Limits | None = None,
) -> tuple[dict[str, bytes], dict, dict]:
    """Build one inactive, target-bound loose package entirely in memory.

    The returned file map is suitable for ``custom_pack_schema.publish_package``.
    Exactly one candidate PTD is included.  Its record binds the complete PC
    YTD, stock PTD, selected texture identities, shape/layout fields and the
    independently regenerated byte-differential proof.
    """

    limits = Limits() if limits is None else limits
    target = package_schema.target_identity(target_manifest_blob)
    candidate, report = build_pc_stock_texture_envelope_candidate(
        pc_blob,
        stock_blob,
        pc_texture_name=pc_texture_name,
        stock_texture_name=stock_texture_name,
        limits=limits,
    )
    source, oracle = _package_evidence(
        report,
        pc_texture_name=pc_texture_name,
        stock_texture_name=stock_texture_name,
    )
    resource = package_schema.LooseResource(
        path=resource_path,
        resource_class="texture-dictionary",
        container="PTD",
        logical_name=logical_name,
        data=candidate,
        source=source,
        oracle=oracle,
    )
    manifest = package_schema.build_manifest(pack_id, target, [resource])
    files = package_schema.expected_package(manifest, {resource_path: candidate})
    return files, manifest, report


def _package_result(files: dict[str, bytes], manifest: dict, report: dict) -> dict:
    record = manifest["resources"][0]
    return {
        "schemaVersion": 1,
        "kind": PACKAGE_RESULT_KIND,
        "packId": manifest["packId"],
        "target": manifest["target"],
        "manifestSha256": _sha256(files["manifest.json"]),
        "resource": {
            "path": record["path"],
            "bytes": record["bytes"],
            "sha256": record["sha256"],
        },
        "source": record["source"],
        "oracle": record["oracle"],
        "qualification": {
            "activationEnabled": False,
            "gpuBehaviorQualified": False,
            "runtimeQualified": False,
            "releaseReady": False,
        },
        "candidateResourceSha256": report["candidateResourceSha256"],
    }


def publish_pc_stock_texture_envelope_package(
    package: Path,
    pc_blob: bytes,
    stock_blob: bytes,
    target_manifest_blob: bytes,
    *,
    pack_id: str,
    resource_path: str,
    logical_name: str,
    pc_texture_name: str,
    stock_texture_name: str,
    limits: Limits | None = None,
) -> dict:
    """Create-new publish one source-bound candidate package."""

    limits = Limits() if limits is None else limits
    files, manifest, report = build_pc_stock_texture_envelope_package(
        pc_blob,
        stock_blob,
        target_manifest_blob,
        pack_id=pack_id,
        resource_path=resource_path,
        logical_name=logical_name,
        pc_texture_name=pc_texture_name,
        stock_texture_name=stock_texture_name,
        limits=limits,
    )
    package_schema.publish_package(
        package,
        files,
        max_resources=1,
        max_resource_bytes=min(limits.max_file_bytes, package_schema.MAX_RESOURCE_BYTES),
    )
    return _package_result(files, manifest, report)


def verify_pc_stock_texture_envelope_package(
    package: Path,
    pc_blob: bytes,
    stock_blob: bytes,
    target_manifest_blob: bytes,
    *,
    pack_id: str,
    resource_path: str,
    logical_name: str,
    pc_texture_name: str,
    stock_texture_name: str,
    limits: Limits | None = None,
) -> dict:
    """Rebuild from both originals and compare every package file exactly."""

    limits = Limits() if limits is None else limits
    files, manifest, expected_report = build_pc_stock_texture_envelope_package(
        pc_blob,
        stock_blob,
        target_manifest_blob,
        pack_id=pack_id,
        resource_path=resource_path,
        logical_name=logical_name,
        pc_texture_name=pc_texture_name,
        stock_texture_name=stock_texture_name,
        limits=limits,
    )
    actual_manifest = package_schema.verify_package(
        package,
        expected_target=manifest["target"],
        expected_manifest=manifest,
        max_resources=1,
        max_resource_bytes=min(limits.max_file_bytes, package_schema.MAX_RESOURCE_BYTES),
    )
    actual_candidate = package_schema.read_regular(
        package / resource_path,
        maximum=manifest["resources"][0]["bytes"],
    )
    actual_report = verify_pc_stock_texture_envelope_candidate(
        pc_blob,
        stock_blob,
        actual_candidate,
        pc_texture_name=pc_texture_name,
        stock_texture_name=stock_texture_name,
        limits=limits,
    )
    if package_schema.canonical_json(actual_report) != package_schema.canonical_json(expected_report):
        raise package_schema.CustomPackSchemaError("fresh package differential proof differs from rebuilt evidence")
    if actual_manifest["resources"][0]["oracle"]["differentialProof"] != actual_report:
        raise package_schema.CustomPackSchemaError("manifest differential proof differs from fresh verification")
    package_schema.compare_package(
        package,
        files,
        expected_target=manifest["target"],
        expected_manifest=manifest,
    )
    return _package_result(files, manifest, actual_report)


def _visual_oracle_replacement(
    stock_blob: bytes,
    *,
    stock_texture_name: str,
    limits: Limits,
) -> tuple[bytes, bytes, dict]:
    """Build one exact, explicitly tiled BC3 quadrant allocation."""

    if type(stock_blob) is not bytes:
        raise AssetError("visual oracle stock input must be immutable complete bytes")
    parsed = parse_ps5_texture_dictionary(stock_blob, limits)
    target = _target(parsed, stock_texture_name)
    if target["format"] != "BC3" or target["mipLevels"] != 1:
        raise AssetError("visual oracle target must be exactly BC3 with one mip")
    if type(target["tileMode"]) is not int or target["tileMode"] not in (1, 5, 9):
        raise AssetError("visual oracle target must use explicit supported tile mode 1, 5 or 9")
    layout = surface_layout(
        target["width"],
        target["height"],
        target["mipLevels"],
        target["format"],
        target["tileMode"],
        max_storage_bytes=min(target["storageBytes"], limits.max_file_bytes),
    )
    if layout.storage_bytes != target["storageBytes"]:
        raise AssetError("visual oracle CPU layout size differs from the selected stock allocation")
    linear_mip = bc3_quadrant_mip(target["width"], target["height"])
    decoded = verify_bc3_quadrant_mip(linear_mip, target["width"], target["height"])
    replacement = tile_mips((linear_mip,), layout)
    readback = untile_mips(replacement, layout)
    if readback != (linear_mip,):
        raise AssetError("visual oracle CPU tile/untile readback differs from the generated mip")
    binding = {
        "policy": VISUAL_ORACLE_POLICY,
        "stockResourceSha256": parsed["resourceSha256"],
        "targetTexture": {
            "name": stock_texture_name,
            "format": target["format"],
            "width": target["width"],
            "height": target["height"],
            "mipLevels": target["mipLevels"],
            "tileMode": target["tileMode"],
            "graphicsOffset": target["graphicsOffset"],
            "allocationBytes": target["storageBytes"],
            "sourceAllocationSha256": target["storageSha256"],
        },
        "pattern": {
            **decoded,
            "tiledAllocationSha256": _sha256(replacement),
            "untiledReadbackSha256": _sha256(readback[0]),
        },
    }
    return replacement, linear_mip, binding


def verify_visual_oracle_stock_texture_envelope_candidate(
    stock_blob: bytes,
    candidate_blob: bytes,
    *,
    stock_texture_name: str,
    limits: Limits | None = None,
) -> dict:
    """Recreate, untile and decode the exact BC3 oracle from a fresh stock PTD."""

    limits = Limits() if limits is None else limits
    replacement, linear_mip, binding = _visual_oracle_replacement(
        stock_blob,
        stock_texture_name=stock_texture_name,
        limits=limits,
    )
    report = verify_stock_texture_envelope_candidate(
        stock_blob,
        candidate_blob,
        texture_name=stock_texture_name,
        replacement=replacement,
        limits=limits,
    )
    candidate = parse_ps5_texture_dictionary(candidate_blob, limits)
    target = _target(candidate, stock_texture_name)
    layout = surface_layout(
        target["width"],
        target["height"],
        target["mipLevels"],
        target["format"],
        target["tileMode"],
        max_storage_bytes=min(target["storageBytes"], limits.max_file_bytes),
    )
    readback = untile_mips(target["allocation"], layout)
    if readback != (linear_mip,):
        raise AssetError("candidate visual oracle untile readback differs from the exact generated mip")
    decoded = verify_bc3_quadrant_mip(readback[0], target["width"], target["height"])
    if decoded != {key: binding["pattern"][key] for key in decoded}:
        raise AssetError("candidate visual oracle decode proof differs from the source-bound pattern")
    report["sourceBinding"] = binding
    report["visualOracle"] = decoded
    report["proof"].update(
        cpuTileUntileRoundTripExact=True,
        generatedLinearMipReadbackExact=True,
        decodedRgbaQuadrantsExact=True,
    )
    report["qualification"].update(activationEnabled=False, gpuBehaviorQualified=False)
    return report


def build_visual_oracle_stock_texture_envelope_candidate(
    stock_blob: bytes,
    *,
    stock_texture_name: str,
    limits: Limits | None = None,
) -> tuple[bytes, dict]:
    """Build one canonical stock-envelope BC3 quadrant candidate."""

    limits = Limits() if limits is None else limits
    replacement, _linear_mip, _binding = _visual_oracle_replacement(
        stock_blob,
        stock_texture_name=stock_texture_name,
        limits=limits,
    )
    candidate, _report = build_stock_texture_envelope_candidate(
        stock_blob,
        texture_name=stock_texture_name,
        replacement=replacement,
        limits=limits,
    )
    return candidate, verify_visual_oracle_stock_texture_envelope_candidate(
        stock_blob,
        candidate,
        stock_texture_name=stock_texture_name,
        limits=limits,
    )


def _visual_oracle_package_evidence(report: dict, *, stock_texture_name: str) -> tuple[dict, dict]:
    binding = report["sourceBinding"]
    source = {
        "bindingPolicy": VISUAL_ORACLE_POLICY,
        "stockEnvelope": {
            "container": "PTD",
            "sha256": binding["stockResourceSha256"],
            "texture": stock_texture_name,
        },
        "targetTexture": binding["targetTexture"],
        "pattern": binding["pattern"],
    }
    oracle = {
        "schemaVersion": 1,
        "kind": VISUAL_ORACLE_PACKAGE_KIND,
        "differentialProof": report,
        "qualification": {
            "activationEnabled": False,
            "gpuBehaviorQualified": False,
            "gpuLayoutQualified": False,
            "runtimeQualified": False,
            "releaseReady": False,
        },
    }
    return source, oracle


def build_visual_oracle_stock_texture_envelope_package(
    stock_blob: bytes,
    target_manifest_blob: bytes,
    *,
    pack_id: str,
    resource_path: str,
    logical_name: str,
    stock_texture_name: str,
    limits: Limits | None = None,
) -> tuple[dict[str, bytes], dict, dict]:
    """Build one inactive, source-bound BC3 visual-oracle package in memory."""

    limits = Limits() if limits is None else limits
    target = package_schema.target_identity(target_manifest_blob)
    candidate, report = build_visual_oracle_stock_texture_envelope_candidate(
        stock_blob,
        stock_texture_name=stock_texture_name,
        limits=limits,
    )
    source, oracle = _visual_oracle_package_evidence(report, stock_texture_name=stock_texture_name)
    resource = package_schema.LooseResource(
        path=resource_path,
        resource_class="texture-dictionary",
        container="PTD",
        logical_name=logical_name,
        data=candidate,
        source=source,
        oracle=oracle,
    )
    manifest = package_schema.build_manifest(pack_id, target, [resource])
    files = package_schema.expected_package(manifest, {resource_path: candidate})
    return files, manifest, report


def _visual_oracle_package_result(files: dict[str, bytes], manifest: dict, report: dict) -> dict:
    record = manifest["resources"][0]
    return {
        "schemaVersion": 1,
        "kind": VISUAL_ORACLE_RESULT_KIND,
        "packId": manifest["packId"],
        "target": manifest["target"],
        "manifestSha256": _sha256(files["manifest.json"]),
        "resource": {
            "path": record["path"],
            "bytes": record["bytes"],
            "sha256": record["sha256"],
        },
        "source": record["source"],
        "oracle": record["oracle"],
        "qualification": {
            "activationEnabled": False,
            "gpuBehaviorQualified": False,
            "gpuLayoutQualified": False,
            "runtimeQualified": False,
            "releaseReady": False,
        },
        "candidateResourceSha256": report["candidateResourceSha256"],
    }


def publish_visual_oracle_stock_texture_envelope_package(
    package: Path,
    stock_blob: bytes,
    target_manifest_blob: bytes,
    *,
    pack_id: str,
    resource_path: str,
    logical_name: str,
    stock_texture_name: str,
    limits: Limits | None = None,
) -> dict:
    """Create-new publish one inactive stock-bound visual-oracle package."""

    limits = Limits() if limits is None else limits
    files, manifest, report = build_visual_oracle_stock_texture_envelope_package(
        stock_blob,
        target_manifest_blob,
        pack_id=pack_id,
        resource_path=resource_path,
        logical_name=logical_name,
        stock_texture_name=stock_texture_name,
        limits=limits,
    )
    package_schema.publish_package(
        package,
        files,
        max_resources=1,
        max_resource_bytes=min(limits.max_file_bytes, package_schema.MAX_RESOURCE_BYTES),
    )
    return _visual_oracle_package_result(files, manifest, report)


def verify_visual_oracle_stock_texture_envelope_package(
    package: Path,
    stock_blob: bytes,
    target_manifest_blob: bytes,
    *,
    pack_id: str,
    resource_path: str,
    logical_name: str,
    stock_texture_name: str,
    limits: Limits | None = None,
) -> dict:
    """Rebuild a visual oracle from fresh stock input and compare every file."""

    limits = Limits() if limits is None else limits
    files, manifest, expected_report = build_visual_oracle_stock_texture_envelope_package(
        stock_blob,
        target_manifest_blob,
        pack_id=pack_id,
        resource_path=resource_path,
        logical_name=logical_name,
        stock_texture_name=stock_texture_name,
        limits=limits,
    )
    actual_manifest = package_schema.verify_package(
        package,
        expected_target=manifest["target"],
        expected_manifest=manifest,
        max_resources=1,
        max_resource_bytes=min(limits.max_file_bytes, package_schema.MAX_RESOURCE_BYTES),
    )
    actual_candidate = package_schema.read_regular(
        package / resource_path,
        maximum=manifest["resources"][0]["bytes"],
    )
    actual_report = verify_visual_oracle_stock_texture_envelope_candidate(
        stock_blob,
        actual_candidate,
        stock_texture_name=stock_texture_name,
        limits=limits,
    )
    if package_schema.canonical_json(actual_report) != package_schema.canonical_json(expected_report):
        raise package_schema.CustomPackSchemaError("fresh visual oracle proof differs from rebuilt evidence")
    if actual_manifest["resources"][0]["oracle"]["differentialProof"] != actual_report:
        raise package_schema.CustomPackSchemaError("manifest visual oracle proof differs from fresh verification")
    package_schema.compare_package(
        package,
        files,
        expected_target=manifest["target"],
        expected_manifest=manifest,
    )
    return _visual_oracle_package_result(files, manifest, actual_report)

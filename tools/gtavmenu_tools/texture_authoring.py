"""Explicit texture metadata choices informed, never decided, by PC bindings.

The presets name observed complete +0x40 words, not universal texture roles.
Source sampler slots neither recover discarded Legacy semantics nor establish
that an authored word is compatible with a new texture or target GPU.
"""

from __future__ import annotations

import json
from collections.abc import Sequence
from dataclasses import replace

from .asset_formats import AssetError, Limits, resource_header
from .asset_materials import CHILD_MATERIAL_SCOPE, MAIN_MATERIAL_SCOPE, read_pc_material_bindings
from .custom_pack_schema import CustomPackSchemaError, canonical_json
from .texture_conversion import digest
from .texture_usage import describe_legacy_usage

AUTHORING_POLICY = "explicit-texture-metadata-experimental-v1"
OBSERVED_SOURCE_POLICY = "observed-legacy-metadata-v1"
STATIC_SOURCE_POLICY = "explicit-static-pixels-v1"
NO_MATERIAL_SCOPE = "no-material-evidence-v1"
AUTHORING_PRESETS = {
    "observed-00200214": 0x00200214,
    "observed-00300201": 0x00300201,
    "observed-00800201": 0x00800201,
    "observed-00800214": 0x00800214,
    "observed-00800215": 0x00800215,
    "observed-00800216": 0x00800216,
    "observed-00800217": 0x00800217,
}
MAX_PROFILE_BYTES = 1024 * 1024
MAX_MATERIAL_SOURCES = 8
_SELECTION_KEYS = {"preset", "rationale", "acknowledgeAmbiguity"}


def profile_material_scope(profile: dict) -> str:
    """Select only a versioned evidence scope, never a caller-supplied override."""
    if type(profile) is not dict:
        raise AssetError("authoring profile must be a JSON object")
    version = profile.get("schemaVersion")
    if type(version) is int:
        if version == 1 and "materialScope" not in profile:
            return MAIN_MATERIAL_SCOPE
        if (
            version == 2
            and type(profile.get("materialScope")) is str
            and profile["materialScope"] == CHILD_MATERIAL_SCOPE
        ):
            return CHILD_MATERIAL_SCOPE
        if (
            version == 3
            and type(profile.get("sourcePolicy")) is str
            and profile["sourcePolicy"] == STATIC_SOURCE_POLICY
            and type(profile.get("materialScope")) is str
            and profile["materialScope"] in (MAIN_MATERIAL_SCOPE, CHILD_MATERIAL_SCOPE)
        ):
            return profile["materialScope"]
        if (
            version == 4
            and type(profile.get("sourcePolicy")) is str
            and profile.get("sourcePolicy") == STATIC_SOURCE_POLICY
            and type(profile.get("materialScope")) is str
            and profile.get("materialScope") == NO_MATERIAL_SCOPE
        ):
            return NO_MATERIAL_SCOPE
    raise AssetError("authoring profile version or material scope is stale or modified")


def profile_source_policy(profile: dict) -> str:
    """Derive source admission from a versioned profile, never from usage bits."""

    profile_material_scope(profile)
    if profile["schemaVersion"] in (3, 4):
        return STATIC_SOURCE_POLICY
    if "sourcePolicy" in profile:
        raise AssetError("authoring profile source policy is stale or modified")
    return OBSERVED_SOURCE_POLICY


def _profile_json(value: object, limits: Limits) -> bytes:
    try:
        encoded = canonical_json(value)
    except CustomPackSchemaError as exc:
        raise AssetError(f"invalid authoring profile JSON: {exc}") from exc
    if len(encoded) > min(MAX_PROFILE_BYTES, limits.max_metadata_bytes, limits.max_total_bytes):
        raise AssetError("authoring profile exceeds the metadata byte limit")
    # canonical_json briefly holds its text representation and encoded bytes.
    if 2 * len(encoded) > limits.max_total_bytes:
        raise AssetError("authoring JSON encoding exceeds the cumulative byte budget")
    return encoded


def _materials(material_blobs: Sequence[bytes], limits: Limits, material_scope: str) -> tuple[list[dict], int]:
    if not isinstance(material_blobs, Sequence) or isinstance(material_blobs, (str, bytes, bytearray)):
        raise AssetError("authoring needs a sequence of original material resources")
    if material_scope == NO_MATERIAL_SCOPE:
        if len(material_blobs):
            raise AssetError("no-material-evidence scope must not discard supplied material resources")
        return [], 0
    if not 1 <= len(material_blobs) <= MAX_MATERIAL_SOURCES:
        raise AssetError("authoring needs between one and eight original material resources")
    by_hash: dict[str, bytes] = {}
    retained = 0
    for blob in material_blobs:
        if type(blob) is not bytes or not 16 <= len(blob) <= limits.max_file_bytes:
            raise AssetError("authoring material is empty or exceeds the file byte limit")
        identity = digest(blob)
        if identity in by_hash:
            raise AssetError("authoring material resources must have distinct SHA-256 identities")
        header = resource_header(blob)
        retained += len(blob) + header["systemBytes"] + header["graphicsBytes"]
        if retained > limits.max_total_bytes:
            raise AssetError("authoring materials exceed the cumulative encoded/decoded byte budget")
        by_hash[identity] = blob
    rows = []
    for _identity, blob in sorted(by_hash.items()):
        result = read_pc_material_bindings(blob, limits, scope=material_scope)
        rows.append(result | {"bytes": len(blob)})
    return rows, retained


def build_authoring_template(
    source: dict,
    material_blobs: Sequence[bytes],
    limits: Limits,
    *,
    material_scope: str = MAIN_MATERIAL_SCOPE,
    source_policy: str = OBSERVED_SOURCE_POLICY,
) -> dict:
    """Reparse supplied original YFTs and leave every author decision unresolved.

    ``source`` is the complete validated dictionary intake. Callers own source
    admission and pass limits with their retained source bytes already removed.
    Material entry limits are separate from the candidate's texture-entry limit.
    A template reserves three canonical equivalents: its retained object, the
    encoder's text and its encoded bytes. This is not a Python heap estimate.
    """

    if type(material_scope) is not str or material_scope not in (
        MAIN_MATERIAL_SCOPE,
        CHILD_MATERIAL_SCOPE,
        NO_MATERIAL_SCOPE,
    ):
        raise AssetError("unsupported authoring material scope")
    if type(source_policy) is not str or source_policy not in (OBSERVED_SOURCE_POLICY, STATIC_SOURCE_POLICY):
        raise AssetError("unsupported authoring source policy")
    if material_scope == NO_MATERIAL_SCOPE and source_policy != STATIC_SOURCE_POLICY:
        raise AssetError("no-material-evidence scope requires explicit static-image re-authoring")
    materials, retained = _materials(material_blobs, limits, material_scope)
    local = {row["name"].lower(): row["name"] for row in source["textures"]}
    bindings: dict[str, list[dict]] = {name: [] for name in local.values()}
    external, nulls = [], []
    for material in materials:
        shaders = {shader["shaderIndex"]: shader for shader in material["shaders"]}
        for binding in material["bindings"]:
            shader = shaders[binding["shaderIndex"]]
            observation = {
                "materialSha256": material["sourceSha256"],
                "shaderNameHash": shader["nameHash"],
                "shaderFileNameHash": shader["fileNameHash"],
                **binding,
            }
            name = binding["textureName"]
            if name is None:
                nulls.append(observation)
            elif name.lower() in local:
                bindings[local[name.lower()]].append(observation)
            else:
                external.append(observation)
    textures = []
    for row in source["textures"]:
        observations = bindings[row["name"]]
        hashes = {binding["samplerHash"] for binding in observations}
        textures.append(
            {key: row[key] for key in ("name", "nameHash", "headerSha256")}
            | {
                "observedSamplers": sorted(
                    {binding["samplerName"] for binding in observations if binding["samplerName"] is not None}
                ),
                "unknownSamplerHashes": sorted(
                    {binding["samplerHash"] for binding in observations if binding["samplerName"] is None}
                ),
                "bindings": observations,
                "multiUse": len(hashes) > 1,
                "unbound": not observations,
                "unknownSampler": any(binding["samplerName"] is None for binding in observations),
                "selection": {"preset": None, "rationale": "", "acknowledgeAmbiguity": False},
            }
        )
        if source_policy == STATIC_SOURCE_POLICY:
            # Preserve and expose the entire source word; no footprint match,
            # usage label or sampler observation selects the author's choice.
            textures[-1]["sourceUsage"] = describe_legacy_usage(
                row["metadata"]["UsageData"], sum(map(len, row["mips"]))
            )
            textures[-1]["selection"]["acknowledgeSourceUsageDiscarded"] = False
    result = {
        "schemaVersion": 1,
        "kind": "gtavmenu-texture-authoring-profile",
        "policy": AUTHORING_POLICY,
        "sourceSha256": source["sourceSha256"],
        "materialSources": [
            {"sha256": row["sourceSha256"]}
            | {
                key: row[key]
                for key in (
                    "bytes",
                    "header",
                    "scope",
                    "shaderCount",
                    "parameterCount",
                    "textureParameterCount",
                    "nonTextureParameterCount",
                    "selectedMetadataBytes",
                    "embeddedTextureDictionaryPresent",
                )
            }
            for row in materials
        ],
        "evidence": {
            "scope": "supplied-main-drawable-shader-groups-only",
            "localBindingCount": sum(len(rows) for rows in bindings.values()),
            "externalBindings": external,
            "nullBindings": nulls,
        },
        "qualification": {
            "sourceMaterialBindingsReparsed": True,
            "fullMaterialGraphCovered": False,
            "samplersDetermineTargetMetadata": False,
            "sourceLegacyMetadataMapped": False,
            "ps5MetadataSemanticsQualified": False,
            "gpuInterpretationQualified": False,
            "runtimeQualified": False,
            "releaseReady": False,
        },
        "textures": textures,
        "acknowledgeUnqualifiedMetadata": False,
    }
    if material_scope == CHILD_MATERIAL_SCOPE:
        result.update(schemaVersion=2, materialScope=material_scope)
        result["evidence"]["scope"] = "supplied-main-and-lod1-child-shader-mappings-only"
        for record, material in zip(result["materialSources"], materials, strict=True):
            # Keep the shared shader table's sampler observations unique. Child
            # uses join these records by material SHA-256 and shaderIndex;
            # traversing children must not invent or duplicate sampler slots.
            record["childShaderMappings"] = material["childShaderMappings"]
    if source_policy == STATIC_SOURCE_POLICY:
        result.update(schemaVersion=3, sourcePolicy=source_policy, materialScope=material_scope)
        result["qualification"]["sourceUsageDataSemanticsTransferred"] = False
    if material_scope == NO_MATERIAL_SCOPE:
        result["schemaVersion"] = 4
        result["evidence"]["scope"] = NO_MATERIAL_SCOPE
        result["qualification"].update(sourceMaterialBindingsReparsed=False, sourceMaterialEvidencePresent=False)
    encoded = _profile_json(result, limits)
    if retained + 3 * len(encoded) > limits.max_total_bytes:
        raise AssetError("authoring materials and template copies exceed the cumulative byte budget")
    return result


def _profile_evidence(
    source: dict, profile: dict, material_blobs: Sequence[bytes], limits: Limits
) -> tuple[bytes, dict]:
    """Validate immutable evidence independently of incomplete author choices.

    Callers reserve their original source/profile objects. This helper reserves
    its encoded profile, both parsed copies and comparison/encoder temporaries
    using five supplied-profile plus three regenerated-template canonical byte
    equivalents, in addition to the materials' encoded/declared-decoded budget.
    The reservation is deliberately conservative, not a Python heap estimate.
    """

    encoded = _profile_json(profile, limits)
    if type(profile) is not dict:
        raise AssetError("authoring profile must be a JSON object")
    material_scope = profile_material_scope(profile)
    source_policy = profile_source_policy(profile)
    template = build_authoring_template(
        source,
        material_blobs,
        replace(limits, max_total_bytes=limits.max_total_bytes - len(encoded)),
        material_scope=material_scope,
        source_policy=source_policy,
    )
    template_encoded = _profile_json(template, limits)
    rows = profile.get("textures")
    if type(rows) is not list or len(rows) != len(template["textures"]):
        raise AssetError("authoring profile needs complete source texture coverage")
    material_bytes = sum(
        row["bytes"] + row["header"]["systemBytes"] + row["header"]["graphicsBytes"]
        for row in template["materialSources"]
    )
    peak_bytes = material_bytes + 5 * len(encoded) + 3 * len(template_encoded)
    if peak_bytes > limits.max_total_bytes:
        raise AssetError("authoring serialized-copy peak exceeds the cumulative byte budget")
    # Only these explicitly author-editable fields are excluded from comparison.
    comparable = json.loads(encoded)
    comparable["acknowledgeUnqualifiedMetadata"] = False
    for row, expected in zip(comparable["textures"], template["textures"], strict=True):
        if type(row) is not dict:
            raise AssetError("each authoring texture must be a JSON object")
        row["selection"] = expected["selection"]
    if _profile_json(comparable, limits) != template_encoded:
        raise AssetError("authoring source identity, texture coverage or binding evidence is stale or modified")
    return encoded, template


def _choice_issues(profile: dict, template: dict) -> list[dict]:
    issues = []

    def issue(field: str, message: str, name: str | None = None) -> None:
        issues.append({"field": field, "texture": name, "message": message})

    if profile.get("acknowledgeUnqualifiedMetadata") is not True:
        issue(
            "acknowledgeUnqualifiedMetadata",
            "authoring requires explicit acknowledgment of unqualified target metadata",
        )
    source_policy = profile_source_policy(template)
    rows = profile["textures"]
    for actual, expected in zip(rows, template["textures"], strict=True):
        name = expected["name"]
        if type(actual) is not dict or type(actual.get("selection")) is not dict:
            issue("selection", "each authoring texture needs a selection object", name)
            continue
        selection = actual["selection"]
        selection_keys = _SELECTION_KEYS
        if source_policy == STATIC_SOURCE_POLICY:
            selection_keys = selection_keys | {"acknowledgeSourceUsageDiscarded"}
        if set(selection) != selection_keys:
            issue("selection", "authoring selection has missing or unknown fields", name)
        if source_policy == STATIC_SOURCE_POLICY and selection.get("acknowledgeSourceUsageDiscarded") is not True:
            issue(
                "acknowledgeSourceUsageDiscarded",
                f"source UsageData discard must be explicitly acknowledged: {name}",
                name,
            )
        preset, rationale = selection.get("preset"), selection.get("rationale")
        if type(preset) is not str or preset not in AUTHORING_PRESETS:
            issue("preset", "every texture needs an explicit observed-word preset", name)
        if (
            type(rationale) is not str
            or not 1 <= len(rationale) <= 512
            or not rationale.strip()
            or any(not 32 <= ord(char) <= 126 for char in rationale)
        ):
            issue("rationale", "every authoring rationale must contain 1-512 printable ASCII characters", name)
        acknowledgment = selection.get("acknowledgeAmbiguity")
        if type(acknowledgment) is not bool:
            issue("acknowledgeAmbiguity", "authoring ambiguity acknowledgment must be a boolean", name)
        if any(expected[key] for key in ("multiUse", "unbound", "unknownSampler")) and not acknowledgment:
            issue("acknowledgeAmbiguity", f"authoring ambiguity must be acknowledged: {name}", name)
    return issues


def check_authoring_profile(source: dict, profile: dict, material_blobs: Sequence[bytes], limits: Limits) -> dict:
    """Recompute evidence and list every unresolved choice without selecting one."""
    encoded, template = _profile_evidence(source, profile, material_blobs, limits)
    return {
        "profileSha256": digest(encoded),
        "profileBytes": len(encoded),
        "sourcePolicy": profile_source_policy(template),
        "materialScope": profile_material_scope(template),
        "materialCount": len(template["materialSources"]),
        "textureCount": len(template["textures"]),
        "issues": _choice_issues(profile, template),
    }


def resolve_authoring_profile(source: dict, profile: dict, material_blobs: Sequence[bytes], limits: Limits) -> dict:
    """Resolve explicit choices only after fresh source/evidence validation."""
    encoded, template = _profile_evidence(source, profile, material_blobs, limits)
    issues = _choice_issues(profile, template)
    if issues:
        raise AssetError(issues[0]["message"])
    validated = json.loads(encoded)
    metadata = {row["name"]: AUTHORING_PRESETS[row["selection"]["preset"]] for row in validated["textures"]}
    result = {
        "targetMetadataByName": metadata,
        "profileSha256": digest(encoded),
        "profile": validated,
        "materialCount": len(template["materialSources"]),
        "textureCount": len(template["textures"]),
    }
    source_policy = profile_source_policy(template)
    if source_policy == STATIC_SOURCE_POLICY:
        result.update(sourcePolicy=source_policy, sourceUsageDataSemanticsTransferred=False)
    return result

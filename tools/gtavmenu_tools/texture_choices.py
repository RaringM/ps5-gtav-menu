"""Compact explicit selections bound to an immutable authoring evidence profile.

An overlay never supplies evidence or chooses a preset. Conversion/checking still
reparse the original source and validate the fully assembled profile.
"""

from __future__ import annotations

import json
import re

from .asset_formats import AssetError
from .custom_pack_schema import CustomPackSchemaError, canonical_json
from .texture_authoring import MAX_PROFILE_BYTES, STATIC_SOURCE_POLICY, profile_source_policy
from .texture_conversion import digest

CHOICES_KIND = "gtavmenu-texture-authoring-choices"
_ROOT_KEYS = {"schemaVersion", "kind", "sourceSha256", "profileSha256", "acknowledgeUnqualifiedMetadata", "textures"}
_SELECTION_KEYS = {"preset", "rationale", "acknowledgeAmbiguity"}


def _encoded(value: object) -> bytes:
    try:
        data = canonical_json(value)
    except CustomPackSchemaError as exc:
        raise AssetError(f"invalid texture choices/profile JSON: {exc}") from exc
    if len(data) > MAX_PROFILE_BYTES:
        raise AssetError("texture choices/profile exceeds the metadata byte limit")
    return data


def _profile(profile: dict) -> tuple[bytes, list[str], set[str]]:
    data = _encoded(profile)
    policy = profile_source_policy(profile)
    if profile.get("kind") != "gtavmenu-texture-authoring-profile" or not isinstance(profile.get("sourceSha256"), str):
        raise AssetError("choices require an authoring evidence profile and source identity")
    if not re.fullmatch(r"[0-9a-f]{64}", profile["sourceSha256"]):
        raise AssetError("choices require an exact source SHA-256")
    rows = profile.get("textures")
    if type(rows) is not list or not 1 <= len(rows) <= 64:
        raise AssetError("choices require a bounded complete texture list")
    names = []
    for row in rows:
        if type(row) is not dict or type(row.get("name")) is not str or not 1 <= len(row["name"]) <= 256:
            raise AssetError("choices require exact bounded texture names")
        names.append(row["name"])
    if len({name.lower() for name in names}) != len(names):
        raise AssetError("choices profile contains duplicate or case-aliased texture names")
    keys = _SELECTION_KEYS | ({"acknowledgeSourceUsageDiscarded"} if policy == STATIC_SOURCE_POLICY else set())
    return data, names, keys


def build_texture_choices(profile: dict) -> dict:
    """Produce compact unresolved choices; never copy or infer preset decisions."""
    data, names, keys = _profile(profile)
    selection = {"preset": None, "rationale": "", "acknowledgeAmbiguity": False}
    if "acknowledgeSourceUsageDiscarded" in keys:
        selection["acknowledgeSourceUsageDiscarded"] = False
    result = {
        "schemaVersion": 1,
        "kind": CHOICES_KIND,
        "sourceSha256": profile["sourceSha256"],
        "profileSha256": digest(data),
        "acknowledgeUnqualifiedMetadata": False,
        "textures": [{"name": name, "selection": dict(selection)} for name in names],
    }
    _encoded(result)
    return result


def apply_texture_choices(profile: dict, choices: dict) -> dict:
    """Return a selected copy, accepting only exact identities/names/editable keys.

    Unresolved choice values are retained so profile-check can aggregate them.
    This function grants no admission; fresh profile validation remains required.
    """
    data, names, selection_keys = _profile(profile)
    choices_data = _encoded(choices)
    if type(choices) is not dict or set(choices) != _ROOT_KEYS:
        raise AssetError("texture choices have missing or unknown root fields")
    if type(choices["schemaVersion"]) is not int or choices["schemaVersion"] != 1 or choices["kind"] != CHOICES_KIND:
        raise AssetError("unsupported texture choices version or kind")
    if choices["sourceSha256"] != profile["sourceSha256"] or choices["profileSha256"] != digest(data):
        raise AssetError("texture choices source/profile identity is stale or modified")
    rows = choices["textures"]
    if type(rows) is not list or len(rows) != len(names):
        raise AssetError("texture choices need exact complete source-name coverage")
    selections = {}
    # Copy bounded canonical JSON, preventing later caller edits from aliasing.
    copied_choices = json.loads(choices_data)
    for row in copied_choices["textures"]:
        if type(row) is not dict or set(row) != {"name", "selection"}:
            raise AssetError("each texture choice needs only name and selection")
        name, selection = row["name"], row["selection"]
        if type(name) is not str or name not in names or name in selections:
            raise AssetError("texture choices contain an unknown, duplicated or case-modified name")
        if type(selection) is not dict or set(selection) != selection_keys:
            raise AssetError("texture choice selection has missing or unknown fields")
        selections[name] = selection
    selected = json.loads(data)
    selected["acknowledgeUnqualifiedMetadata"] = copied_choices["acknowledgeUnqualifiedMetadata"]
    for row in selected["textures"]:
        row["selection"] = selections[row["name"]]
    _encoded(selected)
    return selected

"""Texture serialization policies read from verified compiled effects.

Only bounded bank bytes and the reviewed format projection are consumed.
No executable, private reports, renderer or shader programs are run.
"""

from __future__ import annotations

import hashlib
import json
import struct

from gtavmenu_tools import vehicle_contracts
from gtavmenu_tools.asset_formats import AssetError, Limits, decode_resource
from shader_database import read_banks, unique


def texture_attributes(raw, parameters, contract):
    fields = contract["attributes"]

    def get(fmt, at):
        if not 0 <= at <= len(raw) - struct.calcsize(fmt):
            raise AssetError("compiled texture attribute escapes parameter block")
        return struct.unpack_from(fmt, raw, at)[0]

    result = []
    for parameter in parameters:
        if parameter["kind"] != 0:
            continue
        at = parameter["descriptorOffset"]
        count = get("<B", at + fields["countOffset"])
        relative = get("<H", at + fields["relativeOffset"])
        if count and (not relative or at + relative + count * fields["stride"] > len(raw)):
            raise AssetError("compiled texture attribute table exceeds parameter block")
        names = []
        for index in range(count):
            row = at + relative + index * fields["stride"]
            if get("<I", row) & fields["tagMask"] != fields["defaultNameTag"]:
                continue
            offset = row + get("<I", row + fields["valueOffset"])
            if not 0 <= offset < len(raw):
                raise AssetError("compiled texture default string escapes parameter block")
            end = raw.find(b"\0", offset, min(offset + 1024, len(raw)))
            if end <= offset:
                raise AssetError("compiled texture default string is empty or unterminated")
            try:
                names.append(raw[offset:end].decode("ascii"))
            except UnicodeError as exc:
                raise AssetError("compiled texture default string is not ASCII") from exc
        if len(names) > 1:
            raise AssetError("compiled texture default attribute is ambiguous")
        result.append(parameter | {"attributeCount": count, "defaultTextureNames": names})
    return result


def attach(directory, catalog, bank, material, contract=None, *, manifest=None):
    contract = vehicle_contracts.load()["native"]["texturePolicies"] if contract is None else contract
    blobs = read_banks(directory, catalog, manifest=manifest)
    effects = {e["name"]: e for e in catalog["requiredEffects"]}
    for key, record in bank.items():
        effect = effects[material["mappings"][key]["name"]]
        section = effect["blocks"][4]
        raw = blobs[effect["bank"]][section["offset"] : section["offset"] + section["bytes"]]
        attributes = texture_attributes(raw, effect["parameters"], contract)
        effect["textureAttributes"] = attributes
        policies = {}
        for parameter in record["schema"]["parameters"]:
            if parameter["kind"] != "Texture":
                continue
            if parameter["TextureIndex"] > contract["textureIndexMask"]:
                raise AssetError("serialized texture index exceeds native remapping profile")
            matches = [p for p in effect["parameters"] if p["secondaryHash"] == parameter["nameHash"]]
            if len(matches) > 1 or (matches and matches[0]["kind"] != material["kinds"]["Texture"]):
                raise AssetError("compiled texture policy alias is ambiguous or has a different kind")
            selected = (
                unique(
                    [p for p in attributes if p["secondaryHash"] == parameter["nameHash"]],
                    "compiled texture attributes",
                )
                if matches
                else None
            )
            policies[str(parameter["nameHash"])] = {
                "effectName": effect["name"],
                "effectSha256": effect["sha256"],
                "nativeParameter": parameter,
                "compiledParameter": selected,
                "action": "preserve-native-initialization" if matches else "ignore-absent-compiled-alias",
                "requiredSerializedPointer": 0,
                "nativeTextureValueQualified": False,
            }
        record["textureLoadPolicies"] = policies
    catalog["texturePolicyContract"] = contract
    return contract


def observe(manifest, files):
    import gen9_materials as native_materials
    import resource_page_layout

    rows = []
    for resource in manifest["resources"]:
        name = resource["resource"]["component"]
        _, raw = decode_resource(files[name], Limits().max_file_bytes)
        graph = json.loads(files[resource["resource"]["placement"]])
        raw = resource_page_layout.logical_view(raw, graph)  # paged (route B) -> logical
        audit = json.loads(files[resource["materials"]["placement"]])
        at = unique(
            [r["offset"] for k, r in graph["blocks"].items() if k.endswith(".materials.shader-group")],
            "texture policy final group",
        )
        shaders = native_materials.read_group(raw, graph["sourceBase"], at, manifest["materialContract"])
        for index, (shader, written) in enumerate(zip(shaders, audit["shaders"], strict=True)):
            schema = shader["schema"]
            bank = manifest["materialSchemas"][str(schema["nameHash"])]
            if schema != bank["schema"] or written["nameHash"] != schema["nameHash"]:
                raise AssetError("texture policy final shader schema differs")
            for parameter in written["parameters"]:
                if parameter["sourceIndex"] is not None or parameter["kind"] != "Texture":
                    continue
                policy = bank["textureLoadPolicies"][str(parameter["nativeNameHash"])]
                if (
                    policy["nativeParameter"] != parameter["nativeParameter"]
                    or parameter["textureLoadPolicySha256"]
                    != hashlib.sha256(native_materials.canonical(policy)).hexdigest()
                    or parameter["initialization"] != policy["action"]
                    or not parameter["serializationPolicyQualified"]
                    or parameter["semanticDefaultQualified"]
                    or shader["texturePointers"][policy["nativeParameter"]["TextureIndex"]] != 0
                ):
                    raise AssetError("introduced texture policy readback differs")
                rows.append(
                    {
                        "resource": name,
                        "shaderIndex": index,
                        "shaderNameHash": schema["nameHash"],
                        "parameterIndex": schema["parameters"].index(policy["nativeParameter"]),
                        "parameterHash": parameter["nativeNameHash"],
                        "action": policy["action"],
                        "nativeDefaultAttributes": policy["compiledParameter"],
                        "serializedInfoOffset": shader["header"]["G9_ParamInfosPointer"] - graph["sourceBase"],
                        "serializedTexturesOffset": shader["header"]["G9_TextureRefsPointer"] - graph["sourceBase"],
                    }
                )
    expected = sum(r["materials"]["verification"]["introducedTextureLoadPolicies"] for r in manifest["resources"])
    if len(rows) != expected:
        raise AssetError("introduced texture policy coverage differs")
    return rows

"""Bounded material bindings, constant initialization and remapping readbacks."""

from __future__ import annotations

import hashlib
import json
import re
import struct

import gen9_materials as native_materials
import resource_page_layout
from gtavmenu_tools.asset_formats import AssetError, Limits, decode_resource
from gtavmenu_tools.hashes import joaat


def unique(rows, label):
    if len(rows) != 1:
        raise AssetError(f"{label} is missing or ambiguous")
    return rows[0]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def resource_models(reference, files):
    """Read the actual final PFT model-to-shader bindings, retaining resource bytes."""
    result = []
    material, model_layout = reference["materialContract"], reference["modelContract"]["layout"]
    for resource in reference["resources"]:
        name = resource["resource"]["component"]
        _, raw = decode_resource(files[name], Limits().max_file_bytes)
        graph = json.loads(files[resource["resource"]["placement"]])
        raw = resource_page_layout.logical_view(raw, graph)  # paged (route B) -> logical
        base = graph["sourceBase"]
        group = unique(
            [v["offset"] for k, v in graph["blocks"].items() if k.endswith(".materials.shader-group")],
            "serialized material group",
        )
        header = native_materials.get(raw, group, material["group"])
        count = header["ShadersCount1"]
        if not 0 < count <= 255 or header["ShadersCount2"] != count:
            raise AssetError("shader selection resource group count differs")
        data = native_materials.span(raw, base, header["ShadersPointer"], count * 8, 8)
        shaders = list(struct.unpack(f"<{count}Q", data))
        rows = []
        for label, block in sorted(graph["blocks"].items()):
            if not re.search(r"\.models\.model-\d+$", label):
                continue
            at = block["offset"]
            h = native_materials.get(raw, at, model_layout)
            n = h["GeometriesCount1"]
            if not 0 < n <= 256 or (h["GeometriesCount2"], h["GeometriesCount3"]) != (n, n):
                raise AssetError("shader selection resource model count differs")
            data = native_materials.span(raw, base, h["ShaderMappingPointer"], n * 2, 2)
            mapping = list(struct.unpack(f"<{n}H", data))
            if any(i >= count for i in mapping):
                raise AssetError("shader selection resource mapping exceeds its group")
            for i, shader in enumerate(mapping):
                sh = native_materials.get(raw, shaders[shader] - base, material["shader"])
                rows.append(
                    {
                        "modelOffset": at,
                        "geometryIndex": i,
                        "geometryCount": n,
                        "shaderIndex": shader,
                        "shaderOffset": shaders[shader] - base,
                        "modelMask": h["RenderMaskFlags"] & 255,
                        "bucket": sh["RenderBucket"],
                        "bucketMask": sh["RenderBucketMask"],
                    }
                )
        if len(rows) != resource["models"]["verification"]["geometryCount"]:
            raise AssetError("shader selection model census differs from the converted resource")
        result.append((name, raw, base, group, rows))
    return result


def observe_bindings(reference, files):
    return [
        {"resource": name, "groupOffset": group, "bindings": rows}
        for name, _, _, group, rows in resource_models(reference, files)
    ]


def observe_cache(reference, files, contract):
    """Read final PFTs and retain unresolved compiled-effect requirements."""
    groups, effects = [], {}
    material = reference["materialContract"]
    for resource in reference["resources"]:
        name = resource["resource"]["component"]
        _, raw = decode_resource(files[name], Limits().max_file_bytes)
        graph = json.loads(files[resource["resource"]["placement"]])
        raw = resource_page_layout.logical_view(raw, graph)  # paged (route B) -> logical
        at = unique(
            [r["offset"] for k, r in graph["blocks"].items() if k.endswith(".materials.shader-group")],
            "material parameter-cache serialized group",
        )
        pointer = struct.unpack_from("<Q", raw, at + contract["cachePointerOffset"])[0]
        count = struct.unpack_from("<H", raw, at + contract["cacheCountOffset"])[0]
        capacity = struct.unpack_from("<H", raw, at + contract["cacheCapacityOffset"])[0]
        if pointer or count or capacity:
            raise AssetError("material serialized group contains a nonempty runtime parameter cache")
        shaders = native_materials.read_group(raw, graph["sourceBase"], at, material)
        groups.append(
            {
                "resource": name,
                "groupOffset": at,
                "cachePointer": pointer,
                "cacheCount": count,
                "cacheCapacity": capacity,
                "shaderCount": len(shaders),
            }
        )
        for index, shader in enumerate(shaders):
            schema = shader["schema"]
            key = schema["nameHash"]
            requirement = {
                "nameHash": f"0x{key:08x}",
                "name": material["mappings"][str(key)]["name"],
                "serializedSchemaSha256": digest(native_materials.canonical(schema)),
                "serializedParameterCount": len(schema["parameters"]),
                "requiredRuntimeEvidence": [
                    "Compiled effect identity and parameter descriptors",
                    "Primary and secondary runtime parameter-name hashes and lookup ordering",
                ],
                "runtimeDefinitionProvided": False,
            }
            if key in effects and {k: v for k, v in effects[key].items() if k != "shaderSites"} != requirement:
                raise AssetError("material compiled-effect requirements disagree between shader sites")
            effects.setdefault(key, requirement | {"shaderSites": []})["shaderSites"].append(
                {"resource": name, "shaderIndex": index}
            )
    return {
        "groups": groups,
        "requiredEffects": [effects[k] for k in sorted(effects)],
        "runtimeEffectClosureQualified": False,
    }


def observe_materials(manifest, files):
    observations = []
    name_hash = joaat("diffusecolor2")
    for resource in manifest["resources"]:
        graph = json.loads(files[resource["materials"]["placement"]])
        payload_name = resource["resource"]["component"]
        _, payload = decode_resource(files[payload_name], Limits().max_file_bytes)
        final_graph = json.loads(files[resource["resource"]["placement"]])
        payload = resource_page_layout.logical_view(payload, final_graph)
        for shader in graph["shaders"]:
            selected = [p for p in shader["parameters"] if p["nativeNameHash"] == name_hash]
            if not selected:
                continue
            parameter = unique(selected, "Razor diffusecolor2 parameter")
            if parameter["kind"] != "CBuffer":
                raise AssetError("Razor diffusecolor2 is no longer a buffer parameter")
            from_source = parameter["sourceIndex"] is not None
            block = unique(
                [
                    value
                    for name, value in final_graph["blocks"].items()
                    if name.endswith(f".materials.shader-{shader['index']}")
                ],
                "final composed material shader block",
            )
            shader_at = block["offset"]
            decoded = native_materials.read_shader(
                payload, graph["sourceBase"], shader_at, manifest["materialContract"]
            )
            descriptor = unique(
                [p for p in decoded["schema"]["parameters"] if p["nameHash"] == name_hash],
                "serialized diffusecolor2 descriptor",
            )
            at = sum(decoded["schema"]["bufferSizes"][: descriptor["CBufferIndex"]]) + descriptor["ParamOffset"]
            expected = parameter["targetOffset"] if from_source else parameter["nativeParameter"]
            if descriptor["ParamLength"] != 16 or (at if from_source else descriptor) != expected:
                raise AssetError("serialized diffusecolor2 descriptor differs from its material graph")
            value = bytes.fromhex(decoded["bufferDataHex"])[at : at + descriptor["ParamLength"]]
            if from_source:
                if len(value) != 16 or value.hex() != parameter["encodedValueHex"]:
                    raise AssetError("serialized source diffusecolor2 differs from the copied source constant")
                observations.append(
                    {
                        "resource": payload_name,
                        "shaderIndex": shader["index"],
                        "shaderNameHash": f"0x{shader['nameHash']:08x}",
                        "shaderSystemOffset": shader_at,
                        "nativeParameter": descriptor,
                        "serializedValueHex": value.hex(),
                        "serializedDefaultQualified": False,
                        "initialization": "source-constant",
                    }
                )
                continue
            default = (
                manifest["materialSchemas"][str(shader["nameHash"])].get("compiledDefaults", {}).get(str(name_hash))
            )
            qualified = default is not None
            if len(value) != 16 or parameter["semanticDefaultQualified"] != qualified:
                raise AssetError("accepted diffusecolor2 default qualification differs")
            if qualified:
                if value.hex() != default["valueHex"] or parameter["compiledDefaultSha256"] != digest(
                    native_materials.canonical(default)
                ):
                    raise AssetError("compiled diffusecolor2 default readback differs")
            elif any(value):
                raise AssetError("accepted diffusecolor2 placeholder differs")
            observations.append(
                {
                    "resource": payload_name,
                    "shaderIndex": shader["index"],
                    "shaderNameHash": f"0x{shader['nameHash']:08x}",
                    "shaderSystemOffset": shader_at,
                    "nativeParameter": descriptor,
                    "serializedValueHex": value.hex(),
                    "serializedDefaultQualified": qualified,
                    "initialization": parameter["initialization"],
                }
            )
    return observations


def buffer_state(effect):
    sizes = {}
    for row in effect["constantDefaults"]:
        index, size = row["constantBufferIndex"], row["constantBufferBytes"]
        if index in sizes and sizes[index] != size:
            raise AssetError("constant remap compiled buffer sizes disagree")
        sizes[index] = size
    if sorted(sizes) != list(range(len(sizes))) or not 0 < len(sizes) <= 16:
        raise AssetError("constant remap buffer profile is incomplete")
    buffers = [bytearray(sizes[i]) for i in range(len(sizes))]
    claimed = set()
    for row in effect["constantDefaults"]:
        index, offset, length = row["constantBufferIndex"], row["constantOffset"], row["bytes"]
        span = {(index, i) for i in range(offset, offset + length)}
        value = bytes.fromhex(row["valueHex"])
        if len(value) != length or offset < 0 or offset + length > sizes[index] or span & claimed:
            raise AssetError("constant remap default values overlap or exceed buffers")
        claimed |= span
        buffers[index][offset : offset + length] = value
    return buffers


def plan(shader, effect):
    schema, data = shader["schema"], bytes.fromhex(shader["bufferDataHex"])
    buffers, rows, selected = buffer_state(effect), [], set()
    for index, parameter in enumerate(schema["parameters"]):
        if parameter["kind"] != "CBuffer":
            continue
        target = unique(
            [r for r in effect["constantDefaults"] if r["secondaryHash"] == parameter["nameHash"]],
            "constant remap alias",
        )
        if parameter["ParamLength"] != target["bytes"]:
            raise AssetError("constant remap would truncate or partially initialize a parameter")
        start = sum(schema["bufferSizes"][: parameter["CBufferIndex"]]) + parameter["ParamOffset"]
        value = data[start : start + target["bytes"]]
        if len(value) != target["bytes"] or target["secondaryHash"] in selected:
            raise AssetError("constant remap source extent or alias coverage differs")
        selected.add(target["secondaryHash"])
        buffers[target["constantBufferIndex"]][target["constantOffset"] : target["constantOffset"] + len(value)] = value
        rows.append(
            {
                "parameterIndex": index,
                "parameterHash": parameter["nameHash"],
                "sourceBufferIndex": parameter["CBufferIndex"],
                "sourceOffset": parameter["ParamOffset"],
                "targetBufferIndex": target["constantBufferIndex"],
                "targetOffset": target["constantOffset"],
                "bytes": len(value),
                "valueHex": value.hex(),
            }
        )
    return {
        "bindings": rows,
        "bufferSizes": [len(b) for b in buffers],
        "bufferSha256": [digest(bytes(b)) for b in buffers],
        "retainedDefaults": [
            {k: r[k] for k in ("secondaryHash", "secondaryName", "constantBufferIndex", "constantOffset", "valueHex")}
            for r in effect["constantDefaults"]
            if r["secondaryHash"] not in selected
        ],
    }, [bytes(b) for b in buffers]


def resource_shaders(manifest, files):
    effects = {e["name"]: e for e in manifest["shaderDatabaseObservations"]["requiredEffects"]}
    for resource in manifest["resources"]:
        name = resource["resource"]["component"]
        _, raw = decode_resource(files[name], Limits().max_file_bytes)
        graph = json.loads(files[resource["resource"]["placement"]])
        raw = resource_page_layout.logical_view(raw, graph)  # paged (route B) -> logical
        group = unique(
            [r["offset"] for k, r in graph["blocks"].items() if k.endswith(".materials.shader-group")],
            "constant remap final group",
        )
        for index, shader in enumerate(
            native_materials.read_group(raw, graph["sourceBase"], group, manifest["materialContract"])
        ):
            effect_name = manifest["materialContract"]["mappings"][str(shader["schema"]["nameHash"])]["name"]
            yield name, index, raw, graph["sourceBase"], shader, effects[effect_name]


def observe_remapping(manifest, files):
    return [
        {"resource": name, "shaderIndex": index, "effectName": effect["name"], **plan(shader, effect)[0]}
        for name, index, _, _, shader, effect in resource_shaders(manifest, files)
    ]

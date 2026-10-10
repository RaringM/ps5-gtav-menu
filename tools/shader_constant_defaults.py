"""Constant defaults read from verified compiled effects.

Only bounded bank bytes and the reviewed format projection are consumed.
No executable, private reports, renderer or shader programs are run.
"""

from __future__ import annotations

import hashlib
import json
import struct

from gtavmenu_tools import vehicle_contracts
from gtavmenu_tools.asset_formats import AssetError, Limits, decode_resource
from gtavmenu_tools.hashes import joaat
from shader_database import read_banks, unique


def decode(raw, parameters, database, contract):
    """Read defaults only for the active constant-buffer members already decoded."""
    layout = database["parameters"]

    def get(fmt, at):
        if at < 0 or at + struct.calcsize(fmt) > len(raw):
            raise AssetError("compiled default field exceeds parameter block")
        return struct.unpack_from(fmt, raw, at)[0]

    def text(at, relative):
        if not relative or not 0 <= at + relative < len(raw):
            raise AssetError("compiled default name pointer exceeds parameter block")
        start = at + relative
        end = raw.find(b"\0", start, min(start + 1024, len(raw)))
        if end < 0:
            raise AssetError("compiled default name is unterminated")
        try:
            return raw[start:end].decode("ascii")
        except UnicodeError as exc:
            raise AssetError("compiled default name is not ASCII") from exc

    buffers = []
    for i in range(get("<H", 0)):
        at = layout["headerBytes"] + i * layout["stride"]
        kind = (get("<B", at) >> layout["kindShift"]) & layout["kindMask"]
        if kind == layout["constantKind"] and get("<B", at + layout["activeOffset"]) & layout["activeMask"]:
            size = get("<H", at + layout["constantSizeOffset"])
            if size:
                buffers.append((i, size))
    values = []
    for parameter in parameters:
        if parameter["kind"] != layout["constantKind"]:
            continue
        at = parameter["descriptorOffset"]
        data_type = get("<B", at)
        if data_type >= len(contract["typeWidths"]):
            raise AssetError("compiled default type is outside native width table")
        count = get("<H", at + contract["arrayCountOffset"])
        length = contract["typeWidths"][data_type] * max(1, count)
        destination = get("<H", at + contract["destinationOffset"])
        buffer_index, buffer_size = unique(
            [(n, size) for n, (parent, size) in enumerate(buffers) if parent == parameter["parentIndex"]],
            "compiled default owning buffer",
        )
        if destination + length > buffer_size:
            raise AssetError("compiled default exceeds constant buffer")
        relative = get("<H", at + contract["defaultOffset"])
        if relative and at + relative + length > len(raw):
            raise AssetError("compiled default value exceeds parameter block")
        value = raw[at + relative : at + relative + length] if relative else bytes(length)
        primary = text(at, get("<H", at + contract["primaryNameOffset"]))
        secondary = text(at, get("<H", at + contract["secondaryNameOffset"]))
        if (joaat(primary), joaat(secondary)) != (parameter["primaryHash"], parameter["secondaryHash"]):
            raise AssetError("compiled default names disagree with descriptor hashes")
        values.append(
            parameter
            | {
                "primaryName": primary,
                "secondaryName": secondary,
                "dataType": data_type,
                "arrayCount": count,
                "constantBufferIndex": buffer_index,
                "constantBufferBytes": buffer_size,
                "constantOffset": destination,
                "bytes": length,
                "defaultDataOffset": at + relative if relative else None,
                "valueHex": value.hex(),
                "initialization": "compiled-default-bytes" if relative else "native-buffer-zero",
            }
        )
    return values


def attach(directory, catalog, bank, material, contract=None, *, manifest=None):
    contract = vehicle_contracts.load()["native"]["constantDefaults"] if contract is None else contract
    blobs = read_banks(directory, catalog, manifest=manifest)
    effects = {e["name"]: e for e in catalog["requiredEffects"]}
    for key, record in bank.items():
        effect = effects[material["mappings"][key]["name"]]
        section = effect["blocks"][4]
        raw = blobs[effect["bank"]][section["offset"] : section["offset"] + section["bytes"]]
        values = decode(raw, effect["parameters"], catalog["nativeContract"], contract)
        effect["constantDefaults"] = values
        selected = {}
        for parameter in record["schema"]["parameters"]:
            if parameter["kind"] != "CBuffer":
                continue
            value = unique(
                [v for v in values if v["secondaryHash"] == parameter["nameHash"]], "serialized constant default alias"
            )
            if value["bytes"] != parameter["ParamLength"]:
                raise AssetError("compiled default and serialized constant byte length differ")
            selected[str(parameter["nameHash"])] = value | {
                "effectName": effect["name"],
                "effectSha256": effect["sha256"],
                "bankSha256": hashlib.sha256(blobs[effect["bank"]]).hexdigest(),
                "serializedParameter": parameter,
                "runtimeBufferPlacementMatches": (value["constantOffset"], value["constantBufferIndex"])
                == (parameter["ParamOffset"], parameter["CBufferIndex"]),
            }
        record["compiledDefaults"] = selected
    catalog["constantDefaultContract"] = contract
    return contract


def observe(manifest, files):
    """Read introduced constants from final PFTs against their pinned defaults.

    This uses the serialized descriptor and all buffer copies, not the writer's
    recorded offset or buffer checksum. External compiled banks are not reread.
    """
    import gen9_materials as native_materials
    import resource_page_layout

    observations = []
    for resource in manifest["resources"]:
        name = resource["resource"]["component"]
        _, payload = decode_resource(files[name], Limits().max_file_bytes)
        graph = json.loads(files[resource["resource"]["placement"]])
        payload = resource_page_layout.logical_view(payload, graph)  # paged (route B) -> logical
        audit = json.loads(files[resource["materials"]["placement"]])
        group = unique(
            [r["offset"] for k, r in graph["blocks"].items() if k.endswith(".materials.shader-group")],
            "compiled default serialized group",
        )
        shaders = native_materials.read_group(payload, graph["sourceBase"], group, manifest["materialContract"])
        count = 0
        for index, (shader, written) in enumerate(zip(shaders, audit["shaders"], strict=True)):
            schema = shader["schema"]
            bank = manifest["materialSchemas"][str(schema["nameHash"])]
            if schema != bank["schema"] or written["nameHash"] != schema["nameHash"]:
                raise AssetError("compiled default final shader schema differs")
            values = bytes.fromhex(shader["bufferDataHex"])
            qualified = nonzero = 0
            for parameter in written["parameters"]:
                if parameter["sourceIndex"] is not None or parameter["kind"] != "CBuffer":
                    continue
                descriptor = unique(
                    [p for p in schema["parameters"] if p["nameHash"] == parameter["nativeNameHash"]],
                    "compiled default final parameter",
                )
                default = bank["compiledDefaults"][str(descriptor["nameHash"])]
                start = sum(schema["bufferSizes"][: descriptor["CBufferIndex"]]) + descriptor["ParamOffset"]
                value = values[start : start + descriptor["ParamLength"]]
                if (
                    descriptor != parameter["nativeParameter"]
                    or value.hex() != default["valueHex"]
                    or parameter["encodedValueHex"] != default["valueHex"]
                    or parameter["compiledDefaultSha256"]
                    != hashlib.sha256(native_materials.canonical(default)).hexdigest()
                    or parameter["initialization"] != default["initialization"]
                    or not parameter["semanticDefaultQualified"]
                ):
                    raise AssetError("compiled default final parameter readback differs")
                qualified += 1
                nonzero += any(value)
            count += qualified
            observations.append(
                {
                    "resource": name,
                    "shaderIndex": index,
                    "compiledDefaultParameters": qualified,
                    "nonzeroCompiledDefaults": nonzero,
                    "bufferSha256": hashlib.sha256(values).hexdigest(),
                }
            )
        if count != resource["materials"]["verification"]["compiledDefaultParameters"]:
            raise AssetError("compiled default final parameter coverage differs")
    return observations

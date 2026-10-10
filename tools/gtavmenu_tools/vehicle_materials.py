"""Bounded Legacy fragment material bindings using the published drawable layouts.

Only source-format reads and binding summaries live here. Native dictionary observations
come from verified user templates; shader parameter names come from the published mappings.
Unknown hashes stay unknown and never receive a guessed semantic mapping.
"""

from __future__ import annotations

import struct
from collections import Counter, defaultdict

from . import drawable_contracts
from .asset_formats import AssetError, Limits, read_rpf_member, rpf_members
from .asset_textures import SYSTEM_BASE, ResourceView
from .assets import Budget
from .hashes import joaat


def material_contract() -> dict:
    contracts = drawable_contracts.load()
    names: dict[int, set[str]] = defaultdict(set)
    for shader in contracts["shader"]["mappings"].values():
        for parameter in shader["parameters"].values():
            # Legacy spelling takes precedence over the renamed Gen9 parameter.
            name = parameter.get("old") or parameter.get("name")
            if name:
                names[joaat(name)].add(name)
    return contracts["material"] | {"parameterNames": {key: sorted(value) for key, value in names.items()}}


def _names(contract: dict, value: int) -> list[str]:
    result = contract.get("parameterNames", {}).get(value, [])
    if any(joaat(name) != value for name in result):
        raise AssetError("shader parameter name disagrees with its hash")
    return result


def selected_resource(archive: bytes, member_path: str, budget: Budget, suffix: str) -> bytes:
    parts = member_path.split("!/")
    if not parts or len(parts) > budget.limits.max_depth:
        raise AssetError("selected drawable resource exceeds nesting limit")
    current = archive
    for index, part in enumerate(parts):
        members = rpf_members(current, budget.limits)
        for _ in members:
            budget.entry()
        matches = [member for member in members if member.name == part]
        if len(matches) != 1:
            raise AssetError(f"selected member missing or ambiguous: {part}")
        member = matches[0]
        final = index == len(parts) - 1
        if final and (not member.resource or not part.lower().endswith(suffix)):
            raise AssetError(f"final member must be a {suffix.upper()} resource")
        if not final and (member.resource or not part.lower().endswith(".rpf")):
            raise AssetError("intermediate member must be an RPF container")
        budget.charge(member.size if member.resource else member.unpacked_size)
        current = read_rpf_member(current, member, budget.limits.max_file_bytes)
    return current


def _field(layout: dict, name: str) -> int:
    return layout["fields"][name]["offset"]


def _u8(data: memoryview, offset: int) -> int:
    return data[offset]


def _u16(data: memoryview, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: memoryview, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _u64(data: memoryview, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def _aligned(value: int, alignment: int, label: str) -> None:
    if value % alignment:
        raise AssetError(f"unaligned {label}")


def inspect_legacy_yft(payload: bytes, header: dict, contract: dict, limits: Limits) -> dict:
    if header["version"] != 162:
        raise AssetError("selected drawable reader requires Legacy YFT version 162")
    if header["graphicsBytes"] != 0:
        raise AssetError("selected Legacy YFT path supports system-only fixture resources")
    view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
    system = view.system

    root_pointer = SYSTEM_BASE
    root_offset = view.offset(root_pointer, contract["fragment"]["objectBytes"])
    drawable_pointer = _u64(system, root_offset + _field(contract["fragment"], "drawablePointer"))
    if not drawable_pointer:
        raise AssetError("fragment has no main drawable")
    _aligned(drawable_pointer, 16, "main drawable pointer")
    drawable_offset = view.offset(drawable_pointer, contract["drawable"]["fragmentObjectBytes"])
    shader_group_pointer = _u64(system, drawable_offset + _field(contract["drawable"], "shaderGroupPointer"))
    if not shader_group_pointer:
        raise AssetError("main drawable has no shader group")
    _aligned(shader_group_pointer, 16, "shader group pointer")
    group_offset = view.offset(shader_group_pointer, contract["shaderGroup"]["objectBytes"])
    shaders_pointer = _u64(system, group_offset + _field(contract["shaderGroup"], "shadersPointer"))
    count1 = _u16(system, group_offset + _field(contract["shaderGroup"], "shaderCount1"))
    count2 = _u16(system, group_offset + _field(contract["shaderGroup"], "shaderCount2"))
    if count1 != count2 or count1 > limits.max_entries:
        raise AssetError("shader group counts disagree or exceed inspection limit")
    if bool(shaders_pointer) != bool(count1):
        raise AssetError("shader pointer-array presence disagrees with count")
    _aligned(shaders_pointer, 8, "shader pointer array")
    pointer_offset = view.offset(shaders_pointer, count1 * 8)
    shader_pointers = [_u64(system, pointer_offset + index * 8) for index in range(count1)]
    if len(set(shader_pointers)) != len(shader_pointers) or any(not pointer for pointer in shader_pointers):
        raise AssetError("shader array has a null or duplicate object pointer")

    shaders = []
    for shader_index, shader_pointer in enumerate(shader_pointers):
        _aligned(shader_pointer, 16, "shader object pointer")
        shader_offset = view.offset(shader_pointer, contract["shader"]["objectBytes"])
        parameters_pointer = _u64(system, shader_offset + _field(contract["shader"], "parametersPointer"))
        parameter_count = _u8(system, shader_offset + _field(contract["shader"], "parameterCount"))
        parameter_size = _u16(system, shader_offset + _field(contract["shader"], "parameterSize"))
        parameter_data_size = _u16(system, shader_offset + _field(contract["shader"], "parameterDataSize"))
        texture_parameter_count = _u8(system, shader_offset + _field(contract["shader"], "textureParameterCount"))
        if bool(parameters_pointer) != bool(parameter_count):
            raise AssetError("shader parameter-block presence disagrees with count")
        _aligned(parameters_pointer, 16, "shader parameter block")
        parameter_offset = view.offset(parameters_pointer, parameter_count * contract["parameter"]["prefixBytes"])

        parameters = []
        extra_bytes = 0
        for parameter_index in range(parameter_count):
            offset = parameter_offset + parameter_index * contract["parameter"]["prefixBytes"]
            data_type = _u8(system, offset + _field(contract["parameter"], "dataType"))
            if data_type > 64:
                raise AssetError("shader parameter vector count exceeds selected bound")
            extra_bytes += data_type * 16
            if extra_bytes > limits.max_metadata_bytes:
                raise AssetError("shader parameter data exceeds metadata limit")
            parameters.append(
                {
                    "index": parameter_index,
                    "dataType": data_type,
                    "unknown1": _u8(system, offset + _field(contract["parameter"], "unknown1")),
                    "unknown2": _u16(system, offset + _field(contract["parameter"], "unknown2")),
                    "unknown4": _u32(system, offset + _field(contract["parameter"], "unknown4")),
                    "dataPointer": _u64(system, offset + _field(contract["parameter"], "dataPointer")),
                }
            )

        computed_parameter_size = parameter_count * 16 + extra_bytes
        computed_data_size = 32 + computed_parameter_size + parameter_count * 4
        computed_data_size = (computed_data_size + 15) & ~15
        if parameter_size != computed_parameter_size or parameter_data_size != computed_data_size:
            raise AssetError("shader parameter sizes disagree with pinned reader formula")
        hash_offset = parameter_offset + computed_parameter_size
        view.offset(SYSTEM_BASE + hash_offset, parameter_count * 4)

        embedded_start = parameter_offset + parameter_count * 16
        embedded_intervals = []
        texture_parameters = []
        for parameter in parameters:
            name_hash = _u32(system, hash_offset + parameter["index"] * 4)
            parameter["nameHash"] = f"0x{name_hash:08x}"
            parameter["nameCandidates"] = _names(contract, name_hash)
            pointer = parameter.pop("dataPointer")
            if parameter["dataType"]:
                if not pointer:
                    raise AssetError("non-texture shader parameter has a null data pointer")
                _aligned(pointer, 16, "shader vector-data pointer")
                start = view.offset(pointer, parameter["dataType"] * 16)
                embedded_intervals.append((start, start + parameter["dataType"] * 16))
                parameter["dataSystemOffset"] = start
                continue
            texture = {
                "index": parameter["index"],
                "nameHash": parameter["nameHash"],
                "nameCandidates": parameter["nameCandidates"],
                "unknown1": parameter["unknown1"],
                "dataPointer": f"0x{pointer:x}",
                "textureName": None,
                "textureNameHash": None,
                "textureSystemOffset": None,
            }
            if pointer:
                _aligned(pointer, 16, "texture parameter object")
                texture_offset = view.offset(pointer, contract["textureBase"]["objectBytes"])
                name_pointer = _u64(system, texture_offset + _field(contract["textureBase"], "namePointer"))
                if not name_pointer:
                    raise AssetError("texture parameter object has no name pointer")
                texture_name = view.name(name_pointer)
                texture.update(
                    textureName=texture_name,
                    textureNameHash=f"0x{joaat(texture_name):08x}",
                    textureSystemOffset=texture_offset,
                )
            texture_parameters.append(texture)

        embedded_intervals.sort()
        if extra_bytes:
            expected = embedded_start
            for start, end in embedded_intervals:
                if start != expected:
                    raise AssetError("shader vector-data blocks do not form the reader's embedded span")
                expected = end
            if expected != embedded_start + extra_bytes:
                raise AssetError("shader vector-data span length disagrees with parameter types")
        if texture_parameter_count != len(texture_parameters):
            raise AssetError("shader texture-parameter count disagrees with parameter types")

        shaders.append(
            {
                "index": shader_index,
                "systemOffset": shader_offset,
                "nameHash": f"0x{_u32(system, shader_offset + _field(contract['shader'], 'nameHash')):08x}",
                "fileNameHash": f"0x{_u32(system, shader_offset + _field(contract['shader'], 'fileNameHash')):08x}",
                "renderBucket": _u8(system, shader_offset + _field(contract["shader"], "renderBucket")),
                "renderBucketMask": f"0x{_u32(system, shader_offset + _field(contract['shader'], 'renderBucketMask')):08x}",
                "parametersSystemOffset": parameter_offset,
                "parameterCount": parameter_count,
                "parameterSize": parameter_size,
                "parameterDataSize": parameter_data_size,
                "textureParameterCount": texture_parameter_count,
                "parameters": parameters,
                "textureParameters": texture_parameters,
            }
        )

    return {
        "header": header,
        "root": {
            "systemOffset": root_offset,
            "fileVft": f"0x{_u32(system, root_offset + _field(contract['resourceFile'], 'fileVft')):08x}",
            "drawablePointer": f"0x{drawable_pointer:x}",
        },
        "drawable": {"systemOffset": drawable_offset, "shaderGroupPointer": f"0x{shader_group_pointer:x}"},
        "shaderGroup": {
            "systemOffset": group_offset,
            "textureDictionaryPointer": f"0x{_u64(system, group_offset + _field(contract['shaderGroup'], 'textureDictionaryPointer')):x}",
            "shadersPointer": f"0x{shaders_pointer:x}",
            "shaderCount": count1,
        },
        "shaders": shaders,
    }


def summarize(
    resources: list[dict],
    dictionary: dict,
    stock: dict | None = None,
    usage: dict | None = None,
) -> dict:
    local = {row["name"].lower(): row["name"] for row in dictionary["textures"]}
    bindings = defaultdict(list)
    external = Counter()
    roles = defaultdict(set)
    nulls = Counter()
    parameter_count = 0
    texture_parameter_count = 0
    shader_count = 0
    null_texture_binding_count = 0
    resolved_names = True
    for resource in resources:
        member = resource["member"]
        for shader in resource["shaders"]:
            shader_count += 1
            parameter_count += shader["parameterCount"]
            texture_parameter_count += shader["textureParameterCount"]
            for texture in shader["textureParameters"]:
                candidates = texture["nameCandidates"]
                resolved_names = resolved_names and bool(candidates)
                observation = {
                    "member": member,
                    "shaderIndex": shader["index"],
                    "shaderNameHash": shader["nameHash"],
                    "parameterIndex": texture["index"],
                    "parameterNameHash": texture["nameHash"],
                    "parameterNameCandidates": candidates,
                }
                name = texture["textureName"]
                if name is None:
                    null_texture_binding_count += 1
                    for candidate in candidates or [texture["nameHash"]]:
                        nulls[candidate] += 1
                    continue
                roles[name.lower()].update(candidates)
                key = name.lower()
                if key in local:
                    bindings[local[key]].append(observation)
                else:
                    external[name] += 1
    bound = []
    for texture in dictionary["textures"]:
        observations = bindings.get(texture["name"], [])
        if observations:
            bound.append(
                {
                    "name": texture["name"],
                    "nameHash": texture["nameHash"],
                    "bindingCount": len(observations),
                    "parameterNames": sorted(
                        {
                            candidate
                            for observation in observations
                            for candidate in observation["parameterNameCandidates"]
                        }
                    ),
                    "observations": observations,
                }
            )
    external_rows = [
        {"name": name, "bindingCount": count}
        for name, count in sorted(external.items(), key=lambda row: row[0].lower())
    ]
    stock_names = {name.lower(): name for name in stock["names"]} if stock else {}
    verified_stock = [
        row | {"stockName": stock_names[row["name"].lower()]}
        for row in external_rows
        if row["name"].lower() in stock_names
    ]
    unresolved_external = [row for row in external_rows if row["name"].lower() not in stock_names]
    usage_by_name = {row["name"].lower(): row for row in usage["rows"]} if usage else {}
    correlations = []
    for row in external_rows:
        observed = usage_by_name.get(row["name"].lower())
        if observed is not None:
            correlations.append(
                row
                | {
                    "parameterNames": sorted(roles[row["name"].lower()]),
                    "raw": observed["raw"],
                    "lowClassBits": observed["lowClassBits"],
                    "remainingFlagBits": observed["remainingFlagBits"],
                }
            )
    classes_by_parameter = defaultdict(set)
    values_by_parameter = defaultdict(set)
    for row in correlations:
        for name in row["parameterNames"]:
            classes_by_parameter[name].add(row["lowClassBits"])
            values_by_parameter[name].add(row["raw"])
    role_correlations = [
        {
            "parameterName": name,
            "observedLowClassBits": sorted(classes_by_parameter[name]),
            "observedRawValues": sorted(values_by_parameter[name]),
            "uniqueLowClassInSelectedStockBindings": len(classes_by_parameter[name]) == 1,
        }
        for name in sorted(classes_by_parameter)
    ]
    return {
        "resourceCount": len(resources),
        "shaderCount": shader_count,
        "parameterCount": parameter_count,
        "textureParameterCount": texture_parameter_count,
        "nonnullTextureBindingCount": sum(len(values) for values in bindings.values()) + sum(external.values()),
        "nullTextureBindingCount": null_texture_binding_count,
        "allTextureParameterNamesResolved": resolved_names,
        "packLocalTextureCount": len(local),
        "boundPackLocalTextureCount": len(bound),
        "boundPackLocalTextures": bound,
        "unboundPackLocalTextures": sorted(set(local.values()) - set(bindings)),
        "externalTextureBindings": external_rows,
        "verifiedStockTextureBindings": verified_stock,
        "unresolvedExternalTextureBindings": unresolved_external,
        "selectedStockTextureNameClosureVerified": stock is not None and not unresolved_external,
        "nativeStockUsageCorrelations": correlations,
        "samplerRoleNativeUsageObservations": role_correlations,
        "samplerRolesWithAmbiguousNativeClass": [
            row["parameterName"] for row in role_correlations if not row["uniqueLowClassInSelectedStockBindings"]
        ],
        "nativeUsageMappingValidated": False,
        "nullTextureParameters": [
            {"parameterName": name, "bindingCount": count} for name, count in sorted(nulls.items())
        ],
    }

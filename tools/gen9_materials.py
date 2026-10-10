"""Gen9 (PS5) shader groups and material parameter blocks, written and read back.

The build/read half of native_materials.py, split from the derivation of its contract: the field
encodings and the Legacy -> Gen9 parameter mappings arrive as data (`contract`: the `shader` entry of
the drawable contracts, frozen in data/drawable_contracts and loaded by gtavmenu_tools.drawable_contracts).
Native schemas come from the retail template drawables (schema_bank). Nothing here reads a reference
source, a game executable or a third-party package.
"""

from __future__ import annotations

import json
import math
import struct

from gen9_mesh import digest
from gtavmenu_tools.asset_formats import AssetError
from gtavmenu_tools.hashes import joaat
from native_resource_builder import ObjectArena


def canonical(value) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def hash_name(value: str) -> int:
    return int(value[5:], 16) if value.lower().startswith("hash_") else joaat(value.lower())


def get(data: bytes, at: int, layout: dict) -> dict:
    if at < 0 or at + layout["bytes"] > len(data):
        raise AssetError("shader structure exceeds system storage")
    return {
        k: int.from_bytes(data[at + v["offset"] : at + v["offset"] + v["bytes"]], "little")
        for k, v in layout["fields"].items()
    }


def put(data: bytearray, layout: dict, name: str, value: int):
    f = layout["fields"][name]
    if type(value) is not int or not 0 <= value < (1 << (8 * f["bytes"])):
        raise AssetError("shader integer would truncate")
    data[f["offset"] : f["offset"] + f["bytes"]] = value.to_bytes(f["bytes"], "little")


def span(system: bytes, base: int, pointer: int, size: int, alignment: int = 1) -> bytes:
    at = pointer - base
    if size < 0 or at < 0 or at % alignment or at + size > len(system):
        raise AssetError("shader reference span exceeds aligned system storage")
    return system[at : at + size]


def descriptor(name: int, word: int, contract: dict) -> dict:
    decoded = {key: (word >> f["shift"]) & f["mask"] for key, f in contract["bits"].items()}
    kind = next((key for key, code in contract["kinds"].items() if code == decoded["Type"]), None)
    if kind is None or kind == "Unknown":
        raise AssetError("shader unknown parameter kind requires implementation")
    keys = {
        "Texture": ("TextureIndex",),
        "Sampler": ("SamplerIndex",),
        "CBuffer": ("CBufferIndex", "ParamOffset", "ParamLength"),
    }[kind]
    fields = {key: decoded[key] for key in keys}
    result = {"nameHash": name, "kind": kind, **fields}
    if descriptor_word(result, contract) != word:
        raise AssetError("shader descriptor contains unaccounted bits")
    return result


def descriptor_word(row: dict, contract: dict) -> int:
    fields = {"Type": contract["kinds"][row["kind"]]} | {k: v for k, v in row.items() if k in contract["bits"]}
    result = 0
    for key, value in fields.items():
        bit = contract["bits"][key]
        if not 0 <= value <= bit["mask"]:
            raise AssetError("shader descriptor field overflows")
        result |= value << bit["shift"]
    return result


def read_shader(system: bytes, base: int, root: int, contract: dict) -> dict:
    h = get(system, root, contract["shader"])
    if h["G9_Preset"] != contract["preset"] or h["G9_Unknown_28h"] or h["G9_Unknown_30h"] or h["G9_Unknown_38h"]:
        raise AssetError("shader header exceeds inline finite profile")
    ih = get(system, h["G9_ParamInfosPointer"] - base, contract["info"])
    b, t, u, s, count, multi = (
        ih[k] for k in ("NumBuffers", "NumTextures", "NumUnknowns", "NumSamplers", "NumParams", "Unknown2")
    )
    if not 0 < b <= 16 or not 2 <= multi <= 32 or not 0 < count <= 255 or u:
        raise AssetError("shader parameter multiplicity/count/unknown buffers unsupported")
    raw = span(
        system, base, h["G9_ParamInfosPointer"] + contract["info"]["bytes"], count * contract["descriptorBytes"], 4
    )
    params = [descriptor(*v, contract) for v in struct.iter_unpack("<II", raw)]
    if len({r["nameHash"] for r in params}) != count:
        raise AssetError("shader parameter name hashes collide")
    ptrs = list(struct.unpack("<" + "Q" * (b * multi), span(system, base, h["ParametersPointer"], b * multi * 8, 8)))
    sizes = [ptrs[i + 1] - ptrs[i] for i in range(b)]
    if any(not 0 < n <= 8192 or n % 16 for n in sizes):
        raise AssetError("shader constant buffer sizes are unbounded or unaligned")
    ptrs_bytes, single_bytes = b * multi * 8, sum(sizes)
    expected, at = [], h["ParametersPointer"] + ptrs_bytes
    for _ in range(multi):
        for size in sizes:
            expected.append(at)
            at += size
    if ptrs != expected or at != h["G9_TextureRefsPointer"]:
        raise AssetError("shader buffer copy pointers disagree with source-derived layout")
    total = ptrs_bytes + single_bytes * multi + t * multi * 8 + s
    if h["ParameterDataSize"] != total or h["G9_UnknownParamsPointer"] not in (0, h["ParametersPointer"] + total - s):
        raise AssetError("shader parameter extent/unknown pointer disagrees with layout")
    data = span(system, base, h["ParametersPointer"], total, 8)
    copies = [data[ptrs_bytes + i * single_bytes : ptrs_bytes + (i + 1) * single_bytes] for i in range(multi)]
    if len(set(copies)) != 1:
        raise AssetError("shader constant buffer copies disagree")
    tex_start = ptrs_bytes + single_bytes * multi
    textures = list(struct.unpack("<" + "Q" * (t * multi), data[tex_start : tex_start + t * multi * 8]))
    if any(textures[t:]):
        raise AssetError("shader secondary texture copies are not zero in selected serialization profile")
    if sorted(p["TextureIndex"] for p in params if p["kind"] == "Texture") != list(range(t)) or sorted(
        p["SamplerIndex"] for p in params if p["kind"] == "Sampler"
    ) != list(range(s)):
        raise AssetError("shader texture/sampler indices are incomplete or duplicated")
    claimed = []
    for p in params:
        if p["kind"] != "CBuffer":
            continue
        bi, off, length = p["CBufferIndex"], p["ParamOffset"], p["ParamLength"]
        if (
            bi >= b
            or not length
            or off % 4
            or length % 4
            or off + length > sizes[bi]
            or any(old_b == bi and a < off + length and off < z for old_b, a, z in claimed)
        ):
            raise AssetError("shader constant field spans overlap or exceed buffer bounds")
        claimed.append((bi, off, off + length))
    schema = {
        "nameHash": h["Name"],
        "infoHeader": ih,
        "bufferSizes": sizes,
        "parameters": params,
        "samplersHex": data[-s:].hex() if s else "",
        "parameterBytes": total,
    }
    return {"header": h, "schema": schema, "bufferDataHex": copies[0].hex(), "texturePointers": textures[:t]}


def read_group(system: bytes, base: int, root: int, contract: dict) -> list:
    header = get(system, root, contract["group"])
    count = header["ShadersCount1"]
    if not 0 < count <= 256 or header["ShadersCount2"] != count:
        raise AssetError("shader group counts disagree or exceed limits")
    allowed = {"VFT", "Unknown_4h", "TextureDictionaryPointer", "ShadersPointer", "ShadersCount1", "ShadersCount2"}
    if any(v for k, v in header.items() if k not in allowed):
        raise AssetError("shader group reserved fields exceed serialized Gen9 profile")
    raw = span(system, base, header["ShadersPointer"], count * 8, 8)
    pointers = [v[0] for v in struct.iter_unpack("<Q", raw)]
    if len(set(pointers)) != count:
        raise AssetError("shader group has aliased shader entries")
    return [read_shader(system, base, p - base, contract) for p in pointers]


def schema_bank(references: list, supplementary: frozenset = frozenset()) -> dict:
    """Schemas by shader name hash from (identity, rows) references; references whose identity is in
    `supplementary` (SUPPLEMENTARY_REFERENCE_PINS) contribute only shader types no other reference has, and
    of those the first supplementary reference (in the given order) to carry a type supplies it: retail
    fragments carry different schema variants of one type (vehicle_basic in cheb_skirts and nemesis)."""
    bank = {}
    base = {
        str(row["schema"]["nameHash"]) for identity, rows in references if identity not in supplementary for row in rows
    }
    supplied = {}  # shader type -> the supplementary reference that supplies it
    for identity, rows in references:
        for index, row in enumerate(rows):
            schema = row["schema"]
            key = str(schema["nameHash"])
            if identity in supplementary and (key in base or supplied.setdefault(key, identity) != identity):
                continue
            if key in bank and bank[key]["schema"] != schema:
                raise AssetError("native shader schema differs across target instances")
            if key not in bank:
                bank[key] = {"schema": schema, "sources": [], "parameterValueEvidence": {}}
            source = {"resourceSha256": identity, "shaderIndex": index}
            bank[key]["sources"].append(source)
            for param in schema["parameters"]:
                if param["kind"] == "Sampler":
                    continue
                if param["kind"] == "Texture":
                    observation = {"nonnull": bool(row["texturePointers"][param["TextureIndex"]])}
                else:
                    at = sum(schema["bufferSizes"][: param["CBufferIndex"]]) + param["ParamOffset"]
                    data = bytes.fromhex(row["bufferDataHex"])[at : at + param["ParamLength"]]
                    observation = {"valueHex": data.hex(), "nonzero": any(data)}
                evidence_rows = bank[key]["parameterValueEvidence"].setdefault(str(param["nameHash"]), [])
                found = next((r for r in evidence_rows if r["observation"] == observation), None)
                if found is None:
                    found = {"observation": observation, "sources": []}
                    evidence_rows.append(found)
                found["sources"].append(source)
    return bank


# Source texture parameters the PS5 schemas do not have and whose job the engine does there: the PC
# reflect shaders' static EnvironmentSampler cubemap (gen9 takes the engine's reflection). Such a source
# parameter is dropped (audited as dropped, no texture link) instead of refusing the shader.
ENGINE_SUPPLIED_SOURCE_TEXTURES = frozenset({0xC5BBAE28})  # joaat("EnvironmentSampler")


# Source constants a PS5 schema may lack because they drive a PC-only path: useTessellation (DX11
# hull/domain shaders) is absent from the gen9 terrain_cb_w_4lyr_2tex_blend schema. Dropped and audited
# when the native schema has no such parameter (it is copied whenever the schema has one).
PC_ONLY_SOURCE_CONSTANTS = frozenset({0x4620A35D})  # joaat("useTessellation")


def pack_values(
    payload: bytes, shader: dict, schema: dict, contract: dict, defaults=None, texture_policies=None
) -> tuple[bytes, list, list]:
    mapping = contract["mappings"][str(schema["nameHash"])]
    target = {p["nameHash"]: p for p in schema["parameters"]}
    textures = {p["index"]: p for p in shader["textureParameters"]}
    data, audit, links, written = bytearray(sum(schema["bufferSizes"])), [], [], set()
    for source in shader["parameters"]:
        old = int(source["nameHash"], 0)
        name = mapping["renames"].get(str(old), old)
        if name not in target and source["dataType"] == 0 and old in ENGINE_SUPPLIED_SOURCE_TEXTURES:
            audit.append(
                {"sourceNameHash": old, "nativeNameHash": None, "sourceIndex": source["index"], "kind": "Texture"}
                | {"dropped": "no native parameter; engine-supplied on PS5"}
            )
            continue
        if name not in target and source["dataType"] != 0 and old in PC_ONLY_SOURCE_CONSTANTS:
            audit.append(
                {"sourceNameHash": old, "nativeNameHash": None, "sourceIndex": source["index"], "kind": "CBuffer"}
                | {"dropped": "no native parameter; PC-only constant"}
            )
            continue
        if name not in target or name in written:
            raise AssetError("source shader parameter has no unique native schema entry")
        written.add(name)
        native = target[name]
        row = {"sourceNameHash": old, "nativeNameHash": name, "sourceIndex": source["index"], "kind": native["kind"]}
        if source["dataType"] == 0:
            if native["kind"] != "Texture" or source["index"] not in textures:
                raise AssetError("source texture parameter differs from native kind")
            texture = textures[source["index"]]
            row.update(textureName=texture["textureName"], textureNameHash=texture["textureNameHash"])
            if texture["textureName"] is not None:
                links.append({"textureIndex": native["TextureIndex"], **row})
        else:
            if native["kind"] != "CBuffer":
                raise AssetError("source constant parameter differs from native kind")
            start, length = source["dataSystemOffset"], 16 * source["dataType"]
            if not 0 <= start <= start + length <= len(payload) or native["ParamLength"] > length:
                raise AssetError("source constant extent cannot cover native parameter")
            raw = payload[start : start + length]
            if any(not math.isfinite(v[0]) for v in struct.iter_unpack("<f", raw)):
                raise AssetError("source shader constant is not finite")
            dest = sum(schema["bufferSizes"][: native["CBufferIndex"]]) + native["ParamOffset"]
            copied = raw[: native["ParamLength"]]
            data[dest : dest + len(copied)] = copied
            row.update(
                sourceValueHex=raw.hex(),
                encodedValueHex=copied.hex(),
                unusedSourceTailHex=raw[len(copied) :].hex(),
                targetOffset=dest,
            )
        audit.append(row)
    # Source values take precedence. Absent constants may use freshly derived
    # compiled defaults; every remaining public-conversion zero/null stays open.
    introduced = [p for p in schema["parameters"] if p["nameHash"] not in written and p["kind"] != "Sampler"]
    for p in introduced:
        row = {
            "nativeNameHash": p["nameHash"],
            "kind": p["kind"],
            "sourceIndex": None,
            "initialization": "zero-or-null-from-pinned-public-conversion",
            "nativeParameter": p,
        }
        default = (defaults or {}).get(str(p["nameHash"]))
        if default is not None and p["kind"] == "CBuffer":
            value = bytes.fromhex(default["valueHex"])
            if len(value) != p["ParamLength"]:
                raise AssetError("compiled material default length differs")
            dest = sum(schema["bufferSizes"][: p["CBufferIndex"]]) + p["ParamOffset"]
            data[dest : dest + len(value)] = value
            row.update(
                initialization=default["initialization"],
                encodedValueHex=value.hex(),
                targetOffset=dest,
                compiledDefaultSha256=digest(canonical(default)),
            )
        policy = (texture_policies or {}).get(str(p["nameHash"]))
        if policy is not None and p["kind"] == "Texture":
            if policy["nativeParameter"] != p or policy["requiredSerializedPointer"] != 0:
                raise AssetError("introduced texture serialization policy differs")
            row.update(initialization=policy["action"], textureLoadPolicySha256=digest(canonical(policy)))
        audit.append(row)
    return bytes(data), audit, links


def build(payload: bytes, material: dict, bank: dict, contract: dict, base: int) -> tuple[bytes, dict]:
    if material["shaderGroup"]["textureDictionaryPointer"] != "0x0":
        raise AssetError("embedded source shader dictionary requires composition before material emission")
    arena, rows, external = ObjectArena(), [], []
    root = arena.add("shader-group", bytes(contract["group"]["bytes"]))
    shaders = material["shaders"]
    array = arena.add("shader-pointers", bytes(len(shaders) * 8))
    for key in ("ShadersCount1", "ShadersCount2"):
        put(arena.data, contract["group"], key, len(shaders))
    arena.pointer(contract["group"]["fields"]["ShadersPointer"]["offset"], array)
    arena.pointer(contract["group"]["fields"]["TextureDictionaryPointer"]["offset"], None)
    for index, shader in enumerate(shaders):
        name = int(shader["nameHash"], 0)
        if str(name) not in bank:
            raise AssetError(f"native shader schema missing for {shader['nameHash']}")
        schema = bank[str(name)]["schema"]
        buffer, audit, links = pack_values(
            payload,
            shader,
            schema,
            contract,
            bank[str(name)].get("compiledDefaults"),
            bank[str(name)].get("textureLoadPolicies"),
        )
        for parameter in audit:
            if parameter["sourceIndex"] is None:
                observations = bank[str(name)]["parameterValueEvidence"][str(parameter["nativeNameHash"])]
                parameter["nativeValueEvidenceSha256"] = digest(canonical(observations))
                parameter["nativeNonzeroObserved"] = any(
                    r["observation"].get("nonnull", r["observation"].get("nonzero")) for r in observations
                )
                parameter["semanticDefaultQualified"] = "compiledDefaultSha256" in parameter
                parameter["serializationPolicyQualified"] = (
                    parameter["semanticDefaultQualified"] or "textureLoadPolicySha256" in parameter
                )
        ih = schema["infoHeader"]
        b, t, multi = ih["NumBuffers"], ih["NumTextures"], ih["Unknown2"]
        tag = f"shader-{index}"
        data = bytearray(contract["shader"]["bytes"])
        for key, value in {
            "Name": name,
            "G9_Preset": contract["preset"],
            "RenderBucket": shader["renderBucket"],
            "RenderBucketMask": int(shader["renderBucketMask"], 0),
            "ParameterDataSize": schema["parameterBytes"],
        }.items():
            put(data, contract["shader"], key, value)
        at = arena.add(tag, data)
        info_bytes = bytearray(contract["info"]["bytes"])
        for key, value in ih.items():
            put(info_bytes, contract["info"], key, value)
        info_bytes.extend(
            b"".join(struct.pack("<II", p["nameHash"], descriptor_word(p, contract)) for p in schema["parameters"])
        )
        info_at = arena.add(tag + ".info", info_bytes)
        ptr_bytes = b * multi * 8
        tex_offset = ptr_bytes + len(buffer) * multi
        param = bytes(ptr_bytes) + buffer * multi + bytes(t * multi * 8) + bytes.fromhex(schema["samplersHex"])
        if len(param) != schema["parameterBytes"]:
            raise AssetError("written material extent disagrees with native schema")
        params_at = arena.add(tag + ".parameters", param)
        for repeat in range(multi):
            offset = 0
            for bi, size in enumerate(schema["bufferSizes"]):
                arena.pointer(params_at + (repeat * b + bi) * 8, params_at + ptr_bytes + repeat * len(buffer) + offset)
                offset += size
        for texture in range(t * multi):
            arena.pointer(params_at + tex_offset + texture * 8, None)
        for key, target in (
            ("ParametersPointer", params_at),
            ("G9_TextureRefsPointer", params_at + tex_offset),
            ("G9_ParamInfosPointer", info_at),
            ("G9_UnknownParamsPointer", None),
        ):
            arena.pointer(at + contract["shader"]["fields"][key]["offset"], target)
        arena.pointer(array + index * 8, at)
        for link in links:
            external.append({"offset": params_at + tex_offset + link["textureIndex"] * 8, "shaderIndex": index, **link})
        rows.append(
            {
                "index": index,
                "systemOffset": at,
                "nameHash": name,
                "sourceFileNameHash": shader["fileNameHash"],
                "renderBucket": shader["renderBucket"],
                "renderBucketMask": int(shader["renderBucketMask"], 0),
                "schemaSha256": digest(canonical(schema)),
                "bufferSha256": digest(buffer),
                "parameters": audit,
            }
        )
    blob, placement = arena.render(base)
    placement.update(
        rootOffset=root,
        shaders=rows,
        externalTextureLinks=external,
        textureDictionaryUncomposed=True,
        externalLinksResolved=False,
    )
    verify(blob, placement, bank, contract)
    return blob, placement


def verify(blob: bytes, placement: dict, bank: dict, contract: dict) -> dict:
    group = get(blob, placement["rootOffset"], contract["group"])
    if group["VFT"] or group["Unknown_4h"] or group["TextureDictionaryPointer"]:
        raise AssetError("material component class/dictionary link is outside unresolved profile")
    shaders = read_group(blob, placement["sourceBase"], placement["rootOffset"], contract)
    source_count, introduced, tail_nonzero, native_nonzero = 0, 0, 0, 0
    compiled_defaults, zero_or_null = 0, 0
    texture_policies, dropped = 0, 0
    expected_links = []
    for row, decoded in zip(placement["shaders"], shaders, strict=True):
        h, schema = decoded["header"], decoded["schema"]
        if (
            h["Name"] != row["nameHash"]
            or h["RenderBucket"] != row["renderBucket"]
            or h["RenderBucketMask"] != row["renderBucketMask"]
            or schema != bank[str(row["nameHash"])]["schema"]
            or digest(canonical(schema)) != row["schemaSha256"]
        ):
            raise AssetError("material readback identity/schema differs")
        data = bytes.fromhex(decoded["bufferDataHex"])
        if digest(data) != row["bufferSha256"] or any(decoded["texturePointers"]):
            raise AssetError("material parameter readback differs or symbolic texture link was modified")
        native = {p["nameHash"]: p for p in schema["parameters"]}
        for param in row["parameters"]:
            if param["sourceIndex"] is None:
                introduced += 1
                observations = bank[str(row["nameHash"])]["parameterValueEvidence"][str(param["nativeNameHash"])]
                if digest(canonical(observations)) != param["nativeValueEvidenceSha256"]:
                    raise AssetError("material default comparison evidence changed")
                nonzero = any(r["observation"].get("nonnull", r["observation"].get("nonzero")) for r in observations)
                default = bank[str(row["nameHash"])].get("compiledDefaults", {}).get(str(param["nativeNameHash"]))
                qualified = default is not None and param["kind"] == "CBuffer"
                policy = bank[str(row["nameHash"])].get("textureLoadPolicies", {}).get(str(param["nativeNameHash"]))
                policy_qualified = policy is not None and param["kind"] == "Texture"
                if param["serializationPolicyQualified"] != (qualified or policy_qualified):
                    raise AssetError("introduced parameter serialization qualification differs")
                if policy_qualified:
                    if (
                        param["textureLoadPolicySha256"] != digest(canonical(policy))
                        or param["initialization"] != policy["action"]
                        or param["nativeParameter"] != policy["nativeParameter"]
                    ):
                        raise AssetError("introduced texture serialization policy readback differs")
                    texture_policies += 1
                if nonzero != param["nativeNonzeroObserved"] or param["semanticDefaultQualified"] != qualified:
                    raise AssetError("material unresolved default qualification differs")
                definition = native[param["nativeNameHash"]]
                if param["kind"] == "CBuffer":
                    start = sum(schema["bufferSizes"][: definition["CBufferIndex"]]) + definition["ParamOffset"]
                    value = data[start : start + definition["ParamLength"]]
                    zero_or_null += not any(value)
                    if qualified:
                        if (
                            param["compiledDefaultSha256"] != digest(canonical(default))
                            or param["initialization"] != default["initialization"]
                            or param["encodedValueHex"] != default["valueHex"]
                            or value.hex() != default["valueHex"]
                            or param["targetOffset"] != start
                        ):
                            raise AssetError("compiled material default readback differs")
                        compiled_defaults += 1
                    elif any(value):
                        raise AssetError("unresolved material default contains nonzero bytes")
                else:
                    zero_or_null += 1
                native_nonzero += nonzero
                continue
            if param.get("dropped"):
                allowed = ENGINE_SUPPLIED_SOURCE_TEXTURES if param["kind"] == "Texture" else PC_ONLY_SOURCE_CONSTANTS
                if param["nativeNameHash"] is not None or param["sourceNameHash"] not in allowed:
                    raise AssetError("dropped material parameter is not an engine-supplied texture or PC-only constant")
                dropped += 1
                continue
            source_count += 1
            if param["kind"] == "CBuffer":
                start = param["targetOffset"]
                expected = bytes.fromhex(param["encodedValueHex"])
                if data[start : start + len(expected)] != expected:
                    raise AssetError("material source parameter value changed during packing")
                tail_nonzero += any(bytes.fromhex(param["unusedSourceTailHex"]))
            elif param["textureName"] is not None:
                target = native[param["nativeNameHash"]]
                expected_links.append(
                    (
                        row["index"],
                        h["G9_TextureRefsPointer"] - placement["sourceBase"] + target["TextureIndex"] * 8,
                        param["textureName"],
                        param["textureNameHash"],
                    )
                )
    links = [
        (r["shaderIndex"], r["offset"], r["textureName"], r["textureNameHash"])
        for r in placement["externalTextureLinks"]
    ]
    if links != expected_links:
        raise AssetError("material symbolic texture link coverage differs")
    return {
        "shaderCount": len(shaders),
        "sourceParametersAccountedFor": source_count,
        "droppedEngineSuppliedSourceTextures": dropped,
        "introducedParameters": introduced,
        "introducedZeroOrNullParameters": zero_or_null,
        "compiledDefaultParameters": compiled_defaults,
        "introducedTextureLoadPolicies": texture_policies,
        "unresolvedIntroducedParameters": introduced - compiled_defaults - texture_policies,
        "introducedParametersWithNonzeroNativeExamples": native_nonzero,
        "sourceParametersWithUnusedNonzeroTail": tail_nonzero,
        "unresolvedTextureLinks": len(links),
        "nativeCodeExecuted": False,
    }

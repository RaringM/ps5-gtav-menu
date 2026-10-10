"""Bounded Legacy YFT sampler bindings and optional child shader mappings.

Layout facts follow CodeWalker's Legacy Frag.cs, Drawable.cs and Texture.cs
at commit 485d56bec00262ed7fa472261cce7bbc6202b96e. No reference source or
native executable is needed at runtime. This is not a material converter:
sampler names do not establish PS5 texture roles or metadata values.
"""

from __future__ import annotations

import hashlib
import json
import math
import struct
from itertools import pairwise

from .asset_formats import AssetError, Limits, decode_resource, resource_header
from .asset_textures import SYSTEM_BASE, ResourceView
from .hashes import joaat

DEFAULT_LIMITS = Limits()
MAIN_MATERIAL_SCOPE = "main-drawable-shader-group"
CHILD_MATERIAL_SCOPE = "main-and-lod1-child-shader-mappings-v1"
SAMPLER_NAMES = {
    0xF1FE2B71: "DiffuseSampler",
    0x46B7C64F: "BumpSampler",
    0x608799C6: "SpecSampler",
}


class _Claims:
    """Account for selected metadata and reject partial or cross-kind aliases."""

    def __init__(self, maximum: int):
        self.maximum = maximum
        self.total = 0
        self.spans: set[tuple[int, int, str]] = set()

    def add(self, start: int, size: int, kind: str, *, shared: bool = False) -> None:
        if not size:
            return
        span = (start, start + size, kind)
        if span in self.spans:
            if shared:
                return
            raise AssetError(f"multiply referenced material {kind}")
        self.total += size
        if self.total > self.maximum:
            raise AssetError("selected material metadata exceeds metadata limit")
        self.spans.add(span)

    def validate(self) -> None:
        for left, right in pairwise(sorted(self.spans)):
            if left[1] > right[0]:
                raise AssetError(f"material structure ranges overlap: {left[2]} / {right[2]}")


def _read_lod1_child_mappings(
    view: ResourceView,
    claims: _Claims,
    main_drawable: int,
    main_group: int,
    shader_count: int,
    parameter_count: int,
    limits: Limits,
) -> dict:
    """Read only child/model identity and shader mappings, never mesh data.

    CodeWalker Frag.AssignChildrenShaders assigns the main drawable's group to
    child geometries. This first scope admits null-inherited or explicitly
    shared main groups, not independent child groups. Unique objects are cached;
    every output reference slot is still charged against a cumulative bound.
    """

    system = view.system

    def u64(offset: int) -> int:
        return struct.unpack_from("<Q", system, offset)[0]

    def own(pointer: int, size: int, alignment: int, kind: str) -> int:
        if not pointer or pointer % alignment:
            raise AssetError(f"missing or unaligned child material {kind} pointer")
        offset = view.offset(pointer, size)
        claims.add(offset, size, kind, shared=True)
        return offset

    entry_count = shader_count + parameter_count

    def charge(count: int) -> None:
        nonlocal entry_count
        entry_count += count
        if entry_count > limits.max_entries:
            raise AssetError("cumulative child material entries exceed entry limit")

    charge(0)
    result = {
        "physicsLod": 1,
        "physicsLodGroupPresent": bool(u64(0xF0)),
        "physicsLodPresent": False,
        "childCount": 0,
        "childObjectCount": 0,
        "drawableSlotCount": 0,
        "presentDrawableSlotCount": 0,
        "drawableCount": 0,
        # Number of model pointer-array slots across distinct drawables/LODs,
        # including nulls and repeats. Model objects themselves are deduplicated.
        "modelReferenceCount": 0,
        "modelCount": 0,
        "shaderMappingCount": 0,
        "drawableSlots": [],
        "drawables": [],
        "models": [],
        "otherPhysicsLodsPresent": [],
        "rootDrawableArrayPresent": bool(u64(0x38) or struct.unpack_from("<I", system, 0x48)[0]),
        "rootClothDrawablePresent": bool(u64(0xF8)),
        "allSelectedShaderIndicesValid": True,
        "geometryPayloadsValidated": False,
        "fullMaterialGraphCovered": False,
    }
    if not result["physicsLodGroupPresent"]:
        return result
    group = own(u64(0xF0), 48, 16, "physics LOD group")
    result["otherPhysicsLodsPresent"] = [index for index in (2, 3) if u64(group + 8 + index * 8)]
    lod_pointer = u64(group + 16)
    if not lod_pointer:
        return result
    lod = own(lod_pointer, 304, 16, "physics LOD1")
    result["physicsLodPresent"] = True
    count, duplicate_count = struct.unpack_from("<BB", system, lod + 0x11D)
    if count != duplicate_count:
        raise AssetError("physics LOD1 child counts disagree")
    charge(count * 3)  # Child array slots plus both drawable slots per child.
    table_pointer = u64(lod + 0xD0)
    if bool(table_pointer) != bool(count):
        raise AssetError("physics LOD1 child-array presence disagrees with count")
    table = own(table_pointer, count * 8, 8, "child pointer array") if count else 0
    result["childCount"] = count
    result["drawableSlotCount"] = count * 2
    child_objects: set[int] = set()
    drawable_cache: dict[int, int] = {}
    model_cache: dict[int, int] = {}

    def read_model(pointer: int) -> int | None:
        if not pointer:
            return None
        if pointer in model_cache:
            return model_cache[pointer]
        charge(1)
        model = own(pointer, 48, 16, "model header")
        geometry_count, duplicate_geometry_count = struct.unpack_from("<HH", system, model + 16)
        third_geometry_count = struct.unpack_from("<H", system, model + 46)[0]
        if geometry_count != duplicate_geometry_count or geometry_count != third_geometry_count:
            raise AssetError("child model geometry/shader-mapping counts disagree")
        charge(geometry_count)
        mapping_pointer, geometry_table_pointer = u64(model + 32), u64(model + 8)
        if bool(mapping_pointer) != bool(geometry_count) or bool(geometry_table_pointer) != bool(geometry_count):
            raise AssetError("child model mapping/geometry-array presence disagrees with count")
        mapping = own(mapping_pointer, geometry_count * 2, 2, "shader mapping") if geometry_count else 0
        geometry_table = (
            own(geometry_table_pointer, geometry_count * 8, 8, "geometry pointer array") if geometry_count else 0
        )
        rows = []
        for geometry_index in range(geometry_count):
            shader_index = struct.unpack_from("<H", system, mapping + geometry_index * 2)[0]
            if shader_index >= shader_count:
                raise AssetError("child shader index leaves the main shader group")
            geometry_pointer = u64(geometry_table + geometry_index * 8)
            if geometry_pointer:
                # Only establish a bounded, non-overlapping header span. Vertex,
                # index, bone and bound payloads are deliberately not traversed.
                own(geometry_pointer, 152, 16, "geometry header")
            rows.append(
                {
                    "geometryIndex": geometry_index,
                    "geometryPresent": bool(geometry_pointer),
                    "shaderIndex": shader_index,
                }
            )
        index = len(result["models"])
        model_cache[pointer] = index
        result["models"].append({"modelIndex": index, "geometryCount": geometry_count, "shaderMappings": rows})
        result["shaderMappingCount"] += geometry_count
        return index

    def read_drawable(pointer: int) -> int | None:
        if not pointer:
            return None
        if pointer in drawable_cache:
            return drawable_cache[pointer]
        charge(1)
        # An exact alias of the already-owned main drawable needs no new claim.
        drawable = main_drawable if pointer == SYSTEM_BASE + main_drawable else own(pointer, 336, 16, "child drawable")
        source_group = u64(drawable + 16)
        if source_group not in (0, SYSTEM_BASE + main_group):
            raise AssetError("independent child shader groups are outside the selected material scope")
        high_pointer = u64(drawable + 0x50)
        extra_pointer = u64(drawable + 0xA0)
        model_lods = []
        used_shaders: set[int] = set()
        lod_pointers = [
            ("high", high_pointer),
            ("medium", u64(drawable + 0x58)),
            ("low", u64(drawable + 0x60)),
            ("very-low", u64(drawable + 0x68)),
            ("extra", extra_pointer if extra_pointer != high_pointer else 0),
        ]
        for name, list_pointer in lod_pointers:
            charge(1)
            model_indices = []
            if list_pointer:
                header = own(list_pointer, 16, 16, "model-list header")
                pointer_array, model_count, capacity, reserved = struct.unpack_from("<QHHI", system, header)
                if model_count != capacity or reserved:
                    raise AssetError("child model-list counts or reserved word disagree with selected layout")
                charge(model_count)
                if bool(pointer_array) != bool(model_count):
                    raise AssetError("child model-list pointer-array presence disagrees with count")
                model_table = own(pointer_array, model_count * 8, 8, "model pointer array") if model_count else 0
                result["modelReferenceCount"] += model_count
                for model_slot in range(model_count):
                    index = read_model(u64(model_table + model_slot * 8))
                    model_indices.append(index)
                    if index is not None:
                        # Repeated references do not repeat stored model rows,
                        # but deriving each drawable's used set revisits them.
                        charge(result["models"][index]["geometryCount"])
                        used_shaders.update(
                            row["shaderIndex"]
                            for row in result["models"][index]["shaderMappings"]
                            if row["geometryPresent"]
                        )
            model_lods.append({"lod": name, "present": bool(list_pointer), "modelIndices": model_indices})
        index = len(result["drawables"])
        drawable_cache[pointer] = index
        result["drawables"].append(
            {
                "drawableIndex": index,
                "shaderGroupSource": "main-shared" if source_group else "main-inherited",
                "modelLods": model_lods,
                "usedShaderIndices": sorted(used_shaders),
            }
        )
        return index

    for child_index in range(count):
        child_pointer = u64(table + child_index * 8)
        child = own(child_pointer, 256, 16, "physics child") if child_pointer else None
        if child is not None:
            child_objects.add(child)
        for name, offset in (("drawable1", 0xA0), ("drawable2", 0xA8)):
            pointer = u64(child + offset) if child is not None else 0
            index = read_drawable(pointer)
            result["presentDrawableSlotCount"] += int(index is not None)
            result["drawableSlots"].append(
                {
                    "childIndex": child_index,
                    "childPresent": child is not None,
                    "drawableSlot": name,
                    "drawableIndex": index,
                }
            )
    result["childObjectCount"] = len(child_objects)
    result["drawableCount"] = len(result["drawables"])
    result["modelCount"] = len(result["models"])
    return result


def read_pc_material_bindings(
    blob: bytes, limits: Limits = DEFAULT_LIMITS, *, scope: str = MAIN_MATERIAL_SCOPE
) -> dict:
    """Validate every selected shader/parameter slot, retaining null samplers.

    Only version-162 system-only fragments are admitted. The default scope is
    their main drawable shader group. The explicit child scope additionally
    validates LOD1 child model shader mappings against that same group, without
    parsing mesh payloads. Embedded dictionaries and the full fragment graph
    remain outside both scopes. Shared texture objects or complete name strings
    are legal; shader objects and parameter blocks are owned.
    Every parameter is reported in either ``bindings`` (including nulls) or
    ``nonTextureParameters``. Unknown sampler hashes remain unknown.
    """

    if type(scope) is not str or scope not in (MAIN_MATERIAL_SCOPE, CHILD_MATERIAL_SCOPE):
        raise AssetError("unsupported material evidence scope")
    if type(blob) is not bytes or not 16 <= len(blob) <= limits.max_file_bytes:
        raise AssetError("source YFT is empty or exceeds the byte limit")
    header = resource_header(blob)
    if header["version"] != 162:
        raise AssetError("material reader requires Legacy YFT version 162")
    if header["graphicsBytes"]:
        raise AssetError("material reader supports system-only Legacy YFT resources")
    if len(blob) + header["systemBytes"] > limits.max_total_bytes:
        raise AssetError("source material resource exceeds total byte limit")
    header, payload = decode_resource(blob, limits.max_file_bytes)
    view = ResourceView(payload, header["systemBytes"], 0)
    system = view.system
    claims = _Claims(limits.max_metadata_bytes)

    def u64(offset: int) -> int:
        return struct.unpack_from("<Q", system, offset)[0]

    def u32(offset: int) -> int:
        return struct.unpack_from("<I", system, offset)[0]

    def own(pointer: int, size: int, alignment: int, kind: str, *, shared: bool = False) -> int:
        if not pointer or pointer % alignment:
            raise AssetError(f"missing or unaligned material {kind} pointer")
        offset = view.offset(pointer, size)
        claims.add(offset, size, kind, shared=shared)
        return offset

    own(SYSTEM_BASE, 304, 16, "fragment root")
    drawable = own(u64(48), 336, 16, "main drawable")
    group = own(u64(drawable + 16), 64, 16, "shader group")
    shader_count, capacity = struct.unpack_from("<HH", system, group + 24)
    if shader_count != capacity or shader_count > limits.max_entries:
        raise AssetError("shader group counts disagree or exceed entry limit")
    shader_table_pointer = u64(group + 16)
    if bool(shader_table_pointer) != bool(shader_count):
        raise AssetError("shader pointer-array presence disagrees with count")
    table = own(shader_table_pointer, shader_count * 8, 8, "shader pointer array") if shader_count else 0
    shader_pointers = [u64(table + index * 8) for index in range(shader_count)]
    if len(set(shader_pointers)) != shader_count:
        raise AssetError("shader pointer array contains duplicate objects")

    bindings: list[dict] = []
    nontextures: list[dict] = []
    shaders: list[dict] = []
    parameter_total = 0
    texture_cache: dict[int, tuple[str, str]] = {}
    for shader_index, pointer in enumerate(shader_pointers):
        shader = own(pointer, 48, 16, "shader object")
        parameter_pointer = u64(shader)
        count = system[shader + 16]
        parameter_size, data_size = struct.unpack_from("<HH", system, shader + 20)
        texture_count = system[shader + 39]
        parameter_total += count
        if parameter_total > limits.max_entries:
            raise AssetError("total material parameters exceed entry limit")
        if bool(parameter_pointer) != bool(count):
            raise AssetError("shader parameter-block presence disagrees with count")
        if not count:
            if (parameter_size, data_size, texture_count) != (0, 32, 0):
                raise AssetError("empty shader parameter sizes or texture count disagree")
            parameters = 0
        else:
            if parameter_pointer % 16:
                raise AssetError("unaligned material parameter block pointer")
            parameters = view.offset(parameter_pointer, count * 16)

        types = [system[parameters + index * 16] for index in range(count)]
        if any(data_type > 64 for data_type in types):
            raise AssetError("shader parameter vector count exceeds selected bound")
        vector_bytes = sum(types) * 16
        expected_size = count * 16 + vector_bytes
        expected_data_size = (32 + expected_size + count * 4 + 15) & ~15
        if (parameter_size, data_size) != (expected_size, expected_data_size):
            raise AssetError("shader parameter sizes disagree with Legacy layout")
        if texture_count != types.count(0):
            raise AssetError("shader texture-parameter count disagrees with parameter types")
        if count:
            own(parameter_pointer, data_size, 16, "parameter block")
        hash_offset = parameters + expected_size
        vector_start = parameters + count * 16
        vector_spans = []

        for parameter_index, data_type in enumerate(types):
            at = parameters + parameter_index * 16
            data_pointer = u64(at + 8)
            name_hash = u32(hash_offset + parameter_index * 4)
            identity = {
                "shaderIndex": shader_index,
                "parameterIndex": parameter_index,
                "parameterHash": f"0x{name_hash:08x}",
            }
            if data_type:
                if not data_pointer or data_pointer % 16:
                    raise AssetError("missing or unaligned shader vector-data pointer")
                start = view.offset(data_pointer, data_type * 16)
                stop = start + data_type * 16
                if start < vector_start or stop > vector_start + vector_bytes:
                    raise AssetError("shader vector data leaves its owned embedded span")
                vectors = system[start:stop]
                if any(not math.isfinite(value) for (value,) in struct.iter_unpack("<f", vectors)):
                    raise AssetError("shader parameter contains nonfinite vector data")
                vector_spans.append((start, stop))
                nontextures.append(
                    identity | {"vectorCount": data_type, "vectorDataSha256": hashlib.sha256(vectors).hexdigest()}
                )
                continue

            name = name_digest = None
            if data_pointer:
                texture = own(data_pointer, 80, 16, "texture object", shared=True)
                if texture not in texture_cache:
                    name_pointer = u64(texture + 40)
                    name = view.name(name_pointer)
                    name_offset = view.offset(name_pointer, len(name) + 1)
                    claims.add(name_offset, len(name) + 1, "texture name", shared=True)
                    texture_cache[texture] = (name, f"0x{joaat(name):08x}")
                name, name_digest = texture_cache[texture]
            bindings.append(
                {
                    "textureName": name,
                    "textureNameHash": name_digest,
                    "shaderIndex": shader_index,
                    "parameterIndex": parameter_index,
                    "samplerHash": f"0x{name_hash:08x}",
                    "samplerName": SAMPLER_NAMES.get(name_hash),
                }
            )

        cursor = vector_start
        for start, stop in sorted(vector_spans):
            if start != cursor:
                raise AssetError("shader vector-data blocks overlap or leave an embedded gap")
            cursor = stop
        if cursor != vector_start + vector_bytes:
            raise AssetError("shader vector-data span disagrees with parameter types")
        shaders.append(
            {
                "shaderIndex": shader_index,
                "nameHash": f"0x{u32(shader + 8):08x}",
                "fileNameHash": f"0x{u32(shader + 24):08x}",
                "parameterCount": count,
                "textureParameterCount": texture_count,
            }
        )

    children = None
    if scope == CHILD_MATERIAL_SCOPE:
        children = _read_lod1_child_mappings(view, claims, drawable, group, shader_count, parameter_total, limits)
    claims.validate()
    result = {
        "sourceSha256": hashlib.sha256(blob).hexdigest(),
        "header": header,
        "scope": scope,
        "shaderCount": shader_count,
        "parameterCount": parameter_total,
        "textureParameterCount": len(bindings),
        "nonTextureParameterCount": len(nontextures),
        "bindings": bindings,
        "nonTextureParameters": nontextures,
        "unknownSamplerParameters": [row for row in bindings if row["samplerName"] is None],
        "unboundTextureParameters": [row for row in bindings if row["textureName"] is None],
        "shaders": shaders,
        "selectedMetadataBytes": claims.total,
        "embeddedTextureDictionaryPresent": bool(u64(group + 8)),
        "fullMaterialGraphCovered": False,
        "ps5MetadataMappingQualified": False,
    }
    if children is not None:
        result["childShaderMappings"] = children
        # Count bounded ASCII tokens without first allocating a full report.
        # Reserve three compact-JSON equivalents for retained output and later
        # encoder text/bytes, not a Python heap estimate. All strings and the
        # fixed-depth structure above have already been bounded independently.
        retained_bytes = len(blob) + header["systemBytes"]
        report_bytes = 0
        for token in json.JSONEncoder(ensure_ascii=True, separators=(",", ":")).iterencode(result):
            report_bytes += len(token)
            if report_bytes > limits.max_metadata_bytes:
                raise AssetError("child material report exceeds metadata byte limit")
            if retained_bytes + 3 * report_bytes > limits.max_total_bytes:
                raise AssetError("child material report copies exceed total byte limit")
    return result

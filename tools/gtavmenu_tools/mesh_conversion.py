"""Reversible mesh components, not complete PS5 drawable/fragment resources.

The 320-byte declaration and vertex permutation follow pinned public CodeWalker
Drawable.cs (485d56bec00262ed7fa472261cce7bbc6202b96e), independently read back
without writer placement records. Indices remain uint16 triangle lists. Material
objects, resource roots/pages, skeletons, native views and GPU admission are not
invented here. Callers receive real transformed buffers and retained source IR.
"""

from __future__ import annotations

import hashlib
import math
import struct
from dataclasses import dataclass

from .asset_formats import AssetError, Limits, resource_header
from .asset_geometry import (
    GTAV1_COMPONENTS,
    GTAV1_TYPES,
    MAX_FLOAT_SCALARS,
    MESH_LIMITS,
    YFT_MAIN_MESH_SCOPE,
    PcDrawableMeshes,
    legacy_attributes,
    read_pc_meshes,
)

MESH_LAYOUT_POLICY = "public-gen9-gtav1-static-components-v1"
_GEN9_SLOTS = {0: 0, 4: 3, 8: 14, 24: 4, 25: 5, **{28 + index: 6 + index for index in range(6)}}
_GEN9_FORMATS = {0: 6, 3: 6, 14: 2, 4: 28, 5: 28, **dict.fromkeys(range(6, 12), 16)}
_DECLARATION_BYTES = 320


@dataclass(frozen=True)
class ConvertedGeometry:
    source_offset: int
    shader_index: int
    vertex_count: int
    vertex_stride: int
    declaration: bytes
    vertices: bytes
    indices: bytes


@dataclass(frozen=True)
class ConvertedModel:
    source_offset: int
    source_header: bytes
    bounds: bytes
    shader_mapping: bytes
    geometries: tuple[ConvertedGeometry, ...]


@dataclass(frozen=True)
class ConvertedLod:
    name: str
    source_header: bytes | None
    models: tuple[ConvertedModel, ...]


@dataclass(frozen=True)
class MeshConversion:
    source: PcDrawableMeshes
    lods: tuple[ConvertedLod, ...]
    layout_policy: str = MESH_LAYOUT_POLICY


def _vertex_extent(vertices: bytes, stride: int, limits: Limits) -> int:
    if type(vertices) is not bytes or not vertices or len(vertices) > limits.max_file_bytes:
        raise AssetError("mesh vertex bytes are empty or exceed the file byte limit")
    if not 1 <= stride <= 255 or len(vertices) % stride or len(vertices) // stride > 65535:
        raise AssetError("mesh vertex extent/count disagrees with the declaration")
    if 3 * len(vertices) + 3 * _DECLARATION_BYTES > limits.max_total_bytes:
        raise AssetError("mesh vertex transformation exceeds the cumulative byte budget")
    return len(vertices) // stride


def _finite_vertices(vertices: bytes, declaration: bytes) -> None:
    attributes = legacy_attributes(declaration)
    stride = struct.unpack_from("<H", declaration, 4)[0]
    count = len(vertices) // stride
    if count * sum(attribute.float_scalars for attribute in attributes) > MAX_FLOAT_SCALARS:
        raise AssetError("mesh float scalar work limit exceeded")
    for attribute in attributes:
        if attribute.float_scalars:
            form = struct.Struct("<" + "f" * attribute.float_scalars)
            for vertex in range(count):
                if any(
                    not math.isfinite(value) for value in form.unpack_from(vertices, vertex * stride + attribute.offset)
                ):
                    raise AssetError("mesh vertex transformation contains non-finite float attributes")


def encode_gen9_vertices(vertices: bytes, declaration: bytes, limits: Limits = MESH_LIMITS) -> tuple[bytes, bytes]:
    """Return an actual public-Gen9 declaration and permuted vertex bytes."""

    attributes = legacy_attributes(declaration)
    stride = struct.unpack_from("<H", declaration, 4)[0]
    count = _vertex_extent(vertices, stride, limits)
    _finite_vertices(vertices, declaration)
    source = {attribute.component: attribute for attribute in attributes}
    target_declaration = bytearray(_DECLARATION_BYTES)
    output = bytearray(len(vertices))
    target_offset = 0
    for slot in range(52):
        struct.pack_into("<I", target_declaration, slot * 4, target_offset)
        component = _GEN9_SLOTS.get(slot)
        attribute = source.get(component)
        if attribute is None:
            continue
        target_declaration[208 + slot] = stride
        target_declaration[260 + slot] = _GEN9_FORMATS[component]
        for vertex in range(count):
            start = vertex * stride + attribute.offset
            destination = vertex * stride + target_offset
            output[destination : destination + attribute.byte_width] = vertices[start : start + attribute.byte_width]
        target_offset += attribute.byte_width
    if target_offset != stride:
        raise AssetError("mesh conversion would omit a source attribute")
    struct.pack_into("<Q", target_declaration, 312, stride << 2)
    return bytes(target_declaration), bytes(output)


def decode_gen9_vertices(declaration: bytes, vertices: bytes, limits: Limits = MESH_LIMITS) -> tuple[bytes, bytes]:
    """Read serialized declaration fields afresh and reconstruct Legacy bytes.

    Does not consume encoder copy maps or a source declaration. All active and
    inactive offsets, format codes, steps and the mode/count word are checked.
    """

    if type(declaration) is not bytes or len(declaration) != _DECLARATION_BYTES:
        raise AssetError("Gen9 mesh declaration must contain exactly 320 bytes")
    mode = struct.unpack_from("<Q", declaration, 312)[0]
    stride = (mode >> 2) & 255
    if mode != stride << 2:
        raise AssetError("Gen9 mesh declaration mode/count word is outside the supported profile")
    count = _vertex_extent(vertices, stride, limits)
    source_offsets, flags, cursor = {}, 0, 0
    for slot in range(52):
        offset = struct.unpack_from("<I", declaration, slot * 4)[0]
        step, format_code = declaration[208 + slot], declaration[260 + slot]
        if offset != cursor:
            raise AssetError("Gen9 mesh declaration has a noncanonical offset or attribute gap")
        if not format_code:
            if step:
                raise AssetError("inactive Gen9 mesh attribute has a nonzero step")
            continue
        component = _GEN9_SLOTS.get(slot)
        if component is None or format_code != _GEN9_FORMATS[component] or step != stride:
            raise AssetError("Gen9 mesh attribute format/step is unsupported")
        width = GTAV1_COMPONENTS[component][1]
        if cursor + width > stride:
            raise AssetError("Gen9 mesh attribute leaves its vertex stride")
        source_offsets[component] = offset
        flags |= 1 << component
        cursor += width
    if cursor != stride or not flags & 1:
        raise AssetError("Gen9 mesh attributes do not cover a positioned vertex")
    legacy = struct.pack("<IHBBQ", flags, stride, 0, flags.bit_count(), GTAV1_TYPES)
    output, destination = bytearray(len(vertices)), 0
    for component in sorted(source_offsets):
        width = GTAV1_COMPONENTS[component][1]
        for vertex in range(count):
            start = vertex * stride + source_offsets[component]
            end = vertex * stride + destination
            output[end : end + width] = vertices[start : start + width]
        destination += width
    result = bytes(output)
    _finite_vertices(result, legacy)
    return legacy, result


def _pipeline_budget(blob: bytes, limits: Limits) -> None:
    if type(blob) is not bytes or not 16 <= len(blob) <= limits.max_file_bytes:
        raise AssetError("source mesh resource is empty or exceeds the file byte limit")
    header = resource_header(blob)
    # Conservative payload-byte reservation: input, source decode/IR, target,
    # independent decode/IR, vertex readback and an active encoder copy. This
    # is not a claim to predict Python object allocator overhead.
    if len(blob) + 6 * (header["systemBytes"] + header["graphicsBytes"]) > limits.max_total_bytes:
        raise AssetError("mesh conversion/readback exceeds the cumulative byte budget")


def convert_pc_meshes(blob: bytes, limits: Limits = MESH_LIMITS, *, scope: str = YFT_MAIN_MESH_SCOPE) -> MeshConversion:
    """Transform the complete selected mesh set, or return no component."""

    _pipeline_budget(blob, limits)
    source = read_pc_meshes(blob, limits, scope=scope)
    lods = []
    for lod in source.lods:
        models = []
        for model in lod.models:
            geometries = []
            for geometry in model.geometries:
                declaration, vertices = encode_gen9_vertices(geometry.vertices, geometry.declaration, limits)
                geometries.append(
                    ConvertedGeometry(
                        geometry.source_offset,
                        geometry.shader_index,
                        geometry.vertex_count,
                        geometry.vertex_stride,
                        declaration,
                        vertices,
                        geometry.indices,
                    )
                )
            models.append(
                ConvertedModel(model.source_offset, model.header, model.bounds, model.shader_mapping, tuple(geometries))
            )
        lods.append(ConvertedLod(lod.name, lod.header, tuple(models)))
    result = MeshConversion(source, tuple(lods))
    _verify(source, result, limits)
    return result


def _verify(source: PcDrawableMeshes, candidate: MeshConversion, limits: Limits) -> dict:
    if (
        type(candidate) is not MeshConversion
        or type(candidate.layout_policy) is not str
        or candidate.layout_policy != MESH_LAYOUT_POLICY
    ):
        raise AssetError("unsupported mesh component or layout policy")
    if type(candidate.source) is not PcDrawableMeshes or candidate.source != source:
        raise AssetError("mesh component source identity/evidence differs from the original")
    if type(candidate.lods) is not tuple or len(candidate.lods) != len(source.lods):
        raise AssetError("mesh component LOD coverage differs from the original")
    rows, vertex_bytes, index_bytes = [], 0, 0
    for original_lod, lod in zip(source.lods, candidate.lods, strict=True):
        if (
            type(lod) is not ConvertedLod
            or type(lod.name) is not str
            or type(lod.source_header) is not type(original_lod.header)
            or (lod.name, lod.source_header) != (original_lod.name, original_lod.header)
            or type(lod.models) is not tuple
            or len(lod.models) != len(original_lod.models)
        ):
            raise AssetError("mesh component LOD/model coverage or source header differs")
        for model_index, (original_model, model) in enumerate(zip(original_lod.models, lod.models, strict=True)):
            if (
                type(model) is not ConvertedModel
                or any(type(value) is not bytes for value in (model.source_header, model.bounds, model.shader_mapping))
                or (model.source_offset, model.source_header, model.bounds, model.shader_mapping)
                != (
                    original_model.source_offset,
                    original_model.header,
                    original_model.bounds,
                    original_model.shader_mapping,
                )
                or type(model.geometries) is not tuple
                or len(model.geometries) != len(original_model.geometries)
            ):
                raise AssetError("mesh component model bounds, shader mapping or geometry coverage differs")
            for geometry_index, (original, geometry) in enumerate(
                zip(original_model.geometries, model.geometries, strict=True)
            ):
                if (
                    type(geometry) is not ConvertedGeometry
                    or any(
                        type(value) is not int
                        for value in (
                            geometry.source_offset,
                            geometry.shader_index,
                            geometry.vertex_count,
                            geometry.vertex_stride,
                        )
                    )
                    or (geometry.source_offset, geometry.shader_index, geometry.vertex_count, geometry.vertex_stride)
                    != (original.source_offset, original.shader_index, original.vertex_count, original.vertex_stride)
                    or type(geometry.indices) is not bytes
                    or geometry.indices != original.indices
                    or type(geometry.vertices) is not bytes
                    or len(geometry.vertices) != len(original.vertices)
                ):
                    raise AssetError("mesh component geometry identity, shader index, counts or indices differ")
                declaration, vertices = decode_gen9_vertices(geometry.declaration, geometry.vertices, limits)
                if declaration != original.declaration or vertices != original.vertices:
                    raise AssetError("mesh readback differs from original vertex attributes/declaration")
                vertex_bytes += len(vertices)
                index_bytes += len(geometry.indices)
                rows.append(
                    {
                        "lod": lod.name,
                        "modelIndex": model_index,
                        "geometryIndex": geometry_index,
                        "shaderIndex": geometry.shader_index,
                        "vertexCount": geometry.vertex_count,
                        "indexCount": len(geometry.indices) // 2,
                        "sourceVertexSha256": hashlib.sha256(vertices).hexdigest(),
                        "convertedVertexSha256": hashlib.sha256(geometry.vertices).hexdigest(),
                        "indexSha256": hashlib.sha256(geometry.indices).hexdigest(),
                    }
                )
    return {
        "schemaVersion": 1,
        "kind": "gtavmenu-experimental-mesh-components",
        "scope": source.scope,
        "layoutPolicy": MESH_LAYOUT_POLICY,
        "sourceSha256": source.source_sha256,
        "geometryCount": len(rows),
        "vertexBytes": vertex_bytes,
        "indexBytes": index_bytes,
        "geometries": rows,
        "qualification": {
            "allSelectedMeshAttributeAndIndexBytesRecovered": True,
            "allSelectedBoundsAndShaderIndicesPreserved": True,
            "completeSourceResourceConverted": False,
            "standaloneResourceEmitted": False,
            "materialSemanticsQualified": False,
            "nativeGpuLayoutValidated": False,
            "runtimeQualified": False,
            "releaseReady": False,
        },
    }


def verify_mesh_conversion(blob: bytes, candidate: MeshConversion, limits: Limits = MESH_LIMITS) -> dict:
    """Reparse original bytes and independently read every transformed buffer."""

    _pipeline_budget(blob, limits)
    source = read_pc_meshes(blob, limits)
    return _verify(source, candidate, limits)

"""Bounded, explicitly selected PC Legacy main-drawable mesh intake.

Layout facts: CodeWalker Drawable.cs, Frag.cs and VertexType.cs at
485d56bec00262ed7fa472261cce7bbc6202b96e. This reads the four main-drawable
LOD lists of a system-only v162 YFT, not its complete material/fragment graph.
No child drawable, skeleton, physics, material or standalone YDR conversion is
implied. Unsupported selected meshes reject the complete selected mesh set.
"""

from __future__ import annotations

import hashlib
import math
import struct
from bisect import bisect_left
from dataclasses import dataclass

from .asset_formats import AssetError, Limits, decode_resource, resource_header
from .asset_textures import SYSTEM_BASE, ResourceView

YFT_MAIN_MESH_SCOPE = "legacy-yft-main-static-meshes-v1"
GTAV1_TYPES = 0x7755555555996996
MESH_LIMITS = Limits(max_file_bytes=64 << 20, max_total_bytes=256 << 20, max_entries=4096)
MAX_FLOAT_SCALARS = 8 * 1024 * 1024
LOD_NAMES = ("high", "medium", "low", "very-low")

# Legacy component index -> (name, byte width, number of float32 scalars).
# Blend data and the unmapped UV6/UV7/binormal slots are deliberately absent.
GTAV1_COMPONENTS = {
    0: ("Position", 12, 3),
    3: ("Normal", 12, 3),
    4: ("Colour0", 4, 0),
    5: ("Colour1", 4, 0),
    **{index: (f"TexCoord{index - 6}", 8, 2) for index in range(6, 12)},
    14: ("Tangent", 16, 4),
}
_SUPPORTED_FLAGS = sum(1 << index for index in GTAV1_COMPONENTS)


@dataclass(frozen=True)
class MeshAttribute:
    component: int
    name: str
    offset: int
    byte_width: int
    float_scalars: int


@dataclass(frozen=True)
class PcMeshGeometry:
    source_offset: int
    shader_index: int
    vertex_count: int
    vertex_stride: int
    attributes: tuple[MeshAttribute, ...]
    vertices: bytes
    indices: bytes
    geometry_header: bytes
    vertex_buffer_header: bytes
    index_buffer_header: bytes
    declaration: bytes


@dataclass(frozen=True)
class PcMeshModel:
    source_offset: int
    header: bytes
    # Includes the model-wide box when more than one geometry is present.
    bounds: bytes
    shader_mapping: bytes
    geometries: tuple[PcMeshGeometry, ...]


@dataclass(frozen=True)
class PcMeshLod:
    name: str
    header: bytes | None
    models: tuple[PcMeshModel, ...]


@dataclass(frozen=True)
class PcDrawableMeshes:
    source_sha256: str
    source_bytes: int
    resource_header: bytes
    scope: str
    fragment_header: bytes
    drawable_offset: int
    drawable_header: bytes
    shader_group_header: bytes
    # Original shader headers provide source identity, not writable materials.
    shader_headers: tuple[bytes, ...]
    lods: tuple[PcMeshLod, ...]


def legacy_attributes(declaration: bytes) -> tuple[MeshAttribute, ...]:
    """Decode an exact unskinned GTAV1 declaration; never omit enabled slots."""

    if type(declaration) is not bytes or len(declaration) != 16:
        raise AssetError("mesh declaration must contain exactly 16 bytes")
    flags, stride, unknown, count, types = struct.unpack("<IHBBQ", declaration)
    if types != GTAV1_TYPES or unknown or not flags & 1 or flags & ~_SUPPORTED_FLAGS:
        raise AssetError("mesh declaration is outside the unskinned GTAV1 component subset")
    if not 0 < stride <= 255 or count != flags.bit_count():
        raise AssetError("mesh declaration stride/count is invalid")
    attributes, offset = [], 0
    for component, (name, width, floats) in GTAV1_COMPONENTS.items():
        if flags & (1 << component):
            attributes.append(MeshAttribute(component, name, offset, width, floats))
            offset += width
    if offset != stride:
        raise AssetError("mesh declaration attributes do not cover the exact vertex stride")
    return tuple(attributes)


class _MeshReader:
    def __init__(self, payload: bytes, limits: Limits):
        self.view = ResourceView(payload, len(payload), 0)
        self.limits = limits
        self.spans: list[tuple[int, int, str]] = []
        self.metadata_bytes = 0
        self.data_bytes = 0
        self.entries = 0
        self.float_scalars = 0

    def claim(self, pointer: int, size: int, alignment: int, label: str, *, data: bool = False) -> int:
        if not pointer or pointer % alignment or not 0 < size <= self.limits.max_file_bytes:
            raise AssetError(f"mesh {label} pointer, alignment or size is invalid")
        offset = self.view.offset(pointer, size)
        # Reject aliases before any caller copies metadata or inspects vertices.
        # Keeping disjoint spans sorted also bounds total selected copies by the
        # decoded payload reserved up front, including malformed input graphs.
        span = (offset, offset + size, label)
        position = bisect_left(self.spans, (offset, -1, ""))
        if position and self.spans[position - 1][1] > offset:
            raise AssetError(f"mesh selected ranges overlap or alias: {self.spans[position - 1][2]} / {label}")
        if position < len(self.spans) and offset + size > self.spans[position][0]:
            raise AssetError(f"mesh selected ranges overlap or alias: {label} / {self.spans[position][2]}")
        self.entries += 1
        if self.entries > self.limits.max_entries:
            raise AssetError("mesh cumulative structure/reference entry limit exceeded")
        if data:
            self.data_bytes += size
            if self.data_bytes > self.limits.max_file_bytes:
                raise AssetError("mesh cumulative vertex/index byte work limit exceeded")
        else:
            self.metadata_bytes += size
            if self.metadata_bytes > self.limits.max_metadata_bytes:
                raise AssetError("mesh selected metadata exceeds the metadata byte limit")
        self.spans.insert(position, span)
        return offset

    def fields(self, offset: int, form: str) -> tuple:
        self.view.offset(SYSTEM_BASE + offset, struct.calcsize(form))
        return struct.unpack_from(form, self.view.system, offset)

    def u64(self, offset: int) -> int:
        return self.fields(offset, "<Q")[0]

    def raw(self, offset: int, size: int) -> bytes:
        self.view.offset(SYSTEM_BASE + offset, size)
        return bytes(self.view.system[offset : offset + size])

    def zeros(self, offset: int, ranges: tuple[tuple[int, int], ...], label: str) -> None:
        for start, end in ranges:
            if any(self.view.system[offset + start : offset + end]):
                raise AssetError(f"mesh {label} has unsupported reserved fields or additional buffers")


def _bounds(raw: bytes) -> None:
    for start in range(0, len(raw), 32):
        values = struct.unpack_from("<8f", raw, start)
        if any(not math.isfinite(values[index]) for index in (0, 1, 2, 4, 5, 6)) or any(
            values[index] > values[index + 4] for index in range(3)
        ):
            raise AssetError("mesh model bounds are non-finite or inverted")


def _read_geometry(reader: _MeshReader, pointer: int, shader_index: int) -> dict:
    at = reader.claim(pointer, 152, 16, "geometry")
    reader.zeros(at, ((8, 24), (32, 56), (64, 88), (100, 104), (116, 120), (128, 152)), "geometry")
    indices_count, triangles, geometry_vertices, primitive = reader.fields(at + 88, "<IIHH")
    stride, palette_count = reader.fields(at + 112, "<HH")
    if reader.u64(at + 104) or palette_count:
        raise AssetError("mesh bone palettes are outside the unskinned scope")
    if primitive != 3 or not indices_count or indices_count % 3 or triangles != indices_count // 3:
        raise AssetError("mesh requires exact 16-bit triangle-list index triplets")

    vb = reader.claim(reader.u64(at + 24), 128, 16, "vertex buffer")
    reader.zeros(vb, ((10, 16), (28, 32), (40, 48), (56, 128)), "vertex buffer")
    vb_stride = reader.fields(vb + 8, "<H")[0]
    vertex_count = reader.fields(vb + 24, "<I")[0]
    if not 0 < vertex_count <= 65535 or not 0 < stride <= 255 or stride != vb_stride:
        raise AssetError("mesh vertex count/stride is unsupported or inconsistent")
    if geometry_vertices not in (0, vertex_count):
        raise AssetError("mesh geometry vertex count differs from its buffer")
    first, second = reader.u64(vb + 16), reader.u64(vb + 32)
    if not (first or second) or (first and second and first != second):
        raise AssetError("mesh requires one vertex stream; distinct secondary streams are unsupported")
    stream = first or second
    if reader.u64(at + 120) not in (0, stream):
        raise AssetError("mesh geometry vertex-data pointer differs from its buffer")
    declaration_at = reader.claim(reader.u64(vb + 48), 16, 8, "vertex declaration")
    declaration = reader.raw(declaration_at, 16)
    attributes = legacy_attributes(declaration)
    if struct.unpack_from("<H", declaration, 4)[0] != stride:
        raise AssetError("mesh declaration stride differs from its buffer")
    vertex_at = reader.claim(stream, vertex_count * stride, 4, "vertex data", data=True)
    reader.float_scalars += vertex_count * sum(attribute.float_scalars for attribute in attributes)
    if reader.float_scalars > MAX_FLOAT_SCALARS:
        raise AssetError("mesh cumulative float inspection work limit exceeded")
    for attribute in attributes:
        if not attribute.float_scalars:
            continue
        form = struct.Struct("<" + "f" * attribute.float_scalars)
        for vertex in range(vertex_count):
            values = form.unpack_from(reader.view.system, vertex_at + vertex * stride + attribute.offset)
            if not all(math.isfinite(value) for value in values):
                raise AssetError(f"mesh {attribute.name} contains a non-finite scalar")

    ib = reader.claim(reader.u64(at + 56), 96, 16, "index buffer")
    reader.zeros(ib, ((12, 16), (24, 96)), "index buffer")
    if reader.fields(ib + 8, "<I")[0] != indices_count:
        raise AssetError("mesh geometry and index-buffer counts disagree")
    index_at = reader.claim(reader.u64(ib + 16), indices_count * 2, 2, "index data", data=True)
    index_view = reader.view.system[index_at : index_at + indices_count * 2]
    if any(index >= vertex_count for (index,) in struct.iter_unpack("<H", index_view)):
        raise AssetError("mesh index references a vertex outside its buffer")
    # Defer large byte copies until every selected object/data span is checked.
    return {
        "source_offset": at,
        "shader_index": shader_index,
        "vertex_count": vertex_count,
        "vertex_stride": stride,
        "attributes": attributes,
        "vertex_at": vertex_at,
        "vertex_bytes": vertex_count * stride,
        "index_at": index_at,
        "index_bytes": indices_count * 2,
        "geometry_header": reader.raw(at, 152),
        "vertex_buffer_header": reader.raw(vb, 128),
        "index_buffer_header": reader.raw(ib, 96),
        "declaration": declaration,
    }


def read_pc_meshes(blob: bytes, limits: Limits = MESH_LIMITS, *, scope: str = YFT_MAIN_MESH_SCOPE) -> PcDrawableMeshes:
    """Read every main-drawable mesh in the explicit finite static scope.

    Source class words and selected headers/bounds are retained verbatim. The
    fragment's children, physics and shader parameter graphs are outside this
    scope, not silently converted. Shader headers only bound/source-identify
    geometry indices. A skeleton or unsupported selected mesh rejects all mesh
    output. No source file is written or repaired.
    """

    if type(scope) is not str or scope != YFT_MAIN_MESH_SCOPE:
        raise AssetError("unsupported PC mesh scope")
    if type(blob) is not bytes or not 16 <= len(blob) <= limits.max_file_bytes:
        raise AssetError("source mesh resource is empty or exceeds the file byte limit")
    header = resource_header(blob)
    if header["version"] != 162 or header["graphicsBytes"]:
        raise AssetError("mesh intake requires a system-only Legacy YFT version 162")
    # Retain the input, decoded source and up to one complete selected copy.
    if len(blob) + 2 * header["systemBytes"] > limits.max_total_bytes:
        raise AssetError("mesh intake exceeds the cumulative encoded/decoded/copy byte budget")
    _, payload = decode_resource(blob, limits.max_file_bytes)
    reader = _MeshReader(payload, limits)
    root = reader.claim(SYSTEM_BASE, 304, 16, "fragment root")
    drawable = reader.claim(reader.u64(48), 336, 16, "main drawable")
    if reader.u64(drawable + 24) or reader.u64(drawable + 144):
        raise AssetError("main drawable skeleton/joints are outside the unskinned mesh scope")
    if any(reader.fields(drawable + 152, "<HHI")):
        raise AssetError("main drawable optional model-block fields are outside the mesh scope")
    bounds = reader.fields(drawable + 32, "<4f")
    if not all(math.isfinite(value) for value in bounds) or bounds[3] < 0:
        raise AssetError("main drawable bounding sphere is invalid")
    _bounds(reader.raw(drawable + 48, 32))
    distances = reader.fields(drawable + 112, "<4f")
    if any(not math.isfinite(value) or value < 0 for value in distances):
        raise AssetError("main drawable LOD distances are non-finite or negative")
    lod_pointers = reader.fields(drawable + 80, "<4Q")
    if reader.u64(drawable + 160) not in (0, lod_pointers[0]):
        raise AssetError("extra drawable model lists are outside the selected mesh scope")

    group = reader.claim(reader.u64(drawable + 16), 64, 16, "shader group")
    if reader.u64(group + 8):
        raise AssetError("embedded texture dictionaries are outside the selected mesh scope")
    shader_count, shader_capacity = reader.fields(group + 24, "<HH")
    if not 0 < shader_count == shader_capacity <= limits.max_entries:
        raise AssetError("mesh shader-group count/capacity is invalid")
    table = reader.claim(reader.u64(group + 16), shader_count * 8, 8, "shader table")
    shader_headers = tuple(
        reader.raw(reader.claim(reader.u64(table + index * 8), 48, 16, "shader header"), 48)
        for index in range(shader_count)
    )
    lods = []
    geometry_count = 0
    for name, pointer in zip(LOD_NAMES, lod_pointers, strict=True):
        if not pointer:
            lods.append((name, None, []))
            continue
        list_at = reader.claim(pointer, 16, 8, "model-list header")
        array, count, capacity, reserved = reader.fields(list_at, "<QHHI")
        if not 0 < count == capacity <= limits.max_entries or reserved:
            raise AssetError("mesh model-list count/capacity/reserved word is unsupported")
        array_at = reader.claim(array, count * 8, 8, "model-pointer array")
        models = []
        for model_index in range(count):
            at = reader.claim(reader.u64(array_at + model_index * 8), 48, 16, "model")
            first_count, second_count, reserved = reader.fields(at + 16, "<HHI")
            binding, _render_mask, third_count = reader.fields(at + 40, "<IHH")
            if binding:
                raise AssetError("mesh model skin/rigid bone binding is outside the unskinned scope")
            if not 0 < first_count == second_count == third_count <= limits.max_entries or reserved:
                raise AssetError("mesh model geometry counts/reserved word are unsupported")
            geometry_count += first_count
            if geometry_count > limits.max_entries:
                raise AssetError("mesh cumulative geometry limit exceeded")
            geometries_at = reader.claim(reader.u64(at + 8), first_count * 8, 8, "geometry-pointer array")
            mapping_at = reader.claim(reader.u64(at + 32), first_count * 2, 2, "shader mapping")
            bounds_bytes = (first_count + (first_count > 1)) * 32
            bounds_at = reader.claim(reader.u64(at + 24), bounds_bytes, 16, "model bounds")
            raw_bounds = reader.raw(bounds_at, bounds_bytes)
            _bounds(raw_bounds)
            geometries = []
            for index in range(first_count):
                shader_index = reader.fields(mapping_at + index * 2, "<H")[0]
                if shader_index >= shader_count:
                    raise AssetError("mesh shader index leaves the source shader group")
                geometries.append(_read_geometry(reader, reader.u64(geometries_at + index * 8), shader_index))
            models.append((at, reader.raw(at, 48), raw_bounds, reader.raw(mapping_at, first_count * 2), geometries))
        lods.append((name, reader.raw(list_at, 16), models))
    if not geometry_count:
        raise AssetError("selected main drawable has no supported mesh geometry")
    output_lods = []
    for name, list_header, models in lods:
        output_models = []
        for at, model_header, bounds, mapping, geometries in models:
            output_geometries = []
            for geometry in geometries:
                values = {
                    key: value
                    for key, value in geometry.items()
                    if key not in ("vertex_at", "vertex_bytes", "index_at", "index_bytes")
                }
                output_geometries.append(
                    PcMeshGeometry(
                        **values,
                        vertices=reader.raw(geometry["vertex_at"], geometry["vertex_bytes"]),
                        indices=reader.raw(geometry["index_at"], geometry["index_bytes"]),
                    )
                )
            output_models.append(PcMeshModel(at, model_header, bounds, mapping, tuple(output_geometries)))
        output_lods.append(PcMeshLod(name, list_header, tuple(output_models)))
    return PcDrawableMeshes(
        source_sha256=hashlib.sha256(blob).hexdigest(),
        source_bytes=len(blob),
        resource_header=blob[:16],
        scope=scope,
        fragment_header=reader.raw(root, 304),
        drawable_offset=drawable,
        drawable_header=reader.raw(drawable, 336),
        shader_group_header=reader.raw(group, 64),
        shader_headers=shader_headers,
        lods=tuple(output_lods),
    )

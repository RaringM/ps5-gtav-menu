#!/usr/bin/env python3
"""Convert a PC Legacy drawable (.ydr v165) into a PS5 drawable (.pdr v159) on a retail template.

First use: weapon models (menu-ctl.sh convert-weapon). A PC weapon mod replaces
the models of a stock weapon and keeps that weapon's skeleton, so the stock PS5 drawable of the same
name is the template. The template keeps everything the PC source does not define for the PS5:
the drawable header (LOD distances, render masks, name, embedded bound), the skeleton and the
resource version. The source supplies the geometry, the models and the shaders:

- meshes and models: gen9_mesh.build / gen9_models.build, the writers the vehicle converter
  uses (Gen9 declaration, buffers and views from the frozen drawable contracts). The Gen9
  vertex is the Legacy vertex with its attributes reordered; no value changes.
- shaders: gen9_materials.build. Its schema bank comes from the shader instances of the retail
  template(s) (--shader-template adds more retail drawables, e.g. one that uses normal_spec);
  parameters absent from the source take the retail instance's value. Texture references are
  clones of a retail Gen9 texture reference object with the source's texture name.
- the skeleton must equal the template's (names, tags, parents, flags; transforms within 1e-4,
  quaternions up to sign); the converter refuses otherwise rather than mixing skeletons.

The template's high model list is replaced, its other LOD lists must be empty (retail weapons carry
only the high list). Its old index buffers pointed into the graphics pages, which the output drops
(everything sits in system pages, like the hardware-proven converted vehicles); their data pointers
are cleared. The output is re-read and every source vertex, index, bone id, model field, shader
value and texture name is compared; the template bytes are compared outside the edited fields.

This tool does not read the target manifest, the frozen converter reports or the converter
constants table: nothing here comes from the ELF-derived vehicle evidence (and so it does not need
the vehicle converter's target-identity gate). Its inputs are the PC source, retail PS5 resources of
the same game (pinned by sha256 in the pack recipe) and the frozen drawable contracts
(data/drawable_contracts, gtavmenu_tools.drawable_contracts: the PC Legacy and Gen9 layouts and the Legacy ->
Gen9 shader parameter mappings, derived from public CodeWalker sources by
devtools/generators/generate_drawable_contracts.py).

  convert_pc_drawable.py --source w_pi_combatpistol.ydr --template retail/w_pi_combatpistol.pdr \\
      --shader-template retail/w_pi_combatpistol_mag1.pdr --output out/w_pi_combatpistol.pdr \\
      [--ytd w_pi_combatpistol.ytd --ptd-output out/w_pi_combatpistol.ptd --max-texture-size 1024]

Writes the .pdr, a <output>.report.json and, with --ytd, the texture dictionary
(convert_pc_ytd_writer.py). Refuses to overwrite. Needs numpy for --ytd and nothing else outside the
standard library (no capstone, no reference sources: --reference-dir is accepted and not read).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

import gen9_materials  # noqa: E402
import gen9_mesh  # noqa: E402
import gen9_models  # noqa: E402
import resource_page_layout  # noqa: E402
from gtavmenu_tools import drawable_contracts  # noqa: E402
from gtavmenu_tools.asset_formats import AssetError, Limits, decode_resource, resource_header  # noqa: E402
from gtavmenu_tools.asset_textures import (  # noqa: E402
    LEGACY_FORMATS,
    ResourceView,
    inspect_legacy_dictionary,
    linear_mip_sizes,
)
from gtavmenu_tools.hashes import joaat  # noqa: E402
from native_resource_builder import ObjectArena  # noqa: E402

SOURCE_VERSION = 165  # PC Legacy .ydr
TARGET_VERSION = 159  # PS5 / Gen9 .pdr
SYS, GFX = 0x50000000, 0x60000000
LODS = ("High", "Medium", "Low", "VeryLow")
# Copied from the source; the reserved lanes Unknown_3Ch/Unknown_4Ch keep the template's words.
BOUNDS_FIELDS = ("BoundingCenter", "BoundingSphereRadius", "BoundingBoxMin", "BoundingBoxMax")
PAGE_INFO_RECORD, PAGE_INFO_HEADER = resource_page_layout.INFO_RECORD, resource_page_layout.INFO_HEADER
SKELETON = {"bones": 0x20, "inverse": 0x28, "transforms": 0x30, "count": 0x5E}  # CodeWalker Skeleton
BONE = {"bytes": 0x50, "name": 0x38, "flags": 0x40, "tag": 0x44, "parent": 0x32}  # CodeWalker Bone
TRANSFORM_TOLERANCE = 1e-4
# Drawable root words past DrawableBase (CodeWalker Drawable; the same offsets in retail PS5 .pdr roots):
# +0xA8 name pointer, +0xB0 light attributes (pointer, count, capacity), +0xC8 embedded bound.
ROOT_BYTES, ROOT_NAME, ROOT_LIGHTS, ROOT_BOUND = 0xD0, 0xA8, 0xB0, 0xC8
# Light attributes (CodeWalker LightAttributes, BlockLength 168; ResourceSimpleList64 at ROOT_LIGHTS:
# pointer, u16 count, u16 capacity). One record layout for PC Legacy and PS5 Gen9: CodeWalker's
# EnsureGen9 only zeroes Unknown_0h/_4h, and a read-only survey of every light list in the retail
# 01.010.002 .pdr/.pdd/.pft found the same 168-byte records with
# those two words zero (PC exporters leave text there: OpenIV writes "OpenIV") and the reserved words
# +0x14/+0x48/+0xA4 zero. Offsets: Position +0x08, colour + flashiness +0x18, Intensity +0x1C, Flags
# +0x20, BoneId +0x24, Type +0x26, GroupId +0x27, TimeFlags +0x28, Falloff +0x2C, FalloffExponent
# +0x30, culling plane +0x34/+0x40, ShadowBlur +0x44, volume +0x4C..+0x60 (LightHash +0x57), fade
# distances +0x64, ShadowNearClip +0x68, corona +0x5C/+0x6C/+0x70, Direction +0x74, Tangent +0x80,
# cone angles +0x8C/+0x90, Extent +0x94, ProjectedTextureHash +0xA0.
# The game agrees: its drawable load fixup relocates +0xB0 and writes a class pointer into the first qword
# of every 0xA8-byte record (so Unknown_0h/_4h is a runtime slot), its destructor walks the CAPACITY
# (+0xBA) records (count must equal capacity), the per-entity light creation sums the archetype's effects
# and the u16 count as 32-bit and skips a light when the light pool is full (no per-drawable cap), and
# the object-only light gatherer copies light pointers into a 180-entry stack array WITHOUT a bound
# check: a drawable with more than OBJECT_LIGHT_LIMIT lights must never be an object archetype
# (physics/dynamic).
LIGHT_BYTES = 168
LIGHT_CLEARED = (0x00, 0x04)  # Unknown_0h/_4h: zero on PS5
LIGHT_RESERVED = (0x14, 0x48, 0xA4)  # Unknown_14h/_48h/_A4h: zero in both
LIGHT_TYPES = {1: "point", 2: "spot", 4: "capsule"}
LIGHT_FLOATS = (  # (offset, count) of the float runs
    (0x08, 3),  # Position
    (0x1C, 1),  # Intensity
    (0x2C, 6),  # Falloff, FalloffExponent, CullingPlaneNormal, CullingPlaneOffset
    (0x4C, 2),  # VolumeIntensity, VolumeSizeScale
    (0x58, 3),  # VolumeOuterIntensity, CoronaSize, VolumeOuterExponent
    (0x68, 3),  # ShadowNearClip, CoronaIntensity, CoronaZBias
    (0x74, 11),  # Direction, Tangent, ConeInnerAngle, ConeOuterAngle, Extent
)
MAX_LIGHTS = 1024  # well past the largest retail list (241, survey); the list count is a u16
OBJECT_LIGHT_LIMIT = 180  # the object gatherer's stack array (see above); retail objects stay below it
LOD_FIELDS = ("LodDistHigh", "LodDistMed", "LodDistLow", "LodDistVlow")
MASK_FIELDS = ("RenderMaskFlagsHigh", "RenderMaskFlagsMed", "RenderMaskFlagsLow", "RenderMaskFlagsVlow")
TEMPLATE_DEFAULT = "retail-template-instance"
# --ytd: a texture a shader names but the .ytd lacks gets a neutral one-colour stand-in, per parameter.
# Flat normal: x = y = 0.5 both in RGB and in x-in-alpha layouts. No specular. White diffuse. Detail
# maps (normal_spec_detail and kin) perturb the normal by their x/y around 0.5: flat, as BumpTex.
NEUTRAL_TEXTURES = {
    "BumpTex": (128, 128, 255, 128),
    "SpecularTex": (0, 0, 0, 0),
    "DiffuseTex": (255, 255, 255, 255),
    "DetailTex": (128, 128, 255, 128),
    # A weapon palette shader's tint palette (weapon_normal_spec_detail_palette and the cutout/plain ones): a white
    # stand-in named WEAPON_TINT_PALETTE that make_weapon_model_pack.py --tint-palette turns into the 128x32 palette
    # (weapon-conversion.md 11.2). Only those three shaders have the parameter.
    "DiffuseTexPal": (255, 255, 255, 255),
}
# The .ptd member a substituted weapon palette shader's DiffuseTexPal reads (tools/make_weapon_model_pack.py TINT_PALETTE).
WEAPON_TINT_PALETTE = "gm_weapon_tint_pal"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical(value) -> bytes:
    return (json.dumps(value, sort_keys=True, indent=1, allow_nan=False) + "\n").encode()


# ---- frozen contracts ---------------------------------------------------------------------------


def contracts(reference_dir: Path | None = None) -> dict:
    """Every layout and shader table the converter reads: the reviewed frozen file data/drawable_contracts
    (gtavmenu_tools.drawable_contracts; checked against its sha256). `reference_dir` (the callers' old
    --reference-dir, the pinned CodeWalker sources) is no longer read: devtools/generators/
    generate_drawable_contracts.py derives the file from those sources (--check compares)."""
    del reference_dir
    return drawable_contracts.load()


def field(layout: dict, name: str) -> dict:
    return layout["fields"][name]


def read_field(data, at: int, layout: dict, name: str) -> int:
    row = field(layout, name)
    return int.from_bytes(data[at + row["offset"] : at + row["offset"] + row["bytes"]], "little")


def drawable_field(c: dict, name: str) -> int:
    return field(c["geometry"]["drawable"], name)["offset"]


# ---- PC source (.ydr) ---------------------------------------------------------------------------


class Source:
    """A Legacy .ydr: the drawable is the resource root (a .yft wraps it in a fragment).

    `graphics` admits graphics pages; only an embedded texture dictionary may point into them
    (every geometry claim is a system offset, so vertex or index data there is refused)."""

    def __init__(self, blob: bytes, c: dict, limits: Limits, *, graphics: bool = False, entry: str | None = None):
        self.header, self.payload = decode_resource(blob, limits.max_file_bytes)
        if self.header["version"] != SOURCE_VERSION or (self.header["graphicsBytes"] and not graphics):
            raise AssetError(f"source must be a system-only Legacy .ydr/.ydd (version {SOURCE_VERSION})")
        self.view = ResourceView(self.payload, self.header["systemBytes"], self.header["graphicsBytes"])
        self.system = self.view.system
        self.c, self.limits = c, limits
        self.spans: list[tuple[int, int, str]] = []
        # Ped route: exporters (e.g. the one behind several 2017 add-on peds) leave TrianglesCount and
        # VerticesCount 0 on a triangle list; native_mesh writes both from the buffers anyway.
        self.zero_triangle_counts = False
        self.zero_count_geometries = 0
        # Ped and prop routes write the geometry's vertex-data pointer from the buffer (native_mesh), so a
        # stale geometry copy (Map Builder props: the pointer of another buffer) is counted, not refused.
        self.buffer_pointer_wins = False
        self.ignored_vertex_pointers = 0
        # Declarations whose type word differs from GTAV1 only in the nibbles of absent components.
        self.normalized_declarations = 0
        # System offset of the drawable: 0 for a .ydr; a .ydd (drawable dictionary) entry by name.
        self.root = 0 if entry is None else self.dictionary_entry(entry)

    def dictionary_entry(self, name: str) -> int:
        """The drawable of key joaat(name) in a Legacy drawable dictionary (pgDictionary root:
        +0x20 sorted key array, +0x28 count, +0x30 drawable pointer array, the same in Gen9)."""
        keys, count, capacity = struct.unpack_from("<QHH", self.system, 0x20)
        values, value_count, value_capacity = struct.unpack_from("<QHH", self.system, 0x30)
        if not 0 < count == value_count <= 4096 or count > capacity or value_count > value_capacity:
            raise AssetError("source is not a drawable dictionary (key/drawable counts)")
        hashes = struct.unpack_from(f"<{count}I", self.system, self.view.offset(keys, 4 * count))
        if list(hashes) != sorted(hashes):
            raise AssetError("source dictionary keys are not sorted")
        if joaat(name) not in hashes:
            raise AssetError(f"source dictionary has no entry {name}")
        array = self.view.offset(values, 8 * count)
        return self.view.offset(self.u64(array + 8 * hashes.index(joaat(name))), ROOT_BYTES)

    def root_bytes(self, offset: int, size: int) -> bytes:
        return bytes(self.system[self.root + offset : self.root + offset + size])

    def claim(self, pointer: int, size: int, alignment: int, label: str) -> int:
        if not pointer or pointer % alignment:
            raise AssetError(f"source {label} pointer is null or unaligned")
        start = self.view.offset(pointer, size)
        for a, b, other in self.spans:
            if a < start + size and start < b and (a, b, other) != (start, start + size, label):
                raise AssetError(f"source {label} overlaps {other}")
        if (start, start + size, label) not in self.spans:
            self.spans.append((start, start + size, label))
        return start

    def u64(self, at: int) -> int:
        return struct.unpack_from("<Q", self.system, at)[0]

    def materials(self) -> dict:
        """The material row gen9_materials.build reads (pc_drawable_materials.inspect_legacy_yft shape)."""
        mc = self.c["material"]
        group_pointer = self.u64(self.root + drawable_field(self.c, "ShaderGroupPointer"))
        group = self.view.offset(group_pointer, mc["shaderGroup"]["objectBytes"])
        g = {name: read_field(self.system, group, mc["shaderGroup"], name) for name in mc["shaderGroup"]["fields"]}
        count = g["shaderCount1"]
        if count != g["shaderCount2"] or not 0 < count <= 64:
            raise AssetError("source shader group counts disagree or are out of range")
        array = self.view.offset(g["shadersPointer"], count * 8)
        shaders = []
        for index in range(count):
            at = self.view.offset(self.u64(array + index * 8), mc["shader"]["objectBytes"])
            s = {name: read_field(self.system, at, mc["shader"], name) for name in mc["shader"]["fields"]}
            n, pp = s["parameterCount"], s["parametersPointer"]
            block = self.view.offset(pp, n * mc["parameter"]["prefixBytes"])
            extra = sum(self.system[block + i * 16] * 16 for i in range(n))
            if s["parameterSize"] != n * 16 + extra:
                raise AssetError("source shader parameter size disagrees with the reader formula")
            hashes = block + s["parameterSize"]
            self.view.offset(SYS + hashes, n * 4)
            parameters, textures = [], []
            for i in range(n):
                row = block + i * mc["parameter"]["prefixBytes"]
                data_type = read_field(self.system, row, mc["parameter"], "dataType")
                pointer = read_field(self.system, row, mc["parameter"], "dataPointer")
                name_hash = f"0x{struct.unpack_from('<I', self.system, hashes + i * 4)[0]:08x}"
                parameter = {"index": i, "dataType": data_type, "nameHash": name_hash}
                if data_type:
                    if data_type > 64 or not pointer or pointer % 16:
                        raise AssetError("source constant parameter is empty, unaligned or too large")
                    parameter["dataSystemOffset"] = self.view.offset(pointer, data_type * 16)
                else:
                    texture = {"index": i, "nameHash": name_hash, "textureName": None, "textureNameHash": None}
                    if pointer:
                        t = self.view.offset(pointer, mc["textureBase"]["objectBytes"])
                        name = self.view.name(read_field(self.system, t, mc["textureBase"], "namePointer"))
                        texture.update(textureName=name, textureNameHash=f"0x{gen9_materials.hash_name(name):08x}")
                    textures.append(texture)
                parameters.append(parameter)
            if s["textureParameterCount"] != len(textures):
                raise AssetError("source texture parameter count disagrees with the parameter types")
            shaders.append(
                {
                    "index": index,
                    "nameHash": f"0x{s['nameHash']:08x}",
                    "fileNameHash": f"0x{s['fileNameHash']:08x}",
                    "renderBucket": s["renderBucket"],
                    "renderBucketMask": f"0x{s['renderBucketMask']:08x}",
                    "parameters": parameters,
                    "textureParameters": textures,
                }
            )
        return {"shaderGroup": {"textureDictionaryPointer": f"0x{g['textureDictionaryPointer']:x}"}, "shaders": shaders}

    def geometry(self, shader_count: int) -> dict:
        """Model lists in the shape gen9_mesh.build / gen9_models.build read."""
        gc = self.c["geometry"]
        lods = []
        for lod, suffix in zip(LODS, ("High", "Medium", "Low", "VeryLow"), strict=True):
            pointer = self.u64(self.root + drawable_field(self.c, f"DrawableModels{suffix}Pointer"))
            entry = {"lod": lod, "models": []}
            lods.append(entry)
            if not pointer:
                continue
            at = self.claim(pointer, gc["listHeader"]["bytes"], 8, "model-list")
            array, count, capacity, _ = struct.unpack_from("<QHHI", self.system, at)
            if count != capacity or not 0 < count <= 64:
                raise AssetError(f"source {lod} model list count/capacity is unsupported")
            array_at = self.claim(array, count * 8, 8, "model-array")
            for index in range(count):
                model_at = self.claim(self.u64(array_at + index * 8), gc["model"]["prefixBytes"], 16, "model")
                counts = {read_field(self.system, model_at, gc["model"], f"GeometriesCount{i}") for i in (1, 2, 3)}
                if len(counts) != 1 or not 0 < min(counts) <= 256:
                    raise AssetError("source model geometry counts disagree or are out of range")
                n = counts.pop()
                pointers = self.claim(
                    read_field(self.system, model_at, gc["model"], "GeometriesPointer"), n * 8, 8, "geometry-array"
                )
                mapping = self.claim(
                    read_field(self.system, model_at, gc["model"], "ShaderMappingPointer"), n * 2, 2, "shader-map"
                )
                model = {"index": index, "systemOffset": model_at, "geometries": []}
                for gi in range(n):
                    shader = struct.unpack_from("<H", self.system, mapping + gi * 2)[0]
                    if shader >= shader_count:
                        raise AssetError("source geometry names a shader outside its group")
                    geo = self.claim(self.u64(pointers + gi * 8), gc["geometry"]["prefixBytes"], 8, "geometry")
                    model["geometries"].append({"index": gi, "systemOffset": geo, "shaderIndex": shader})
                entry["models"].append(model)
        if not lods[0]["models"]:
            raise AssetError("source drawable has no high model list")
        return {"lods": lods}

    def meshes(self, geometry: dict) -> list[dict]:
        """Vertex/index streams per geometry (pc_mesh_buffers.inspect row shape, Legacy GTAV1 only)."""
        gc, pc = self.c["geometry"], self.c["pcMesh"]
        semantics = {row["value"]: name for name, row in pc["semantics"].items()}
        rows = []
        for lod in geometry["lods"]:
            for model in lod["models"]:
                for item in model["geometries"]:
                    at = item["systemOffset"]
                    g = {name: read_field(self.system, at, gc["geometry"], name) for name in gc["geometry"]["fields"]}
                    vb_at = self.claim(g["VertexBufferPointer"], pc["vertexBuffer"]["bytes"], 8, "vertex-buffer")
                    ib_at = self.claim(g["IndexBufferPointer"], pc["indexBuffer"]["bytes"], 8, "index-buffer")
                    vb = {
                        k: read_field(self.system, vb_at, pc["vertexBuffer"], k) for k in pc["vertexBuffer"]["fields"]
                    }
                    ib = {k: read_field(self.system, ib_at, pc["indexBuffer"], k) for k in pc["indexBuffer"]["fields"]}
                    decl_at = self.claim(vb["InfoPointer"], pc["declaration"]["bytes"], 8, "declaration")
                    decl = {
                        k: read_field(self.system, decl_at, pc["declaration"], k) for k in pc["declaration"]["fields"]
                    }
                    count, stride, indices = vb["VertexCount"], vb["VertexStride"], ib["IndicesCount"]
                    if not 0 < count <= 65535 or not 0 < stride <= 256 or indices <= 0:
                        raise AssetError("source mesh counts/stride are empty or out of range")
                    if g["VerticesCount"] not in (0, count) or (g["VertexStride"], g["IndicesCount"]) != (
                        stride,
                        indices,
                    ):
                        raise AssetError("source geometry and buffer counts/stride disagree")
                    components, offset = [], 0
                    for index in range(16):
                        if decl["Flags"] & (1 << index):
                            spec = pc["supportedComponents"].get(str((decl["Types"] >> (index * 4)) & 15))
                            if spec is None:
                                raise AssetError("source vertex component type is unsupported")
                            components.append(
                                {
                                    "semantic": semantics[index],
                                    "type": spec["name"],
                                    "offset": offset,
                                    "bytes": spec["bytes"],
                                }
                            )
                            offset += spec["bytes"]
                    if decl["Stride"] != stride or offset != stride:
                        raise AssetError("source declaration extent differs from the vertex stride")
                    data = vb["DataPointer1"] or vb["DataPointer2"]
                    if (
                        (self.zero_triangle_counts or self.buffer_pointer_wins)
                        and data
                        and g["VertexDataPointer"] != data
                    ):
                        # ped route: the geometry copy of the pointer is rewritten anyway (native_mesh); exporters
                        # leave it 0 or garbage (Nike pants: 0xb75d70c). The buffer's data pointer is authoritative.
                        self.ignored_vertex_pointers += 1
                    elif not data or g["VertexDataPointer"] != data:
                        raise AssetError("source geometry vertex-data pointer differs from its buffer")
                    vertex_at = self.claim(data, count * stride, 4, "vertex-data")
                    index_at = self.claim(ib["IndicesPointer"], indices * pc["indexElementBytes"], 2, "index-data")
                    raw_indices = bytes(self.system[index_at : index_at + indices * 2])
                    if max(struct.unpack(f"<{indices}H", raw_indices)) >= count:
                        raise AssetError("source index references a vertex outside its buffer")
                    bones = []
                    if g["BoneIdsCount"]:
                        bone_at = self.claim(g["BoneIdsPointer"], g["BoneIdsCount"] * 2, 2, "bone-ids")
                        bones = list(struct.unpack_from(f"<{g['BoneIdsCount']}H", self.system, bone_at))
                    primitive = pc["triangleElements"]
                    zero = self.zero_triangle_counts and g["TrianglesCount"] == 0
                    self.zero_count_geometries += zero
                    vertices = bytes(self.system[vertex_at : vertex_at + count * stride])
                    types = decl["Types"]
                    legacy = int(self.c["mesh"]["legacyTypes"], 16)
                    unused = sum(15 << (4 * i) for i in range(16) if not decl["Flags"] & (1 << i))
                    if types != legacy and (types ^ legacy) & ~unused == 0:
                        # Exporters fill the type nibbles of absent components freely (Tactical MP5K: 0x67...);
                        # only present components carry a format, and those match GTAV1.
                        types = legacy
                        self.normalized_declarations += 1
                    rows.append(
                        {
                            "lod": lod["lod"],
                            "modelIndex": model["index"],
                            "geometryIndex": item["index"],
                            "shaderIndex": item["shaderIndex"],
                            "vertexCount": count,
                            "vertexStride": stride,
                            "indexCount": indices,
                            "boneIds": bones,
                            "declarationTypes": hex(types),
                            "vertexBufferFlags": vb["Flags"],
                            "triangleDeclarationAgrees": g["Unknown_62h"] == primitive
                            and indices % primitive == 0
                            and (zero or g["TrianglesCount"] == indices // primitive),
                            "components": components,
                            "vertexStreams": [
                                {"systemOffset": vertex_at, "bytes": count * stride, "sha256": digest(vertices)}
                            ],
                            "indices": {"systemOffset": index_at, "bytes": indices * 2, "sha256": digest(raw_indices)},
                        }
                    )
        return rows

    def skeleton(self) -> list[dict]:
        return read_skeleton(self.system, self.u64(self.root + drawable_field(self.c, "SkeletonPointer")), SYS)

    def lights(self) -> list[bytes]:
        """The drawable's light attribute records (LIGHT_BYTES each), as stored."""
        pointer, count, capacity = struct.unpack_from("<QHH", self.system, self.root + ROOT_LIGHTS)
        if not count:
            return []
        if count > capacity or count > MAX_LIGHTS:
            raise AssetError(f"source light list count {count} exceeds its capacity or {MAX_LIGHTS}")
        at = self.claim(pointer, count * LIGHT_BYTES, 4, "lights")
        return [bytes(self.system[at + i * LIGHT_BYTES : at + (i + 1) * LIGHT_BYTES]) for i in range(count)]

    def has_bound(self) -> bool:
        return bool(self.u64(self.root + ROOT_BOUND))

    def embedded_textures(self, pointer: int, *, repair_stride: bool = False) -> list[dict]:
        """The embedded Legacy texture dictionary at `pointer` (legacy_textures; any bad texture refuses)."""
        return legacy_textures(self.view, pointer, self.limits, repair_stride=repair_stride)[0]


def legacy_textures(
    view: ResourceView, pointer: int, limits: Limits, *, skip_bad: bool = False, repair_stride: bool = False
) -> tuple[list, list]:
    """A Legacy texture dictionary at `pointer` (a .ytd root at 0x50000000, or one embedded in a
    drawable) in the shape convert_pc_ytd_writer.convert reads (name, format, width, height,
    mipLevels, linear mips): the .ytd root checks, per texture. With `skip_bad` a texture whose
    format or stride/mip spans are unusable is left out and named in the second list instead of
    refusing the dictionary (a .ytd with one malformed texture is common). With `repair_stride` a
    block-compressed texture whose stride was written for another block size (BC5 with a BC1 stride,
    repair_pc_texture_stride.py) takes its block-rounded linear chain instead, only when that chain
    stays in the graphics pages and overlaps no other texture's chain; such rows carry "strideRepaired"."""
    if pointer % 16:
        raise AssetError("texture dictionary is unaligned")
    view.offset(pointer, 64)
    hp, hc, hcap, _ = view.system_struct("<QHHI", pointer + 32)
    tp, tc, tcap, _ = view.system_struct("<QHHI", pointer + 48)
    if hc != tc or hc > hcap or tc > tcap or not 0 < hc <= min(limits.max_entries, 1024):
        raise AssetError("texture dictionary counts are inconsistent or out of range")
    view.offset(hp, hc * 4)
    view.offset(tp, tc * 8)
    textures, skipped, seen, spans = [], [], set(), []
    for index in range(hc):
        name_hash = view.system_struct("<I", hp + index * 4)[0]
        at = view.system_struct("<Q", tp + index * 8)[0]
        if at % 16:
            raise AssetError("texture object is unaligned")
        view.offset(at, 144)
        name = view.name(view.system_struct("<Q", at + 40)[0])
        if joaat(name) != name_hash or name.lower() in seen:
            raise AssetError(f"texture {name!r} disagrees with its key or repeats")
        seen.add(name.lower())
        width, height, depth, stride, fmt, _, levels = view.system_struct("<4HIBB", at + 80)
        problem, repaired = None, False
        if depth != 1 or fmt not in LEGACY_FORMATS:
            problem = f"unsupported depth/format {fmt:#x}"
        else:
            format_name, edge, block_bytes = LEGACY_FORMATS[fmt]
            sizes = linear_mip_sizes(width, height, levels, edge, block_bytes)
            stored = [stride * height >> (2 * level) for level in range(levels)]
            # Exporters add 2x2/1x1 mips the Legacy layout cannot describe: keep the matching prefix.
            agree = next((i for i, (a, b) in enumerate(zip(sizes, stored, strict=True)) if a != b), levels)
            if agree == 0 and repair_stride and edge == 4 and sizes[0] % height == 0:
                fixed = [sizes[0] // height * height >> (2 * level) for level in range(levels)]
                agree = next((i for i, (a, b) in enumerate(zip(sizes, fixed, strict=True)) if a != b), levels)
                repaired = agree > 0
            if agree == 0:
                problem = "stride/mip spans differ from the linear layout"
            levels, sizes = agree, sizes[:agree]
        if problem:
            if not skip_bad:
                raise AssetError(f"texture {name}: {problem}")
            skipped.append(f"{name}: {problem}")
            continue
        data = view.offset(view.system_struct("<Q", at + 112)[0], sum(sizes), graphics=True)
        if any(a < data + sum(sizes) and data < b for a, b in spans):
            raise AssetError(f"texture {name}: data overlaps another texture")
        spans.append((data, data + sum(sizes)))
        mips, offset = [], data
        for size in sizes:
            mips.append(bytes(view.graphics[offset : offset + size]))
            offset += size
        textures.append(
            {"name": name, "format": format_name, "width": width, "height": height, "mipLevels": levels}
            | {"mips": tuple(mips)}
            | ({"strideRepaired": True} if repaired else {})
        )
    return textures, skipped


def ps5_texture_name(name: str) -> str:
    """A PC texture name in the PS5 writer's name policy ([A-Za-z0-9][A-Za-z0-9_]*, <= 127 bytes):
    other characters ('-', '+', ' ', ...) become '_', and a 'script_rt_' (script render target, which
    the writer refuses) becomes 'gm_rt_', so the PC placeholder the mod embeds draws as a plain texture
    instead of waiting for a script to register the target. Drawable references and dictionaries are
    renamed with the same rule, so they still match by hash."""
    out = re.sub(r"(?i)script_rt_", "gm_rt_", re.sub(r"[^A-Za-z0-9_]", "_", name))[:127]
    return out if out[:1].isalnum() else ("t" + out)[:127]


def trim_texture(texture: dict, max_size: int | None) -> dict:
    """Drop mips wider/taller than max_size and below one compression block (as --max-texture-size)."""
    edge = 4 if texture["format"].startswith("BC") else 1
    width, height, mips = texture["width"], texture["height"], list(texture["mips"])
    top = 0
    while max_size and top + 1 < len(mips) and max(width, height) >> top > max_size:
        top += 1
    keep = 0
    while top + keep < len(mips) and min(width, height) >> (top + keep) >= edge:
        keep += 1
    if keep == 0:
        keep = 1  # a texture smaller than one block stays as authored
    return texture | {
        "width": width >> top,
        "height": height >> top,
        "mipLevels": keep,
        "mips": tuple(mips[top : top + keep]),
    }


def drop_skeleton(bones: list[dict], rows: list[dict]) -> dict | None:
    """A prop skeleton that is only a root bone at identity, with no geometry bound to bones, carries
    nothing a static map entity uses (map doors author one); it is dropped. Anything else is refused."""
    if not bones:
        return None
    root = bones[0]
    if (
        len(bones) != 1
        or root["parent"] != -1
        or any(abs(v) > TRANSFORM_TOLERANCE for v in root["translation"])
        or any(abs(v - 1.0) > TRANSFORM_TOLERANCE for v in root["scale"])
        or min(max(abs(a - b) for a, b in zip(root["rotation"], q, strict=True)) for q in ((0, 0, 0, 1), (0, 0, 0, -1)))
        > TRANSFORM_TOLERANCE
        or any(row["boneIds"] for row in rows)
    ):
        raise AssetError("prop source has a skeleton beyond one identity root bone (or bound geometry); refused")
    return {"bones": 1, "name": root["name"], "dropped": True}


def read_skeleton(system, pointer: int, base: int) -> list[dict]:
    if not pointer:
        return []
    at = pointer - base

    def u64(o):
        return struct.unpack_from("<Q", system, o)[0]

    count = struct.unpack_from("<H", system, at + SKELETON["count"])[0]
    bones_at, inverse_at, transforms_at = (u64(at + SKELETON[k]) - base for k in ("bones", "inverse", "transforms"))
    bones = []
    for i in range(count):
        b = bones_at + i * BONE["bytes"]
        name_at = u64(b + BONE["name"]) - base
        name = bytes(system[name_at : name_at + 64]).split(b"\0", 1)[0].decode("ascii")
        bones.append(
            {
                "name": name,
                "tag": struct.unpack_from("<H", system, b + BONE["tag"])[0],
                "flags": struct.unpack_from("<H", system, b + BONE["flags"])[0],
                "parent": struct.unpack_from("<h", system, b + BONE["parent"])[0],
                "rotation": struct.unpack_from("<4f", system, b),
                "translation": struct.unpack_from("<3f", system, b + 0x10),
                "scale": struct.unpack_from("<3f", system, b + 0x20),
                "inverse": struct.unpack_from("<16f", system, inverse_at + 64 * i),
                "transform": struct.unpack_from("<16f", system, transforms_at + 64 * i),
            }
        )
    return bones


# The w lane of each row of a bone's 4x4 inverse/transform matrix (rows: three axes, then the translation). The engine
# stores these matrices as three axis vectors plus a translation (16 bytes each); the w lanes are padding: retail
# weapons carry values such as 2.2344 there and PC exporters (ZModeler) 4.0/-3.0 (the Colt M4A1 mod, F4.2), so the
# skeleton check ignores them. The template's skeleton is kept either way.
MATRIX_PADDING = (3, 7, 11, 15)


def compare_skeletons(source: list[dict], template: list[dict]) -> dict:
    """Same bones in the same order; transforms equal within tolerance (quaternions up to sign; matrix padding
    lanes ignored and only reported)."""
    if [(b["name"], b["tag"], b["flags"], b["parent"]) for b in source] != [
        (b["name"], b["tag"], b["flags"], b["parent"]) for b in template
    ]:
        raise AssetError("source skeleton differs from the template's (bone names/tags/flags/parents)")
    worst = padding = 0.0
    for a, b in zip(source, template, strict=True):
        rotation = min(
            max(abs(x - y) for x, y in zip(a["rotation"], b["rotation"], strict=True)),
            max(abs(x + y) for x, y in zip(a["rotation"], b["rotation"], strict=True)),
        )
        others = [abs(x - y) for key in ("translation", "scale") for x, y in zip(a[key], b[key], strict=True)]
        for key in ("inverse", "transform"):
            for lane, (x, y) in enumerate(zip(a[key], b[key], strict=True)):
                if lane in MATRIX_PADDING:
                    padding = max(padding, abs(x - y))
                else:
                    others.append(abs(x - y))
        worst = max(worst, rotation, *others)
    if not worst <= TRANSFORM_TOLERANCE:
        raise AssetError(
            f"source skeleton transforms differ from the template's by {worst:g} (> {TRANSFORM_TOLERANCE})"
        )
    report = {"bones": len(source), "maximumTransformDifference": worst, "keeps": "template skeleton"}
    if padding > TRANSFORM_TOLERANCE:
        report["ignoredMatrixPaddingDifference"] = padding
    return report


def rigid_rows(rows: list[dict]) -> bool:
    """Every geometry unskinned: no bone ids and no blend weights/indices in its vertices."""
    blend = {"BlendWeights", "BlendIndices"}
    return all(not row["boneIds"] and not blend & {c["semantic"] for c in row["components"]} for row in rows)


def rename_source_textures(material: dict, names: dict[str, str], fills: dict[str, str]) -> list[dict]:
    """Rename the source shaders' texture references and fill empty sampler parameters (ped route)."""
    lower = {old.lower(): new for old, new in names.items()}
    fill = {joaat(parameter): name for parameter, name in fills.items()}
    rows = []
    for shader in material["shaders"]:
        for texture in shader["textureParameters"]:
            old = texture["textureName"]
            new = lower.get(old.lower()) if old is not None else fill.get(int(texture["nameHash"], 16))
            if new is None or new == old:
                continue
            texture["textureName"] = new
            texture["textureNameHash"] = f"0x{gen9_materials.hash_name(new):08x}"
            rows.append({"shaderIndex": shader["index"], "parameter": texture["nameHash"], "from": old, "to": new})
    return rows


def drop_unmapped_parameters(material: dict, bank: dict, c: dict) -> list[dict]:
    """Ped route: PC ped shaders carry parameters the PS5 (Gen9) schema of the same shader no longer
    has (the 'ped' family: TextureSamplerDiffPal, StubbleControl and one more constant). They are
    dropped and reported instead of refusing the shader; a dropped texture parameter must be empty or bound
    to the engine's stand-in `givemechecker` (exporters write it into TextureSamplerDiffPal)."""
    dropped = []
    for shader in material["shaders"]:
        key = str(int(shader["nameHash"], 0))
        mapping = c["shader"]["mappings"][key]
        target = {p["nameHash"] for p in bank[key]["schema"]["parameters"]}
        keep = []
        for parameter in shader["parameters"]:
            old = int(parameter["nameHash"], 0)
            if mapping["renames"].get(str(old), old) in target:
                keep.append(parameter)
                continue
            texture = next((t for t in shader["textureParameters"] if t["index"] == parameter["index"]), None)
            name = texture["textureName"] if texture is not None else None
            if name is not None and name.lower() != "givemechecker":
                raise AssetError(f"source texture {name} is bound to a parameter PS5 lacks")
            dropped.append(
                {"shaderIndex": shader["index"], "parameter": parameter["nameHash"]}
                | ({"texture": name} if name else {})
            )
        shader["parameters"] = keep
        indices = {p["index"] for p in keep}
        shader["textureParameters"] = [t for t in shader["textureParameters"] if t["index"] in indices]
    return dropped


# Weapon route (F4.2, weapon-conversion.md 9): a PC shader no retail weapon drawable carries, taken as the nearest
# weapon shader (`--shader-substitute auto`). ZModeler exports scope lenses as vehicle glass (Colt M4A1 mod).
AUTO_SHADER_SUBSTITUTES = {"vehicle_vehglass": "weapon_normal_spec_alpha"}


def neutral_texture_name(parameter: str) -> str:
    """The stand-in texture a substituted shader's empty sampler gets (NEUTRAL_TEXTURES; --ytd adds it)."""
    if parameter == "DiffuseTexPal":
        return WEAPON_TINT_PALETTE
    return f"gm_neutral_{parameter.lower()}"


def substitute_shaders(material: dict, substitutes: dict[int, int], c: dict) -> list[dict]:
    """Retarget source shaders by name hash (old -> new): the shader keeps its textures on the parameters the
    new shader has (by Legacy name), every constant takes the retail instance's value (the source's are
    dropped: their meaning belongs to the old shader) and texture parameters the new shader lacks are dropped
    and listed. The new shader's normal/specular/detail samplers the source leaves empty get the neutral
    stand-ins (flat normal, no specular), as a texture missing from the .ytd does."""
    rows = []
    for shader in material["shaders"]:
        old = int(shader["nameHash"], 0)
        new = substitutes.get(old)
        if new is None:
            continue
        mapping = c["shader"]["mappings"][str(new)]
        target = {int(k) for k in mapping["parameters"]}
        keep, dropped = [], []
        for parameter in shader["parameters"]:
            name = int(parameter["nameHash"], 0)
            texture = next((t for t in shader["textureParameters"] if t["index"] == parameter["index"]), None)
            if parameter["dataType"] == 0 and mapping["renames"].get(str(name), name) in target:
                keep.append(parameter)
            elif texture is not None and texture["textureName"] is not None:
                dropped.append(texture["textureName"])
        indices = {p["index"] for p in keep}
        shader["parameters"] = keep
        shader["textureParameters"] = [t for t in shader["textureParameters"] if t["index"] in indices]
        native = {mapping["renames"].get(str(int(p["nameHash"], 0)), int(p["nameHash"], 0)): p["index"] for p in keep}
        neutral = []
        for key, row in sorted(mapping["parameters"].items()):
            if row.get("type") != "Texture" or row.get("name") not in NEUTRAL_TEXTURES or "old" not in row:
                continue
            if row["name"] == "DiffuseTex":
                continue  # the source's own colour stays (a missing one is refused by the --ytd check)
            name = neutral_texture_name(row["name"])
            texture = {"textureName": name, "textureNameHash": f"0x{gen9_materials.hash_name(name):08x}"}
            if int(key) in native:
                current = next(t for t in shader["textureParameters"] if t["index"] == native[int(key)])
                if current["textureName"] is None:
                    current.update(texture)
                    neutral.append(name)
                continue
            index = max([p["index"] for p in shader["parameters"]] + [-1]) + 1
            shader["parameters"].append({"index": index, "dataType": 0, "nameHash": f"0x{joaat(row['old']):08x}"})
            shader["textureParameters"].append({"index": index, "nameHash": f"0x{joaat(row['old']):08x}", **texture})
            neutral.append(name)
        shader["nameHash"] = f"0x{new:08x}"
        shader["fileNameHash"] = f"0x{joaat(mapping['name'] + '.sps'):08x}"
        rows.append(
            {
                "shaderIndex": shader["index"],
                "from": c["shader"]["mappings"][str(old)]["name"],
                "to": mapping["name"],
                "textures": [t["textureName"] for t in shader["textureParameters"] if t["textureName"]],
                "droppedTextures": dropped,
                "neutralTextures": neutral,
            }
        )
    return rows


def resolve_substitutes(spec: list[str], wanted: set[int], carried: set[int], c: dict) -> dict[int, int]:
    """FROM=TO shader names (or `auto`: AUTO_SHADER_SUBSTITUTES for a wanted shader no carrier has) -> hashes."""
    names = {row["name"]: int(key) for key, row in c["shader"]["mappings"].items()}
    pairs = []
    for item in spec:
        if item == "auto":
            pairs += [(a, b) for a, b in AUTO_SHADER_SUBSTITUTES.items() if names.get(a) in wanted - carried]
            continue
        old, sep, new = item.partition("=")
        if not sep or old not in names or new not in names:
            raise AssetError(f"--shader-substitute takes FROM=TO shader names (or auto), not {item!r}")
        pairs.append((old, new))
    return {names[a]: names[b] for a, b in pairs}


PED_TRANSLATION_TOLERANCE = 0.05  # metres; facial rigs of different retail peds differ by ~2 cm


def compare_ped_skeleton(
    source: list[dict], ped: list[dict], own: list[dict], rows: list[dict], *, merge_missing: bool = False
) -> dict:
    """Ped component route: the source skeleton (the add-on's copy, e.g. ig_bankman's) must have the
    ped skeleton's bones by tag with the same parents; translations may differ by at most
    PED_TRANSLATION_TOLERANCE (bind pose: the game skins with the ped's skeleton, so a larger
    difference would move vertices). Every geometry bone palette is remapped by tag onto the target
    skeleton: the template drawable's own copy when it has one (retail head drawables), else the ped
    skeleton (palettes then index it directly). The source skeleton itself is not written.
    `merge_missing`: a source bone the target skeleton lacks (an older PC rig's MH_*_CalfBack helper)
    takes its nearest source ancestor that the target has; such bones are listed as merged."""
    if not source:
        raise AssetError("ped route: the source drawable has no skeleton to map its palettes by tag")
    in_ped = {bone["tag"] for bone in ped}
    if len(in_ped) != len(ped) or len({b["tag"] for b in source}) != len(source):
        raise AssetError("ped route: bone tags repeat")
    # Parents and bind pose are compared with the skeleton the palettes will index: a streamed
    # component's own copy is a subset of the ped skeleton whose parents skip the bones it leaves
    # out (RB_L_ThighRoll under SKEL_Pelvis), so it is compared with the template's subset.
    reference = own or ped
    by_tag = {bone["tag"]: index for index, bone in enumerate(reference)}
    differences, worst, merged = [], {"translation": 0.0, "rotation": 0.0}, {}
    for bone in source:
        if bone["tag"] not in in_ped:
            raise AssetError(f"ped route: source bone {bone['name']} (tag {bone['tag']}) is not in the ped skeleton")
        if bone["tag"] not in by_tag:
            if not merge_missing:
                raise AssetError(f"ped route: source bone {bone['name']} is not in the template's own skeleton")
            ancestor = bone
            while ancestor["parent"] >= 0 and ancestor["tag"] not in by_tag:
                ancestor = source[ancestor["parent"]]
            if ancestor["tag"] not in by_tag:
                raise AssetError(f"ped route: no ancestor of {bone['name']} is in the template's skeleton")
            merged[bone["tag"]] = ancestor["tag"]
            continue
        other = reference[by_tag[bone["tag"]]]
        parent = source[bone["parent"]]["tag"] if bone["parent"] >= 0 else None
        expected = reference[other["parent"]]["tag"] if other["parent"] >= 0 else None
        if parent != expected:
            raise AssetError(f"ped route: bone {bone['name']} has another parent than in the target skeleton")
        moved = max(abs(x - y) for x, y in zip(bone["translation"], other["translation"], strict=True))
        turned = min(
            max(abs(x - y) for x, y in zip(bone["rotation"], other["rotation"], strict=True)),
            max(abs(x + y) for x, y in zip(bone["rotation"], other["rotation"], strict=True)),
        )
        worst = {"translation": max(worst["translation"], moved), "rotation": max(worst["rotation"], turned)}
        if moved > TRANSFORM_TOLERANCE or turned > TRANSFORM_TOLERANCE or bone["flags"] != other["flags"]:
            differences.append(
                {
                    "bone": bone["name"],
                    "pedBone": other["name"],
                    "translation": round(moved, 5),
                    "rotation": round(turned, 5),
                    "flags": [f"0x{bone['flags']:x}", f"0x{other['flags']:x}"],
                }
            )
        if moved > PED_TRANSLATION_TOLERANCE:
            raise AssetError(f"ped route: bone {bone['name']} is {moved:.3f} m from the ped's (bind pose differs)")
    target = own or ped
    index = {bone["tag"]: i for i, bone in enumerate(target)}
    remap = []
    for bone in source:
        tag = merged.get(bone["tag"], bone["tag"])
        if tag not in index:
            raise AssetError(f"ped route: source bone {bone['name']} is not in the template's own skeleton")
        remap.append(index[tag])
    remapped = 0
    for row in rows:
        if not row["boneIds"]:
            raise AssetError("ped route: a skinned ped geometry without a bone palette")
        if max(row["boneIds"]) >= len(source):
            raise AssetError("ped route: a palette names a bone outside the source skeleton")
        new = [remap[b] for b in row["boneIds"]]
        remapped += new != row["boneIds"]
        row["boneIds"] = new
    return {
        "bones": len(source),
        "pedBones": len(ped),
        "target": "template skeleton" if own else "ped skeleton (no copy in the drawable)",
        "palettesRemapped": remapped,
        "maximumDifference": {k: round(v, 5) for k, v in worst.items()},
        "mergedBones": [
            {"bone": b["name"], "into": next(x["name"] for x in source if x["tag"] == merged[b["tag"]])}
            for b in source
            if b["tag"] in merged
        ],
        "differingBones": differences,
    }


def bake_ped_palettes(payload: bytes, rows: list[dict], geometry: dict, bones: int, c: dict) -> tuple[bytes, dict]:
    """Ped route, after compare_ped_skeleton: every retail ped component geometry carries the identity
    palette over its drawable's whole skeleton (boneIds = 0..n-1, n = the skeleton's bone count) and
    every model's matrix count (SkeletonBinding low byte) is n. A remapped palette (FRANKLIN-run1: the mod's 24 bones onto the 28-bone uppr_014_u
    skeleton, palette [0..12, 14, ..]) is therefore written into the vertex blend indices instead
    (index i -> palette[i]); the palette becomes the identity of length n and each model counts n
    matrices, so the result skins the same whether the engine reads the palette or the blend index
    directly, and no blend index exceeds the model's matrix count. A blend index outside the palette
    must carry zero weight (it is written 0). Returns the patched payload (the caller converts from it)
    and a report; each row's vertex-stream digest is updated to the patched bytes."""
    if not 0 < bones <= 0xFF:
        raise AssetError("ped route: target skeleton bone count does not fit the model matrix count")
    data = bytearray(payload)
    done: dict[int, tuple[int, ...]] = {}
    changed = 0
    for row in rows:
        palette = tuple(row["boneIds"])
        if not palette or max(palette) >= bones:
            raise AssetError("ped route: a palette names a bone outside the target skeleton")
        components = {component["semantic"]: component for component in row["components"]}
        indices, weights = components.get("BlendIndices"), components.get("BlendWeights")
        if indices is None or weights is None or indices["bytes"] != 4 or weights["bytes"] != 4:
            raise AssetError("ped route: a skinned geometry without 4-byte blend indices and weights")
        stride, count = row["vertexStride"], row["vertexCount"]
        for stream in row["vertexStreams"]:
            start = stream["systemOffset"]
            if start in done:
                if done[start] != palette:
                    raise AssetError("ped route: geometries sharing vertex data have different palettes")
                stream["sha256"] = digest(bytes(data[start : start + count * stride]))
                continue
            done[start] = palette
            for vertex in range(count):
                at = start + vertex * stride
                for k in range(4):
                    old = data[at + indices["offset"] + k]
                    if old < len(palette):
                        new = palette[old]
                    elif data[at + weights["offset"] + k]:
                        raise AssetError("ped route: a weighted blend index lies outside its palette")
                    else:
                        new = 0
                    changed += new != old
                    data[at + indices["offset"] + k] = new
            stream["sha256"] = digest(bytes(data[start : start + count * stride]))
        row["boneIds"] = list(range(bones))
    binding = field(c["geometry"]["model"], "SkeletonBinding")
    models = []
    for lod in geometry["lods"]:
        for model in lod["models"]:
            at = model["systemOffset"] + binding["offset"]
            value = struct.unpack_from("<I", data, at)[0]
            if not value >> 8 & 0xFF:
                raise AssetError("ped route: a model without the skinned flag")
            struct.pack_into("<I", data, at, value & ~0xFF | bones)
            models.append({"from": f"0x{value:x}", "to": f"0x{value & ~0xFF | bones:x}"})
    return bytes(data), {
        "paletteBones": bones,
        "geometries": len(rows),
        "changedBlendIndices": changed,
        "modelSkeletonBindings": models,
    }


# ---- retail template ----------------------------------------------------------------------------


class Template:
    def __init__(self, blob: bytes, c: dict, label: str):
        self.blob, self.label, self.c = blob, label, c
        self.header, self.payload = decode_resource(blob, 1 << 28)
        if self.header["version"] != TARGET_VERSION:
            raise AssetError(f"{label}: template must be a PS5 drawable (version {TARGET_VERSION})")
        self.system = self.payload[: self.header["systemBytes"]]

    def u64(self, at: int) -> int:
        return struct.unpack_from("<Q", self.system, at)[0]

    def shaders(self) -> list:
        group = self.u64(drawable_field(self.c, "ShaderGroupPointer")) - SYS
        return gen9_materials.read_group(self.system, SYS, group, self.c["shader"])

    def high_list(self) -> tuple[int, list[int]]:
        for suffix in ("Medium", "Low", "VeryLow"):
            if self.u64(drawable_field(self.c, f"DrawableModels{suffix}Pointer")):
                raise AssetError(f"{self.label}: template has a {suffix} model list (only High is replaced)")
        if self.u64(drawable_field(self.c, "DrawableModelsPointer")):
            raise AssetError(f"{self.label}: template has a separate model-block pointer")
        at = self.u64(drawable_field(self.c, "DrawableModelsHighPointer")) - SYS
        array, count, capacity, _ = struct.unpack_from("<QHHI", self.system, at)
        if count != capacity or not count:
            raise AssetError(f"{self.label}: template high list is empty or partial")
        return at, [self.u64(array - SYS + 8 * i) - SYS for i in range(count)]

    def index_data_slots(self) -> list[int]:
        """System offsets of the template's index-data pointers (the only graphics-page references)."""
        mesh = self.c["mesh"]["layouts"]
        _, models = self.high_list()
        slots = []
        for model in models:
            m = gen9_models.read(self.system, SYS, model, self.c["model"])
            for g in m["geometryPointers"]:
                ib = self.u64(g - SYS + field(mesh["geometry"], "IndexBufferPointer")["offset"]) - SYS
                slots.append(ib + field(mesh["IndexBuffer"], "IndicesPointer")["offset"])
        graphics = [
            o for o in range(0, len(self.system) - 7, 8) if GFX <= self.u64(o) < GFX + self.header["graphicsBytes"]
        ]
        if sorted(graphics) != sorted(s for s in slots if GFX <= self.u64(s) < GFX + self.header["graphicsBytes"]):
            raise AssetError(f"{self.label}: graphics pages hold more than the replaced index data")
        return graphics

    def texture_reference(self) -> bytes:
        """The template's Gen9 texture reference object; every reference must be the same apart from its name."""
        size = self.c["material"]["textureBase"]["objectBytes"]
        name_at = field(self.c["material"]["textureBase"], "namePointer")["offset"]
        shapes = set()
        for row in self.shaders():
            for pointer in row["texturePointers"]:
                if pointer:
                    raw = bytearray(self.system[pointer - SYS : pointer - SYS + size])
                    raw[name_at : name_at + 8] = bytes(8)
                    shapes.add(bytes(raw))
        if len(shapes) != 1:
            raise AssetError(
                f"{self.label}: template texture references differ beyond their names ({len(shapes)} shapes)"
            )
        return shapes.pop()


def shader_bank(templates: list[Template], wanted: set[str], c: dict, *, first_only: bool = False) -> tuple[dict, dict]:
    """Schemas of the wanted shaders from the retail templates. Instances of one shader in different
    retail drawables differ in their sampler-state bytes (samplersHex); `first_only` (prop route) takes
    each shader from the first template that carries it, in the order given, instead of requiring
    every instance to agree."""
    unique = {digest(t.blob): t for t in templates}  # the template may also be named as a shader template
    references = [(identity, t.shaders()) for identity, t in unique.items()]
    if first_only:
        taken: set[int] = set()
        for _identity, rows in references:
            keep = [r for r in rows if r["schema"]["nameHash"] not in taken]
            taken |= {r["schema"]["nameHash"] for r in rows}
            rows[:] = list({r["schema"]["nameHash"]: r for r in reversed(keep)}.values())[::-1]
    bank = gen9_materials.schema_bank(references)
    missing = sorted(wanted - bank.keys())
    if missing:
        raise AssetError(
            "no retail template carries shader(s) "
            + ", ".join(f"0x{int(m):08x}" for m in missing)
            + "; add a --shader-template drawable that uses it"
        )
    first = {}
    for identity, rows in references:
        for row in rows:
            first.setdefault(str(row["schema"]["nameHash"]), (identity, row))
    bank = {key: bank[key] for key in sorted(wanted)}
    for key, entry in bank.items():
        identity, row = first[key]
        schema, data = entry["schema"], bytes.fromhex(row["bufferDataHex"])
        defaults = {}
        for p in schema["parameters"]:
            if p["kind"] == "CBuffer":
                at = sum(schema["bufferSizes"][: p["CBufferIndex"]]) + p["ParamOffset"]
                defaults[str(p["nameHash"])] = {
                    "valueHex": data[at : at + p["ParamLength"]].hex(),
                    "initialization": TEMPLATE_DEFAULT,
                    "resourceSha256": identity,
                }
        entry["compiledDefaults"] = defaults
    return bank, {k: c["shader"]["mappings"][k]["name"] for k in bank}


# ---- light attributes ---------------------------------------------------------------------------


def native_light(record: bytes) -> bytes:
    """A PC light attribute record as PS5 stores it: Unknown_0h/_4h zeroed, everything else verbatim.
    Refuses a record outside the surveyed shape (length, light type, reserved words, non-finite floats)."""
    if len(record) != LIGHT_BYTES:
        raise AssetError("light record length differs")
    kind = record[0x26]
    if kind not in LIGHT_TYPES:
        raise AssetError(f"light type {kind} is not point/spot/capsule")
    if any(struct.unpack_from("<I", record, at)[0] for at in LIGHT_RESERVED):
        raise AssetError("light reserved word is non-zero")
    for at, count in LIGHT_FLOATS:
        if not all(math.isfinite(v) for v in struct.unpack_from(f"<{count}f", record, at)):
            raise AssetError("light attribute float is not finite")
    out = bytearray(record)
    for at in LIGHT_CLEARED:
        out[at : at + 4] = bytes(4)
    return bytes(out)


def select_lights(records: list[bytes], keep: bool, maximum: int | None, dropped: dict) -> list[bytes]:
    """The PS5 records to write for a source's light list: none with `keep` False (listed as
    dropped["lights"]), else native_light of each, cut to the first `maximum` (the rest listed as
    dropped["lightsPastMaximum"])."""
    if not keep:
        if records:
            dropped["lights"] = len(records)
        return []
    if maximum is not None and maximum < 0:
        raise AssetError("maximum light count is negative")
    out = [native_light(r) for r in records]
    if maximum is not None and len(out) > maximum:
        dropped["lightsPastMaximum"] = len(out) - maximum
        out = out[:maximum]
    return out


def light_report(records: list[bytes]) -> dict:
    """Counts per type and the flags/bone/projection words of the written records (for the pack notes)."""
    types: dict[str, int] = {}
    for record in records:
        name = LIGHT_TYPES[record[0x26]]
        types[name] = types.get(name, 0) + 1
    words = [struct.unpack_from("<I", r, 0x20)[0] for r in records]
    return {
        "count": len(records),
        "types": dict(sorted(types.items())),
        "flags": sorted({f"0x{w:x}" for w in words}),
        "bones": sorted({struct.unpack_from("<H", r, 0x24)[0] for r in records}),
        "projectedTextures": sorted(
            {f"0x{struct.unpack_from('<I', r, 0xA0)[0]:08x}" for r in records} - {"0x00000000"}
        ),
        "exceedsObjectLimit": len(records) > OBJECT_LIGHT_LIMIT,
        "sha256": digest(b"".join(records)),
    }


# ---- composition --------------------------------------------------------------------------------


def bucket_mask(template_mask: int, shader_masks: list[int]) -> int:
    """A drawable's high render mask as retail writes it: the template's upper bytes with the low byte
    (one bit per render bucket) the OR of the bucket masks of the shaders its high models draw with.
    Retail ped entries: ped/ped_emissive 0xff01, ped_decal 0xff04, ped_hair_cutout_alpha hair 0xff08,
    cop sunglasses (ped + ped_alpha) 0xff03."""
    buckets = 0
    for mask in shader_masks:
        buckets |= mask & 0xFF
    return (template_mask & ~0xFF) | buckets


def ped_render_mask(template: Template, material_placement: dict, rows: list[dict], c: dict) -> dict | None:
    """Ped route: the template's high render mask names the template's own render buckets (the cop
    head: bucket 0); a converted drawable drawing in another bucket (ped_hair_cutout_alpha: bucket 3)
    needs that bucket's bit, or the engine skips it in that pass. None when the mask stays."""
    at = drawable_field(c, "RenderMaskFlagsHigh")
    old = struct.unpack_from("<I", template.system, at)[0]
    used = sorted({row["shaderIndex"] for row in rows if row["lod"] == "High"})
    new = bucket_mask(old, [material_placement["shaders"][i]["renderBucketMask"] for i in used])
    return None if new == old else {"template": old, "written": new}


def prop_root(template: Template, source: Source, c: dict, name: str, lights: int = 0) -> tuple[bytes, dict]:
    """--prop: the drawable root alone, rebuilt on the template's root words.

    Only the template's first ROOT_BYTES are kept (its class word and reserved words). Every pointer
    is cleared: no template skeleton, models, lights, joints, embedded bound or name survives, so the
    template need not be the same model. The source supplies bounds, LOD distances and the high
    render mask; the lower lists are not written (masks 0). `lights` is the count/capacity of the
    source's light list the caller writes (its pointer slot stays 0 here)."""
    root = bytearray(template.system[:ROOT_BYTES])
    slots = [8, ROOT_NAME, ROOT_LIGHTS, ROOT_BOUND, 0xC0]
    slots += [drawable_field(c, f) for f in ("ShaderGroupPointer", "SkeletonPointer", "JointsPointer")]
    slots += [drawable_field(c, f"DrawableModels{s}Pointer") for s in ("High", "Medium", "Low", "VeryLow", "")]
    for slot in slots:
        struct.pack_into("<Q", root, slot, 0)
    struct.pack_into("<HH", root, ROOT_LIGHTS + 8, lights, lights)
    for at in range(0, ROOT_BYTES, 8):
        if at not in slots and (struct.unpack_from("<Q", root, at)[0] >> 28) in (5, 6):
            word = struct.unpack_from("<Q", root, at)[0]
            if word >> 32 == 0:
                raise AssetError(f"{template.label}: template root word +{at:#x} looks like a pointer")
    for name_ in (*BOUNDS_FIELDS, *LOD_FIELDS, "RenderMaskFlagsHigh"):
        row = field(c["geometry"]["drawable"], name_)
        root[row["offset"] : row["offset"] + row["bytes"]] = source.system[row["offset"] : row["offset"] + row["bytes"]]
    for name_ in MASK_FIELDS[1:]:
        row = field(c["geometry"]["drawable"], name_)
        root[row["offset"] : row["offset"] + row["bytes"]] = bytes(row["bytes"])
    lods = struct.unpack_from("<4f", root, field(c["geometry"]["drawable"], "LodDistHigh")["offset"])
    if not all(math.isfinite(v) and 0 < v <= 1e5 for v in lods):
        raise AssetError("source LOD distances are not finite and positive")
    return bytes(root), {
        "name": name + ".#dr",
        "lodDistances": list(lods),
        "renderMaskHigh": f"0x{struct.unpack_from('<I', root, field(c['geometry']['drawable'], 'RenderMaskFlagsHigh')['offset'])[0]:x}",
    }


def convert(
    source_blob: bytes,
    template: Template,
    shader_templates: list[Template],
    c: dict,
    *,
    prop: str | None = None,
    lights: bool = True,
    max_lights: int | None = None,
    entry: str | None = None,
    ped_skeleton: list[dict] | None = None,
    merge_missing_bones: bool = False,
    texture_names: dict[str, str] | None = None,
    fill_textures: dict[str, str] | None = None,
    source_skeleton: list[dict] | None = None,
    ped_prop: bool = False,
    zero_triangle_counts: bool = False,
    shader_substitutes: list[str] | None = None,
    first_carrier: bool = False,
) -> tuple[bytes, dict]:
    """`prop` (the drawable name) selects the prop route: any retail drawable is the template, only
    its root words are kept (prop_root), an embedded texture dictionary is returned in the report's
    "embeddedTextures" (not written into the drawable), lower LOD lists and the embedded bound are
    dropped and listed, and a single identity root bone is dropped (drop_skeleton). The source's light
    attributes are written as PS5 records (native_light; the report's "lights"); `lights=False` drops
    and lists them instead, and `max_lights` keeps only the first N (the rest are listed as dropped).

    Ped component route (convert_pc_ped.py): `entry` takes the source drawable from a .ydd by key;
    `ped_skeleton` (the bones of the ped's skeleton, e.g. the retail .pft) replaces the template
    skeleton check with compare_ped_skeleton and remaps every bone palette by tag onto the template's
    skeleton (its own copy, as retail head drawables carry) or, without one, onto the ped skeleton,
    then bakes the remap into the blend indices with retail identity palettes (bake_ped_palettes);
    `texture_names` renames source texture references (old -> new, case-insensitive) and
    `fill_textures` names a texture for a source sampler parameter that has none (PC parameter name
    -> texture name, e.g. VolumeSampler -> givemechecker as retail ped shaders bind it).
    `source_skeleton` (ped route) is the skeleton an entry without its own copy indexes: in a PC
    component .ydd only the head entry carries one; the others index the ped's .yft skeleton.
    `ped_prop` is the rigid ped prop route (a `p_<anchor>_<nnn>` entry of a PC `<ped>_p.ydd`, or a
    .ydr): like the ped route (graphics pages, embedded textures, zero triangle counts, PC-only
    shader parameters dropped) but unskinned; a single identity root bone is dropped.
    `zero_triangle_counts` (wheel route, convert_pc_wheel.py) admits the ped route's exporter quirk on any
    route: TrianglesCount/VerticesCount 0 on a triangle list (native_mesh writes both from the buffers).
    Weapon route (F4.2): `shader_substitutes` (FROM=TO names, or `auto`) retargets source shaders no carrier has
    (substitute_shaders; the report's "substitutedShaders"); `first_carrier` takes each shader's schema from the
    first carrier that has it (template, then the shader templates in order) instead of requiring all to agree."""
    if prop is not None and (entry is not None or ped_skeleton is not None or ped_prop):
        raise AssetError("the prop route takes a .ydr root drawable, not a ped dictionary entry")
    if ped_prop and ped_skeleton is not None:
        raise AssetError("a ped prop is rigid; it takes no ped skeleton")
    ped = ped_skeleton is not None or ped_prop
    limits = Limits()
    source = Source(source_blob, c, limits, graphics=prop is not None or ped, entry=entry)
    source.zero_triangle_counts = ped or zero_triangle_counts
    source.buffer_pointer_wins = prop is not None
    material = source.materials()
    substituted = []
    if shader_substitutes:
        carried = {row["schema"]["nameHash"] for t in (template, *shader_templates) for row in t.shaders()}
        wanted_hashes = {int(s["nameHash"], 0) for s in material["shaders"]}
        substitutes = resolve_substitutes(shader_substitutes, wanted_hashes, carried, c)
        substituted = substitute_shaders(material, substitutes, c)
    retextured = rename_source_textures(material, texture_names or {}, fill_textures or {})
    embedded = []
    dropped: dict = {}
    if material["shaderGroup"]["textureDictionaryPointer"] != "0x0":
        if prop is None and not ped:
            raise AssetError("source drawable embeds a texture dictionary; ship it as the .ytd instead")
        embedded = source.embedded_textures(
            int(material["shaderGroup"]["textureDictionaryPointer"], 16), repair_stride=ped
        )
        material["shaderGroup"]["textureDictionaryPointer"] = "0x0"
    renamed = {}
    if not ped:  # names the PS5 writer would refuse ('-', ' ', ...): the .ytd/.ptd is renamed the same way
        for shader in material["shaders"]:
            for texture in shader["textureParameters"]:
                name = texture["textureName"]
                if name is not None and ps5_texture_name(name) != name:
                    renamed[name] = texture["textureName"] = ps5_texture_name(name)
                    texture["textureNameHash"] = f"0x{gen9_materials.hash_name(renamed[name]):08x}"
    geometry = source.geometry(len(material["shaders"]))
    if prop is not None:
        lower = sum(len(lod["models"]) for lod in geometry["lods"][1:])
        if lower:
            dropped["lowerLodModels"] = lower
            geometry = {
                "lods": [geometry["lods"][0], *({"lod": lod["lod"], "models": []} for lod in geometry["lods"][1:])]
            }
        if source.has_bound():
            dropped["embeddedBound"] = True
    lower_ped = sum(len(lod["models"]) for lod in geometry["lods"][1:]) if ped else 0
    if lower_ped:
        # ped route: the template is high-only (native_drawable_dictionary.entry_resource high_only, the shape
        # of retail high-only components such as csb_bride's); a source's medium/low models are left out
        geometry = {"lods": [geometry["lods"][0], *({"lod": lod["lod"], "models": []} for lod in geometry["lods"][1:])]}
    rows = source.meshes(geometry)
    payload = source.payload
    if prop is not None or ped_prop:
        if ped_prop and any(row["boneIds"] for row in rows):
            raise AssetError("ped prop route: the source geometry is skinned (ped props are rigid)")
        skeleton = drop_skeleton(source.skeleton(), rows) or {"bones": 0}
    elif ped_skeleton is not None:
        own = read_skeleton(template.system, template.u64(drawable_field(c, "SkeletonPointer")), SYS)
        bones = source.skeleton() or source_skeleton or []
        skeleton = compare_ped_skeleton(bones, ped_skeleton, own, rows, merge_missing=merge_missing_bones)
        payload, skeleton["bakedPalettes"] = bake_ped_palettes(payload, rows, geometry, len(own or ped_skeleton), c)
    else:
        own = source.skeleton()
        retail = read_skeleton(template.system, template.u64(drawable_field(c, "SkeletonPointer")), SYS)
        if not own and retail and rigid_rows(rows):
            # A skeleton-less PC model (an HD .ydr exported without bones, e.g. the Tactical MP5K's w_sb_smg_hi;
            # F4.2): unskinned geometry in model space, bound to bone 0 like the source; the weapon keeps the
            # template's skeleton (attach points, muzzle), only the moving parts do not animate.
            skeleton = {"bones": len(retail), "source": "none (rigid model on bone 0)", "keeps": "template skeleton"}
        else:
            skeleton = compare_skeletons(own, retail)
    groups = [{"owner": {"kind": "main"}, "mesh": {"geometries": rows}, "geometry": geometry}]
    mesh_blob, mesh_placement = gen9_mesh.build(payload, groups, c["mesh"], SYS)
    model_blob, model_placement = gen9_models.build(payload, groups, mesh_blob, mesh_placement, c["model"], SYS)
    wanted = {str(int(s["nameHash"], 0)) for s in material["shaders"]}
    bank, shader_names = shader_bank(
        [template, *shader_templates], wanted, c, first_only=prop is not None or first_carrier
    )
    if substituted:
        # A retargeted shader takes the render bucket of the retail instance it now copies.
        first = {}
        for t in (template, *shader_templates):
            for row in t.shaders():
                first.setdefault(row["schema"]["nameHash"], row["header"])
        for row in substituted:
            shader = material["shaders"][row["shaderIndex"]]
            header = first[int(shader["nameHash"], 0)]
            row["renderBucket"] = [shader["renderBucket"], header["RenderBucket"]]
            shader["renderBucket"] = header["RenderBucket"]
            shader["renderBucketMask"] = f"0x{header['RenderBucketMask']:08x}"
    unmapped = drop_unmapped_parameters(material, bank, c) if ped else []
    material_blob, material_placement = gen9_materials.build(source.payload, material, bank, c["shader"], SYS)
    reference = template.texture_reference()
    name_at = field(c["material"]["textureBase"], "namePointer")["offset"]
    edited = {
        8: "pages-info",
        drawable_field(c, "ShaderGroupPointer"): "materials",
        drawable_field(c, "DrawableModelsHighPointer"): "high-list",
    }
    light_records: list[bytes] = []
    render_mask = None
    if prop is not None:
        old_models, index_slots = [], []
        light_records = select_lights(source.lights(), lights, max_lights, dropped)
        data, root_report = prop_root(template, source, c, prop, len(light_records))
        base = data
        edited[ROOT_NAME] = "name"
        if light_records:
            edited[ROOT_LIGHTS] = "lights"
    else:
        root_report = None
        _, old_models = template.high_list()
        index_slots = template.index_data_slots()
        # The template stays at offset 0 (its own pointers stay valid); edited slots become fixups.
        data = bytearray(template.system)
        info_at = struct.unpack_from("<Q", data, 8)[0] - SYS
        info_bytes = PAGE_INFO_HEADER + PAGE_INFO_RECORD * (data[info_at + 8] + data[info_at + 9])
        data[info_at : info_at + info_bytes] = bytes(info_bytes)
        for slot in index_slots:
            struct.pack_into("<Q", data, slot, 0)
        for slot in edited:
            struct.pack_into("<Q", data, slot, 0)
        for name in BOUNDS_FIELDS:
            row = field(c["geometry"]["drawable"], name)
            data[row["offset"] : row["offset"] + row["bytes"]] = source.root_bytes(row["offset"], row["bytes"])
        base = template.system
        if ped:
            render_mask = ped_render_mask(template, material_placement, rows, c)
            if render_mask is not None:
                struct.pack_into("<I", data, drawable_field(c, "RenderMaskFlagsHigh"), render_mask["written"])

    arena = ObjectArena(limit=64 * 1024 * 1024)
    arena.add("template", bytes(data))
    if prop is not None:
        arena.pointer(ROOT_NAME, arena.add("name", root_report["name"].encode("ascii") + b"\0"))
    if light_records:
        arena.pointer(ROOT_LIGHTS, arena.add("lights", b"".join(light_records)))
    models_at = arena.include("models", model_blob, model_placement)
    materials_at = arena.include("materials", material_blob, material_placement)
    links = []
    for index, link in enumerate(material_placement["externalTextureLinks"]):
        ref = bytearray(reference)
        ref_at = arena.add(f"texture-ref-{index}", bytes(ref))
        name = link["textureName"].encode("ascii") + b"\0"
        text_at = arena.add(f"texture-name-{index}", name)
        arena.pointer(ref_at + name_at, text_at)
        arena.bind(materials_at + link["offset"], ref_at)
        shader = str(material_placement["shaders"][link["shaderIndex"]]["nameHash"])
        parameter = c["shader"]["mappings"][shader]["parameters"][str(link["nativeNameHash"])]["name"]
        links.append(
            {
                "shaderIndex": link["shaderIndex"],
                "textureIndex": link["textureIndex"],
                "name": link["textureName"],
                "parameter": parameter,
            }
        )
    count = len(model_placement["models"])
    items_at = arena.add("high-items", bytes(8 * count))
    for i, model in enumerate(model_placement["models"]):
        if model["lod"] != "High":
            raise AssetError("only the source's high model list is converted")
        arena.pointer(items_at + 8 * i, models_at + model["systemOffset"])
    list_at = arena.add("high-list", struct.pack("<QHHI", 0, count, count, 0))
    arena.pointer(list_at, items_at)
    largest = max(b["bytes"] for b in arena.blocks.values())
    page = resource_page_layout.page_size_for(largest)
    estimate = sum(b["bytes"] for b in arena.blocks.values()) // page + 3
    info_new = arena.add("pages-info", bytes(PAGE_INFO_HEADER + PAGE_INFO_RECORD * estimate))
    arena.pointer(8, info_new)
    arena.pointer(drawable_field(c, "ShaderGroupPointer"), materials_at + material_placement["rootOffset"])
    arena.pointer(drawable_field(c, "DrawableModelsHighPointer"), list_at)
    paged, remap = arena.paginate(page)
    if remap(0) != 0:
        raise AssetError("template block moved off offset 0")
    payload, _ = paged.render(SYS)
    payload = bytearray(payload)
    pages = paged.pages
    if len(pages) > estimate:
        raise AssetError("page-info reservation is too small")
    payload[remap(info_new) + 8] = len(pages)
    payload[remap(info_new) + 9] = 0
    system_flags = (int(template.header["systemFlags"], 16) & 0xF0000000) | resource_page_layout.flags_for_pages(pages)
    graphics_flags = int(template.header["graphicsFlags"], 16) & 0xF0000000
    if resource_page_layout.pc_pages(system_flags & 0x0FFFFFFF) != pages:
        raise AssetError("page flags do not reproduce the page list")
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    blob = struct.pack("<4sIII", b"RSC7", TARGET_VERSION, system_flags, graphics_flags)
    blob += packer.compress(bytes(payload)) + packer.flush()
    report = {
        "kind": "gtavmenu-pc-drawable-conversion",
        "source": {"sha256": digest(source_blob), "version": source.header["version"], "bytes": len(source_blob)},
        "template": {"sha256": digest(template.blob), "header": template.header},
        "shaderTemplates": [{"sha256": digest(t.blob), "label": t.label} for t in shader_templates],
        "skeleton": skeleton,
        "shaders": [
            {
                "index": row["index"],
                "name": shader_names[str(row["nameHash"])],
                "nameHash": f"0x{row['nameHash']:08x}",
                "sourceParameters": sum(p["sourceIndex"] is not None for p in row["parameters"]),
                "templateDefaults": [
                    f"0x{p['nativeNameHash']:08x}"
                    for p in row["parameters"]
                    if p.get("initialization") == TEMPLATE_DEFAULT
                ],
                "nullTextures": [
                    f"0x{p['nativeNameHash']:08x}"
                    for p in row["parameters"]
                    if p["sourceIndex"] is None and p["kind"] == "Texture"
                ],
            }
            for row in material_placement["shaders"]
        ],
        "textureLinks": links,
        "models": [
            {"geometries": len(m["geometryIndices"]), "skeletonBinding": m["semantics"]["fields"]["SkeletonBinding"]}
            for m in model_placement["models"]
        ],
        "geometries": [
            {k: row[k] for k in ("vertexCount", "vertexStride", "indexCount", "shaderIndex")}
            | {"bones": len(row["boneIds"])}
            for row in mesh_placement["geometries"]
        ],
        "replacedTemplateModels": len(old_models),
        "clearedIndexDataPointers": len(index_slots),
        **({"prop": root_report, "dropped": dropped, "renamedTextures": renamed} if prop else {}),
        **({"renamedTextures": renamed} if renamed and not prop else {}),
        **({"lights": light_report(light_records)} if light_records else {}),
        **({"sourceEntry": entry} if entry else {}),
        **({"zeroTriangleCountGeometries": source.zero_count_geometries} if source.zero_count_geometries else {}),
        **({"ignoredGeometryVertexPointers": source.ignored_vertex_pointers} if source.ignored_vertex_pointers else {}),
        **({"normalizedDeclarationTypes": source.normalized_declarations} if source.normalized_declarations else {}),
        **({"retexturedParameters": retextured} if retextured else {}),
        **({"substitutedShaders": substituted} if substituted else {}),
        **({"droppedSourceParameters": unmapped} if unmapped else {}),
        **({"embeddedTextures": [t["name"] for t in embedded]} if embedded else {}),
        **({"droppedLowerLodModels": lower_ped} if lower_ped else {}),
        **({"renderMaskHigh": {k: f"0x{v:x}" for k, v in render_mask.items()}} if render_mask else {}),
        "pages": pages,
        "flags": [f"0x{system_flags:08x}", f"0x{graphics_flags:08x}"],
        "output": {"sha256": digest(blob), "bytes": len(blob)},
    }
    verify(
        blob,
        template,
        source,
        rows,
        material,
        model_placement,
        links,
        reference,
        edited,
        index_slots,
        c,
        base,
        render_mask=render_mask and render_mask["written"],
    )
    if prop is not None:
        verify_lights(blob, source.lights()[: len(light_records)])
    report["verified"] = True
    if embedded:
        report["_embedded"] = embedded  # popped by callers before writing the report
    return blob, report


def verify(
    blob,
    template,
    source,
    rows,
    material,
    model_placement,
    links,
    reference,
    edited,
    index_slots,
    c,
    base=None,
    *,
    render_mask: int | None = None,
) -> None:
    """Re-read the written resource and compare it with the source and the template. `render_mask`
    (ped route, ped_render_mask) is the high render mask written over the template's."""
    header, payload = decode_resource(blob, 1 << 28)
    if resource_header(blob)["version"] != TARGET_VERSION or header["graphicsBytes"]:
        raise AssetError("written drawable header/version differs")
    system = payload[: header["systemBytes"]]

    def u64(at):
        return struct.unpack_from("<Q", system, at)[0]

    # Template bytes are unchanged outside the edited slots, bounds, page info and cleared index pointers
    # (prop route: the rebuilt root is unchanged outside its pointer slots).
    base = template.system if base is None else base
    skip = set()
    for slot in [*edited, *index_slots]:
        skip.update(range(slot, slot + 8))
    if base is template.system:
        info = struct.unpack_from("<Q", template.system, 8)[0] - SYS
        skip.update(
            range(
                info,
                info + PAGE_INFO_HEADER + PAGE_INFO_RECORD * (template.system[info + 8] + template.system[info + 9]),
            )
        )
        for name in BOUNDS_FIELDS:
            row = field(c["geometry"]["drawable"], name)
            skip.update(range(row["offset"], row["offset"] + row["bytes"]))
    if render_mask is not None:
        at = drawable_field(c, "RenderMaskFlagsHigh")
        skip.update(range(at, at + 4))
        if struct.unpack_from("<I", system, at)[0] != render_mask:
            raise AssetError("written high render mask differs from the shaders' render buckets")
    if any(system[i] != base[i] for i in range(len(base)) if i not in skip):
        raise AssetError("template bytes changed outside the edited fields")
    for name in BOUNDS_FIELDS:
        row = field(c["geometry"]["drawable"], name)
        if system[row["offset"] : row["offset"] + row["bytes"]] != source.root_bytes(row["offset"], row["bytes"]):
            raise AssetError("drawable bounds differ from the source")
    # Shaders: schema from the bank, names, texture references.
    group = u64(drawable_field(c, "ShaderGroupPointer")) - SYS
    shaders = gen9_materials.read_group(system, SYS, group, c["shader"])
    if [f"0x{s['header']['Name']:08x}" for s in shaders] != [s["nameHash"] for s in material["shaders"]]:
        raise AssetError("written shader names differ from the source")
    size = len(reference)
    name_at = field(c["material"]["textureBase"], "namePointer")["offset"]
    seen = []
    for index, row in enumerate(shaders):
        for slot, pointer in enumerate(row["texturePointers"]):
            if not pointer:
                continue
            raw = bytearray(system[pointer - SYS : pointer - SYS + size])
            text_at = struct.unpack_from("<Q", raw, name_at)[0] - SYS
            raw[name_at : name_at + 8] = bytes(8)
            if bytes(raw) != reference:
                raise AssetError("written texture reference differs from the template object")
            seen.append(
                {
                    "shaderIndex": index,
                    "textureIndex": slot,
                    "name": system[text_at : system.index(b"\0", text_at)].decode(),
                }
            )
    expected = [{k: r[k] for k in ("shaderIndex", "textureIndex", "name")} for r in links]
    if sorted(seen, key=lambda r: (r["shaderIndex"], r["textureIndex"])) != sorted(
        expected, key=lambda r: (r["shaderIndex"], r["textureIndex"])
    ):
        raise AssetError("written texture names differ from the source")
    # Models and geometry: every source byte recovered.
    lst = u64(drawable_field(c, "DrawableModelsHighPointer")) - SYS
    array, count, capacity, _ = struct.unpack_from("<QHHI", system, lst)
    if count != capacity or count != len(model_placement["models"]):
        raise AssetError("written high list count differs")
    mesh = c["mesh"]
    layouts = mesh["layouts"]
    by_key = {(r["modelIndex"], r["geometryIndex"]): r for r in rows}
    for mi in range(count):
        model = gen9_models.read(system, SYS, u64(array - SYS + 8 * mi) - SYS, c["model"])
        if gen9_models.semantics(model) != model_placement["models"][mi]["semantics"]:
            raise AssetError("written model fields differ from the source")
        for gi, pointer in enumerate(model["geometryPointers"]):
            src = by_key[(model_placement["models"][mi]["modelIndex"], gi)]
            g = gen9_mesh.get(system, pointer - SYS, layouts["geometry"])
            vb = gen9_mesh.get(system, g["VertexBufferPointer"] - SYS, layouts["VertexBuffer"])
            ib = gen9_mesh.get(system, g["IndexBufferPointer"] - SYS, layouts["IndexBuffer"])
            stride, n = vb["VertexStride"], vb["VertexCount"]
            raw = system[vb["DataPointer1"] - SYS : vb["DataPointer1"] - SYS + n * stride]
            decl = system[vb["InfoPointer"] - SYS : vb["InfoPointer"] - SYS + layouts["declaration"]["bytes"]]
            legacy, components = gen9_mesh.inverse_vertices(raw, decl, stride, mesh)
            indices = system[ib["IndicesPointer"] - SYS : ib["IndicesPointer"] - SYS + ib["IndicesCount"] * 2]
            bones = (
                list(struct.unpack_from(f"<{g['BoneIdsCount']}H", system, g["BoneIdsPointer"] - SYS))
                if g["BoneIdsCount"]
                else []
            )
            if (
                digest(legacy) != src["vertexStreams"][0]["sha256"]
                or components != src["components"]
                or digest(indices) != src["indices"]["sha256"]
                or bones != src["boneIds"]
                or g["VertexDataPointer"] != vb["DataPointer1"]
                or (g["VerticesCount"], g["VertexStride"], g["IndicesCount"]) != (n, stride, ib["IndicesCount"])
                or model["shaderMapping"][gi] != src["shaderIndex"]
            ):
                raise AssetError("written geometry differs from the source")
            for p in (vb["G9_SRVPointer"], ib["G9_SRVPointer"], vb["InfoPointer"]):
                if not SYS <= p < SYS + len(system):
                    raise AssetError("written buffer view/declaration pointer leaves system storage")
    # New objects carry only arena fixups (system pointers); the template part must not keep any
    # pointer into the dropped graphics pages (vertex data may hold any bit pattern, so it is not scanned).
    if ROOT_NAME in edited:
        text = u64(ROOT_NAME) - SYS
        if not 0 <= text < len(system) or not system[text : system.index(b"\0", text)].endswith(b".#dr"):
            raise AssetError("written drawable name is missing")
    if any(GFX <= u64(o) < GFX + template.header["graphicsBytes"] for o in range(0, len(base) - 7, 8)):
        raise AssetError("written drawable still references the template's graphics pages")
    for name, count in (("BoundingCenter", 4), ("BoundingBoxMin", 3), ("BoundingBoxMax", 3)):
        if not all(math.isfinite(v) for v in struct.unpack_from(f"<{count}f", system, drawable_field(c, name))):
            raise AssetError("written bounding sphere is not finite")


def verify_lights(blob: bytes, source_records: list[bytes]) -> None:
    """Re-read the written light list: count == capacity == the source's, and every record equals the
    source record byte for byte outside Unknown_0h/_4h (which must read zero)."""
    header, payload = decode_resource(blob, 1 << 28)
    system = payload[: header["systemBytes"]]
    pointer, count, capacity = struct.unpack_from("<QHH", system, ROOT_LIGHTS)
    if (count, capacity) != (len(source_records), len(source_records)) or bool(pointer) != bool(source_records):
        raise AssetError("written light list count/capacity/pointer differs from the source")
    if not source_records:
        return
    at = pointer - SYS
    if at < 0 or at % 4 or at + count * LIGHT_BYTES > len(system):
        raise AssetError("written light list leaves system storage")
    for i, record in enumerate(source_records):
        written = system[at + i * LIGHT_BYTES : at + (i + 1) * LIGHT_BYTES]
        if any(written[o : o + 4] != bytes(4) for o in LIGHT_CLEARED):
            raise AssetError("written light Unknown_0h/_4h is not zero")
        if any(written[o] != record[o] for o in range(8, LIGHT_BYTES)):
            raise AssetError(f"written light {i} differs from the source")


def ytd_textures(ytd: bytes, max_size: int | None = None, *, skip_bad: bool = False) -> tuple[list, list]:
    """(textures, skipped) of a PC .ytd in convert_pc_ytd_writer.convert's input shape. With max_size
    the mip chains are first trimmed (convert_pc_ytd_writer.trim_mips: top mips above it and the
    sub-block tail that the Legacy layout cannot describe). Without skip_bad the whole-dictionary
    admission of read_pc_texture_dictionary applies; with it, unusable textures are left out."""
    from dataclasses import replace

    import convert_pc_ytd_writer
    import pc_gen9_resources
    from gtavmenu_tools.texture_conversion import read_pc_texture_dictionary

    if pc_gen9_resources.resource_family(ytd, ".ytd") == "gen9":  # Enhanced PC build (.ytd v5)
        return pc_gen9_resources.gen9_ytd_textures(ytd, max_size, skip_bad=skip_bad)
    if max_size is not None:
        ytd, _trims = convert_pc_ytd_writer.trim_mips(ytd, max_size)
    limits = replace(Limits(), max_file_bytes=1 << 28, max_total_bytes=1 << 31, max_entries=4096)
    if not skip_bad:
        return list(read_pc_texture_dictionary(ytd, limits)["textures"]), []
    header, payload = decode_resource(ytd, limits.max_total_bytes)
    if header["version"] != 13:
        raise AssetError("a .ytd must be a Legacy texture dictionary (version 13)")
    view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
    return legacy_textures(view, SYS, limits, skip_bad=True)


def texture_dictionary(
    links: list[dict], sources: list[list[dict]], templates: Path | None, max_size: int | None, *, only_linked: bool
) -> tuple[bytes, dict]:
    """One PS5 dictionary from several PC texture lists (earlier lists win a name, as a drawable's
    embedded textures win over its archetype's .ytd on PC). `links` are textureLinks rows of the
    drawables that will use it: a linked name no list has gets a neutral stand-in for its parameter
    (refused for other parameters); with `only_linked` unlinked textures are left out."""
    import convert_pc_ytd_writer

    chosen: dict[str, dict] = {}
    conflicts = []
    for textures in sources:
        for texture in textures:
            texture = texture | {"name": ps5_texture_name(texture["name"])}
            key = texture["name"].lower()
            if key in chosen:
                if chosen[key]["mips"] != texture["mips"] or chosen[key]["format"] != texture["format"]:
                    conflicts.append(texture["name"])
                continue
            chosen[key] = texture
    linked = {link["name"].lower() for link in links}
    missing: dict[str, tuple[str, str]] = {}
    for link in links:
        if link["name"].lower() in chosen:
            continue
        if link["parameter"] not in NEUTRAL_TEXTURES:
            raise AssetError(f"no texture {link['name']} ({link['parameter']}) and no neutral stand-in for it")
        if missing.setdefault(link["name"].lower(), (link["name"], link["parameter"]))[1] != link["parameter"]:
            raise AssetError(f"{link['name']} is missing and bound to different parameters")
    kept = [t for key, t in sorted(chosen.items()) if not only_linked or key in linked]
    if not kept and not missing:
        raise AssetError("texture dictionary would be empty")
    textures = [trim_texture(t, max_size) for t in kept]
    textures += [
        convert_pc_ytd_writer.solid_texture(name, NEUTRAL_TEXTURES[parameter]) for name, parameter in missing.values()
    ]
    ptd, rows = convert_pc_ytd_writer.convert({"textures": textures}, templates)
    keys = ("name", "sourceFormat", "format", "width", "height", "mipLevels", "tileMode")
    return ptd, {
        "textures": [dict(zip(keys, row, strict=True)) for row in rows],
        "nameConflicts": sorted(conflicts),
        "leftOut": sorted(key for key in chosen if key not in {t["name"].lower() for t in kept}),
        "neutralTextures": [
            {"name": name, "parameter": parameter, "rgba": list(NEUTRAL_TEXTURES[parameter])}
            for name, parameter in sorted(missing.values())
        ],
        "sha256": digest(ptd),
        "bytes": len(ptd),
    }


def write_new(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(data)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, required=True, help="PC Legacy drawable (.ydr)")
    parser.add_argument(
        "--template", type=Path, required=True, help="retail PS5 drawable of the same model (loose RSC7)"
    )
    parser.add_argument(
        "--shader-template", type=Path, action="append", default=[], help="more retail drawables for shader schemas"
    )
    parser.add_argument("--output", type=Path, required=True, help="new .pdr")
    parser.add_argument(
        "--reference-dir", type=Path, help="not read (the layouts are frozen in data/drawable_contracts); accepted"
    )
    parser.add_argument("--ytd", type=Path, help="PC texture dictionary to convert alongside")
    parser.add_argument("--ptd-output", type=Path, help="new .ptd for --ytd")
    parser.add_argument("--max-texture-size", type=int, help="--ytd: drop mips above this size (and below one block)")
    parser.add_argument("--templates", type=Path, help="--ytd: retail template cache (fetch_retail_templates.py)")
    parser.add_argument(
        "--prop",
        metavar="NAME",
        help="prop route for a model with no same-name retail drawable: --template is any retail drawable "
        "(only its root words are kept); NAME is the drawable name (the archetype's). Embedded textures go "
        "to --ptd-output (with --ytd's)",
    )
    parser.add_argument(
        "--no-lights", action="store_true", help="--prop: drop the source's light attributes instead of writing them"
    )
    parser.add_argument("--max-lights", type=int, help="--prop: write only the first N light attributes")
    parser.add_argument(
        "--shader-substitute",
        action="append",
        default=[],
        metavar="FROM=TO",
        help="retarget a source shader no retail template carries (names, e.g. vehicle_vehglass="
        "weapon_normal_spec_alpha; `auto`: the known weapon substitutes): its textures stay, constants are the "
        "retail instance's",
    )
    parser.add_argument(
        "--first-carrier",
        action="store_true",
        help="each shader's schema from the first template that carries it (template, then --shader-template in "
        "order) instead of requiring every carrier to agree",
    )
    args = parser.parse_args(argv)
    if not args.prop and bool(args.ytd) != bool(args.ptd_output):
        parser.error("--ytd and --ptd-output go together")
    if args.prop and args.ytd and not args.ptd_output:
        parser.error("--ytd needs --ptd-output")
    for path in (args.output, args.output.with_name(args.output.name + ".report.json"), args.ptd_output):
        if path is not None and path.exists():
            raise SystemExit(f"refusing to overwrite {path}")
    try:
        c = contracts(args.reference_dir)
        template = Template(args.template.read_bytes(), c, args.template.name)
        extra = [Template(p.read_bytes(), c, p.name) for p in args.shader_template]
        blob, report = convert(
            args.source.read_bytes(),
            template,
            extra,
            c,
            prop=args.prop,
            lights=not args.no_lights,
            max_lights=args.max_lights,
            shader_substitutes=args.shader_substitute,
            first_carrier=args.first_carrier,
        )
        embedded = report.pop("_embedded", [])
        ptd = None
        if args.prop and args.ptd_output:
            sources = [embedded] + ([ytd_textures(args.ytd.read_bytes(), args.max_texture_size)[0]] if args.ytd else [])
            ptd, report["textures"] = texture_dictionary(
                report["textureLinks"], sources, args.templates, args.max_texture_size, only_linked=False
            )
            report["neutralTextures"] = report["textures"]["neutralTextures"]
        elif args.prop and embedded:
            raise AssetError("the source embeds textures; give --ptd-output for them")
        elif args.ytd:
            import convert_pc_ytd_writer

            ytd = args.ytd.read_bytes()
            header, payload = decode_resource(ytd, 1 << 30)
            listed = [row["name"] for row in inspect_legacy_dictionary(payload, header, Limits())["textures"]]
            names = {ps5_texture_name(name).lower() for name in listed}
            missing = {}
            for link in report["textureLinks"]:
                if link["name"].lower() not in names:
                    if link["parameter"] not in NEUTRAL_TEXTURES:
                        raise AssetError(f"--ytd lacks {link['name']} ({link['parameter']}); no neutral stand-in")
                    if (
                        missing.setdefault(link["name"].lower(), (link["name"], link["parameter"]))[1]
                        != link["parameter"]
                    ):
                        raise AssetError(f"{link['name']} is missing and bound to different parameters")
            extra = [
                convert_pc_ytd_writer.solid_texture(name, NEUTRAL_TEXTURES[parameter])
                for name, parameter in sorted(missing.values())
            ]
            if all(ps5_texture_name(name) == name for name in listed):
                ptd, textures = convert_pc_ytd_writer.convert_file(ytd, args.templates, args.max_texture_size, extra)
            else:  # the drawable's references were renamed by ps5_texture_name; the dictionary follows
                renamed_rows = [
                    t | {"name": ps5_texture_name(t["name"])} for t in ytd_textures(ytd, args.max_texture_size)[0]
                ]
                ptd, rows = convert_pc_ytd_writer.convert({"textures": renamed_rows + extra}, args.templates)
                keys = ("name", "sourceFormat", "format", "width", "height", "mipLevels", "tileMode")
                textures = {"textures": [dict(zip(keys, row, strict=True)) for row in rows]}
            report["textures"] = textures | {"sha256": digest(ptd), "bytes": len(ptd)}
            report["neutralTextures"] = [
                {"name": name, "parameter": parameter, "rgba": list(NEUTRAL_TEXTURES[parameter])}
                for name, parameter in sorted(missing.values())
            ]
    except (AssetError, OSError, ValueError, KeyError, struct.error) as error:
        raise SystemExit(f"error: {error}") from None
    write_new(args.output, blob)
    if ptd is not None:
        write_new(args.ptd_output, ptd)
    write_new(args.output.with_name(args.output.name + ".report.json"), canonical(report))
    print(
        f"wrote {args.output} bytes={len(blob)} pages={report['pages']} shaders="
        + ",".join(s["name"] for s in report["shaders"])
        + f" geometries={len(report['geometries'])}"
        + (f" lights={report['lights']['count']}" if "lights" in report else "")
        + (f"; {args.ptd_output} bytes={len(ptd)} textures={len(report['textures']['textures'])}" if ptd else "")
    )
    for row in report.get("substitutedShaders", []):
        dropped = f", dropped textures {', '.join(row['droppedTextures'])}" if row["droppedTextures"] else ""
        print(f"note: shader {row['shaderIndex']} {row['from']} written as {row['to']} (textures kept{dropped})")
    for row in report.get("neutralTextures", []):
        print(f"note: --ytd lacks {row['name']} ({row['parameter']}); added a neutral 16x16 stand-in {row['rgba']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

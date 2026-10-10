#!/usr/bin/env python3
"""Read-only structural walk of ped resources: the reference a PC->PS5 ped converter targets.

Input: loose RSC7 members (retail members of your own game, or PC files from an OPEN RPF). The member kind comes from the extension:

  .pdd / .ydd   drawable dictionary (PS5 Gen9 v159 / PC Legacy v165): one entry per component
                drawable; per drawable the skeleton, shaders (name, texture slots, texture names),
                LOD model lists and geometries (vertex count, stride, declaration, bone ids)
  .pdr / .ydr   one drawable (same report as a dictionary entry)
  .pft / .yft   fragment (PS5 v171 / PC v162): the main drawable's skeleton (bone count, tags,
                names), drawable array, physics LOD group (ragdoll children), cloth
  .pmt / .ymt   ped variation info (CPedVariationInfo, PSO-in-resource v2): per component the
                drawables with texture counts, propMask and alternatives; props, comp infos,
                selection sets, the DLC name hash
  .ptd / .ytd   texture dictionary: count, names, formats (PS5 v5 via ps5_texture_reader;
                PC Legacy v13 names only)
  .pld / .yld   cloth dictionary: entry count and names

Field offsets follow the CodeWalker sources at commit 485d56b (Drawable.cs: DrawableBase, ShaderGroup, ShaderFX Gen9/Legacy, ShaderParamInfosG9, Skeleton, Bone,
DrawableModel, DrawableGeometry, VertexBuffer/VertexDeclarationG9) and the pmt layout of
make_ped_apparel_pack.py. Names that are only hashes are resolved against component drawable
patterns and a list of shader / parameter names (--shader-names adds every <Name>/name= of a
CodeWalker shaders-gen9 XML). Nothing is written; no game code runs.

  inspect_ped_resources.py s_m_y_cop_01.pft s_m_y_cop_01.pdd s_m_y_cop_01.pmt [--json] [--brief]
"""

from __future__ import annotations

import argparse
import contextlib
import json
import re
import struct
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

from gtavmenu_tools.asset_formats import AssetError, Limits, decode_resource  # noqa: E402
from gtavmenu_tools.hashes import joaat  # noqa: E402
from gtavmenu_tools.meta_resource import Meta, joaat_cs  # noqa: E402

SYS, GFX = 0x50000000, 0x60000000
COMPONENTS = ("head", "berd", "hair", "uppr", "lowr", "hand", "feet", "teef", "accs", "task", "decl", "jbib")
PROP_ANCHORS = ("p_head", "p_eyes", "p_ears", "p_mouth", "p_lhand", "p_rhand", "p_lwrist", "p_rwrist", "p_hip")
PROP_ANCHORS += ("p_lfoot", "p_rfoot", "ph_lhand", "ph_rhand")
# Ped and common shader names (CodeWalker shaders-gen9 485d56b <Name> values); hashes resolve to them.
SHADER_NAMES = [
    "ped",
    "ped_alpha",
    "ped_cloth",
    "ped_cloth_enveff",
    "ped_decal",
    "ped_decal_decoration",
    "ped_decal_exp",
    "ped_decal_nodiff",
    "ped_default",
    "ped_default_cloth",
    "ped_default_enveff",
    "ped_default_mp",
    "ped_default_palette",
    "ped_emissive",
    "ped_enveff",
    "ped_fur",
    "ped_hair_cutout_alpha",
    "ped_hair_spiked",
    "ped_nopeddamagedecals",
    "ped_palette",
    "ped_wrinkle",
    "ped_wrinkle_cloth",
    "ped_wrinkle_cloth_enveff",
    "ped_wrinkle_cs",
    "ped_wrinkle_enveff",
    "cloth_default",
    "cloth_normal_spec",
    "cloth_normal_spec_tnt",
    "cloth_spec_alpha",
    "default",
    "normal",
    "normal_spec",
    "spec",
    "cutout_hard",
    "emissive",
    "decal",
    "vehicle_mesh",
    "vehicle_paint1",
    "vehicle_tire",
    "vehicle_vehglass",
    "vehicle_lightsemissive",
    "vehicle_badges",
    "vehicle_decal",
    "vehicle_interior2",
    "vehicle_shuts",
    "vehicle_dash_emissive",
    "vehicle_licenseplate",
    "vehicle_vehglass_inner",
]
PARAM_NAMES = [
    "DiffuseTex",
    "DiffuseSampler",
    "BumpTex",
    "BumpSampler",
    "SpecularTex",
    "SpecularSampler",
    "DetailTex",
    "DetailSampler",
    "DiffuseNoBorderTexSampler",
    "WrinkleMaskTex_0",
    "WrinkleMaskTex_1",
    "WrinkleMaskTex_2",
    "WrinkleMaskTex_3",
    "WrinkleTex_A",
    "WrinkleTex_B",
    "WrinkleSampler_A",
    "WrinkleSampler_B",
    "WrinkleMaskSampler_0",
    "WrinkleMaskSampler_1",
    "WrinkleMaskSampler_2",
    "WrinkleMaskSampler_3",
    "PedEnvEffTex",
    "EnvEffTexTileUV",
    "EnvEffThickness",
    "DiffuseTexPal",
    "TextureSamplerDiffPal",
    "StubbleTex",
    "StubbleSampler",
    "ComboHairTexture",
    "depthbuffertex",
    "PedDamageTex",
    "BloodTex",
    "TattooTex",
    "ClothNormalTex",
    "ClothNoiseTex",
    "anisoNoiseSpecSampler",
    "noiseSampler",
    "tintPaletteSampler",
    "TintPaletteTex",
]
# Gen9 declaration slot -> semantic (VertexDeclarationG9.GetLegacyComponentIndex comment table).
G9_SEMANTICS = {0: "POSITION", 4: "NORMAL", 8: "TANGENT", 16: "BLENDWEIGHT", 20: "BLENDINDICES", 24: "COLOR0"}
G9_SEMANTICS.update({25: "COLOR1"} | {28 + i: f"TEXCOORD{i}" for i in range(8)})
G9_FORMATS = {2: "f32x4", 6: "f32x3", 10: "f16x4", 16: "f32x2", 24: "r10g10b10a2", 28: "unorm8x4", 30: "uint8x4"}
G9_FORMATS[34] = "f16x2"
# Legacy declaration flag bits (VertexDeclarationTypes.GTAV1 component order).
LEGACY_SEMANTICS = ("POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "COLOR0", "COLOR1")
LEGACY_SEMANTICS += (*tuple(f"TEXCOORD{i}" for i in range(8)), "TANGENT", "BINORMAL")
# CPedVariationInfo (PSO struct hashes and offsets as in make_ped_apparel_pack.py).
VARIATION_INFO = 0x16760659
VI_AVAIL, VI_COMPONENTS, VI_SELECTION_SETS, VI_COMP_INFOS, VI_PROPS, VI_DLC = 0x04, 0x10, 0x20, 0x30, 0x40, 0x68
PROP_META, PROP_ANCHOR_ARRAY = 0x08, 0x18
RACE_MASK = 0x10
_LIMITS = Limits(max_file_bytes=1 << 28, max_total_bytes=1 << 30, max_metadata_bytes=64 << 20)
_PRINTABLE = re.compile(rb"[\x20-\x7e]{1,128}")


class PedResourceError(ValueError):
    pass


class Res:
    """Bounded pointer view of an inflated RSC7 payload (system 0x5..., graphics 0x6...)."""

    def __init__(self, blob: bytes):
        try:
            self.header, self.payload = decode_resource(blob, _LIMITS.max_total_bytes)
        except AssetError as error:
            raise PedResourceError(str(error)) from error
        self.version = self.header["version"]
        self.system = self.header["systemBytes"]
        self.graphics = self.header["graphicsBytes"]

    def off(self, pointer: int, size: int = 1) -> int:
        if pointer >= SYS and pointer + size <= SYS + self.system:
            return pointer - SYS
        if pointer >= GFX and pointer + size <= GFX + self.graphics:
            return self.system + pointer - GFX
        raise PedResourceError(f"pointer {pointer:#x} outside the resource")

    def u(self, pointer: int, fmt: str, delta: int = 0):
        size = struct.calcsize(fmt)
        return struct.unpack_from(fmt, self.payload, self.off(pointer + delta, size))[0]

    def q(self, pointer: int, delta: int = 0) -> int:
        return self.u(pointer, "<Q", delta)

    def raw(self, pointer: int, size: int) -> bytes:
        at = self.off(pointer, size)
        return self.payload[at : at + size]

    def cstr(self, pointer: int) -> str | None:
        if not pointer:
            return None
        at = self.off(pointer)
        match = _PRINTABLE.match(self.payload, at)
        if not match or self.payload[match.end() : match.end() + 1] != b"\0":
            return None
        return match.group().decode("ascii")

    def where(self, pointer: int) -> str:
        if not pointer:
            return "null"
        if SYS <= pointer < SYS + self.system:
            return "system"
        if GFX <= pointer < GFX + self.graphics:
            return "graphics"
        return "outside"


class Names:
    """Hash -> name for component drawables, props, shaders and shader parameters."""

    def __init__(self, extra: list[str] | None = None):
        words = list(SHADER_NAMES) + list(PARAM_NAMES) + list(extra or [])
        for comp in COMPONENTS:
            for index in range(256):
                for suffix in ("u", "r", "m"):
                    words.append(f"{comp}_{index:03d}_{suffix}")
                    words.extend(f"{comp}_{index:03d}_{suffix}_{alt}" for alt in range(1, 4))
        for anchor in PROP_ANCHORS:
            words.extend(f"{anchor}_{index:03d}" for index in range(256))
        self.table = {}
        for word in words:
            self.table.setdefault(joaat(word), word)
            self.table.setdefault(joaat_cs(word), word)

    def __call__(self, value: int) -> str:
        return self.table.get(value, f"0x{value:08x}")


def shader_names_from_xml(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return re.findall(r"<Name>([^<]{1,80})</Name>", text) + re.findall(r'(?:name|old)="([^"]{1,80})"', text)


# ---- drawables ----------------------------------------------------------------------------------


def _texture_name(r: Res, texture: int) -> str | None:
    """TextureBase name pointer at +0x28 (Legacy and Gen9 texture objects)."""
    if not texture or r.where(texture) != "system":
        return None
    return r.cstr(r.q(texture, 0x28))


def shader(r: Res, pointer: int, names: Names) -> dict:
    gen9 = r.version in (159, 171)
    if gen9:
        name_hash, preset = r.u(pointer, "<I", 0x00), r.u(pointer, "<I", 0x04)
        texture_refs, infos = r.q(pointer, 0x10), r.q(pointer, 0x20)
        bucket = r.u(pointer, "<B", 0x39)
        out = {"name": names(name_hash), "preset": f"0x{preset:08x}", "renderBucket": bucket}
        if not infos:
            return out | {"textures": {}, "params": 0}
        n_buf, n_tex, n_unk, n_smp, n_par = (r.u(infos, "<B", i) for i in range(5))
        textures, cbuffer = {}, []
        for i in range(n_par):
            param_hash, data = r.u(infos, "<I", 8 + i * 8), r.u(infos, "<I", 12 + i * 8)
            kind = data & 3
            if kind == 0:
                slot = (data >> 2) & 0xFF
                texture = r.q(texture_refs, slot * 8) if texture_refs and slot < n_tex else 0
                textures[names(param_hash)] = _texture_name(r, texture) or ("-" if not texture else "?")
            elif kind == 3:
                cbuffer.append(names(param_hash))
        counts = {"buffers": n_buf, "textures": n_tex, "unknowns": n_unk, "samplers": n_smp, "params": n_par}
        return out | {"counts": counts, "textures": textures, "cbuffer": cbuffer}
    params, name_hash, count = r.q(pointer, 0x00), r.u(pointer, "<I", 0x08), r.u(pointer, "<B", 0x10)
    file_hash = r.u(pointer, "<I", 0x18)
    textures = {}
    if params and count:
        # 16-byte rows {u8 DataType, ..., u64 data}, then the embedded Vector4 data (16 bytes per
        # DataType unit), then one u32 name hash per row (CodeWalker ShaderParametersBlock Legacy).
        units = sum(r.u(params, "<B", i * 16) for i in range(count))
        hashes_at = params + count * 16 + units * 16
        for i in range(count):
            data_type, data = r.u(params, "<B", i * 16), r.q(params, i * 16 + 8)
            if data_type == 0:
                param_hash = r.u(hashes_at, "<I", i * 4)
                textures[names(param_hash)] = _texture_name(r, data) or ("-" if not data else "?")
    return {"name": names(name_hash), "file": f"0x{file_hash:08x}", "params": count, "textures": textures}


def declaration(r: Res, vb: int) -> tuple[int, int, int, list[str]]:
    """(vertex count, stride, vertex data pointer, semantics) of a vertex buffer."""
    if r.version in (159, 171):
        count, stride = r.u(vb, "<I", 0x08), r.u(vb, "<H", 0x0C)
        data, decl = r.q(vb, 0x18), r.q(vb, 0x38)
        types = r.raw(decl + 260, 52)
        sems = [f"{G9_SEMANTICS.get(i, f'slot{i}')}:{G9_FORMATS.get(t, t)}" for i, t in enumerate(types) if t]
        return count, stride, data, sems
    stride, data, count, decl = r.u(vb, "<H", 0x08), r.q(vb, 0x10), r.u(vb, "<I", 0x18), r.q(vb, 0x30)
    flags = r.u(decl, "<I", 0x00)
    sems = [LEGACY_SEMANTICS[i] for i in range(16) if flags >> i & 1]
    return count, stride, data, sems


def models(r: Res, model_list: int) -> list[dict]:
    if not model_list:
        return []
    array, count = r.q(model_list), r.u(model_list, "<H", 8)
    out = []
    for m in range(count):
        model = r.q(array, 8 * m)
        geometries, n_geo = r.q(model, 0x08), r.u(model, "<H", 0x10)
        shader_map = r.q(model, 0x20)
        rows = []
        for g in range(n_geo):
            geometry = r.q(geometries, 8 * g)
            vertices, stride, data, sems = declaration(r, r.q(geometry, 0x18))
            ib = r.q(geometry, 0x38)
            indices = r.u(ib, "<I", 0x08)
            index_data = r.q(ib, 0x18 if r.version in (159, 171) else 0x10)
            bone_ids = r.u(geometry, "<H", 0x72)
            shader_index = r.u(shader_map, "<H", 2 * g) if shader_map else None
            rows.append(
                {
                    "shader": shader_index,
                    "vertices": vertices,
                    "indices": indices,
                    "stride": stride,
                    "boneIds": bone_ids,
                    "pages": f"{r.where(data)}/{r.where(index_data)}",
                    "declaration": sems,
                }
            )
        binding = r.u(model, "<I", 0x28)
        out.append({"skinned": bool(binding & 0xFF) or None, "binding": f"0x{binding:08x}", "geometries": rows})
    return out


def skeleton(r: Res, pointer: int, bone_names: bool) -> dict | None:
    if not pointer:
        return None
    bones, count = r.q(pointer, 0x20), r.u(pointer, "<H", 0x5E)
    tags, tag_capacity, tag_count = r.q(pointer, 0x10), r.u(pointer, "<H", 0x18), r.u(pointer, "<H", 0x1A)
    out = {
        "bones": count,
        "boneTagCapacity": tag_capacity,
        "boneTagCount": tag_count,
        "tagMap": r.where(tags),
        "childIndices": r.u(pointer, "<H", 0x60),
        "signatures": [f"0x{r.u(pointer, '<I', o):08x}" for o in (0x50, 0x54, 0x58)],
    }
    if bone_names and bones:
        rows = []
        for i in range(count):
            bone = bones + i * 0x50
            parent, tag = r.u(bone, "<h", 0x32), r.u(bone, "<H", 0x44)
            rows.append(f"{i}:{r.cstr(r.q(bone, 0x38)) or '?'}(tag {tag}, parent {parent})")
        out["boneNames"] = rows
    return out


def drawable(r: Res, pointer: int, names: Names, bone_names: bool = False) -> dict:
    shader_group = r.q(pointer, 0x10)
    shaders = []
    embedded = None
    if shader_group:
        embedded = r.where(r.q(shader_group, 0x08))
        array, count = r.q(shader_group, 0x10), r.u(shader_group, "<H", 0x18)
        shaders = [shader(r, r.q(array, 8 * i), names) for i in range(count)]
    lods = {}
    for label, off in (("high", 0x50), ("med", 0x58), ("low", 0x60), ("vlow", 0x68)):
        lods[label] = models(r, r.q(pointer, off))
    name = None
    with contextlib.suppress(PedResourceError):
        name = r.cstr(r.q(pointer, 0xA8))
    return {
        "name": name,
        "skeleton": skeleton(r, r.q(pointer, 0x18), bone_names),
        "embeddedTxd": embedded,
        "shaders": shaders,
        "lodDistances": [round(r.u(pointer, "<f", 0x70 + 4 * i), 2) for i in range(4)],
        "renderMasks": [f"0x{r.u(pointer, '<I', 0x80 + 4 * i):x}" for i in range(4)],
        "joints": r.where(r.q(pointer, 0x90)),
        "lods": lods,
    }


def dictionary(r: Res) -> list[tuple[int, int]]:
    """(hash, value pointer) rows of a pgDictionary root (hashes +0x20, values +0x30)."""
    keys, n_keys = r.q(SYS, 0x20), r.u(SYS, "<H", 0x28)
    values, n_values = r.q(SYS, 0x30), r.u(SYS, "<H", 0x38)
    if n_keys != n_values or n_keys > 4096:
        raise PedResourceError(f"dictionary counts disagree or are too large ({n_keys}/{n_values})")
    return [(r.u(keys, "<I", 4 * i), r.q(values, 8 * i)) for i in range(n_keys)]


def inspect_dictionary(r: Res, names: Names, bone_names: bool) -> dict:
    rows = []
    for key, value in dictionary(r):
        rows.append({"hash": f"0x{key:08x}", "entry": names(key)} | drawable(r, value, names, bone_names))
    return {"kind": "drawable dictionary", "entries": rows}


def inspect_fragment(r: Res, names: Names, bone_names: bool) -> dict:
    frag = SYS
    main = r.q(frag, 0x30)
    out = {"kind": "fragment", "name": r.cstr(r.q(frag, 0x58))}
    out["main"] = drawable(r, main, names, bone_names) if main else None
    # FragType +0xA8 BoneTransforms, +0x60 environment cloth list {ptr, u16 count}.
    out["boneTransforms"] = r.where(r.q(frag, 0xA8))
    out["environmentCloths"] = r.u(frag, "<H", 0x68) if r.q(frag, 0x60) else 0
    out["drawableArray"] = r.u(frag, "<I", 0x48) if r.q(frag, 0x38) else 0
    out["cloth"] = r.where(r.q(frag, 0xF8))
    group = r.q(frag, 0xF0)
    physics = []
    for level, off in enumerate((0x10, 0x18, 0x20), 1):
        lod = r.q(group, off) if group else 0
        if lod:
            physics.append({"lod": level, "children": r.u(lod, "<B", 0x11D), "groups": r.u(lod, "<B", 0x11C)})
    out["physicsLods"] = physics
    return out


# ---- variation info (pmt / ymt) -----------------------------------------------------------------


def inspect_variations(r: Res) -> dict:
    meta = Meta(r.payload)
    if meta.magic != b"0DRP":
        raise PedResourceError("not a parser meta resource (PRD0)")
    found = [at for h, size, at in meta.blocks if h == VARIATION_INFO and size >= 0x70]
    if len(found) != 1:
        raise PedResourceError("no single CPedVariationInfo block")
    b, vi = r.payload, found[0]

    def array(at: int) -> tuple[int | None, int]:
        pointer, count = struct.unpack_from("<QH", b, at)
        return (meta.ref(pointer) if pointer else None), count

    avail = b[vi + VI_AVAIL : vi + VI_AVAIL + 12]
    data, count = array(vi + VI_COMPONENTS)
    components = {}
    for comp, index in enumerate(avail):
        if index == 0xFF or data is None or index >= count:
            continue
        entry = data + index * 0x18
        drawables, n_drawables = array(entry + 8)
        rows = []
        for d in range(n_drawables):
            at = drawables + d * 0x30
            _tex, n_tex = array(at + 8)
            rows.append(
                {"textures": n_tex, "propMask": b[at], "alternatives": b[at + 1], "race": bool(b[at] & RACE_MASK)}
            )
        components[COMPONENTS[comp]] = {"numAvailTex": b[entry], "drawables": rows}
    props_meta = array(vi + VI_PROPS + PROP_META)[1]
    anchors = array(vi + VI_PROPS + PROP_ANCHOR_ARRAY)[1]
    return {
        "kind": "ped variation info",
        "components": components,
        "componentDrawables": sum(len(c["drawables"]) for c in components.values()),
        "componentTextures": sum(d["textures"] for c in components.values() for d in c["drawables"]),
        "compInfos": array(vi + VI_COMP_INFOS)[1],
        "selectionSets": array(vi + VI_SELECTION_SETS)[1],
        "props": {"numAvailProps": b[vi + VI_PROPS], "propMetaData": props_meta, "anchors": anchors},
        "dlcNameHash": f"0x{struct.unpack_from('<I', b, vi + VI_DLC)[0]:08x}",
    }


# ---- textures and cloth -------------------------------------------------------------------------


def inspect_textures(blob: bytes, r: Res, names: Names) -> dict:
    if r.version == 5:
        from gtavmenu_tools.ps5_texture_reader import parse_ps5_texture_dictionary

        try:
            parsed = parse_ps5_texture_dictionary(blob, _LIMITS)
        except AssetError as error:
            # The strict reader covers the formats the texture writer emits; list the rest raw.
            return inspect_textures_raw(r, names) | {"strictReader": str(error)}
        rows = [
            f"{t['name']} {t['width']}x{t['height']} {t['format']} mips={t['mipLevels']}" for t in parsed["textures"]
        ]
        formats = Counter(f"{t['format']}/tile{t['tileMode']}" for t in parsed["textures"])
        return {
            "kind": "texture dictionary",
            "count": parsed["textureCount"],
            "formats": dict(formats),
            "textures": rows,
        }
    # PC Legacy Texture: width/height u16 +0x50/+0x52, D3D format u32 +0x58, levels u8 +0x5D.
    rows, formats = [], Counter()
    for key, value in dictionary(r):
        width, height = r.u(value, "<H", 0x50), r.u(value, "<H", 0x52)
        code, levels = r.u(value, "<I", 0x58), r.u(value, "<B", 0x5D)
        fmt = LEGACY_FORMAT_NAMES.get(code) or (
            struct.pack("<I", code).decode("latin-1") if code > 0xFF else f"d3d{code}"
        )
        formats[fmt] += 1
        rows.append(f"{_texture_name(r, value) or names(key)} {width}x{height} {fmt} mips={levels}")
    return {"kind": "texture dictionary", "count": len(rows), "formats": dict(formats), "textures": rows}


PS5_FORMAT_NAMES = {71: "BC1", 74: "BC2", 77: "BC3", 80: "BC4", 83: "BC5", 98: "BC7", 87: "BGRA8", 28: "RGBA8"}
PS5_FORMAT_NAMES[65] = "A8"
LEGACY_FORMAT_NAMES = {21: "A8R8G8B8", 28: "A8", 50: "L8"}


def inspect_textures_raw(r: Res, names: Names) -> dict:
    """PS5 v5 dictionary without the strict reader: 88-byte texture objects (width/height +24,
    dimension/format/tile mode/AA/levels +30, name +40), every format code reported as found."""
    rows, formats = [], Counter()
    for key, value in dictionary(r):
        width, height = r.u(value, "<H", 24), r.u(value, "<H", 26)
        code, tile, levels = r.u(value, "<B", 31), r.u(value, "<B", 32), r.u(value, "<B", 34)
        fmt = PS5_FORMAT_NAMES.get(code, f"code{code}")
        formats[f"{fmt}/tile{tile}"] += 1
        rows.append(f"{r.cstr(r.q(value, 40)) or names(key)} {width}x{height} {fmt} tile={tile} mips={levels}")
    return {"kind": "texture dictionary", "count": len(rows), "formats": dict(formats), "textures": rows}


def inspect_cloth(r: Res, names: Names) -> dict:
    rows = [names(key) for key, _value in dictionary(r)]
    return {"kind": "cloth dictionary", "count": len(rows), "entries": rows}


KINDS = {
    "pdd": "dict",
    "ydd": "dict",
    "pdr": "drawable",
    "ydr": "drawable",
    "pft": "frag",
    "yft": "frag",
    "pmt": "meta",
    "ymt": "meta",
    "ptd": "txd",
    "ytd": "txd",
    "pld": "cloth",
    "yld": "cloth",
}


def inspect(path: Path, names: Names, bone_names: bool = False) -> dict:
    kind = KINDS.get(path.suffix.lower().lstrip("."))
    if kind is None:
        raise PedResourceError(f"{path.name}: unknown extension (one of {', '.join(sorted(KINDS))})")
    blob = path.read_bytes()
    r = Res(blob)
    head = {"file": path.name, "version": r.version, "systemBytes": r.system, "graphicsBytes": r.graphics}
    if kind == "dict":
        return head | inspect_dictionary(r, names, bone_names)
    if kind == "drawable":
        return head | {"kind": "drawable"} | drawable(r, SYS, names, bone_names)
    if kind == "frag":
        return head | inspect_fragment(r, names, bone_names)
    if kind == "meta":
        return head | inspect_variations(r)
    if kind == "txd":
        return head | inspect_textures(blob, r, names)
    return head | inspect_cloth(r, names)


# ---- text report --------------------------------------------------------------------------------


def _drawable_lines(d: dict, indent: str, brief: bool) -> list[str]:
    lines = []
    skel = d.get("skeleton")
    if skel:
        lines.append(
            f"{indent}skeleton bones={skel['bones']} tags={skel['boneTagCount']}/{skel['boneTagCapacity']} "
            f"tagMap={skel['tagMap']}"
        )
        for row in skel.get("boneNames", []):
            lines.append(f"{indent}  {row}")
    lod_counts = " ".join(f"{k}={len(v)}" for k, v in d["lods"].items())
    lines.append(f"{indent}lods {lod_counts} dist={d['lodDistances']} masks={d['renderMasks']}")
    for i, s in enumerate(d["shaders"]):
        tex = ", ".join(f"{k}={v}" for k, v in s.get("textures", {}).items())
        lines.append(f"{indent}shader[{i}] {s['name']} bucket={s.get('renderBucket', '-')} {tex}")
    if brief:
        return lines
    for label, rows in d["lods"].items():
        for m, model in enumerate(rows):
            for g, geo in enumerate(model["geometries"]):
                lines.append(
                    f"{indent}{label}.m{m}.g{g} shader={geo['shader']} v={geo['vertices']} i={geo['indices']} "
                    f"stride={geo['stride']} boneIds={geo['boneIds']} vb/ib={geo['pages']} decl={' '.join(geo['declaration'])}"
                )
    return lines


def text_report(result: dict, brief: bool) -> list[str]:
    head = (
        f"== {result['file']} v{result['version']} {result.get('kind')} "
        f"sys={result['systemBytes']:#x} gfx={result['graphicsBytes']:#x}"
    )
    lines = [head]
    kind = result.get("kind")
    if kind == "drawable dictionary":
        shaders = Counter(s["name"] for e in result["entries"] for s in e["shaders"])
        skinned = sum(1 for e in result["entries"] if any(g["boneIds"] for g in _geometries(e)))
        lines.append(f"  entries={len(result['entries'])} skinnedEntries={skinned} shaders={dict(shaders)}")
        for entry in result["entries"]:
            lines.append(f"  [{entry['entry']}] name={entry['name']}")
            lines.extend(_drawable_lines(entry, "    ", brief))
    elif kind == "drawable":
        lines.extend(_drawable_lines(result, "  ", brief))
    elif kind == "fragment":
        lines.append(
            f"  name={result['name']} drawableArray={result['drawableArray']} cloth={result['cloth']} "
            f"environmentCloths={result['environmentCloths']} boneTransforms={result['boneTransforms']} "
            f"physicsLods={result['physicsLods']}"
        )
        if result["main"]:
            lines.extend(_drawable_lines(result["main"], "  ", brief))
    elif kind == "ped variation info":
        lines.append(
            f"  componentDrawables={result['componentDrawables']} componentTextures={result['componentTextures']} "
            f"compInfos={result['compInfos']} selectionSets={result['selectionSets']} props={result['props']} "
            f"dlcNameHash={result['dlcNameHash']}"
        )
        for comp, info in result["components"].items():
            tex = [d["textures"] for d in info["drawables"]]
            race = "".join("r" if d["race"] else "u" for d in info["drawables"])
            alts = sum(d["alternatives"] for d in info["drawables"])
            lines.append(f"  {comp}: drawables={len(tex)} textures={tex} suffix={race} alternatives={alts}")
    elif kind in ("texture dictionary", "cloth dictionary"):
        rows = result.get("textures", result.get("entries", []))
        lines.append(f"  count={result['count']} formats={result.get('formats', '-')}")
        if "strictReader" in result:
            lines.append(f"  strict PS5 texture reader refused: {result['strictReader']}")
        lines.extend(f"  {row}" for row in (rows[:8] if brief else rows))
    return lines


def _geometries(d: dict) -> list[dict]:
    return [g for rows in d["lods"].values() for model in rows for g in model["geometries"]]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("files", type=Path, nargs="+")
    parser.add_argument("--json", action="store_true", help="print one JSON document per file")
    parser.add_argument("--brief", action="store_true", help="no per-geometry rows")
    parser.add_argument("--bones", action="store_true", help="list every bone (name, tag, parent)")
    parser.add_argument("--shader-names", type=Path, help="CodeWalker shaders-gen9 XML: more hash names")
    args = parser.parse_args(argv)
    names = Names(shader_names_from_xml(args.shader_names) if args.shader_names else None)
    status = 0
    for path in args.files:
        try:
            result = inspect(path, names, args.bones)
        except (PedResourceError, AssetError, struct.error, OSError) as error:
            print(f"== {path.name}: error: {error}")
            status = 1
            continue
        if args.json:
            print(json.dumps(result, indent=1))
        else:
            print("\n".join(text_report(result, args.brief)))
    return status


if __name__ == "__main__":
    raise SystemExit(main())

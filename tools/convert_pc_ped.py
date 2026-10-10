#!/usr/bin/env python3
"""Convert one PC add-on ped component (.ydd entry, Legacy v165) into a retail PS5 ped dictionary.

Stage S3 of the ped conversion (PEDCOMP-run1). The PC drawable is grafted
onto a retail drawable of the target dictionary by convert_pc_drawable.py (ped route):
- template = the target entry of the retail .pdd as a standalone high-only .pdr
  (native_drawable_dictionary.entry_resource); it keeps the drawable header (LOD distances, high
  render mask, name) and its own skeleton copy if it has one (retail head drawables);
- the PC drawable supplies models, geometry (Gen9 declaration, buffers) and shaders (schemas from
  the retail ped entries named by --shader-entry and the template);
- the source skeleton must be the ped skeleton (--pft: same bone tags and parents, translations
  within 5 cm) and every bone palette is remapped by tag (identity for the standard 98-bone peds),
  then written into the vertex blend indices: every geometry gets the retail identity palette over
  the template's whole skeleton and every model that matrix count (FRANKLIN-run1 fix);
- the converted drawable is parsed back into an object graph (native_drawable_dictionary
  parse_drawable) and replaces the target key in the retail dictionary; index data moves to the
  graphics pages like retail; the dictionary is written with the S2 pack layout (PEDDD-run1).

Textures (--ptd): the retail ped .ptd re-emitted texture by texture (tiled allocations and object
metadata kept) through the hardware-qualified writer, plus the PC textures named by --texture
(converted by convert_pc_ytd_writer.convert: BC1/BC3/BC5 kept, BC2 and uncompressed re-encoded to BC3; an
uncompressed texture larger than --max-texture-size is box-filtered down and given a full mip chain).
A --texture whose new name is a retail texture replaces it (the variation system binds the
component's diffuse by name: lowr_diff_000_a_uni for lowr drawable 000, texture a).

A new ped (S5, PEDMOD-run1): --keep names the keys the written dictionary keeps (e.g. only the
converted head_000_r on the template of the retail head, which carries the skeleton copy), --ptd is
left out so the .ptd holds only the --texture textures, and --variations report records the PC .ymt
components the .ydd does not have instead of refusing (the mods ship them so).

Verification: the written .pdd is re-read strictly (full coverage), every untouched entry is
semantically identical to retail, the new entry is identical to the converted drawable, the .pmt
variation check passes, and every texture name the dictionary's shaders reference (other than the
retail global ones, givemechecker and the like) is in the written .ptd.

  convert_pc_ped.py component --ydd BigGoose.ydd --entry head_000_r --pdd cop.pdd --target lowr_000_u \\
      --pft cop.pft --pmt cop.pmt --output out/gm_cop_01.pdd \\
      --rename head_diff_000_a_whi=lowr_diff_000_a_uni --rename head_normal_000=gm_goose_normal_000 \\
      --fill VolumeSampler=givemechecker \\
      --ptd cop.ptd --ytd BigGoose.ytd --texture head_diff_000_a_whi=lowr_diff_000_a_uni ... \\
      --ptd-output out/gm_cop_01.ptd --max-texture-size 1024

Writes the .pdd, the .ptd and <output>.report.json; refuses to overwrite. Needs numpy (the `packs` extra)
and nothing else outside the standard library. Run on PC files from outside the mod directory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import stat
import struct
import sys
import zlib
from dataclasses import dataclass, replace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

import convert_pc_drawable as cpd  # noqa: E402
import native_drawable_dictionary as ndd  # noqa: E402
import numpy as np  # noqa: E402
from gtavmenu_tools import ps5_texture_writer as writer  # noqa: E402
from gtavmenu_tools.asset_formats import AssetError, Limits, decode_resource  # noqa: E402
from gtavmenu_tools.hashes import joaat  # noqa: E402
from gtavmenu_tools.ps5_texture_reader import parse_ps5_texture_dictionary  # noqa: E402
from gtavmenu_tools.texture_layout import surface_layout  # noqa: E402
from gtavmenu_tools.texture_names import SOURCE_NAME_POLICY  # noqa: E402

SYS = 0x50000000
LIMITS = replace(Limits(), max_file_bytes=1 << 28, max_total_bytes=1 << 31, max_entries=4096)
# Texture names retail ped shaders bind that no ped .ptd holds (global/engine textures).
GLOBAL_TEXTURES = frozenset({"givemechecker", "long_hair_noise", "enveff_gray"})
# ped_hair_cutout_alpha / ped_hair_spiked anisotropic-specular noise (AnisoNoiseSpecTex). Retail component peds
# name `long_hair_noise` (salton hair_001_r, csb_bride) or the `givemechecker` stand-in (juggernaut_03, salton
# hair_000_r); the texture itself lives in comp_peds_generic.ptd (componentpeds_comp_peds.rpf, the component
# peds' shared dictionary), and a_f_y_hiker_01.ptd ships its own copy of it. PC exports leave the sampler empty.
HAIR_NOISE = "long_hair_noise"
HAIR_NOISE_SAMPLER = "AnisoNoiseSpecSampler"
HAIR_NOISE_PTD = "peds/comp_peds_generic.ptd"  # fetch-templates cache name
HAIR_NOISE_MODES = ("packed", "reference", "checker")
VIEW_INITIALIZERS = {value.hex(): key for key, value in writer._VIEW_INITIALIZERS.items()}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ---- skeleton -----------------------------------------------------------------------------------


def fragment_skeleton(blob: bytes) -> list[dict]:
    """The bones of a ped .pft (Gen9 fragment: +0x30 primary drawable, its +0x18 skeleton)."""
    header, payload = decode_resource(blob, 1 << 28)
    if header["version"] != 171:
        raise AssetError("--pft must be a PS5 fragment (version 171)")
    system = payload[: header["systemBytes"]]
    drawable = struct.unpack_from("<Q", system, 0x30)[0] - SYS
    if not 0 <= drawable < len(system):
        raise AssetError("--pft has no primary drawable")
    bones = cpd.read_skeleton(system, struct.unpack_from("<Q", system, drawable + 0x18)[0], SYS)
    if not bones:
        raise AssetError("--pft has no skeleton")
    return bones


# ---- drawable -----------------------------------------------------------------------------------


def convert_component(args, c: dict) -> tuple[bytes, dict]:
    pdd = args.pdd.read_bytes()
    graph = ndd.parse(pdd)
    template_pdd = args.template_pdd or args.pdd
    template_entry = args.template_entry or args.target
    template_graph = ndd.parse(template_pdd.read_bytes()) if args.template_pdd else graph
    template_blob = ndd.entry_resource(template_graph, template_entry, high_only=True, strip_textures=True)
    template = cpd.Template(template_blob, c, f"{template_pdd.name}:{template_entry}")
    extra = [cpd.Template(ndd.entry_resource(graph, name), c, f"{args.pdd.name}:{name}") for name in args.shader_entry]
    extra += [cpd.Template(p.read_bytes(), c, p.name) for p in args.shader_template]
    names = dict(pair.split("=", 1) for pair in args.rename)
    fills = dict(pair.split("=", 1) for pair in args.fill)
    blob, report = cpd.convert(
        args.ydd.read_bytes(),
        template,
        extra,
        c,
        entry=args.entry,
        ped_skeleton=fragment_skeleton(args.pft.read_bytes()),
        merge_missing_bones=args.merge_missing_bones,
        texture_names=names,
        fill_textures=fills,
    )
    embedded = report.pop("_embedded", [])
    txd = embedded_dictionary(embedded, args) if embedded else None
    if txd is not None:
        report["embeddedDictionary"] = txd[1]
    converted = assemble(blob, txd[0] if txd else None)
    ndd.put_drawable(graph, args.target, converted)
    blanks = [name for name in args.blank if name != args.blank_with]
    if args.target in blanks:
        raise AssetError("--blank names the --target entry")
    for name in blanks:
        ndd.replace_entry(graph, name, args.blank_with)
    if args.keep:
        if args.target not in args.keep or set(blanks) - set(args.keep):
            raise AssetError("--keep must name the --target and every --blank entry")
        ndd.select_entries(graph, args.keep)
    out = ndd.write(graph, "pack")
    report["dictionary"] = verify_dictionary(
        pdd, out, blob, args.target, args.pmt, blanks, args.blank_with, args.keep, args.variations, txd and txd[0]
    )
    report["template"]["entry"] = f"{template_pdd.name}:{template_entry}"
    report["output"] = {"bytes": len(out), "sha256": digest(out)}
    return out, report


def assemble(blob: bytes, txd: bytes | None) -> ndd.Graph:
    """The converted drawable as an object graph, with its embedded texture dictionary if any."""
    drawable = ndd.parse_drawable(blob, coverage=False)
    if txd is not None:
        ndd.embed_textures(drawable, ndd.parse_texture_dictionary(txd))
    return drawable


def embedded_dictionary(textures: list[dict], args) -> tuple[bytes, dict]:
    """The source drawable's embedded Legacy textures as a PS5 texture dictionary (writer route),
    to embed in the converted drawable like retail streamed components carry their normal, spec
    and wrinkle maps. Names are kept (the shaders reference them)."""
    import convert_pc_ytd_writer

    prepared = [cpd.trim_texture(resample(t, args.max_texture_size), args.max_texture_size) for t in textures]
    blob, rows = convert_pc_ytd_writer.convert({"textures": prepared}, args.templates)
    keys = ("name", "sourceFormat", "format", "width", "height", "mipLevels", "tileMode")
    return blob, {
        "textures": [dict(zip(keys, row, strict=True)) for row in rows],
        "strideRepaired": sorted(t["name"] for t in textures if t.get("strideRepaired")),
        "bytes": len(blob),
        "sha256": digest(blob),
    }


def verify_dictionary(
    retail: bytes,
    out: bytes,
    drawable_blob: bytes,
    target: str,
    pmt: Path | None,
    blanks: list[str] = (),
    blank_with: str | None = None,
    keep: list[str] = (),
    variations: str = "require",
    txd: bytes | None = None,
) -> dict:
    """Strict re-read; untouched entries equal retail, blanked entries equal the retail --blank-with
    drawable; the new entry equals the converted drawable; with `keep` only those keys remain.
    `variations` "report" records pmt drawables without a key instead of refusing (an add-on ped
    whose .ymt lists components its .ydd does not have, as the PC mods ship)."""
    before, after = ndd.parse(retail), ndd.parse(out)
    key = joaat(target)
    wanted = {joaat(name) for name in keep}
    expected = [(k, d) for k, d in before.entries() if not keep or k in wanted]
    if [k for k, _ in expected] != [k for k, _ in after.entries()]:
        raise AssetError("written dictionary keys differ from the retail (or --keep) keys")
    problems = []
    retail_entries = dict(before.entries())
    blank_keys = {joaat(name) for name in blanks}
    for (k, old), (_, new) in zip(expected, after.entries(), strict=True):
        if k in blank_keys:
            old = retail_entries[joaat(blank_with)]
        if k != key:
            problems += [f"{k:#010x}: {p}" for p in ndd.subgraph_compare(old, new)]
    fresh = assemble(drawable_blob, txd)
    for obj in fresh.objects:
        if obj.kind == "index-data":
            obj.region = "gfx"
    problems += [f"{target}: {p}" for p in ndd.subgraph_compare(fresh.root, dict(after.entries())[key])]
    if problems:
        raise AssetError("written dictionary differs: " + "; ".join(problems[:5]))
    variation = ndd.check_variations(after, pmt.read_bytes()) if pmt else []
    if variation and variations == "require":
        raise AssetError("variation check: " + "; ".join(variation))
    decoded = ndd.describe(out)
    entry = next(e for e in decoded["entries"] if e["entry"] == target)
    return {
        "entries": len(after.entries()),
        "census": ndd.census(after),
        "untouchedEntriesIdentical": True,
        "blankedEntries": {"entries": list(blanks), "copyOf": blank_with} if blanks else None,
        "newEntryIdenticalToConvertedDrawable": True,
        "variationCheck": (variation or "consistent") if pmt else None,
        "newEntry": {
            "lods": {k: [g["vertices"] for m in v for g in m["geometries"]] for k, v in entry["lods"].items() if v},
            "lodDistances": entry["lodDistances"],
            "renderMasks": entry["renderMasks"],
        },
        "textureNames": sorted(texture_names(decoded)),
    }


def texture_names(decoded: dict) -> set[str]:
    names = set()
    for entry in decoded["entries"]:
        for shader in entry.get("shaders", []):
            for value in shader.get("textures", {}).values():
                if value and value not in ("-", "?"):
                    names.add(value)
    return names


# ---- textures -----------------------------------------------------------------------------------


def resample(texture: dict, max_size: int) -> dict:
    """An uncompressed texture (BGRA8 family) larger than max_size, box-filtered down to fit, with a
    full mip chain to 4x4 (its own lower mips are discarded)."""
    width, height = texture["width"], texture["height"]
    if texture["format"] not in ("BGRA8", "BGRX8", "RGBA8") or max(width, height) <= max_size:
        return texture
    image = np.frombuffer(texture["mips"][0], dtype=np.uint8).reshape(height, width, 4).astype(np.float32)
    while max(image.shape[:2]) > max_size:
        image = image.reshape(image.shape[0] // 2, 2, image.shape[1] // 2, 2, 4).mean(axis=(1, 3))
    mips = []
    while min(image.shape[:2]) >= 4:
        mips.append(np.clip(np.rint(image), 0, 255).astype(np.uint8).tobytes())
        if min(image.shape[:2]) < 8:
            break
        image = image.reshape(image.shape[0] // 2, 2, image.shape[1] // 2, 2, 4).mean(axis=(1, 3))
    h, w = (height, width)
    while max(h, w) > max_size:
        h, w = h // 2, w // 2
    return texture | {"width": w, "height": h, "mipLevels": len(mips), "mips": tuple(mips)}


def retail_inputs(blob: bytes) -> list[writer.Ps5TextureInput]:
    """Writer inputs that reproduce a retail .ptd texture by texture (allocation, shape, metadata)."""
    return [writer_input(t) for t in parse_ps5_texture_dictionary(blob, LIMITS)["textures"]]


def writer_input(t: dict) -> writer.Ps5TextureInput:
    """One texture row of the strict PS5 reader as a writer input (allocation, shape, metadata kept)."""
    m = t["metadata"]
    # Retail views start with a serialized VFT word; the writer (hardware-qualified) writes 0 there.
    view = VIEW_INITIALIZERS.get("0" * 16 + m["rawViewHex"][16:])
    if view is None:
        raise AssetError(f"{t['name']}: retail view bytes are outside the writer's initializers")
    layout = surface_layout(t["width"], t["height"], t["mipLevels"], t["format"], t["tileMode"])
    if layout.storage_bytes != len(t["allocation"]):
        raise AssetError(f"{t['name']}: retail allocation is not the derived surface layout")
    return writer.Ps5TextureInput(
        t["name"],
        t["width"],
        t["height"],
        t["formatCode"],
        t["mipLevels"],
        t["tileMode"],
        t["allocation"],
        layout.alignment_bytes,
        writer.Ps5TextureMetadata(m["flags"], m["metadataCandidate"], m["referenceCount"], m["viewType"], view),
    )


def retail_texture(blob: bytes, name: str) -> tuple[writer.Ps5TextureInput, dict]:
    """Texture `name` of a retail .ptd as a writer input, read alone: the strict reader's per-texture
    checks on that one entry, so a dictionary holding formats the writer does not cover (the A8
    afro_hair_noise next to long_hair_noise in comp_peds_generic.ptd) still lends its other textures."""
    from gtavmenu_tools import ps5_texture_reader as reader

    header, payload = decode_resource(blob, LIMITS.max_file_bytes)
    if header["version"] != 5:
        raise AssetError(f"{name}: the dictionary is no PS5 texture dictionary (version {header['version']})")
    view = reader._View(payload, header)
    view.span(reader.SYSTEM_BASE, 64, "dictionary root", 16)
    key_pointer, count, key_capacity = view.unpack("<QHH", 32)
    value_pointer, value_count, value_capacity = view.unpack("<QHH", 48)
    if not 0 < count == value_count <= min(key_capacity, value_capacity) <= LIMITS.max_entries:
        raise AssetError(f"{name}: dictionary counts are inconsistent")
    keys_at = view.span(key_pointer, key_capacity * 4, "key table", 4)
    values_at = view.span(value_pointer, value_capacity * 8, "value table", 8)
    keys = [view.unpack("<I", keys_at + 4 * i)[0] for i in range(count)]
    if joaat(name) not in keys:
        raise AssetError(f"the dictionary has no texture {name}")
    index = keys.index(joaat(name))
    t = reader._texture(view, view.unpack("<Q", values_at + 8 * index)[0], keys[index])
    t["allocation"] = bytes(view.graphics[t["graphicsOffset"] : t["graphicsOffset"] + t["storageBytes"]])
    row = {k: t[k] for k in ("name", "width", "height", "format", "mipLevels", "tileMode")}
    return writer_input(t), row | {"allocationSha256": digest(t["allocation"]), "dictionarySha256": digest(blob)}


def merge_textures(args) -> tuple[bytes, dict]:
    wanted = dict(pair.split("=", 1) for pair in args.texture)
    # trim first (as convert_pc_drawable): the sub-block 2x2/1x1 tail mips exporters add fail admission
    textures, _ = cpd.ytd_textures(args.ytd.read_bytes(), args.max_texture_size)
    if getattr(args, "all_textures", False):
        wanted |= {t["name"]: t["name"] for t in textures if t["name"] not in wanted}
    by_name = {t["name"].lower(): t for t in textures}
    new = []
    for old, name in wanted.items():
        if old.lower() not in by_name:
            raise AssetError(f"--ytd has no texture {old}")
        texture = resample(by_name[old.lower()], args.max_texture_size)
        new.append(cpd.trim_texture(texture, args.max_texture_size) | {"name": name})
    return build_ptd(new, args.ptd.read_bytes() if args.ptd else None, args.templates)


def build_ptd(
    new: list[dict], retail_blob: bytes | None, templates: Path | None, *, extra: list | None = None
) -> tuple[bytes, dict]:
    """A PS5 .ptd: the retail dictionary `retail_blob` re-emitted texture by texture (if given) with
    the prepared PC textures `new` (convert_pc_ytd_writer input shape, already resampled and trimmed)
    added; a new texture whose name is a retail one replaces it. `extra` (writer inputs: retail textures
    of other dictionaries, the packed long_hair_noise) are added unless a texture of that name is there."""
    import convert_pc_ytd_writer

    converted, rows = convert_pc_ytd_writer.convert({"textures": new}, templates) if new else (b"", [])
    converted_inputs = retail_inputs(converted) if new else []
    retail = retail_inputs(retail_blob) if retail_blob is not None else []
    if not converted_inputs and not retail:
        raise AssetError("no textures for the texture dictionary (no PC texture and no retail base)")
    retail_blob = retail_blob or b""
    new_names = {t.name.lower() for t in converted_inputs}
    kept = [t for t in retail if t.name.lower() not in new_names]
    replaced = sorted(t.name for t in retail if t.name.lower() in new_names)
    present = new_names | {t.name.lower() for t in kept}
    added = [t for t in extra or [] if t.name.lower() not in present]
    kept += added
    writer.MAX_TEXTURES = convert_pc_ytd_writer.MAX_DICTIONARY_TEXTURES
    blob = writer.write_ps5_texture_dictionary([*kept, *converted_inputs], LIMITS, name_policy=SOURCE_NAME_POLICY)
    stream = zlib.decompressobj(-15)
    payload = stream.decompress(blob[16:])
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    blob = blob[:16] + packer.compress(payload) + packer.flush()
    written = {t["name"].lower(): t for t in parse_ps5_texture_dictionary(blob, LIMITS)["textures"]}
    for source in [*kept, *converted_inputs]:
        t = written.get(source.name.lower())
        if (
            t is None
            or t["allocation"] != source.allocation
            or t["metadata"]["metadataCandidate"] != (source.metadata.metadata_candidate)
        ):
            raise AssetError(f"{source.name}: written texture differs from its input")
    keys = ("name", "sourceFormat", "format", "width", "height", "mipLevels", "tileMode")
    return blob, {
        "retailTextures": len(retail),
        "replacedRetailTextures": replaced,
        "added": [dict(zip(keys, row, strict=True)) for row in rows],
        **({"addedRetail": sorted(t.name for t in added)} if added else {}),
        "textures": len(written),
        "names": sorted(t["name"] for t in written.values()),
        "bytes": len(blob),
        "sha256": digest(blob),
        "retailSha256": digest(retail_blob) if retail_blob else None,
    }


# ---- variations (.ymt -> .pmt) ------------------------------------------------------------------


def same_pages(a: bytes, b: bytes) -> bool:
    """Two RSC7 headers of the same version whose system and graphics flags give the same page lists."""
    import resource_page_layout

    va, sa, ga = struct.unpack_from("<III", a, 4)
    vb, sb, gb = struct.unpack_from("<III", b, 4)
    return va == vb and all(
        x >> 28 == y >> 28
        and resource_page_layout.pc_pages(x & 0x0FFFFFFF) == resource_page_layout.pc_pages(y & 0x0FFFFFFF)
        for x, y in ((sa, sb), (ga, gb))
    )


def convert_variations(ymt: bytes, retail_pmt: bytes, component: str | None, race: bool) -> tuple[bytes, dict]:
    """A PC ped .ymt as a PS5 .pmt. Both are the same RSC7 v2 PSO-in-resource (0DRP) meta; every
    struct info of the .ymt must equal the retail .pmt's (size and member rows), then the bytes are
    used as they are. `component` instead writes a copy reduced to that one component (drawable
    000, first texture, no props; make_ped_apparel_pack.patch_pmt without a DLC name)."""
    import make_ped_apparel_pack as apparel
    from gtavmenu_tools.meta_resource import Meta

    header, payload = apparel.read_rsc7(ymt, "ymt")
    _, retail = apparel.read_rsc7(retail_pmt, "retail pmt")
    rewrapped = False
    if header[4:16] != retail_pmt[4:16]:
        # exporters spell the same page list with another base (one 8 KiB page: 0x08000004 against the
        # retail 0x00020000); the retail header then describes the payload as well and replaces it
        if not same_pages(header, retail_pmt[:16]):
            raise AssetError("the .ymt resource header (version/flags) differs from the retail .pmt's")
        header, ymt, rewrapped = retail_pmt[:16], retail_pmt[:16] + ymt[16:], True
    ours, theirs = Meta(payload), Meta(retail)
    if ours.magic != b"0DRP" or theirs.magic != b"0DRP":
        raise AssetError("not a parser meta (PRD0)")
    differing = sorted(f"0x{k:08x}" for k, v in ours.structs.items() if theirs.structs.get(k) != v)
    if differing:
        raise AssetError("struct infos differ from the retail .pmt: " + ", ".join(differing))
    out = ymt if component is None else apparel.write_rsc7(header, apparel.patch_pmt(payload, component, None, race))
    return out, {
        **({"headerRewrapped": "retail page flags (same page list)"} if rewrapped else {}),
        "structInfos": len(ours.structs),
        "structInfosEqualRetail": True,
        "reducedTo": component,
        "bytes": len(out),
        "sha256": digest(out),
    }


# ---- whole ped: several components, textures and props (roadmap F3.1) ----------------------------

COMPONENTS = ("head", "berd", "hair", "uppr", "lowr", "hand", "feet", "teef", "accs", "task", "decl", "jbib")
# Prop anchors in eAnchorPoints order (p_<anchor>_<nnn> drawables, p_<anchor>_diff_<nnn>_<letter> textures);
# the two PH_ hand anchors are named by analogy (no retail prop seen on them).
ANCHORS = ("head", "eyes", "ears", "mouth", "lhand", "rhand", "lwrist", "rwrist", "hip", "lfoot", "rfoot")
ANCHORS += ("ph_lhand", "ph_rhand")
# CPVTextureData texId -> the race suffix of an `_r` drawable's diffuse (`_u` drawables use "uni").
RACES = ("uni", "whi", "bla", "chi", "lat", "ara", "bal", "jam", "kor", "ita", "pak")
VARIATION_INFO = 0x16760659  # CPedVariationInfo: u8 availComp[12] +0x04, CPVComponentData array +0x10
VI_AVAIL, VI_COMPONENTS, VI_PROP_ANCHORS = 0x04, 0x10, 0x58  # CPedPropInfo +0x40: anchors array at +0x18
COMPONENT_DATA, DRAWABLE_DATA, TEXTURE_DATA, ANCHOR_PROPS = 0x18, 0x30, 3, 0x18
RACE_MASK = 0x10  # CPVDrawblData propMask bit of an `_r` drawable
# Shaders the converter refuses (no retail schema bank chosen yet); --skip-unsupported leaves them out. Empty
# since F3.2: ped_hair_cutout_alpha takes its schema from the juggernaut_03 hair_000_u carrier (identical to the
# salton and bride hair schemas but for the sampler-state bytes; ped-conversion-feasibility.md 14).
UNSUPPORTED_SHADERS: dict[int, str] = {}
DRAWABLE_KEY = re.compile(rf"({'|'.join(COMPONENTS)})_(\d{{3}})_([ur])")
PROP_KEY = re.compile(rf"p_({'|'.join(ANCHORS)})_(\d{{3}})")
PLACEHOLDER = "accs_002_u"  # retail cop entry: a 3 cm triangle (PEDDD-v2), textures pointed at givemechecker
FALLBACK_TEMPLATE = "uppr_001_u"
DIFFUSE_SAMPLER = joaat("DiffuseSampler")
SYS_BASE = 0x50000000


class Unsupported(AssetError):
    pass


def read_variations(blob: bytes) -> dict:
    """The variation layout of a ped .pmt/.ymt (RSC7 PSO-in-resource): per component its drawables
    (race flag, alternatives, the texture ids that give each diffuse its race suffix) and per prop
    anchor the texture count of each prop."""
    import make_ped_apparel_pack as apparel
    from gtavmenu_tools.meta_resource import Meta

    _, payload = apparel.read_rsc7(blob, "variations")
    meta = Meta(payload)
    found = [at for h, size, at in meta.blocks if h == VARIATION_INFO and size >= 0x70]
    if meta.magic != b"0DRP" or len(found) != 1:
        raise AssetError("the variation file has no single CPedVariationInfo")
    vi = found[0]

    def array(at: int) -> tuple[int, int]:
        pointer, count = struct.unpack_from("<QH", payload, at)
        start = meta.ref(pointer) if pointer else None
        if count and start is None:
            raise AssetError("variation array pointer is not a data block reference")
        return start or 0, count

    data, count = array(vi + VI_COMPONENTS)
    components = {}
    for comp, index in enumerate(payload[vi + VI_AVAIL : vi + VI_AVAIL + len(COMPONENTS)]):
        if index == 0xFF:
            continue
        if index >= count:
            raise AssetError(f"{COMPONENTS[comp]}: component index {index} outside {count} entries")
        drawables, n = array(data + COMPONENT_DATA * index + 8)
        rows = []
        for d in range(n):
            at = drawables + DRAWABLE_DATA * d
            textures, n_tex = array(at + 8)
            ids = [payload[textures + TEXTURE_DATA * t] for t in range(n_tex)]
            if any(i >= len(RACES) for i in ids):
                raise AssetError(f"{COMPONENTS[comp]} drawable {d}: texture race id outside {RACES}")
            rows.append({"race": bool(payload[at] & RACE_MASK), "alternatives": payload[at + 1], "texIds": ids})
        components[COMPONENTS[comp]] = rows
    props = {}
    anchors, n_anchors = array(vi + VI_PROP_ANCHORS)
    for a in range(n_anchors):
        at = anchors + ANCHOR_PROPS * a
        counts, n = array(at)
        anchor = struct.unpack_from("<i", payload, at + 0x10)[0]
        if not 0 <= anchor < len(ANCHORS) or ANCHORS[anchor] in props:
            raise AssetError(f"prop anchor {anchor} is unknown or repeated")
        props[ANCHORS[anchor]] = [payload[counts + i] for i in range(n)]
    return {"components": components, "props": props}


def drawable_key(comp: str, index: int, race: bool) -> str:
    return f"{comp}_{index:03d}_{'r' if race else 'u'}"


def slot_diffuses(v: dict, key: str) -> list[str]:
    """The diffuse texture names the variation layout binds to drawable or prop `key` (in order)."""
    m = PROP_KEY.fullmatch(key)
    if m:
        anchor, index = m.group(1), int(m.group(2))
        counts = v["props"].get(anchor, [])
        n = counts[index] if index < len(counts) else 0
        return [f"p_{anchor}_diff_{index:03d}_{chr(97 + t)}" for t in range(n)]
    m = DRAWABLE_KEY.fullmatch(key)
    rows = v["components"].get(m.group(1), []) if m else []
    index = int(m.group(2)) if m else 0
    if not m or index >= len(rows) or rows[index]["race"] != (m.group(3) == "r"):
        return []
    race = rows[index]["race"]
    return [
        f"{m.group(1)}_diff_{index:03d}_{chr(97 + t)}_{RACES[i] if race else 'uni'}"
        for t, i in enumerate(rows[index]["texIds"])
    ]


def variation_names(v: dict) -> dict:
    drawables, alternatives, textures, props, prop_textures = [], [], [], [], []
    for comp, rows in v["components"].items():
        for index, row in enumerate(rows):
            key = drawable_key(comp, index, row["race"])
            drawables.append(key)
            alternatives += [f"{key}_{k}" for k in range(1, row["alternatives"] + 1)]
            textures += slot_diffuses(v, key)
    for anchor, counts in v["props"].items():
        for index in range(len(counts)):
            key = f"p_{anchor}_{index:03d}"
            props.append(key)
            prop_textures += slot_diffuses(v, key)
    return {
        "drawables": drawables,
        "alternatives": alternatives,
        "textures": textures,
        "props": props,
        "propTextures": prop_textures,
    }


def dictionary_keys(blob: bytes) -> set[int]:
    return {key for key, _ in ndd.parse(blob).entries()}


def texture_dictionary_names(blob: bytes) -> set[str]:
    return {t["name"].lower() for t in parse_ps5_texture_dictionary(blob, LIMITS)["textures"]}


def variation_check(
    v: dict, pdd: bytes, ptd: bytes, props_pdd: bytes | None = None, props_ptd: bytes | None = None
) -> dict:
    """Round trip of the variation layout against the shipped members: every drawable (and
    alternative), every diffuse texture of every drawable, every prop and prop texture the layout
    lists must be a key or texture of the written dictionaries; dictionary keys the layout does not
    list are reported too. `consistent` is true when nothing is missing or unlisted."""
    names = variation_names(v)
    keys, textures = dictionary_keys(pdd), texture_dictionary_names(ptd)
    prop_keys = dictionary_keys(props_pdd) if props_pdd else set()
    prop_textures = texture_dictionary_names(props_ptd) if props_ptd else set()

    def split(wanted: list[str], have: set, by_hash: bool) -> dict:
        missing = [n for n in wanted if (joaat(n) if by_hash else n.lower()) not in have]
        return {"listed": len(wanted), "resolved": len(wanted) - len(missing), "missing": missing}

    report = {
        "drawables": split(names["drawables"], keys, True),
        "alternatives": split(names["alternatives"], keys, True),
        "textures": split(names["textures"], textures, False),
        "props": split(names["props"], prop_keys, True),
        "propTextures": split(names["propTextures"], prop_textures, False),
    }
    listed = {joaat(n) for n in names["drawables"] + names["alternatives"]}
    report["unlistedKeys"] = [f"0x{k:08x}" for k in sorted(keys - listed)]
    report["unlistedPropKeys"] = [f"0x{k:08x}" for k in sorted(prop_keys - {joaat(n) for n in names["props"]})]
    report["consistent"] = not any(r["missing"] for r in report.values() if isinstance(r, dict)) and not (
        report["unlistedKeys"] or report["unlistedPropKeys"]
    )
    return report


def describe_check(report: dict) -> str:
    parts = [
        f"{label} {report[key]['resolved']}/{report[key]['listed']}"
        for key, label in (
            ("drawables", "drawables"),
            ("textures", "textures"),
            ("props", "props"),
            ("propTextures", "prop textures"),
        )
    ]
    return ", ".join(parts) + (": consistent" if report["consistent"] else ": NOT consistent")


def candidate_names(v: dict) -> dict[int, str]:
    """Names a ped .ydd key can have (keys are stored as hashes only)."""
    names = [drawable_key(c, i, r) for c in COMPONENTS for i in range(128) for r in (False, True)]
    names += [f"p_{a}_{i:03d}" for a in ANCHORS for i in range(128)]
    names += variation_names(v)["alternatives"]
    return {joaat(n): n for n in names}


def legacy_dictionary_keys(blob: bytes) -> list[int]:
    """The key hashes of a PC (Legacy v165) drawable dictionary, in stored order."""
    header, payload = decode_resource(blob, 1 << 28)
    if header["version"] != 165:
        raise AssetError("a PC .ydd must be a Legacy drawable dictionary (version 165)")
    system = payload[: header["systemBytes"]]
    pointer, count = struct.unpack_from("<QH", system, 0x20)
    at = pointer - SYS_BASE
    if not 0 < count <= 4096 or not 0 <= at <= len(system) - 4 * count:
        raise AssetError("the .ydd key array is outside its system pages")
    return list(struct.unpack_from(f"<{count}I", system, at))


def legacy_fragment_skeleton(blob: bytes) -> list[dict]:
    """The bones of a PC ped .yft (Legacy v162 fragment: +0x30 primary drawable, its +0x18 skeleton;
    the skeleton records have the Gen9 layout)."""
    header, payload = decode_resource(blob, 1 << 28)
    if header["version"] != 162:
        raise AssetError("--yft must be a PC fragment (version 162)")
    system = payload[: header["systemBytes"]]
    drawable = struct.unpack_from("<Q", system, 0x30)[0] - SYS_BASE
    if not 0 <= drawable < len(system) - 0x20:
        raise AssetError("--yft has no primary drawable")
    return cpd.read_skeleton(system, struct.unpack_from("<Q", system, drawable + 0x18)[0], SYS_BASE)


# Retail component peds whose skeleton fetch-templates caches (<templates>/peds/<name>.pdd/.pft/.pmt): the rig a
# mod's .yft skeleton matches is the conversion base (templates, skeleton copies, the pack's .pft). The cop
# (the standard 98-bone male rig) is the given --pdd/--pft/--retail-pmt; a_f_y_topless_01 is the 100-bone
# female ambient rig (ped-conversion-feasibility.md 14.6).
RIG_TEMPLATES = ("a_f_y_topless_01",)


def rig_key(bones: list[dict]) -> list[tuple[int, int]]:
    """A skeleton's identity for rig matching: (tag, parent tag) per bone, in order."""
    tags = [b["tag"] for b in bones]
    return [(b["tag"], tags[b["parent"]] if b["parent"] >= 0 else -1) for b in bones]


def choose_rig(args) -> dict | None:
    """With the mod's .yft: keep the given rig (--pft) when the skeletons match bone for bone (tags and
    parents), else switch --pdd/--pft/--retail-pmt to the first fetched RIG_TEMPLATES ped that matches. None
    when nothing is switched (the given rig, or no .yft, or no match: the skeleton check then reports)."""
    if args.yft is None or args.variations == "retail":
        return None
    want = rig_key(legacy_fragment_skeleton(args.yft.read_bytes()))
    if rig_key(fragment_skeleton(args.pft.read_bytes())) == want:
        return None
    for name in RIG_TEMPLATES:
        paths = [args.templates / "peds" / f"{name}.{e}" for e in ("pdd", "pft", "pmt")] if args.templates else []
        if paths and all(p.is_file() for p in paths) and rig_key(fragment_skeleton(paths[1].read_bytes())) == want:
            args.pdd, args.pft, args.retail_pmt = paths
            return {"rig": name, "bones": len(want)}
    return None


@dataclass
class Part:
    key: str  # the written dictionary key (component drawable or p_ prop)
    path: Path  # PC .ydd (entry) or .ydr (root drawable)
    entry: str | None  # source key in the .ydd
    ytd: Path | None  # the PC textures that come with it
    main: bool = False  # an entry of the converted mod itself (its .ytd is added whole)
    yft: Path | None = None  # another mod's skeleton (its entries without their own copy index it)

    @property
    def prop(self) -> bool:
        return self.key.startswith("p_")


# ---- rigid props from map objects (PEDMC-run1 finding; ped-conversion-feasibility.md 13.7) -------

# Ped props are drawn in their anchor bone's frame. The head anchor is SKEL_Head (IK_Head has the same
# bind pose): +X up the neck, +Y forward (the face), +Z the ped's left, measured on the retail cop:
# its cap p_head_000 spans x 0.091..0.210 (y -0.128..0.160 with the peak forward), its sunglasses sit
# at x 0.03..0.16, y up to 0.13; the three cop heads, taken into the SKEL_Head frame by the .pft's
# inverse bind matrix, top out at x 0.188 with the top 3 cm centred at (y, z) = (0.027, 0.000).
PROP_ANCHOR_TOP = {"head": (0.188, 0.027, 0.0)}
# A map object (.ydr) is authored Z-up with its origin at its base: turned so that its +Z is the
# bone's +X (+Y stays forward), its bounding-box base centre on the anchor's top.
MAP_OBJECT_ROTATION = (0.0, 90.0, 0.0)
# Vertex colour 0 of the retail cop props: R 255, G 128, B 0 (every vertex within ~10 %). A map
# object carries baked map ambient there instead (`default` scales natural ambient by R, artificial
# by G; the Malibu plant, an interior prop: R 76, G 120), dim outdoors; its alpha is kept.
PED_PROP_COLOUR = (0xFF, 0x80, 0x00)


def rotation_matrix(rx: float, ry: float, rz: float) -> tuple[tuple[float, ...], ...]:
    """Rz @ Ry @ Rx (degrees): turn about the X axis first, then Y, then Z (fixed axes)."""

    def axis(deg: float, a: int, b: int) -> list[list[float]]:
        m = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]
        cos, sin = math.cos(math.radians(deg)), math.sin(math.radians(deg))
        cos, sin = round(cos, 12) + 0.0, round(sin, 12) + 0.0  # exact quarter turns
        m[a][a], m[a][b], m[b][a], m[b][b] = cos, -sin, sin, cos
        return m

    def mul(p, q):
        return [[sum(p[i][k] * q[k][j] for k in range(3)) for j in range(3)] for i in range(3)]

    m = mul(axis(rz, 0, 1), mul(axis(ry, 2, 0), axis(rx, 1, 2)))
    return tuple(tuple(row) for row in m)


def turn(m, v, d=(0.0, 0.0, 0.0)) -> tuple[float, float, float]:
    return tuple(m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] + d[i] for i in range(3))


def parse_prop_transforms(rows: list[str]) -> dict[str, tuple | str]:
    """--prop-transform KEY=RX,RY,RZ,DX,DY,DZ (degrees, metres), KEY=auto or KEY=none."""
    out: dict[str, tuple | str] = {}
    for row in rows:
        key, _, value = row.partition("=")
        if not PROP_KEY.fullmatch(key) or key in out:
            raise AssetError(f"--prop-transform takes a prop KEY (p_head_000) once, not {row!r}")
        if value in ("auto", "none"):
            out[key] = value
            continue
        try:
            numbers = tuple(float(x) for x in value.split(","))
        except ValueError:
            numbers = ()
        if len(numbers) != 6 or not all(math.isfinite(x) for x in numbers):
            raise AssetError(f"--prop-transform {row}: give KEY=RX,RY,RZ,DX,DY,DZ, KEY=auto or KEY=none")
        out[key] = numbers
    return out


def place_prop(blob: bytes, c: dict, key: str, given, colours: bool, entry: str | None = None) -> tuple[bytes, dict]:
    """A rigid prop source re-placed for its anchor: the Legacy resource with every position turned
    and moved (normals, tangents and binormals turned), the model and drawable bounds re-derived and,
    for a map object (`colours`), vertex colour 0 set to the retail ped props' value. `given` is
    "auto" (map object: Z-up base onto the anchor's top), "none" (kept where it is) or (rx, ry, rz, dx,
    dy, dz); `entry` names a .ydd entry. The resource is re-encoded with its own header, so the
    converter reads it as any source."""
    header, payload = decode_resource(blob, 1 << 28)
    payload = bytearray(payload)
    source = cpd.Source(blob, c, cpd.Limits(), graphics=True, entry=entry)
    geometry = source.geometry(len(source.materials()["shaders"]))
    rows = source.meshes(geometry)
    positions = []
    for row in rows:
        kinds = {x["semantic"]: x for x in row["components"]}
        if kinds.get("Position", {}).get("type") != "Float3":
            raise AssetError(f"{key}: prop placement reads Float3 positions only")
        at, stride = row["vertexStreams"][0]["systemOffset"], row["vertexStride"]
        o = kinds["Position"]["offset"]
        positions += [struct.unpack_from("<3f", payload, at + i * stride + o) for i in range(row["vertexCount"])]
    low = [min(p[i] for p in positions) for i in range(3)]
    high = [max(p[i] for p in positions) for i in range(3)]
    if given == "auto":
        anchor = PROP_KEY.fullmatch(key).group(1)
        if anchor not in PROP_ANCHOR_TOP:
            raise AssetError(f"{key}: automatic placement knows the head anchor only; give --prop-transform")
        rotation, mode = MAP_OBJECT_ROTATION, "auto"
        m = rotation_matrix(*rotation)
        base = turn(m, ((low[0] + high[0]) / 2, (low[1] + high[1]) / 2, low[2]))
        offset = tuple(round(t - b, 6) + 0.0 for t, b in zip(PROP_ANCHOR_TOP[anchor], base, strict=True))
    elif given == "none":
        rotation, offset, mode = (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), "none"
        m = rotation_matrix(*rotation)
    else:
        rotation, offset, mode = tuple(given[:3]), tuple(given[3:]), "given"
        m = rotation_matrix(*rotation)
    moves = {"Position": True, "Normal": False, "Tangent": False, "Binormal": False}  # turned and moved / turned
    recoloured, done = 0, set()
    for row in rows:
        at, stride = row["vertexStreams"][0]["systemOffset"], row["vertexStride"]
        if at in done:  # geometries sharing one vertex buffer
            continue
        done.add(at)
        for comp in row["components"]:
            sem, kind, o = comp["semantic"], comp["type"], comp["offset"]
            if sem in moves:
                if kind not in ("Float3", "Float4") or (sem == "Position" and kind != "Float3"):
                    raise AssetError(f"{key}: prop placement cannot turn a {kind} {sem}")
                for i in range(row["vertexCount"]):
                    v = struct.unpack_from("<3f", payload, at + i * stride + o)
                    moved = turn(m, v, offset if moves[sem] else (0.0, 0.0, 0.0))
                    struct.pack_into("<3f", payload, at + i * stride + o, *moved)
            elif sem == "Colour0" and colours and kind == "Colour":
                for i in range(row["vertexCount"]):
                    payload[at + i * stride + o : at + i * stride + o + 3] = bytes(PED_PROP_COLOUR)
                recoloured += row["vertexCount"]

    def box(lo, hi):
        corners = [turn(m, (x, y, z), offset) for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])]
        return [min(p[i] for p in corners) for i in range(3)], [max(p[i] for p in corners) for i in range(3)]

    gc = c["geometry"]
    for lod in geometry["lods"]:
        for model in lod["models"]:
            n = len(model["geometries"])
            pointer = cpd.read_field(source.system, model["systemOffset"], gc["model"], "BoundsPointer")
            boxes = source.view.offset(pointer, 32 * (n + (n > 1)))
            for b in range(boxes, boxes + 32 * (n + (n > 1)), 32):
                lo, hi = box(struct.unpack_from("<3f", payload, b), struct.unpack_from("<3f", payload, b + 16))
                struct.pack_into("<3f", payload, b, *lo)
                struct.pack_into("<3f", payload, b + 16, *hi)
    root = {name: source.root + cpd.field(gc["drawable"], name)["offset"] for name in cpd.BOUNDS_FIELDS}
    lo, hi = box(
        struct.unpack_from("<3f", payload, root["BoundingBoxMin"]),
        struct.unpack_from("<3f", payload, root["BoundingBoxMax"]),
    )
    struct.pack_into("<3f", payload, root["BoundingBoxMin"], *lo)
    struct.pack_into("<3f", payload, root["BoundingBoxMax"], *hi)
    centre = turn(m, struct.unpack_from("<3f", payload, root["BoundingCenter"]), offset)
    struct.pack_into("<3f", payload, root["BoundingCenter"], *centre)
    if len(payload) != header["systemBytes"] + header["graphicsBytes"]:
        raise AssetError(f"{key}: placed source changed size")
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    out = blob[:16] + packer.compress(bytes(payload)) + packer.flush()
    after = [turn(m, p, offset) for p in positions]
    report = {
        "mode": mode,
        "rotation": list(rotation),
        "offset": list(offset),
        "transform": ",".join(f"{x:g}" for x in (*rotation, *offset)),
        "sourceBox": [low, high],
        "placedBox": [[min(p[i] for p in after) for i in range(3)], [max(p[i] for p in after) for i in range(3)]],
        "recolouredVertices": recoloured,
        "sourceSha256": digest(blob),
    }
    return out, report


class PedContext:
    """Shared inputs of one whole-ped conversion: contracts, retail dictionaries, skeletons."""

    def __init__(self, args, c: dict, v: dict):
        self.args, self.c, self.v = args, c, v
        self.base = ndd.parse(args.pdd.read_bytes())
        self.props_base = ndd.parse(args.props_pdd.read_bytes()) if args.props_pdd else None
        self.carriers = [(p.name, ndd.parse(p.read_bytes())) for p in args.carrier]
        self.extra_templates = [cpd.Template(p.read_bytes(), c, p.name) for p in args.shader_template]
        self.pft = fragment_skeleton(args.pft.read_bytes())
        self.yft = legacy_fragment_skeleton(args.yft.read_bytes()) if args.yft else []
        self.names = candidate_names(v) | {joaat(n): n for n in (PLACEHOLDER, FALLBACK_TEMPLATE)}
        self._templates: dict[tuple[int, str], cpd.Template] = {}
        self.notes: list[str] = []
        self._skeletons: dict[Path, list[dict]] = {}
        self.prop_transforms = parse_prop_transforms(getattr(args, "prop_transform", []))
        self.hair_noise = getattr(args, "hair_noise", "packed")
        self._noise: tuple | None = None

    def noise_texture(self) -> tuple[writer.Ps5TextureInput, dict]:
        """The retail long_hair_noise (--hair-noise packed) from comp_peds_generic.ptd (--hair-noise-ptd, else
        <--templates>/peds/comp_peds_generic.ptd, which fetch-templates fills)."""
        if self._noise is None:
            path = getattr(self.args, "hair_noise_ptd", None)
            if path is None and self.args.templates is not None:
                path = self.args.templates / HAIR_NOISE_PTD
            if path is None or not path.is_file():
                raise AssetError(
                    f"--hair-noise packed copies {HAIR_NOISE} from the retail {HAIR_NOISE_PTD} (fetch-templates fills it;"
                    " or --hair-noise-ptd FILE), not found; --hair-noise reference|checker needs no copy"
                )
            texture, row = retail_texture(path.read_bytes(), HAIR_NOISE)
            self._noise = (texture, row | {"from": path.name})
        return self._noise

    def skeleton(self, part: Part) -> list[dict]:
        if part.yft is None:
            return self.yft
        if part.yft not in self._skeletons:
            self._skeletons[part.yft] = legacy_fragment_skeleton(part.yft.read_bytes())
        return self._skeletons[part.yft]

    def entry_names(self, graph: ndd.Graph) -> list[str]:
        return sorted(self.names[k] for k, _ in graph.entries() if k in self.names)

    def template(self, graph: ndd.Graph, name: str, label: str, **kw) -> cpd.Template:
        key = (id(graph), name + repr(sorted(kw.items())))
        if key not in self._templates:
            self._templates[key] = cpd.Template(ndd.entry_resource(graph, name, **kw), self.c, f"{label}:{name}")
        return self._templates[key]


def choose_template(ctx: PedContext, part: Part, has_skeleton: bool) -> str:
    """The retail entry a part is converted onto (it keeps that entry's LOD distances, render masks
    and skeleton copy): a drawable with its own skeleton (a head) goes onto the retail head, which
    carries the skeleton copy; otherwise the same key, the same component or anchor, else a fallback."""
    graph = ctx.props_base if part.prop else ctx.base
    if graph is None:
        raise AssetError(f"{part.key}: a prop needs the retail prop dictionary (--props-pdd)")
    names = ctx.entry_names(graph)
    if part.prop:
        anchor = part.key.rsplit("_", 1)[0]
        same = [n for n in names if n.rsplit("_", 1)[0] == anchor]
        return part.key if part.key in names else (same or names)[0]
    if has_skeleton:
        heads = [n for n in names if n.startswith("head_")]
        if not heads:
            raise AssetError(f"{part.key}: no retail head entry (with a skeleton copy) to convert it onto")
        return part.key if part.key in heads else heads[0]
    same = [n for n in names if n[:4] == part.key[:4] and not n.startswith("head_")]
    others = [FALLBACK_TEMPLATE] if FALLBACK_TEMPLATE in names else [n for n in names if not n.startswith("head_")]
    return part.key if part.key in names else (same or others)[0]


def shader_carriers(ctx: PedContext, wanted: set[int], template: cpd.Template) -> list[cpd.Template]:
    """Retail drawables whose shaders give the part the schemas its template lacks: entries of the
    retail ped and prop dictionaries and of the --carrier dictionaries, then the --shader-template
    drawables, in that order; a carrier is taken only when it adds a missing shader and every schema
    it carries equals the one already chosen (the schema bank requires one schema per shader)."""
    have = {row["schema"]["nameHash"]: row["schema"] for row in template.shaders()}
    missing = wanted - have.keys()
    unsupported = sorted(UNSUPPORTED_SHADERS[h] for h in missing if h in UNSUPPORTED_SHADERS)
    if unsupported:
        raise Unsupported("needs " + ", ".join(unsupported))
    pools = [("base", ctx.base)] + ([("props", ctx.props_base)] if ctx.props_base else []) + ctx.carriers
    candidates = [lambda g=graph, n=name, lb=label: ctx.template(g, n, lb) for label, graph in pools
                  for name in ctx.entry_names(graph)]  # fmt: skip
    candidates += [lambda t=t: t for t in ctx.extra_templates]
    chosen = []
    for candidate in candidates:
        if not missing:
            break
        t = candidate()
        rows = {row["schema"]["nameHash"]: row["schema"] for row in t.shaders()}
        if not rows.keys() & missing or any(h in have and have[h] != s for h, s in rows.items()):
            continue
        chosen.append(t)
        have |= rows
        missing -= rows.keys()
    if missing:
        raise AssetError(
            "no retail carrier has shader(s) "
            + ", ".join(f"0x{h:08x}" for h in sorted(missing))
            + " (fetch-templates fills peds/u_m_m_juggernaut_03.pdd and the shader carriers; --carrier and"
            + " --shader-template add more)"
        )
    return chosen


def slot_renames(source: str, target: str, names: list[str], v: dict) -> dict[str, str]:
    """Texture renames for a drawable moved from key `source` to key `target`: every texture named
    for the source slot (<comp>_<kind>_<nnn>..., p_<anchor>_<kind>_<nnn>...) is renamed for the
    target slot; a diffuse takes the race suffix the variation layout gives that texture letter."""
    sm = PROP_KEY.fullmatch(source) or DRAWABLE_KEY.fullmatch(source)
    tm = PROP_KEY.fullmatch(target) or DRAWABLE_KEY.fullmatch(target)
    if not sm or not tm or bool(PROP_KEY.fullmatch(source)) != bool(PROP_KEY.fullmatch(target)):
        raise AssetError(f"cannot move {source} to {target} (a prop moves to a prop, a drawable to a drawable)")
    prefix = "p_" if source.startswith("p_") else ""
    pattern = re.compile(rf"{prefix}{sm.group(1)}_([a-z0-9]+)_{sm.group(2)}(_.*)?")
    diffuses = {name[-1] if prefix else name.split("_")[-2]: name for name in slot_diffuses(v, target)}
    out = {}
    for name in names:
        m = pattern.fullmatch(name.lower())
        if not m:
            continue
        kind, rest = m.group(1), m.group(2) or ""
        letter = re.fullmatch(r"_([a-z])(?:_[a-z]{3})?", rest)
        if kind == "diff" and letter and letter.group(1) in diffuses:
            out[name] = diffuses[letter.group(1)]
        elif kind == "diff" and letter and not prefix:
            race = "whi" if target.endswith("_r") else "uni"
            out[name] = f"{tm.group(1)}_diff_{tm.group(2)}_{letter.group(1)}_{race}"
        else:
            out[name] = f"{prefix}{tm.group(1)}_{kind}_{tm.group(2)}{rest}"
    return out


def prepared_textures(ytd: Path, max_size: int, notes: list | None = None, keep=None) -> list[dict]:
    """A PC .ytd's textures as build_ptd inputs (trimmed; large uncompressed ones resampled), only
    those whose name `keep` accepts (default all). A dictionary the whole-dictionary admission
    refuses is read texture by texture instead: block-compressed textures whose sides are not powers
    of two are resampled (pow2_texture), textures that stay unusable are left out (both listed in
    `notes`)."""
    try:
        textures, _ = cpd.ytd_textures(ytd.read_bytes(), max_size)
        fix = False
    except AssetError:
        textures, skipped = cpd.ytd_textures(ytd.read_bytes(), max_size, skip_bad=True)
        fix = True
        if notes is not None:
            notes += [f"{ytd.name}: left out {row}" for row in skipped]
    textures = [t for t in textures if keep is None or keep(t["name"])]
    if fix:
        textures = [pow2_texture(t, max_size, notes) for t in textures]
    return [cpd.trim_texture(resample(t, max_size), max_size) for t in textures]


def _pow2(value: int) -> bool:
    return value > 0 and not value & (value - 1)


def pow2_texture(texture: dict, max_size: int, notes: list | None = None) -> dict:
    """A BC1/BC3 PC texture whose sides are not powers of two (exporters write e.g. 1000x1000; its
    lower mips do not fit the Legacy layout, so only the top ones read): the top mip decoded,
    resampled (bilinear) to the nearest powers of two at most max_size, and re-encoded in its own
    format with a full mip chain down to one block."""
    import bc_encode

    width, height, fmt = texture["width"], texture["height"], texture["format"]
    if fmt not in ("BC1", "BC3") or (_pow2(width) and _pow2(height)):
        return texture
    top = texture["mips"][0]
    if fmt == "BC1":
        # a BC1 cutout (3-colour blocks, index 3 = transparent: hair cards) keeps its holes as BC3
        # alpha; the encoder writes opaque 4-colour BC1 only
        color, alpha = top, bc1_alpha(top, width, height)
        if alpha.min() == 255:
            alpha = None
        else:
            fmt = "BC3"
    else:
        blocks = np.frombuffer(top, dtype=np.uint8).reshape(-1, 16)
        color, alpha = blocks[:, 8:].tobytes(), bc_encode.decode_bc3_alpha(top, width, height)
    image = np.empty((height, width, 4), dtype=np.float32)
    image[..., :3] = bc_encode.decode_bc1_rgb(color, width, height)
    image[..., 3] = 255 if alpha is None else alpha

    def side(value: int) -> int:
        return min(max_size, 1 << max(2, round(np.log2(value))))

    new_w, new_h = side(width), side(height)
    ys = np.clip((np.arange(new_h) + 0.5) * height / new_h - 0.5, 0, height - 1)
    xs = np.clip((np.arange(new_w) + 0.5) * width / new_w - 0.5, 0, width - 1)
    y0, x0 = np.floor(ys).astype(int), np.floor(xs).astype(int)
    y1, x1 = np.minimum(y0 + 1, height - 1), np.minimum(x0 + 1, width - 1)
    fy, fx = (ys - y0)[:, None, None], (xs - x0)[None, :, None]
    rows = image[y0] * (1 - fy) + image[y1] * fy
    image = rows[:, x0] * (1 - fx) + rows[:, x1] * fx
    mips = []
    while True:
        mips.append(bc_encode.encode(fmt, np.clip(np.rint(image), 0, 255).astype(np.uint8)))
        if min(image.shape[:2]) < 8:
            break
        image = image.reshape(image.shape[0] // 2, 2, image.shape[1] // 2, 2, 4).mean(axis=(1, 3))
    if notes is not None:
        kind = fmt if fmt == texture["format"] else f"{texture['format']} (cut-out alpha) -> {fmt}"
        notes.append(f"{texture['name']}: {width}x{height} {kind} resampled to {new_w}x{new_h}, {len(mips)} mips")
    return texture | {"width": new_w, "height": new_h, "format": fmt, "mipLevels": len(mips), "mips": tuple(mips)}


def bc1_alpha(data: bytes, width: int, height: int) -> np.ndarray:
    """The 1-bit alpha of linear BC1 blocks as (H, W) uint8: 0 where a 3-colour block (c0 <= c1) uses
    index 3 (transparent black), else 255."""
    w4, h4 = max(1, (width + 3) // 4), max(1, (height + 3) // 4)
    blocks = np.frombuffer(data, dtype=np.uint8)[: w4 * h4 * 8].reshape(h4 * w4, 8)
    c0 = blocks[:, 0].astype(np.uint16) | (blocks[:, 1].astype(np.uint16) << 8)
    c1 = blocks[:, 2].astype(np.uint16) | (blocks[:, 3].astype(np.uint16) << 8)
    bits = blocks[:, 4:8].copy().view("<u4")[:, 0]
    index = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    alpha = np.where((c0 <= c1)[:, None] & (index == 3), 0, 255).astype(np.uint8)
    image = alpha.reshape(h4, w4, 4, 4).transpose(0, 2, 1, 3).reshape(h4 * 4, w4 * 4)
    return image[:height, :width]


def plan_textures(ctx: PedContext, parts: list[Part], base_names: set[str], main_ytd: Path | None) -> tuple:
    """(textures in write order, per-key texture renames, copied diffuses). The mod's own .ytd goes in
    whole under its names; a part from another file (or moved to another key) brings the textures
    named for its source slot, renamed for its key, and every other texture its shaders name. A
    diffuse the variation layout lists for a part but no source ships is a copy of the part's first
    diffuse (or of the texture its shaders use as diffuse). Later sources win on a name clash."""
    max_size = ctx.args.max_texture_size
    out: dict[str, dict] = {}
    if main_ytd is not None:
        for t in prepared_textures(main_ytd, max_size, ctx.notes):
            out[t["name"].lower()] = t
    cache: dict[tuple, list[dict]] = {}
    names: dict[Path, list[str]] = {}
    renames: dict[str, dict[str, str]] = {}
    own_diffuse: dict[str, str] = {}
    embedded_diffuse: dict[str, dict | None] = {}
    for part in parts:
        refs = texture_references(ctx, part)
        diffuse = [name for name, parameter in refs if parameter == DIFFUSE_SAMPLER]
        renames[part.key] = {}
        if part.main or part.ytd is None:
            if diffuse:
                own_diffuse[part.key] = diffuse[0]
            if part.prop and part.entry is None and diffuse:
                # a map object embeds its textures: its diffuse becomes the prop slot's diffuse (the
                # variation layout binds that name), never the retail texture of the replaced prop
                embedded_diffuse[part.key] = embedded_texture(ctx, part, diffuse[0])
            continue
        if part.ytd not in names:
            names[part.ytd] = [t["name"] for t in cpd.ytd_textures(part.ytd.read_bytes(), None, skip_bad=True)[0]]
        moved = slot_renames(part.entry, part.key, names[part.ytd], ctx.v) if part.entry else {}
        renames[part.key] = moved
        referenced = {name.lower() for name, _ in refs}
        wanted = {n.lower() for n in moved} | referenced
        key = (part.ytd, frozenset(wanted))
        if key not in cache:
            cache[key] = prepared_textures(part.ytd, max_size, ctx.notes, lambda n, w=wanted: n.lower() in w)
        textures = cache[key]
        for t in textures:
            if t["name"] in moved:
                out[moved[t["name"]].lower()] = t | {"name": moved[t["name"]]}
            elif t["name"].lower() in referenced:
                out[t["name"].lower()] = t
        if diffuse:
            own_diffuse[part.key] = moved.get(diffuse[0], diffuse[0])
    copies = []
    for part in parts:
        # the PC textures win over the retail ones of the slot: a diffuse letter the part does not
        # ship is a copy of its first one, not the retail texture painted for the retail drawable
        wanted = slot_diffuses(ctx.v, part.key)
        present = [name for name in wanted if name.lower() in out]
        source = out.get(present[0].lower()) if present else out.get(own_diffuse.get(part.key, "").lower())
        source = source or embedded_diffuse.get(part.key)
        if source is None:
            continue
        for name in wanted:
            if name.lower() not in out:
                out[name.lower()] = source | {"name": name}
                copies.append({"texture": name, "copyOf": source["name"]})
    return list(out.values()), renames, copies


def embedded_texture(ctx: PedContext, part: Part, name: str) -> dict | None:
    """Texture `name` of the part's embedded dictionary (trimmed like the .ytd ones), if it has it."""
    source = cpd.Source(part.path.read_bytes(), ctx.c, cpd.Limits(), graphics=True, entry=part.entry)
    pointer = int(source.materials()["shaderGroup"]["textureDictionaryPointer"], 16)
    if not pointer:
        return None
    max_size = ctx.args.max_texture_size
    for t in source.embedded_textures(pointer, repair_stride=True):
        if t["name"].lower() == name.lower():
            return cpd.trim_texture(resample(t, max_size), max_size)
    return None


def texture_references(ctx: PedContext, part: Part) -> list[tuple[str, int]]:
    """(texture name, PC parameter hash) of every named texture the part's shaders use."""
    source = cpd.Source(part.path.read_bytes(), ctx.c, cpd.Limits(), graphics=True, entry=part.entry)
    return [
        (t["textureName"], int(t["nameHash"], 16))
        for shader in source.materials()["shaders"]
        for t in shader["textureParameters"]
        if t["textureName"]
    ]


def convert_part(ctx: PedContext, part: Part, renames: dict[str, str], available: set[str]) -> tuple:
    """One PC drawable converted onto its retail template entry: (drawable graph, report)."""
    args, c = ctx.args, ctx.c
    blob = part.path.read_bytes()
    source = cpd.Source(blob, c, cpd.Limits(), graphics=True, entry=part.entry)
    material = source.materials()
    wanted = {int(s["nameHash"], 16) for s in material["shaders"]}
    has_skeleton = bool(source.skeleton())
    name = choose_template(ctx, part, has_skeleton)
    graph = ctx.props_base if part.prop else ctx.base
    template = ctx.template(graph, name, "props" if part.prop else "base", high_only=True, strip_textures=True)
    carriers = shader_carriers(ctx, wanted, template)
    names = dict(renames)
    lower = {old.lower(): new for old, new in renames.items()}
    fills = {"VolumeSampler": "givemechecker"}
    # hair shaders (ped_hair_cutout_alpha): an empty anisotropic noise sampler binds long_hair_noise as the
    # retail salton/bride hair does (--hair-noise checker: givemechecker, as juggernaut_03's hair)
    noise = "givemechecker" if ctx.hair_noise == "checker" else HAIR_NOISE
    fills[HAIR_NOISE_SAMPLER] = noise
    if noise != HAIR_NOISE:
        names.setdefault(HAIR_NOISE, noise)
    diffuses = slot_diffuses(ctx.v, part.key)
    if diffuses and diffuses[0].lower() in available:
        # the variation layout binds the slot's diffuse by name; a source that names a texture it does
        # not ship (exporters leave the authoring name) or none gets the slot's first diffuse instead
        for shader in material["shaders"]:
            for t in shader["textureParameters"]:
                if int(t["nameHash"], 16) != DIFFUSE_SAMPLER:
                    continue
                if t["textureName"] is None:
                    fills["DiffuseSampler"] = diffuses[0]
                elif lower.get(t["textureName"].lower(), t["textureName"]).lower() not in available:
                    names[t["textureName"]] = diffuses[0]
    placement = None
    if part.prop:
        # a map object (.ydr) is placed onto its anchor by default; a prop of a PC _p.ydd is already
        # authored in the anchor's frame (kept unless --prop-transform names it)
        anchor = PROP_KEY.fullmatch(part.key).group(1)
        default = "auto" if part.entry is None and anchor in PROP_ANCHOR_TOP else "none"
        given = ctx.prop_transforms.get(part.key, default)
        colours = part.entry is None and not getattr(args, "keep_prop_colours", False)
        if given != "none" or colours:
            blob, placement = place_prop(blob, c, part.key, given, colours, entry=part.entry)
        if part.entry is None and given == "none":
            ctx.notes.append(f"{part.key}: map object kept in its own frame (--prop-transform places it)")
    out, report = cpd.convert(
        blob,
        template,
        carriers,
        c,
        entry=part.entry,
        ped_skeleton=None if part.prop else ctx.pft,
        source_skeleton=None if part.prop else ctx.skeleton(part),
        merge_missing_bones=args.merge_missing_bones,
        texture_names=names,
        fill_textures=fills,
        ped_prop=part.prop,
    )
    embedded = report.pop("_embedded", [])
    if placement is not None:
        report["placement"] = placement
    if part.prop and part.entry is None:
        # a map object's diffuse went into the .ptd under the slot's name (plan_textures): embed only
        # the textures its shaders still reference
        linked = {row["name"].lower() for row in report["textureLinks"]}
        dropped = sorted(t["name"] for t in embedded if t["name"].lower() not in linked)
        embedded = [t for t in embedded if t["name"].lower() in linked]
        if dropped:
            report["embeddedTexturesMoved"] = dropped
    txd = embedded_dictionary(embedded, args) if embedded else None
    if txd is not None:
        report["embeddedDictionary"] = txd[1]
    report["template"]["entry"] = name
    report["shaderCarriers"] = [t.label for t in carriers]
    return out, txd[0] if txd else None, report


MAX_INPUT_DIRECTORY_ENTRIES = 4096
MAX_PROP_INPUT_BYTES = 256 * 1024 * 1024


def _plain_input(path: Path, *, directory: bool = False) -> None:
    """Refuse linked path components, special files and unbounded prop inputs before reading them."""
    for part in (path, *path.parents):
        if part.is_symlink():
            raise AssetError(f"symbolic links are refused: {part}")
    mode = path.stat().st_mode
    if directory:
        if not stat.S_ISDIR(mode):
            raise AssetError(f"not an input directory: {path}")
    elif not stat.S_ISREG(mode) or not 0 < path.stat().st_size <= MAX_PROP_INPUT_BYTES:
        raise AssetError(f"prop input must be a regular file of 1..{MAX_PROP_INPUT_BYTES} bytes: {path}")


def discover_props(ydd: Path) -> dict[str, Path]:
    """Find only <ped>_p.ydd/.ytd beside a ped or in its sibling props/ directory.

    Matching is case-insensitive, as for other PC mod inputs. Never recurse or pick the first
    of competing candidates, even if the copies currently have identical bytes. A texture file
    without its prop dictionary, or a pair split across both directories, is ambiguous input.
    """
    ydd = ydd.absolute()
    _plain_input(ydd)
    if ydd.suffix.lower() != ".ydd" or ydd.stem.lower().endswith("_p"):
        raise AssetError("props discovery needs the ped's component .ydd, not its _p.ydd")
    adjacent = ydd.parent
    sibling = adjacent.parent / "props"
    directories = [adjacent]
    if sibling != adjacent and (sibling.exists() or sibling.is_symlink()):
        directories.append(sibling)
    wanted = {f"{ydd.stem.lower()}_p.{ext}": ext for ext in ("ydd", "ytd")}
    found: dict[str, Path] = {}
    for directory in directories:
        _plain_input(directory, directory=True)
        with os.scandir(directory) as entries:
            for index, entry in enumerate(entries):
                if index >= MAX_INPUT_DIRECTORY_ENTRIES:
                    raise AssetError(f"input directory exceeds {MAX_INPUT_DIRECTORY_ENTRIES} entries: {directory}")
                ext = wanted.get(entry.name.lower())
                if ext is None:
                    continue
                path = directory / entry.name
                _plain_input(path)
                if ext in found:
                    raise AssetError(f"conflicting prop inputs: {found[ext]} and {path}")
                found[ext] = path
    if "ytd" in found and "ydd" not in found:
        raise AssetError(f"prop textures have no matching prop dictionary: {found['ytd']}")
    if len(found) == 2 and found["ydd"].parent != found["ytd"].parent:
        raise AssetError("prop dictionary and textures are split across adjacent and sibling props/ directories")
    return found


def resolve_props(args) -> None:
    if args.props_ydd is None and args.props_ytd is None:
        found = discover_props(args.ydd)
        args.props_ydd, args.props_ytd = found.get("ydd"), found.get("ytd")
    elif args.props_ydd is None:
        raise AssetError("--props-ytd requires --props-ydd")
    else:
        for path in (args.props_ydd, args.props_ytd):
            if path is not None:
                _plain_input(path)


def plan_parts(args, v: dict, names: dict[int, str]) -> list[Part]:
    """The mod's own entries (all, or --only), its discovered or explicit prop dictionary, then
    every --drawable KEY=FILE[:ENTRY] (replacing a mod entry of the same key)."""
    parts: dict[str, Part] = {}
    for path, ytd in ((args.ydd, args.ytd), (args.props_ydd, args.props_ytd)):
        if path is None:
            continue
        for key in legacy_dictionary_keys(path.read_bytes()):
            if key not in names:
                raise AssetError(f"{path.name}: key 0x{key:08x} is no component or prop name (<comp>_<nnn>_<u|r>)")
            name = names[key]
            if args.only and name not in args.only:
                continue
            if (path == args.ydd) == bool(PROP_KEY.fullmatch(name)):
                raise AssetError(f"{path.name}: {name} belongs in the {'prop' if path == args.ydd else 'ped'} .ydd")
            parts[name] = Part(name, path, name, ytd, main=True)
    if args.only and not set(args.only) <= parts.keys():
        raise AssetError("--only names no entry of the mod: " + ", ".join(sorted(set(args.only) - parts.keys())))
    for row in args.drawable:
        key, _, source = row.partition("=")
        file, _, entry = source.partition(":")
        if not (DRAWABLE_KEY.fullmatch(key) or PROP_KEY.fullmatch(key)) or not file:
            raise AssetError(f"--drawable takes KEY=FILE[:ENTRY] (KEY like uppr_001_u or p_eyes_000), not {row!r}")
        path = Path(file)
        siblings = {p.name.lower(): p for p in path.parent.glob("*") if p.is_file()}
        ytd, yft = siblings.get(path.stem.lower() + ".ytd"), siblings.get(path.stem.lower() + ".yft")
        if path.suffix.lower() == ".ydd":
            entry = entry or key
        elif path.suffix.lower() == ".ydr" and not entry:
            entry = None
        else:
            raise AssetError(f"--drawable {row}: FILE is a .ydd (with :ENTRY when the key differs) or a .ydr")
        parts.pop(key, None)
        parts[key] = Part(key, path, entry, ytd, yft=yft if path.suffix.lower() == ".ydd" else None)
    return list(parts.values())


def put_entry(graph: ndd.Graph, key: str, drawable: ndd.Graph | None, names: list[str] = ()) -> None:
    """Key `key` gets `drawable`, or (None) a placeholder: a copy of PLACEHOLDER whose textures are
    all the global givemechecker. A key the dictionary lacks is added (in hash order) first, as a copy
    of PLACEHOLDER, or (a rig without it, a converted drawable replacing the copy) of the first of `names`."""
    present = any(k == joaat(key) for k, _ in graph.entries())
    has_placeholder = any(k == joaat(PLACEHOLDER) for k, _ in graph.entries())
    if drawable is None and not has_placeholder:
        raise AssetError(f"{key}: --placeholders needs the cop's {PLACEHOLDER} (this rig has none)")
    if not present:
        ndd.add_entry(graph, key, PLACEHOLDER if has_placeholder else names[0])
    elif drawable is None and key != PLACEHOLDER:
        ndd.replace_entry(graph, key, PLACEHOLDER)
    if drawable is not None:
        ndd.put_drawable(graph, key, drawable)
    else:
        ndd.rename_textures(dict(graph.entries())[joaat(key)])


def convert_dictionary(ctx: PedContext, parts: list[Part], base_blob: bytes, graph: ndd.Graph, keep_base: bool, **kw):
    """Convert `parts` into `graph` (the retail dictionary, which also supplies the templates); with
    `keep_base` the retail entries stay (retail variation layout), else only the converted keys."""
    args = ctx.args
    base_ptd, main_ytd = kw["base_ptd"], kw["main_ytd"]
    base_names = texture_dictionary_names(base_ptd) if base_ptd else set()
    converted, skipped, reports = {}, [], []
    kept = []
    for part in parts:  # refuse (or leave out) unsupported shaders before any texture work
        source = cpd.Source(part.path.read_bytes(), ctx.c, cpd.Limits(), graphics=True, entry=part.entry)
        hashes = {int(s["nameHash"], 16) for s in source.materials()["shaders"]}
        reasons = sorted(UNSUPPORTED_SHADERS[h] for h in hashes & UNSUPPORTED_SHADERS.keys())
        if reasons and not args.skip_unsupported:
            raise AssetError(f"{part.key}: needs {', '.join(reasons)}; --skip-unsupported converts the rest")
        if reasons:
            skipped.append({"key": part.key, "reason": "needs " + ", ".join(reasons)})
        else:
            kept.append(part)
    parts = kept
    textures, renames, copies = plan_textures(ctx, parts, base_names, main_ytd)
    available = base_names | {t["name"].lower() for t in textures}
    for part in parts:
        out, txd, report = convert_part(ctx, part, renames[part.key], available)
        converted[part.key] = (out, txd)
        reports.append(report | {"key": part.key, "from": f"{part.path.name}:{part.entry or '(root)'}"})
    placeholders = [row["key"] for row in skipped] if args.placeholders else []
    if not converted:
        raise AssetError("nothing was converted")
    entry_names = ctx.entry_names(graph)
    for key, (out, txd) in converted.items():
        put_entry(graph, key, assemble(out, txd), entry_names)
    for key in placeholders:
        put_entry(graph, key, None)
    if not keep_base:
        ndd.select_entries(graph, [*converted, *placeholders])
    blob = ndd.write(graph, "pack")
    noise_users = [
        r["key"] for r in reports if any(link["name"].lower() == HAIR_NOISE for link in r["textureLinks"])
    ]  # fmt: skip
    extra, noise_report = [], None
    if noise_users:
        noise_report = {"mode": ctx.hair_noise, "texture": HAIR_NOISE, "keys": noise_users}
        if any(t["name"].lower() == HAIR_NOISE for t in textures):
            noise_report["shippedByMod"] = True  # the mod's own copy is in the .ptd already
        elif ctx.hair_noise == "packed":
            texture, row = ctx.noise_texture()
            extra = [texture]
            noise_report["packed"] = row
    ptd, ptd_report = build_ptd(textures, base_ptd, args.templates, extra=extra)
    verified = verify_ped_dictionary(base_blob, blob, converted, keep_base, placeholders)
    have = texture_dictionary_names(ptd)
    missing = sorted(n for n in verified["textureNames"] if n.lower() not in have | GLOBAL_TEXTURES)
    if extra and HAIR_NOISE not in have:
        missing.append(HAIR_NOISE)
    if missing:
        raise AssetError("the dictionary references textures the .ptd lacks: " + ", ".join(missing))
    report = {
        "converted": reports,
        "skipped": skipped,
        "placeholders": placeholders,
        "copiedDiffuses": copies,
        "dictionary": verified | {"bytes": len(blob), "sha256": digest(blob)},
        "textures": ptd_report,
        **({"hairNoise": noise_report} if noise_report else {}),
    }
    return blob, ptd, report


def verify_ped_dictionary(base: bytes, out: bytes, converted: dict, keep_base: bool, placeholders: list) -> dict:
    """Strict re-read; each converted key equals its converted drawable; with keep_base every other
    key equals the retail entry; the key set is exactly what was asked for."""
    before, after = ndd.parse(base), ndd.parse(out)
    entries = dict(after.entries())
    wanted = {joaat(k) for k in [*converted, *placeholders]}
    if keep_base:
        wanted |= {k for k, _ in before.entries()}
    if set(entries) != wanted:
        raise AssetError("written dictionary keys differ from the converted (and kept) keys")
    problems = []
    for key, (blob, txd) in converted.items():
        fresh = assemble(blob, txd)
        for obj in fresh.objects:
            if obj.kind == "index-data":
                obj.region = "gfx"
        problems += [f"{key}: {p}" for p in ndd.subgraph_compare(fresh.root, entries[joaat(key)])]
    touched = {joaat(k) for k in [*converted, *placeholders]}
    if keep_base:
        for k, old in before.entries():
            if k not in touched:
                problems += [f"{k:#010x}: {p}" for p in ndd.subgraph_compare(old, entries[k])]
    if problems:
        raise AssetError("written dictionary differs: " + "; ".join(problems[:5]))
    return {
        "entries": len(entries),
        "census": ndd.census(after),
        "convertedEntriesIdentical": True,
        "retailEntriesIdentical": keep_base,
        "textureNames": sorted(texture_names(ndd.describe(out))),
    }


def convert_ped(args, c: dict) -> tuple[dict[str, bytes], dict]:
    """A whole PC add-on ped: every component drawable of its .ydd (and the props of its _p.ydd),
    its textures, its variation file; or (--variations retail) PC parts placed into the retail
    template ped's variation layout, keeping its other drawables, textures and props."""
    resolve_props(args)
    retail = args.variations == "retail"
    if retail and not (args.ptd and args.props_pdd and args.props_ptd):
        raise AssetError("--variations retail needs the retail ped's --ptd, --props-pdd and --props-ptd")
    given = args.pft.name
    rig = choose_rig(args)
    if rig is not None:
        rig["insteadOf"] = given
    if retail:
        pmt, pmt_report = args.retail_pmt.read_bytes(), {"retail": args.retail_pmt.name}
    else:
        component, race = None, False
        if args.reduce_pmt:
            m = DRAWABLE_KEY.fullmatch(args.reduce_pmt)
            if not m:
                raise AssetError("--reduce-pmt takes a component drawable key (head_000_r)")
            component, race = m.group(1), m.group(3) == "r"
        pmt, pmt_report = convert_variations(args.ymt.read_bytes(), args.retail_pmt.read_bytes(), component, race)
    v = read_variations(pmt)
    ctx = PedContext(args, c, v)
    parts = plan_parts(args, v, ctx.names)
    names = variation_names(v)
    listed = names["drawables"] + names["alternatives"] + names["props"]
    if retail:
        foreign = [p.key for p in parts if p.key not in listed]
        if foreign:
            raise AssetError("not in the retail variation layout: " + ", ".join(foreign))
    else:
        # a mod entry its own .ymt does not list can never be picked (exporter leftovers): left out
        unlisted = [p.key for p in parts if p.main and p.key not in listed]
        parts = [p for p in parts if not (p.main and p.key in unlisted)]
        ctx.notes += [f"{key}: not in the mod's .ymt (never picked), left out" for key in unlisted]
    components = [p for p in parts if not p.prop]
    props = [p for p in parts if p.prop]
    unknown = sorted(set(ctx.prop_transforms) - {p.key for p in props})
    if unknown:
        raise AssetError("--prop-transform names no converted prop: " + ", ".join(unknown))
    base = args.pdd.read_bytes()
    files: dict[str, bytes] = {}
    files[f"{args.name}.pdd"], files[f"{args.name}.ptd"], report = convert_dictionary(
        ctx,
        components,
        base,
        ctx.base,
        retail,
        base_ptd=args.ptd.read_bytes() if retail else None,
        main_ytd=args.ytd,
    )
    files[f"{args.name}.pmt"] = pmt
    # the rig the drawables were converted on: the pack's skeleton fragment (the retail .pft as it is)
    files[f"{args.name}.pft"] = args.pft.read_bytes()
    if props:
        if ctx.props_base is None:
            raise AssetError("props need the retail prop dictionary (--props-pdd) for their templates")
        files[f"{args.name}_p.pdd"], files[f"{args.name}_p.ptd"], report["props"] = convert_dictionary(
            ctx,
            props,
            args.props_pdd.read_bytes(),
            ctx.props_base,
            retail,
            base_ptd=args.props_ptd.read_bytes() if retail else None,
            main_ytd=args.props_ytd,
        )
    elif retail:
        # the retail props as they are (no prop replaced): the ped's PropsName names this copy
        files[f"{args.name}_p.pdd"], files[f"{args.name}_p.ptd"] = (
            args.props_pdd.read_bytes(),
            args.props_ptd.read_bytes(),
        )
        report["props"] = {"retail": [args.props_pdd.name, args.props_ptd.name]}
    check = variation_check(
        v,
        files[f"{args.name}.pdd"],
        files[f"{args.name}.ptd"],
        files.get(f"{args.name}_p.pdd"),
        files.get(f"{args.name}_p.ptd"),
    )
    if args.strict_variations and not check["consistent"]:
        raise AssetError("variation check: " + describe_check(check) + " " + json.dumps(_missing(check)))
    report = {
        "kind": "gtavmenu-pc-ped-conversion",
        "ped": args.name,
        "variationsFrom": "retail" if retail else "mod",
        "rig": rig or {"rig": args.pft.name},
        "variations": pmt_report,
        **report,
        "notes": ctx.notes,
        "variationCheck": check,
        "files": {name: {"bytes": len(data), "sha256": digest(data)} for name, data in files.items()},
    }
    return files, report


def _missing(check: dict) -> dict:
    return {k: v["missing"] for k, v in check.items() if isinstance(v, dict) and v.get("missing")} | {
        k: check[k] for k in ("unlistedKeys", "unlistedPropKeys") if check[k]
    }


# ---- CLI ----------------------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    discover = sub.add_parser("props", help="find exact-stem props beside a ped or in its sibling props/ directory")
    discover.add_argument("--ydd", type=Path, required=True)
    discover.add_argument("--null", action="store_true", help="write extension/path pairs separated by NUL")
    cp = sub.add_parser("component", help="one PC component drawable into a retail ped dictionary")
    cp.add_argument("--ydd", type=Path, required=True, help="PC Legacy drawable dictionary")
    cp.add_argument("--entry", required=True, help="source key, e.g. head_000_r")
    cp.add_argument("--pdd", type=Path, required=True, help="retail PS5 ped dictionary (loose RSC7)")
    cp.add_argument("--target", required=True, help="dictionary key the converted drawable replaces")
    cp.add_argument("--pft", type=Path, required=True, help="retail PS5 ped skeleton fragment")
    cp.add_argument("--template-pdd", type=Path, help="retail dictionary of the template entry (default --pdd)")
    cp.add_argument("--template-entry", help="template key (default --target), e.g. a drawable whose skeleton fits")
    cp.add_argument(
        "--merge-missing-bones",
        action="store_true",
        help="skin source bones the template skeleton lacks to their nearest ancestor it has (reported)",
    )
    cp.add_argument("--pmt", type=Path, help="retail PS5 ped variation info (consistency check)")
    cp.add_argument("--shader-entry", action="append", default=[], help="more retail entries for shader schemas")
    cp.add_argument("--shader-template", type=Path, action="append", default=[], help="more retail .pdr")
    cp.add_argument("--rename", action="append", default=[], metavar="OLD=NEW", help="texture reference rename")
    cp.add_argument("--fill", action="append", default=[], metavar="PARAM=TEX", help="texture for an empty sampler")
    cp.add_argument(
        "--blank",
        action="append",
        default=[],
        metavar="ENTRY",
        help="give ENTRY a deep copy of --blank-with (retail accs_002_u: a 3 cm triangle), hiding that component",
    )
    cp.add_argument("--blank-with", default="accs_002_u")
    cp.add_argument(
        "--keep", action="append", default=[], metavar="ENTRY", help="keep only these keys (a new ped's dictionary)"
    )
    cp.add_argument(
        "--variations",
        choices=("require", "report"),
        default="require",
        help="report: pmt drawables without a key are recorded, not refused (PC add-ons ship them so)",
    )
    cp.add_argument("--output", type=Path, required=True)
    cp.add_argument(
        "--reference-dir", type=Path, help="accepted and not read (the layouts come from data/drawable_contracts)"
    )
    cp.add_argument("--ptd", type=Path, help="retail PS5 ped texture dictionary to extend (else only --texture)")
    cp.add_argument("--ytd", type=Path, help="PC texture dictionary with the --texture sources")
    cp.add_argument("--texture", action="append", default=[], metavar="PCNAME=NAME", help="PC texture to add")
    cp.add_argument(
        "--all-textures", action="store_true", help="add every --ytd texture under its own name (after --texture)"
    )
    cp.add_argument("--ptd-output", type=Path)
    cp.add_argument("--max-texture-size", type=int, default=1024)
    cp.add_argument("--templates", type=Path, help="retail texture template cache (default build/assets)")
    pp = sub.add_parser("ped", help="a whole PC add-on ped: every component, its textures, props and variations")
    pp.add_argument("--ydd", type=Path, required=True, help="the mod's component dictionary")
    pp.add_argument("--ytd", type=Path, help="the mod's textures (added whole)")
    pp.add_argument("--ymt", type=Path, help="the mod's variation file (the .pmt, unless --variations retail)")
    pp.add_argument("--yft", type=Path, help="the mod's skeleton: the bones entries without their own copy index")
    pp.add_argument("--props-ydd", type=Path, help="the mod's prop dictionary (<ped>_p.ydd)")
    pp.add_argument("--props-ytd", type=Path, help="the mod's prop textures (<ped>_p.ytd)")
    pp.add_argument("--name", required=True, help="the new ped's model name (member stem)")
    pp.add_argument("--pdd", type=Path, required=True, help="retail ped dictionary (templates; retail base)")
    pp.add_argument("--pft", type=Path, required=True, help="retail ped skeleton fragment (the rig)")
    pp.add_argument("--retail-pmt", type=Path, required=True, help="retail ped variation file")
    pp.add_argument("--ptd", type=Path, help="retail ped textures (--variations retail)")
    pp.add_argument("--props-pdd", type=Path, help="retail prop dictionary (prop templates; retail props)")
    pp.add_argument("--props-ptd", type=Path, help="retail prop textures (--variations retail)")
    pp.add_argument("--carrier", type=Path, action="append", default=[], help="retail .pdd lending shader schemas")
    pp.add_argument(
        "--shader-template", type=Path, action="append", default=[], help="retail .pdr lending schemas (last resort)"
    )
    pp.add_argument(
        "--variations",
        choices=("mod", "retail"),
        default="mod",
        help="mod: the mod's .ymt; retail: the retail ped's layout, drawables, textures and props, PC parts replacing keys",
    )
    pp.add_argument("--only", action="append", default=[], metavar="KEY", help="convert only these mod entries")
    pp.add_argument(
        "--drawable",
        action="append",
        default=[],
        metavar="KEY=FILE[:ENTRY]",
        help="KEY from another PC .ydd entry (or .ydr); its textures come from FILE's .ytd, renamed for KEY",
    )
    pp.add_argument("--reduce-pmt", metavar="KEY", help="the mod's .ymt reduced to this one drawable's component")
    pp.add_argument("--skip-unsupported", action="store_true", help="leave out drawables with unsupported shaders")
    pp.add_argument("--placeholders", action="store_true", help="skipped drawables become a tiny placeholder")
    pp.add_argument("--strict-variations", action="store_true", help="refuse unless the variation round trip is whole")
    pp.add_argument(
        "--prop-transform",
        action="append",
        default=[],
        metavar="KEY=RX,RY,RZ,DX,DY,DZ",
        help="place prop KEY in its anchor bone's frame: turn (degrees about X, then Y, then Z) and move (metres);"
        " KEY=auto (default for a map object .ydr on a head anchor: Z-up base onto the top of the head) or KEY=none",
    )
    pp.add_argument(
        "--keep-prop-colours",
        action="store_true",
        help="keep a map object prop's vertex colour 0 (default: the retail ped props' value, not its baked map ambient)",
    )
    pp.add_argument(
        "--hair-noise",
        choices=HAIR_NOISE_MODES,
        default="packed",
        help="hair shaders' empty AnisoNoiseSpecSampler: packed (long_hair_noise, a copy of the retail texture in"
        " the ped's .ptd; default), reference (long_hair_noise by name only) or checker (givemechecker)",
    )
    pp.add_argument(
        "--hair-noise-ptd",
        type=Path,
        help=f"the retail dictionary holding {HAIR_NOISE} (default <--templates>/{HAIR_NOISE_PTD})",
    )
    pp.add_argument("--merge-missing-bones", action="store_true")
    pp.add_argument("--max-texture-size", type=int, default=1024)
    pp.add_argument("--templates", type=Path, help="retail texture template cache (default build/assets)")
    pp.add_argument(
        "--reference-dir", type=Path, help="accepted and not read (the layouts come from data/drawable_contracts)"
    )
    pp.add_argument("--out", type=Path, required=True, help="output directory (files must not exist)")
    vp = sub.add_parser("pmt", help="a PC .ymt as a PS5 .pmt (struct infos checked against a retail .pmt)")
    vp.add_argument("--ymt", type=Path, required=True)
    vp.add_argument("--retail-pmt", type=Path, required=True)
    vp.add_argument("--component", help="reduce to this one component (drawable 000)")
    vp.add_argument("--race", action="store_true", help="--component's drawable is _r (race textures)")
    vp.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.command == "props":
        try:
            found = discover_props(args.ydd)
        except (AssetError, OSError) as error:
            raise SystemExit(f"error: {error}") from None
        if args.null:
            for ext in ("ydd", "ytd"):
                if ext in found:
                    sys.stdout.write(f"{ext}\0{found[ext]}\0")
        else:
            print(json.dumps({ext: str(path) for ext, path in sorted(found.items())}))
        return 0
    if args.command == "ped":
        return main_ped(args, parser)
    if args.command == "pmt":
        if args.output.exists():
            raise SystemExit(f"refusing to overwrite {args.output}")
        try:
            out, report = convert_variations(
                args.ymt.read_bytes(), args.retail_pmt.read_bytes(), args.component, args.race
            )
        except (AssetError, OSError, ValueError, struct.error) as error:
            raise SystemExit(f"error: {error}") from None
        cpd.write_new(args.output, out)
        print(f"wrote {args.output} bytes={len(out)} sha256={report['sha256']} reduced={args.component}")
        return 0
    if bool(args.ptd_output) != bool(args.ytd and (args.texture or args.all_textures)) or (
        args.ptd and not args.ptd_output
    ):
        parser.error("--ytd, --texture and --ptd-output go together (--ptd optionally)")
    report_path = args.output.with_name(args.output.name + ".report.json")
    for path in (args.output, report_path, args.ptd_output):
        if path is not None and path.exists():
            raise SystemExit(f"refusing to overwrite {path}")
    try:
        c = cpd.contracts(args.reference_dir)
        out, report = convert_component(args, c)
        ptd = None
        if args.ptd_output:
            ptd, report["textures"] = merge_textures(args)
            have = {name.lower() for name in report["textures"]["names"]}
            missing = sorted(n for n in report["dictionary"]["textureNames"] if n.lower() not in have | GLOBAL_TEXTURES)
            if missing:
                raise AssetError("the dictionary references textures the .ptd lacks: " + ", ".join(missing))
    except (AssetError, OSError, ValueError, KeyError, struct.error) as error:
        raise SystemExit(f"error: {error}") from None
    cpd.write_new(args.output, out)
    if ptd is not None:
        cpd.write_new(args.ptd_output, ptd)
    cpd.write_new(report_path, cpd.canonical(report))
    print(
        f"wrote {args.output} bytes={len(out)} entry={args.target} <- {args.ydd.name}:{args.entry} "
        f"geometries={len(report['geometries'])} shaders=" + ",".join(s["name"] for s in report["shaders"])
    )
    if ptd is not None:
        print(f"wrote {args.ptd_output} bytes={len(ptd)} textures={report['textures']['textures']}")
    return 0


def main_ped(args, parser) -> int:
    if args.variations == "mod" and not args.ymt:
        parser.error("--ymt is required unless --variations retail")
    report_path = args.out / f"{args.name}.report.json"
    try:
        c = cpd.contracts(args.reference_dir)
        files, report = convert_ped(args, c)
    except (AssetError, OSError, ValueError, KeyError, struct.error) as error:
        raise SystemExit(f"error: {error}") from None
    args.out.mkdir(parents=True, exist_ok=True)
    for name in [*files, report_path.name]:
        if (args.out / name).exists():
            raise SystemExit(f"refusing to overwrite {args.out / name}")
    for name, data in files.items():
        cpd.write_new(args.out / name, data)
    cpd.write_new(report_path, cpd.canonical(report))
    for section in (report, report.get("props") or {}):
        for row in section.get("converted", []):
            shaders = ",".join(s["name"] for s in row["shaders"])
            print(f"converted {row['key']} <- {row['from']} onto {row['template']['entry']} shaders={shaders}")
        for row in section.get("skipped", []):
            print(f"skipped {row['key']}: {row['reason']}")
        for row in section.get("copiedDiffuses", []):
            print(f"texture {row['texture']} is a copy of {row['copyOf']}")
        for row in section.get("converted", []):
            if "renderMaskHigh" in row:
                mask = row["renderMaskHigh"]
                print(
                    f"render mask {row['key']}: {mask['template']} -> {mask['written']} (its shaders' render buckets)"
                )
        noise = section.get("hairNoise")
        if noise:
            where = f" (copy from {noise['packed']['from']})" if "packed" in noise else ""
            where = " (the mod's own copy)" if noise.get("shippedByMod") else where
            print(f"hair noise: {noise['texture']} {noise['mode']}{where} for {', '.join(noise['keys'])}")
    if "bones" in report["rig"]:
        print(
            f"rig: {report['rig']['rig']} ({report['rig']['bones']} bones; the mod's skeleton is not the {report['rig']['insteadOf']} rig)"
        )
    for note in report["notes"]:
        print(f"note: {note}")
    for name, row in report["files"].items():
        print(f"wrote {args.out / name} bytes={row['bytes']} sha256={row['sha256']}")
    check = report["variationCheck"]
    print("variations: " + describe_check(check))
    for key, missing in _missing(check).items():
        print(f"  {key}: {', '.join(missing)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

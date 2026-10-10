#!/usr/bin/env python3
"""Convert a PC mod's own wheel models into a runtime wheel pack (roadmap F1.3; vehicle-data-files-re.md B.1).

A carcols `<Wheels>` item is found by the hash of its `<wheelName>`, and its model is the DRAWABLE of that
name: the engine requests `<wheelName>` from the drawable store and parents its texture dictionary to the
global vehicle-mod dictionary (vehicle-data-files-re.md B.1). So a wheel becomes one pack member
`<wheelname>.pdr` that carries its own textures embedded (a pack .ptd is not on the wheel's texture
chain), plus a CARCOLS_FILE row with the `<Wheels>` items.

  model  one PC wheel `.ydr` -> `<name>.pdr`:
         - prop drawable route of convert_pc_drawable.py (only the template's root words are kept; the
           source supplies geometry, bounds, LOD distance and shaders; shader schemas from the retail
           wheel template, `vehicle_tire`); zero triangle counts (an exporter quirk on triangle lists)
           are rebuilt from the index count;
         - rigid repair: a wheel with no skeleton whose geometry carries a one-bone palette and blend
           weights/indices (exporters write skinned vertices for rigid wheels; retail wheels are rigid,
           SkeletonBinding 0) is made rigid on the PC source first: every vertex must put its full
           weight on one influence; the palette and the blend components are removed and the models'
           SkeletonBinding cleared;
         - textures: the drawable's embedded dictionary (and a `<name>.ytd` beside it, when the mod has
           one) is converted (convert_pc_ytd_writer.py) and embedded in the shader group. A texture the
           shaders name that the dictionary lacks stays a reference when the retail shared vehicle
           dictionary has it (corpus/vehshare-a.ptd: `vehicle_generic_*`, `blank_normal`, ...; resolved
           through the vehicle-mod dictionary chain like retail wheels' references); any other missing
           texture gets a neutral stand-in (listed);
         - round trip: the written .pdr is parsed back in full (native_drawable_dictionary.py: every
           byte owned, no stray pointers), compared object by object with the converted drawable plus
           its dictionary, and its counts are checked (geometries = source geometries, embedded
           texture names, references outside the dictionary only to shared names, SkeletonBinding 0,
           no bone palette).
  pack   the whole route from a mod as downloaded (its .oiv/.zip package, dlc.rpf or folder; untrusted
         data, bounded reads): the `<Wheels>` items of its carcols, the models they name (`--only`
         picks), each converted as above (a custom-tyre `<wheelVariation>` model the mod ships too),
         a carcols row through tools/prepare_carcols.py `--ship-wheel` (labels `<prefix>_WHEEL_<n>`;
         text from the mod's own text table, else from the mod's label key, e.g. CHIRON_CLASSIQUE_01 ->
         "Chiron Classique 01"; `--label NAME=TEXT` sets one), `--type` moves every wheel into one
         wheel type, then tools/build_runtime_pack.py and tools/validate_runtime_pack.py. A `.yft`
         wheel model is refused (not supported yet) and named.

  convert_pc_wheel.py model --source wheel_classique_01.ydr --output out/wheel_classique_01.pdr \\
      --templates build/retail-templates
  convert_pc_wheel.py pack --source "Gta5KoRn Car Pack v1.3.oiv" --id gtavmenu-korn-wheels-v1 \\
      --templates build/retail-templates --work build/convert-wheels/ID --output-root build/custom-assets
  convert_pc_wheel.py pack --source MOD --list          (the mod's wheels and their models; writes nothing)

Retail inputs (fetch_retail_templates.py cache): wheels/wheel_loride_01.pdr (template and vehicle_tire
schema), corpus/vehshare-a.ptd (shared texture names), corpus/tornado6.ptd and corpus/banshee-a.ptd
(texture writer metadata). Needs numpy (textures; the `packs` extra) and nothing else outside the standard
library.
"""

from __future__ import annotations

import argparse
import hashlib
import math
import mmap
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib
from dataclasses import dataclass, field, replace
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

import convert_pc_drawable as cpd  # noqa: E402
import native_drawable_dictionary as ndd  # noqa: E402
from gtavmenu_tools import runtime_pack  # noqa: E402
from gtavmenu_tools.asset_formats import (  # noqa: E402
    AssetError,
    Limits,
    gxt2_entries,
    read_rpf_member,
    rpf_header,
    rpf_members,
)
from gtavmenu_tools.hashes import joaat  # noqa: E402
from gtavmenu_tools.ymap import MOD_LIMITS, label_name  # noqa: E402

TEMPLATE = "wheels/wheel_loride_01.pdr"
SHARED = "corpus/vehshare-a.ptd"
WHEEL_TYPES = (  # carcols <Wheels> children in order (VWT_*); the engine has 13 per-type arrays
    "sport",
    "muscle",
    "lowrider",
    "suv",
    "offroad",
    "tuner",
    "bike",
    "highend",
    "bennys-original",
    "bennys-bespoke",
    "open-wheel",
    "street",
    "track",
)
NAME = re.compile(r"[A-Za-z0-9_]{1,63}")
LABEL_TEXT = re.compile(r"[\x20-\x7d]{1,63}")  # printable ASCII without ~ (a game text format token)
BLEND = ("BlendWeights", "BlendIndices")
MAX_MEMBER_BYTES = 64 * 1024 * 1024
MAX_READ_BYTES = 512 * 1024 * 1024
MAX_PACKAGE_RPF = 4 * 1024 * 1024 * 1024
MAX_PACKAGE_TOTAL = 8 * 1024 * 1024 * 1024  # every member unpacked from one .oiv/.zip together
MAX_PACKAGE_MEMBERS = 4096
MAX_DEPTH = 4
MAX_WHEELS = 32
SYS = cpd.SYS


class WheelError(AssetError):
    pass


def digest(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


# ---- rigid repair (PC source) -------------------------------------------------------------------


def _field(layout: dict, name: str) -> tuple[int, int]:
    row = layout["fields"][name]
    return row["offset"], row["bytes"]


def _put(payload: bytearray, at: int, layout: dict, name: str, value: int) -> None:
    offset, size = _field(layout, name)
    payload[at + offset : at + offset + size] = value.to_bytes(size, "little")


def rigid_source(blob: bytes, c: dict) -> tuple[bytes, dict | None]:
    """The PC wheel with its skinned-but-rigid geometry made rigid (see the module docstring); the blob
    unchanged and None when nothing is skinned. Refuses a wheel with a skeleton, a palette of more than
    one bone or other than bone 0, a vertex whose weight is split, or a shared vertex buffer."""
    source = cpd.Source(blob, c, Limits(), graphics=True)
    source.zero_triangle_counts = True
    material = source.materials()
    geometry = source.geometry(len(material["shaders"]))
    rows = source.meshes(geometry)
    skinned = [r for r in rows if r["boneIds"] or any(x["semantic"] in BLEND for x in r["components"])]
    if not skinned:
        return blob, None
    if source.skeleton():
        raise WheelError("the wheel has a skeleton and skinned geometry; only rigid wheels convert")
    gc, pc = c["geometry"], c["pcMesh"]
    payload = bytearray(source.payload)
    items = {
        (lod["lod"], m["index"], g["index"]): g["systemOffset"]
        for lod in geometry["lods"]
        for m in lod["models"]
        for g in m["geometries"]
    }
    buffers: set[int] = set()
    outside = 0
    for row in skinned:
        names = [x["semantic"] for x in row["components"]]
        if row["boneIds"] not in ([], [0]) or sorted(set(names) & set(BLEND)) != sorted(BLEND):
            raise WheelError(
                f"geometry {row['geometryIndex']}: palette {row['boneIds']} / blend components "
                "do not describe a one-bone rigid mesh"
            )
        comps = {x["semantic"]: x for x in row["components"]}
        if any(comps[s]["bytes"] != 4 for s in BLEND):
            raise WheelError(f"geometry {row['geometryIndex']}: blend components are not 4-byte vectors")
        geo = items[(row["lod"], row["modelIndex"], row["geometryIndex"])]
        vb = struct.unpack_from("<Q", payload, geo + _field(gc["geometry"], "VertexBufferPointer")[0])[0] - SYS
        if vb in buffers:
            raise WheelError("two geometries share a vertex buffer")
        buffers.add(vb)
        decl = struct.unpack_from("<Q", payload, vb + _field(pc["vertexBuffer"], "InfoPointer")[0])[0] - SYS
        stride, count = row["vertexStride"], row["vertexCount"]
        start = row["vertexStreams"][0]["systemOffset"]
        keep = [x for x in row["components"] if x["semantic"] not in BLEND]
        new_stride = sum(x["bytes"] for x in keep)
        weights, indices = comps["BlendWeights"]["offset"], comps["BlendIndices"]["offset"]
        out = bytearray()
        for v in range(count):
            vertex = payload[start + v * stride : start + (v + 1) * stride]
            w = vertex[weights : weights + 4]
            if sorted(w) != [0, 0, 0, 255]:
                raise WheelError(f"geometry {row['geometryIndex']} vertex {v}: blend weight split {list(w)}")
            outside += vertex[indices + w.index(255)] != 0
            out += b"".join(vertex[x["offset"] : x["offset"] + x["bytes"]] for x in keep)
        payload[start : start + count * stride] = out + bytes(count * (stride - new_stride))
        flags_at = decl + _field(pc["declaration"], "Flags")[0]
        flags = struct.unpack_from("<I", payload, flags_at)[0]
        semantics = pc["semantics"]
        flags &= ~sum(1 << semantics[s]["value"] for s in BLEND)
        _put(payload, decl, pc["declaration"], "Flags", flags)
        _put(payload, decl, pc["declaration"], "Stride", new_stride)
        _put(payload, decl, pc["declaration"], "Count", len(keep))
        _put(payload, vb, pc["vertexBuffer"], "VertexStride", new_stride)
        _put(payload, geo, gc["geometry"], "VertexStride", new_stride)
        _put(payload, geo, gc["geometry"], "BoneIdsCount", 0)
        _put(payload, geo, gc["geometry"], "BoneIdsPointer", 0)
    bindings = []
    for lod in geometry["lods"]:
        for model in lod["models"]:
            at = model["systemOffset"] + _field(gc["model"], "SkeletonBinding")[0]
            binding = struct.unpack_from("<I", payload, at)[0]
            if binding not in (0, 0x101):  # rigid, or HasSkin with one matrix (bone index 0)
                raise WheelError(f"model {model['index']}: SkeletonBinding {binding:#x} is not a one-bone binding")
            bindings.append(binding)
            struct.pack_into("<I", payload, at, 0)
    header = source.header
    flags = (int(str(header["systemFlags"]), 0), int(str(header["graphicsFlags"]), 0))
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    out_blob = struct.pack("<4sIII", b"RSC7", header["version"], *flags) + packer.compress(bytes(payload))
    out_blob += packer.flush()
    check = cpd.Source(out_blob, c, Limits(), graphics=True)  # the repaired source reads back consistently
    check.zero_triangle_counts = True
    after = check.meshes(check.geometry(len(check.materials()["shaders"])))
    for old, new in zip(rows, after, strict=True):
        kept = [(x["semantic"], x["type"]) for x in old["components"] if x["semantic"] not in BLEND]
        if [(x["semantic"], x["type"]) for x in new["components"]] != kept or new["boneIds"]:
            raise WheelError(f"geometry {old['geometryIndex']}: the rigid repair did not read back")
        if (new["vertexCount"], new["indices"]["sha256"]) != (old["vertexCount"], old["indices"]["sha256"]):
            raise WheelError(f"geometry {old['geometryIndex']}: the rigid repair changed counts or indices")
        a, b = old["vertexStreams"][0]["systemOffset"], new["vertexStreams"][0]["systemOffset"]
        for v in range(old["vertexCount"]):
            before = source.system[a + v * old["vertexStride"] : a + (v + 1) * old["vertexStride"]]
            want = b"".join(
                before[x["offset"] : x["offset"] + x["bytes"]] for x in old["components"] if x["semantic"] not in BLEND
            )
            if bytes(check.system[b + v * new["vertexStride"] : b + (v + 1) * new["vertexStride"]]) != want:
                raise WheelError(f"geometry {old['geometryIndex']} vertex {v}: the rigid repair changed a value")
    return out_blob, {
        "geometries": len(skinned),
        "removedComponents": list(BLEND),
        "palettes": sorted({tuple(r["boneIds"]) for r in skinned}),
        "blendIndicesNotZero": outside,
        "modelSkeletonBindings": [f"{b:#x}" for b in bindings],
    }


# ---- one wheel ----------------------------------------------------------------------------------


@dataclass
class Retail:
    template: cpd.Template
    shared: frozenset[str]
    templates: Path
    c: dict


def load_retail(templates: Path, reference_dir: Path | None) -> Retail:
    missing = [
        n for n in (TEMPLATE, SHARED, "corpus/tornado6.ptd", "corpus/banshee-a.ptd") if not (templates / n).is_file()
    ]
    if missing:
        raise WheelError(f"the template cache {templates} lacks {', '.join(missing)} (run fetch-templates)")
    c = cpd.contracts(reference_dir)
    template = cpd.Template((templates / TEMPLATE).read_bytes(), c, Path(TEMPLATE).name)
    graph = ndd.parse_texture_dictionary((templates / SHARED).read_bytes())
    shared = frozenset(
        bytes(o.data).rstrip(b"\0").decode("ascii").lower() for o in graph.objects if o.kind == "texture-name"
    )
    return Retail(template, shared, templates, c)


def _texture_names(graph: ndd.Graph, kind_owner: str) -> list[str]:
    """Texture names of `kind_owner` objects ('txd-texture' embedded, 'texture' references)."""
    names = []
    for obj in graph.objects:
        if obj.kind == kind_owner and 0x28 in obj.refs:
            names.append(bytes(obj.refs[0x28][0].data).rstrip(b"\0").decode("ascii"))
    return sorted(names, key=str.lower)


def convert_wheel(
    source_blob: bytes, name: str, retail: Retail, *, ytd: bytes | None = None, max_texture_size: int | None = None
) -> tuple[bytes, dict]:
    """One PC wheel drawable -> the PS5 `<name>.pdr` with its textures embedded, and its report."""
    if not NAME.fullmatch(name):
        raise WheelError(f"wheel name {name!r} is not [A-Za-z0-9_] of at most 63 characters")
    rigid_blob, rigid = rigid_source(source_blob, retail.c)
    blob, report = cpd.convert(
        rigid_blob,
        retail.template,
        [retail.template],
        retail.c,
        prop=name.lower(),
        lights=False,
        zero_triangle_counts=True,
    )
    embedded = report.pop("_embedded", [])
    sources = [embedded] + ([cpd.ytd_textures(ytd, max_texture_size)[0]] if ytd else [])
    own = {cpd.ps5_texture_name(t["name"]).lower() for textures in sources for t in textures}
    links = [
        link
        for link in report["textureLinks"]
        if link["name"].lower() in own or link["name"].lower() not in retail.shared
    ]
    if not own and not links:
        raise WheelError(f"{name}: the wheel names only shared textures and embeds none")
    ptd, textures = cpd.texture_dictionary(links, sources, retail.templates, max_texture_size, only_linked=False)
    drawable = ndd.parse_drawable(blob, coverage=False)
    ndd.embed_textures(drawable, ndd.parse_texture_dictionary(ptd))
    out = ndd.write(drawable, "pack")
    proof = round_trip(out, blob, ptd, report, retail.shared)
    report["wheel"] = {
        "name": name.lower(),
        "member": f"{name.lower()}.pdr",
        "rigidRepair": rigid,
        "embeddedDictionary": textures,
        "sharedReferences": sorted(
            {link["name"] for link in report["textureLinks"]} - set(proof["embeddedTextures"]), key=str.lower
        ),
        "roundTrip": proof,
        "output": {"bytes": len(out), "sha256": digest(out)},
    }
    return out, report


def round_trip(out: bytes, drawable_blob: bytes, ptd: bytes, report: dict, shared: frozenset[str]) -> dict:
    """Parse the written .pdr back in full and compare it with what was converted (module docstring)."""
    written = ndd.parse_drawable(out)  # full coverage: every nonzero byte owned, no undeclared pointer
    fresh = ndd.parse_drawable(drawable_blob, coverage=False)
    ndd.embed_textures(fresh, ndd.parse_texture_dictionary(ptd))
    problems = ndd.subgraph_compare(fresh.root, written.root)
    if problems:
        raise WheelError("written wheel differs from the converted drawable: " + "; ".join(problems[:5]))
    kinds: dict[str, int] = {}
    for obj in written.objects:
        kinds[obj.kind] = kinds.get(obj.kind, 0) + 1
    embedded = _texture_names(written, "txd-texture")
    references = _texture_names(written, "texture")
    lowered = {n.lower() for n in embedded}
    outside = sorted({n for n in references if n.lower() not in lowered and n.lower() not in shared})
    if outside:
        raise WheelError(f"texture references neither embedded nor shared: {', '.join(outside)}")
    geometries = len(report["geometries"])
    if kinds.get("geometry") != geometries:
        raise WheelError(f"written geometries {kinds.get('geometry')} != converted {geometries}")
    if kinds.get("bone-ids") or kinds.get("skeleton"):
        raise WheelError("the written wheel keeps a bone palette or skeleton")
    bindings = [struct.unpack_from("<I", o.data, 0x28)[0] for o in written.objects if o.kind == "model"]
    if any(bindings):
        raise WheelError(f"model SkeletonBinding not rigid: {[hex(b) for b in bindings]}")
    if kinds.get("txd") != 1:
        raise WheelError("the written wheel has no embedded texture dictionary")
    return {
        "parsedInFull": True,
        "identicalToConverted": True,
        "geometries": geometries,
        "models": len(bindings),
        "embeddedTextures": embedded,
        "textureReferences": len(references),
        "census": ndd.census(written),
    }


# ---- the mod ------------------------------------------------------------------------------------


@dataclass
class ModFiles:
    """What a mod ships that the wheel route reads, by lower-case base name (first copy wins)."""

    metas: dict[str, list[tuple[str, bytes]]] = field(default_factory=dict)  # carcols*.meta
    texts: list[tuple[str, bytes]] = field(default_factory=list)  # global.gxt2 (English first)
    models: dict[str, tuple[str, bytes]] = field(default_factory=dict)  # <name>.ydr/.yft/.ytd
    notes: list[str] = field(default_factory=list)


def _wanted_meta(base: str) -> bool:
    return base.endswith(".meta") and "carcols" in base


def walk_archive(view, where: str, files: ModFiles, models: set[str] | None, budget: list[int]) -> None:
    """Members of a PC RPF7 (OPEN/NONE tables; uncompressed nested archives up to MAX_DEPTH levels).
    `models` None: collect carcols metas and text tables; else the base names in `models`."""
    pending = [(0, len(view), "", 0)]
    while pending:
        base, size, prefix, depth = pending.pop()

        def read_at(offset: int, count: int, base: int = base, size: int = size) -> bytes:
            if offset < 0 or count < 0 or offset + count > size:
                raise AssetError("archive read outside its bounds")
            return bytes(view[base + offset : base + offset + count])

        try:
            end = rpf_header(read_at(0, 16), size)["tableEnd"]
            table = rpf_members(read_at(0, end), MOD_LIMITS, file_size=size, read_at=read_at, name_check=label_name)
        except AssetError as error:
            if not prefix:
                raise WheelError(f"{where}: {error} (an encrypted PC archive: export its files with OpenIV)") from error
            files.notes.append(f"{where}:{prefix.rstrip('/')}: {error}")
            continue
        for member in table:
            path = prefix + member.name
            lower = member.name.lower()
            if lower.endswith(".rpf") and not member.resource:
                if member.compressed or member.encrypted or depth >= MAX_DEPTH:
                    files.notes.append(f"{where}:{path}: nested archive is compressed, encrypted or too deep")
                else:
                    pending.append((base + member.offset, member.size, path + "/", depth + 1))
                continue
            take = (_wanted_meta(lower) or lower == "global.gxt2") if models is None else lower in models
            if not take:
                continue
            if member.size > MAX_MEMBER_BYTES or member.encrypted:
                files.notes.append(f"{where}:{path}: encrypted or larger than {MAX_MEMBER_BYTES} bytes")
                continue
            budget[0] += member.size
            if budget[0] > MAX_READ_BYTES:
                raise WheelError("the mod's matching files exceed the reader budget")
            blob = read_rpf_member(read_at(member.offset, member.size), replace(member, offset=0), MAX_MEMBER_BYTES)
            _add(files, lower, f"{where}:{path}", blob, models)


def _add(files: ModFiles, base: str, where: str, blob: bytes, models: set[str] | None) -> None:
    if models is not None:
        if base in files.models:
            files.notes.append(f"{where}: a second {base} (kept {files.models[base][0]})")
        else:
            files.models[base] = (where, blob)
    elif base == "global.gxt2":
        english = "american" in where.lower()
        files.texts.insert(0, (where, blob)) if english else files.texts.append((where, blob))
    else:
        files.metas.setdefault(base, []).append((where, blob))


def _take_file(path: Path, label: str, budget: list[int]) -> bytes:
    """A loose file's bytes within the per-file and total read limits (as walk_archive's members)."""
    size = path.stat().st_size
    if size > MAX_MEMBER_BYTES:
        raise WheelError(f"{label}: larger than {MAX_MEMBER_BYTES} bytes")
    budget[0] += size
    if budget[0] > MAX_READ_BYTES:
        raise WheelError(f"the mod's matching files exceed the {MAX_READ_BYTES} byte reader budget")
    return path.read_bytes()


def _loose(path: Path, files: ModFiles, models: set[str] | None, budget: list[int]) -> None:
    base = path.name.lower()
    take = (_wanted_meta(base) or base == "global.gxt2") if models is None else base in models
    if take:
        _add(files, base, str(path), _take_file(path, str(path), budget), models)


def _archive_file(path: Path, where: str, files: ModFiles, models: set[str] | None, budget: list[int]) -> None:
    with path.open("rb") as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as view:
        walk_archive(view, where, files, models, budget)


def unpack_packages(source: Path, work: Path) -> list[tuple[Path, str]]:
    """The .rpf archives inside an .oiv/.zip package, streamed into `work` (bounded); loose files of the
    package are written there too (under their base names). Returns (file, label) pairs."""
    out: list[tuple[Path, str]] = []
    work.mkdir(parents=True, exist_ok=True)
    try:
        package = zipfile.ZipFile(source)
    except zipfile.BadZipFile as error:
        raise WheelError(f"{source.name}: not a readable .oiv/.zip package ({error})") from error
    with package:
        infos = package.infolist()
        if len(infos) > MAX_PACKAGE_MEMBERS:
            raise WheelError(f"{source.name}: more than {MAX_PACKAGE_MEMBERS} entries")
        unpacked = 0
        for index, info in enumerate(infos):
            if info.is_dir():
                continue
            base = PurePosixPath(info.filename.replace("\\", "/")).name.lower()
            if not re.fullmatch(r"[a-z0-9_.+ -]{1,96}", base):
                continue
            if not (base.endswith((".rpf", ".ydr", ".yft", ".ytd", ".gxt2")) or _wanted_meta(base)):
                continue
            if info.file_size > MAX_PACKAGE_RPF:
                raise WheelError(f"{source.name}:{info.filename}: larger than {MAX_PACKAGE_RPF} bytes")
            unpacked += info.file_size
            if unpacked > MAX_PACKAGE_TOTAL:
                raise WheelError(f"{source.name}: the members to unpack exceed {MAX_PACKAGE_TOTAL} bytes")
            target = work / f"{index:03d}-{base.replace(' ', '_')}"
            with package.open(info) as stream, target.open("xb") as sink:
                shutil.copyfileobj(stream, sink, 1 << 20)
            out.append((target, f"{source.name}:{info.filename}"))
    return out


def read_mod(source: Path, work: Path, models: set[str] | None = None) -> ModFiles:
    """Carcols metas and text tables (models None), or the named model files, of a mod."""
    files, budget = ModFiles(), [0]
    if source.is_symlink():
        raise WheelError(f"{source}: symbolic links are refused")
    if source.is_dir():
        for directory, dirs, names in os.walk(source, followlinks=False):
            for name in sorted(dirs + names):
                if Path(directory, name).is_symlink():
                    raise WheelError(f"{Path(directory, name)}: symbolic links are refused")
            for name in sorted(names):
                path = Path(directory, name)
                if name.lower().endswith(".rpf") and path.stat().st_size:
                    _archive_file(path, str(path.relative_to(source)), files, models, budget)
                elif name.lower().endswith((".oiv", ".zip")):
                    unpacked_dir = work / ("package-" + re.sub(r"[^A-Za-z0-9_.-]", "_", name))
                    if not unpacked_dir.is_dir():
                        unpack_packages(path, unpacked_dir)
                    for unpacked in sorted(unpacked_dir.iterdir()):
                        _read_unpacked(unpacked, f"{name}:{unpacked.name[4:]}", files, models, budget)
                else:
                    _loose(path, files, models, budget)
    elif source.is_file() and source.name.lower().endswith(".rpf"):
        _archive_file(source, source.name, files, models, budget)
    elif source.is_file() and source.name.lower().endswith((".oiv", ".zip")):
        unpacked_dir = work / "package"
        if not unpacked_dir.is_dir():
            unpack_packages(source, unpacked_dir)
        for unpacked in sorted(unpacked_dir.iterdir()):
            _read_unpacked(unpacked, f"{source.name}:{unpacked.name[4:]}", files, models, budget)
    else:
        raise WheelError(f"{source}: give the mod's .oiv/.zip package, its dlc.rpf or the mod folder")
    return files


def _read_unpacked(path: Path, label: str, files: ModFiles, models: set[str] | None, budget: list[int]) -> None:
    if path.name.lower().endswith(".rpf"):
        _archive_file(path, label, files, models, budget)
    else:
        base = path.name[4:].lower()  # NNN- prefix
        take = (_wanted_meta(base) or base == "global.gxt2") if models is None else base in models
        if take:
            _add(files, base, label, _take_file(path, label, budget), models)


# ---- carcols ------------------------------------------------------------------------------------


@dataclass
class Wheel:
    type: int
    name: str
    variation: str | None
    label: str | None
    rim_radius: float
    rear: bool
    origin: str


def _tag(item: str, name: str) -> str | None:
    match = re.search(rf"<{name}>\s*([^<]*?)\s*</{name}>", item)
    return match.group(1) if match else None


def parse_wheels(carcols: str, origin: str) -> list[Wheel]:
    """Every `<Wheels>` item of a carcols file, with its wheel type (child index of <Wheels>)."""
    import prepare_carcols

    block = re.search(r"<Wheels>(.*?)</Wheels>", carcols, re.S)
    if block is None:
        return []
    body = block.group(1)
    types = prepare_carcols.wheel_types(body)
    if len(types) > len(WHEEL_TYPES):
        raise WheelError(f"{origin}: <Wheels> has {len(types)} wheel types; the game has {len(WHEEL_TYPES)}")
    wheels = []
    for index, (_s, _e, inner_start, inner_end) in enumerate(types):
        inner = body[inner_start:inner_end]
        for s, e in prepare_carcols.items(inner):
            item = inner[s:e]
            name = _tag(item, "wheelName") or ""
            if not NAME.fullmatch(name):
                raise WheelError(f"{origin}: wheel name {name!r} is not [A-Za-z0-9_] of at most 63 characters")
            variation = _tag(item, "wheelVariation") or None
            if variation is not None and not NAME.fullmatch(variation):
                raise WheelError(f"{origin}: {name}: wheelVariation {variation!r} is not a model name")
            label = _tag(item, "modShopLabel") or None
            radius = re.search(r'<rimRadius\s+value="([^"]+)"', item)
            try:
                rim = float(radius.group(1)) if radius else math.nan
            except ValueError:
                rim = math.nan
            if not (math.isfinite(rim) and 0.05 <= rim <= 2.0):
                raise WheelError(f"{origin}: {name}: rimRadius missing or outside 0.05..2.0")
            rear = re.search(r'<rear\s+value="([^"]+)"', item)
            wheels.append(
                Wheel(index, name, variation, label, rim, bool(rear and rear.group(1).lower() == "true"), origin)
            )
    return wheels


def humanize(key: str) -> str:
    """A shop name from a label key or model name: CHIRON_CLASSIQUE_01 -> Chiron Classique 01,
    FORGELINE_GA1R -> Forgeline GA1R, wheel_spt_x -> Wheel Spt X."""
    words = []
    for token in (t for t in key.split("_") if t):
        words.append(
            token.upper() if re.search(r"[0-9]", token) and re.search(r"[A-Za-z]", token) else token.capitalize()
        )
    return " ".join(words)[:63] or key[:63]


def carcols_input(wheels: list[Wheel], wheel_type: int | None) -> str:
    """A carcols holding only these `<Wheels>` items (normalized fields), in 13 wheel-type children."""
    slots: list[list[str]] = [[] for _ in WHEEL_TYPES]
    for wheel in wheels:
        variation = f"<wheelVariation>{wheel.variation}</wheelVariation>" if wheel.variation else "<wheelVariation />"
        label = f"<modShopLabel>{wheel.label}</modShopLabel>" if wheel.label else "<modShopLabel />"
        slots[wheel.type if wheel_type is None else wheel_type].append(
            "<Item>\n"
            f"        <wheelName>{wheel.name}</wheelName>\n"
            f"        {variation}\n"
            f"        {label}\n"
            f'        <rimRadius value="{wheel.rim_radius:.6f}"/>\n'
            f'        <rear value="{"true" if wheel.rear else "false"}"/>\n'
            "      </Item>"
        )
    children = []
    for index, items_ in enumerate(slots):
        comment = f"<!-- VWT {WHEEL_TYPES[index]} -->"
        if items_:
            children.append(f"    <Item>  {comment}\n      " + "\n      ".join(items_) + "\n    </Item>")
        else:
            children.append(f"    <Item/> {comment}")
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n\n<CVehicleModelInfoVarGlobal>\n  <Wheels>\n'
        + "\n".join(children)
        + "\n  </Wheels>\n</CVehicleModelInfoVarGlobal>\n"
    )


EMPTY_VARIATIONS = '<?xml version="1.0" encoding="UTF-8"?>\n<CVehicleModelInfoVariation>\n  <variationData />\n</CVehicleModelInfoVariation>\n'


def prepare(
    wheels: list[Wheel],
    shipped: list[str],
    wheel_type: int | None,
    prefix: str,
    gxt2: bytes | None,
    names: dict[str, str],
    work: Path,
) -> tuple[Path, list[tuple[Wheel, str, str]], str]:
    """The pack's carcols through tools/prepare_carcols.py --ship-wheel (labels PREFIX_WHEEL_<n>), and the
    label rows: --label NAME=TEXT, else the mod's text, else humanize(the mod's label key or the name)."""
    work.mkdir(parents=True, exist_ok=True)
    (work / "wheels-input.meta").write_text(carcols_input(wheels, wheel_type), encoding="utf-8", newline="\n")
    (work / "variations-input.meta").write_text(EMPTY_VARIATIONS, encoding="utf-8", newline="\n")
    argv = [
        sys.executable,
        str(ROOT / "tools/prepare_carcols.py"),
        "--carcols",
        str(work / "wheels-input.meta"),
        "--variations",
        str(work / "variations-input.meta"),
        "--kit-id",
        "4609",
        "--light-id",
        "224",  # unused: the input has no Kits or Lights
        "--shop-label-prefix",
        prefix,
        "--out-labels",
        str(work / "labels.txt"),
        "--out-carcols",
        str(work / "carcols.meta"),
        "--out-variations",
        str(work / "variations-unused.meta"),
    ]
    for name in shipped:
        argv += ["--ship-wheel", name]
    if gxt2 is not None:
        (work / "global.gxt2").write_bytes(gxt2)
        argv += ["--gxt2", str(work / "global.gxt2")]
    run = subprocess.run(argv, capture_output=True, text=True, check=False)
    if run.returncode:
        raise WheelError(f"prepare_carcols: {(run.stderr or run.stdout).strip()}")
    rows = [line.split("=", 1) for line in (work / "labels.txt").read_text(encoding="ascii").splitlines() if line]
    if len(rows) != len(wheels):
        raise WheelError(f"prepare_carcols wrote {len(rows)} wheel labels for {len(wheels)} wheels")
    texts = dict(gxt2_entries(gxt2, Limits())) if gxt2 else {}
    labels = []
    order = [w for t in range(len(WHEEL_TYPES)) for w in wheels if (w.type if wheel_type is None else wheel_type) == t]
    for (key, text), wheel in zip(rows, order, strict=True):
        given = names.get(wheel.name.lower())
        if given is not None:
            text = given
        elif not (wheel.label and texts.get(joaat(wheel.label))):
            text = humanize(wheel.label or wheel.name)
        if not LABEL_TEXT.fullmatch(text):
            raise WheelError(f"{wheel.name}: label text {text!r} is not printable ASCII of at most 63 characters")
        labels.append((wheel, key, text))
    return work / "carcols.meta", labels, run.stdout


# ---- commands -----------------------------------------------------------------------------------


def write_new(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(data)


def cmd_model(args) -> int:
    retail = load_retail(args.templates, args.reference_dir)
    name = args.name or args.source.stem
    ytd = args.ytd.read_bytes() if args.ytd else None
    out, report = convert_wheel(args.source.read_bytes(), name, retail, ytd=ytd, max_texture_size=args.max_texture_size)
    for path in (args.output, args.output.with_name(args.output.name + ".report.json")):
        if path.exists():
            raise WheelError(f"refusing to overwrite {path}")
    write_new(args.output, out)
    write_new(args.output.with_name(args.output.name + ".report.json"), cpd.canonical(report))
    print(summary(report))
    return 0


def summary(report: dict) -> str:
    wheel, trip = report["wheel"], report["wheel"]["roundTrip"]
    rigid = wheel["rigidRepair"]
    return (
        f"wheel {wheel['member']}: {wheel['output']['bytes']} bytes, {trip['geometries']} geometries, "
        f"{len(trip['embeddedTextures'])} embedded textures ({', '.join(trip['embeddedTextures'])}), "
        f"{len(wheel['sharedReferences'])} shared references"
        + (f", rigid repair on {rigid['geometries']} geometries" if rigid else "")
        + (
            f", {report['zeroTriangleCountGeometries']} zero triangle counts rebuilt"
            if report.get("zeroTriangleCountGeometries")
            else ""
        )
        + (
            f", neutral stand-ins {[t['name'] for t in wheel['embeddedDictionary']['neutralTextures']]}"
            if wheel["embeddedDictionary"]["neutralTextures"]
            else ""
        )
    )


def _pick(files: ModFiles) -> tuple[list[Wheel], list[str]]:
    wheels, seen = [], {}
    for _base, copies in sorted(files.metas.items()):
        for where, blob in copies:
            for wheel in parse_wheels(blob.decode("utf-8-sig", "replace").replace("\r\n", "\n"), where):
                key = wheel.name.lower()
                if key in seen:
                    files.notes.append(f"{where}: wheel {wheel.name} again (kept the one in {seen[key]})")
                    continue
                seen[key] = where
                wheels.append(wheel)
    return wheels, [w.name for w in wheels]


def cmd_pack(args) -> int:
    work = args.work
    files = read_mod(args.source, work / "input")
    wheels, _ = _pick(files)
    if not wheels:
        raise WheelError("the mod's carcols has no <Wheels> items (nothing to convert)")
    names = {w.name.lower() for w in wheels} | {w.variation.lower() for w in wheels if w.variation}
    wanted = {f"{n}{s}" for n in names for s in (".ydr", ".yft", ".ytd")}
    models = read_mod(args.source, work / "input", wanted)
    files.notes += models.notes
    rows = []
    for wheel in wheels:
        ydr, yft = models.models.get(f"{wheel.name.lower()}.ydr"), models.models.get(f"{wheel.name.lower()}.yft")
        state = "model " + (ydr or yft)[0] if (ydr or yft) else "no model in the mod (dropped)"
        if yft and not ydr:
            state = f"model {yft[0]} (.yft: not supported yet)"
        rows.append((wheel, ydr, state))
    if args.list:
        for wheel, _ydr, state in rows:
            print(
                f"wheel {wheel.name} type {WHEEL_TYPES[wheel.type]} label {wheel.label or '-'} "
                f"rimRadius {wheel.rim_radius:g}: {state}"
            )
        for note in files.notes:
            print(f"note: {note}")
        return 0
    only = {n.lower() for n in args.only}
    unknown = sorted(only - {w.name.lower() for w in wheels})
    if unknown:
        raise WheelError(f"--only names no <Wheels> item of the mod: {', '.join(unknown)}")
    selected = [(w, ydr) for w, ydr, _ in rows if (not only or w.name.lower() in only)]
    convertible = [(w, ydr) for w, ydr in selected if ydr is not None]
    for wheel, ydr, state in rows:
        if (not only or wheel.name.lower() in only) and ydr is None:
            print(f"note: wheel {wheel.name}: {state}")
    if not convertible:
        raise WheelError("none of the selected wheels has a .ydr model in the mod")
    if len(convertible) > MAX_WHEELS:
        raise WheelError(f"more than {MAX_WHEELS} wheels; pick some with --only")
    retail = load_retail(args.templates, args.reference_dir)
    members_dir = work / "members"
    if members_dir.exists():
        shutil.rmtree(members_dir)
    members: list[tuple[str, Path]] = []
    reports = {}
    done: set[str] = set()
    for wheel, _ydr in convertible:
        model_names = [wheel.name] + (
            [wheel.variation]
            if wheel.variation
            and wheel.variation.lower() != wheel.name.lower()
            and f"{wheel.variation.lower()}.ydr" in models.models
            else []
        )
        for model in model_names:
            if model.lower() in done:
                continue
            source = models.models[f"{model.lower()}.ydr"]
            ytd = models.models.get(f"{model.lower()}.ytd")
            try:
                out, report = convert_wheel(
                    source[1], model, retail, ytd=ytd[1] if ytd else None, max_texture_size=args.max_texture_size
                )
            except AssetError as error:
                raise WheelError(f"wheel {model} ({source[0]}): {error}") from error
            path = members_dir / f"{model.lower()}.pdr"
            write_new(path, out)
            report["wheel"]["source"] = {"where": source[0], "sha256": digest(source[1]), "bytes": len(source[1])}
            write_new(path.with_name(path.name + ".report.json"), cpd.canonical(report))
            reports[model.lower()] = report
            members.append((path.name, path))
            done.add(model.lower())
            print(summary(report))
    kept = [w for w, _ in convertible]
    shipped = sorted(done)
    gxt2 = files.texts[0][1] if files.texts else None
    prefix = args.label_prefix or default_prefix(args.id)
    labels_given = dict(pair.split("=", 1) for pair in args.label)
    for key in labels_given:
        if key.lower() not in {w.name.lower() for w in kept}:
            raise WheelError(f"--label {key}: not one of the converted wheels")
    carcols, labels, log = prepare(
        kept, shipped, args.wheel_type, prefix, gxt2, {k.lower(): v for k, v in labels_given.items()}, work / "carcols"
    )
    for line in log.splitlines():
        if line.startswith("note:") and "dropped" in line:
            raise WheelError(f"prepare_carcols dropped a converted wheel: {line}")
    slug = args.id.removeprefix("gtavmenu-").replace("-", "_")
    archive = args.archive or f"gmwhl_{slug}"[:59] + ".rpf"
    description = args.description or f"{len(kept)} wheels from {args.source.name}"[: runtime_pack.DESCRIPTION_MAX]
    argv = [
        sys.executable,
        str(ROOT / "tools/build_runtime_pack.py"),
        "--id",
        args.id,
        "--archive",
        archive,
        "--description",
        description,
        "--data",
        f"CARCOLS_FILE={carcols}",
        "--output-root",
        str(args.output_root),
        "--plain-toc",
    ]
    for name, path in members:
        argv += ["--member", f"{name}={path}"]
    for _wheel, key, text in labels:
        argv += ["--label", f"{key}={text}"]
    run = subprocess.run(argv, capture_output=True, text=True, check=False)
    if run.returncode:
        raise WheelError(f"build_runtime_pack: {(run.stderr or run.stdout).strip()}")
    pack_report = {
        "kind": "gtavmenu-wheel-pack",
        "id": args.id,
        "source": args.source.name,
        "wheels": [
            {
                "name": w.name,
                "type": WHEEL_TYPES[w.type if args.wheel_type is None else args.wheel_type],
                "label": key,
                "text": text,
                "rimRadius": w.rim_radius,
                "member": f"{w.name.lower()}.pdr",
                "sha256": reports[w.name.lower()]["wheel"]["output"]["sha256"],
            }
            for w, key, text in labels
        ],
        "notes": files.notes,
    }
    (work / "wheel-pack-report.json").write_bytes(cpd.canonical(pack_report))
    for row in pack_report["wheels"]:
        print(f"wheel {row['name']} -> {row['member']} type {row['type']} label {row['label']} \"{row['text']}\"")
    for note in files.notes:
        print(f"note: {note}")
    return 0


def default_prefix(pack_id: str) -> str:
    """GM + the id's words without the gtavmenu- prefix and version, upper case (label keys must be new)."""
    words = re.sub(r"-v\d+$", "", pack_id.removeprefix("gtavmenu-")).replace("-", "_").upper()
    return ("GM_" + words)[:40].rstrip("_")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("model", "pack"):
        p = sub.add_parser(name)
        p.add_argument("--source", type=Path, required=True)
        p.add_argument("--templates", type=Path, default=ROOT / "build/retail-templates")
        p.add_argument(
            "--reference-dir", type=Path, help="accepted and not read (the layouts come from data/drawable_contracts)"
        )
        p.add_argument("--max-texture-size", type=int, help="drop texture mips above this size")
    m = sub.choices["model"]
    m.add_argument("--output", type=Path, required=True, help="new <name>.pdr")
    m.add_argument("--name", help="wheel name (default: the source's base name)")
    m.add_argument("--ytd", type=Path, help="a PC texture dictionary of the wheel beside its embedded one")
    p = sub.choices["pack"]
    p.add_argument("--id", help="pack id")
    p.add_argument("--work", type=Path, help="work directory (unpacked package, members, carcols, reports)")
    p.add_argument("--output-root", type=Path, default=ROOT / "build/custom-assets")
    p.add_argument("--type", dest="wheel_type_name", choices=WHEEL_TYPES, help="put every wheel in this wheel type")
    p.add_argument("--only", action="append", default=[], metavar="NAME", help="convert only these wheels")
    p.add_argument("--label", action="append", default=[], metavar="NAME=TEXT", help="shop name of a wheel")
    p.add_argument("--label-prefix", help="label keys PREFIX_WHEEL_<n> (default GM_<id words>)")
    p.add_argument("--archive", help="archive name (default gmwhl_<id>.rpf)")
    p.add_argument("--description", help="pack description (Manage Packs)")
    p.add_argument("--list", action="store_true", help="list the mod's wheels and models; write nothing")
    args = parser.parse_args(argv)
    if args.max_texture_size is not None and (
        args.max_texture_size < 4 or args.max_texture_size & (args.max_texture_size - 1)
    ):
        parser.error("--max-texture-size takes a power of two")
    try:
        if args.command == "model":
            return cmd_model(args)
        args.wheel_type = None if args.wheel_type_name is None else WHEEL_TYPES.index(args.wheel_type_name)
        if args.work is None or not (args.list or args.id):
            parser.error("pack needs --work and --id (--list: --work only)")
        if args.label_prefix is not None and not re.fullmatch(r"[A-Za-z0-9_]{1,40}", args.label_prefix):
            parser.error("--label-prefix takes [A-Za-z0-9_], at most 40 characters")
        if args.archive is not None and not re.fullmatch(r"[a-z0-9_]{1,59}\.rpf", args.archive):
            parser.error("--archive takes a lowercase NAME.rpf")
        for pair in args.label:
            if "=" not in pair:
                parser.error("--label takes NAME=TEXT")
        return cmd_pack(args)
    except (AssetError, OSError, ValueError, KeyError, struct.error) as error:
        print(f"convert_pc_wheel: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

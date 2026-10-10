#!/usr/bin/env python3
"""Gen9 (GTA V Enhanced PC) resources: detection, a .ytd reader and a route into the PS5 texture writer.

Mods for GTA V Enhanced ship "enhanced" builds of their resources. They are the Gen9 resource family the PS5 game uses too, told apart from
the Legacy PC builds our pc_* readers take by the RSC7 version alone:

    type   Legacy PC   Gen9 (Enhanced PC and PS5)
    .ytd   13          5
    .ydr   165         159
    .ydd   165         159
    .yft   162         171
    .ybn   43          43 (Legacy PC and PS5 agree; no Enhanced .ybn sample yet)

Gen9 texture dictionary (.ytd v5), field by field, derived from the paired enhanced/legacy builds of one
mod (bit-identical mip bytes) and checked against our own PS5 reader (gtavmenu_tools.ps5_texture_reader):
the 0x40 dictionary root is the Legacy one (+0x08 page info, +0x20 key array, +0x30 texture array); each
texture is the PS5 88-byte object with its 32-byte view right after it (+0x30 points at +0x58):

    +0x00 u64  class word (0 in the PC file)    +0x1F u8   DXGI format (71 BC1, 77 BC3, ...)
    +0x08 u32  storage units (blocks)           +0x20 u8   tile mode (0xFF = automatic on PC)
    +0x0C u32  bytes per unit                   +0x21 u8   anti-alias (0)
    +0x10 u32  flags (0x00260208 in both)       +0x22 u8   mip levels
    +0x18 u16  width, +0x1A height, +0x1C depth +0x26 u16  reference count (1)
    +0x1E u8   dimension (1 = 2D)               +0x28 name, +0x30 view, +0x38 data pointers
    +0x40 u8   usage (0x14 diffuse, 0x16 normal, 0x17 specular, as the Legacy object's +0x40)

The PC data is linear: every mip block-rounded, mip 0 first, units * unit bytes == the chain's sum
exactly, and (unlike Legacy) mips below one block (2x2, 1x1) are declared as one block each; the exporter
allocates such a chain one block short, so the next texture overlaps its last mip. The reader returns the
chain down to one block (what retail PS5 chains and the Legacy --max-size trim keep). A PS5 .ptd
has the same versions and object layout but tiled storage (tile mode 1/5/9, padded storage units), so
`texture_family` refuses it rather than reading tiled data as linear.

Conversion: read_gen9_texture_dictionary returns the dictionary in the shape of
texture_conversion.read_pc_texture_dictionary, so convert_pc_ytd_writer.convert (the hardware-qualified
PS5 writer) takes it unchanged; --max-size drops mips above that size, as for Legacy. `convert_any_ytd` routes a Legacy or Gen9 .ytd automatically;
for a mod that ships both builds the two routes write byte-identical .ptd files (the doc section).

  pc_gen9_resources.py detect FILE_OR_DIR...
  pc_gen9_resources.py ytd --ytd enhanced/x.ytd --output out/x.ptd [--max-size 1024] [--templates DIR]
  pc_gen9_resources.py survey enhanced/x.ydd       (Gen9 drawable dictionary: what a PS5 route must change)
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections import Counter
from dataclasses import replace
from itertools import pairwise
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

from gtavmenu_tools.asset_formats import AssetError, Limits, decode_resource, resource_header  # noqa: E402
from gtavmenu_tools.asset_textures import (  # noqa: E402
    SCRIPT_TEXTURE_MARKER,
    ResourceView,
    linear_mip_sizes,
)
from gtavmenu_tools.hashes import joaat  # noqa: E402

LEGACY_VERSIONS = {".ytd": 13, ".ydr": 165, ".ydd": 165, ".yft": 162, ".ybn": 43}
GEN9_VERSIONS = {".ytd": 5, ".ydr": 159, ".ydd": 159, ".yft": 171, ".ybn": 43}
PS5_SUFFIXES = {".ptd": ".ytd", ".pdr": ".ydr", ".pdd": ".ydd", ".pft": ".yft", ".pbn": ".ybn"}

OBJECT_BYTES = 0x58  # the PS5 texture object (ps5_texture_reader._OBJECT_BYTES)
VIEW_BYTES = 0x20
AUTOMATIC_TILE = 0xFF

# DXGI code -> (convert_pc_ytd_writer format name, pixels per block edge, bytes per block/pixel).
# Only formats the PS5 writer route takes; sRGB variants are refused (none observed, gamma unqualified).
DXGI_FORMATS = {
    71: ("BC1", 4, 8),
    74: ("BC2", 4, 16),
    77: ("BC3", 4, 16),
    80: ("BC4", 4, 8),
    83: ("BC5", 4, 16),
    98: ("BC7", 4, 16),
    87: ("BGRA8", 1, 4),
    88: ("BGRX8", 1, 4),
    28: ("RGBA8", 1, 4),
    65: ("A8", 1, 1),
}
DXGI_SRGB = {72: "BC1_SRGB", 75: "BC2_SRGB", 78: "BC3_SRGB", 99: "BC7_SRGB", 29: "RGBA8_SRGB", 91: "BGRA8_SRGB"}

LIMITS = replace(Limits(), max_file_bytes=1 << 28, max_total_bytes=1 << 31, max_entries=4096)


def resource_family(blob: bytes, suffix: str) -> str:
    """'legacy', 'gen9' or 'unknown' from the RSC7 version (a .ybn v43 is 'legacy': the shared layout)."""
    suffix = PS5_SUFFIXES.get(suffix.lower(), suffix.lower())
    try:
        version = resource_header(blob[:16])["version"]
    except AssetError:
        return "unknown"
    if LEGACY_VERSIONS.get(suffix) == version:
        return "legacy"
    if GEN9_VERSIONS.get(suffix) == version:
        return "gen9"
    return "unknown"


def _objects(payload: bytes, header: dict, limits: Limits) -> tuple[ResourceView, list[tuple[int, int]]]:
    view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
    return view, view.dictionary_entries(limits)


def texture_family(blob: bytes, limits: Limits = LIMITS) -> str:
    """'gen9-pc' (automatic tile, exact linear storage), 'ps5' (tiled), 'legacy' or 'unknown' for a .ytd."""
    family = resource_family(blob, ".ytd")
    if family != "gen9":
        return family
    header, payload = decode_resource(blob, min(limits.max_file_bytes, limits.max_total_bytes))
    view, entries = _objects(payload, header, limits)
    tiles = {view.system_struct("<B", pointer + 0x20)[0] for _, pointer in entries}
    if tiles == {AUTOMATIC_TILE}:
        return "gen9-pc"
    return "ps5" if tiles and AUTOMATIC_TILE not in tiles else "unknown"


def read_gen9_texture_dictionary(blob: bytes, limits: Limits = LIMITS, *, skip_bad: bool = False) -> dict:
    """Every texture of an Enhanced PC .ytd (v5) with its linear mips, or reject the whole dictionary.

    The result has the shape of texture_conversion.read_pc_texture_dictionary (name, nameHash, width,
    height, depth, format, formatCode, mipLevels, mips), so convert_pc_ytd_writer.convert takes it.
    With skip_bad an unusable texture (format, shape, special name) is listed in "skipped" instead;
    structural faults (pointers, hashes, overlaps, a PS5 tiled object) always reject.
    """
    if type(blob) is not bytes or not 16 <= len(blob) <= limits.max_file_bytes:
        raise AssetError("source Gen9 YTD is empty or exceeds the byte limit")
    header, payload = decode_resource(blob, min(limits.max_file_bytes, limits.max_total_bytes))
    if header["version"] != GEN9_VERSIONS[".ytd"]:
        raise AssetError(f"Gen9 texture reader requires version 5, not {header['version']}; no fallback")
    if header["systemBytes"] > limits.max_metadata_bytes:
        raise AssetError("source Gen9 YTD system pages exceed the metadata byte limit")
    view, entries = _objects(payload, header, limits)
    if not entries:
        raise AssetError("source Gen9 YTD must contain at least one texture")
    keys = [key for key, _ in entries]
    if keys != sorted(keys):
        raise AssetError("Gen9 texture dictionary keys are not sorted")
    textures, objects, spans, failures = [], [], [], []
    for key, pointer in entries:
        if pointer % 16:
            raise AssetError("unaligned Gen9 texture object")
        start = view.offset(pointer, OBJECT_BYTES + VIEW_BYTES)
        objects.append((start, start + OBJECT_BYTES + VIEW_BYTES))
        raw = bytes(view.system[start : start + OBJECT_BYTES + VIEW_BYTES])
        units, unit_bytes, flags = struct.unpack_from("<IIIxxxx", raw, 0x08)
        width, height, depth, dimension, code, tile, anti_alias, levels = struct.unpack_from("<3H5B", raw, 0x18)
        name_pointer, view_pointer, data_pointer = struct.unpack_from("<3Q", raw, 0x28)
        name = view.name(name_pointer)
        if joaat(name) != key:
            raise AssetError(f"texture name disagrees with dictionary hash: {name}")
        if view_pointer != pointer + OBJECT_BYTES:
            raise AssetError(f"{name}: texture view is not right after its object")
        if tile != AUTOMATIC_TILE:
            raise AssetError(f"{name}: tile mode {tile} is not the PC automatic mode; a PS5 .ptd is not a source")
        if SCRIPT_TEXTURE_MARKER in name.lower():
            failures.append(f"{name}: texture name selects target-native special handling")
            continue
        if depth != 1 or dimension != 1 or anti_alias:
            failures.append(f"{name}: unsupported depth/dimension/anti-alias {depth}/{dimension}/{anti_alias}")
            continue
        if code in DXGI_SRGB:
            failures.append(f"{name}: sRGB format {DXGI_SRGB[code]} is not qualified for the PS5 writer")
            continue
        if code not in DXGI_FORMATS:
            failures.append(f"{name}: unsupported DXGI format {code}")
            continue
        fmt, edge, block_bytes = DXGI_FORMATS[code]
        sizes = linear_mip_sizes(width, height, levels, edge, block_bytes)
        if unit_bytes != block_bytes or units * unit_bytes != sum(sizes):
            failures.append(f"{name}: storage {units}x{unit_bytes} is not the linear {fmt} chain ({sum(sizes)} bytes)")
            continue
        # Mips below one block are not read: the exporter places the next texture over the last
        # of them (its allocation is one block short of the declared chain, a_f_y_socialite_01.ytd).
        keep = sum(1 for level in range(levels) if min(width, height) >> level >= edge)
        if not keep:
            failures.append(f"{name}: {width}x{height} is smaller than one {fmt} block")
            continue
        kept = sum(sizes[:keep])
        offset = view.offset(data_pointer, kept, graphics=True)
        spans.append((offset, offset + kept, name))
        mips, cursor = [], offset
        for size in sizes[:keep]:
            mips.append(bytes(view.graphics[cursor : cursor + size]))
            cursor += size
        textures.append(
            {
                "name": name,
                "nameHash": f"0x{key:08x}",
                "width": width,
                "height": height,
                "depth": depth,
                "stride": block_bytes * ((width + edge - 1) // edge),
                "format": fmt,
                "formatCode": f"0x{code:08x}",
                "mipLevels": keep,
                "mips": tuple(mips),
                "sourceMipLevels": levels,
                "usage": raw[0x40],
                "flags": f"0x{flags:08x}",
                "systemOffset": start,
                "graphicsPointer": f"0x{data_pointer:08x}",
            }
        )
    for spans_of, label in ((objects, "texture objects"), (spans, "texture data")):
        ordered = sorted(spans_of)
        if any(left[1] > right[0] for left, right in pairwise(ordered)):
            raise AssetError(f"overlapping Gen9 {label}")
    if failures and not skip_bad:
        raise AssetError("whole-dictionary admission failed: " + "; ".join(failures))
    return {"layout": "pc-gen9-ytd-v5", "textureCount": len(textures), "textures": textures, "skipped": failures}


def gen9_ytd_textures(ytd: bytes, max_size: int | None = None, *, skip_bad: bool = False) -> tuple[list, list]:
    """(textures, skipped) like convert_pc_drawable.ytd_textures, for an Enhanced PC .ytd (the hook)."""
    source = read_gen9_texture_dictionary(ytd, skip_bad=skip_bad)
    textures = [trim_texture_mips(texture, max_size)[0] for texture in source["textures"]]
    return textures, source["skipped"]


def trim_texture_mips(texture: dict, max_size: int | None = None) -> tuple[dict, dict | None]:
    """The texture with mips above max_size and below one compression block dropped (None: unchanged)."""
    if max_size is not None and (max_size < 4 or max_size & (max_size - 1)):
        raise AssetError("--max-size must be a power of two >= 4")
    edge = 4 if texture["format"].startswith("BC") else 1
    width, height, levels = texture["width"], texture["height"], texture["mipLevels"]
    top = 0
    while max_size is not None and top + 1 < levels and max(width, height) >> top > max_size:
        top += 1
    keep = 0
    while top + keep < levels and min(width, height) >> (top + keep) >= edge:
        keep += 1
    if keep == 0 or (top, keep) == (0, levels):
        return texture, None
    trimmed = texture | {
        "width": max(1, width >> top),
        "height": max(1, height >> top),
        "mipLevels": keep,
        "mips": tuple(texture["mips"][top : top + keep]),
    }
    return trimmed, {
        "name": texture["name"],
        "from": [width, height, levels],
        "to": [trimmed["width"], trimmed["height"], keep],
        "droppedTopMips": top,
        "droppedTailMips": levels - top - keep,
    }


def convert_gen9_ytd(ytd: bytes, templates: Path | None, max_size: int | None = None) -> tuple[bytes, dict]:
    """A PS5 .ptd from an Enhanced PC .ytd through convert_pc_ytd_writer.convert (needs numpy)."""
    import convert_pc_ytd_writer as writer  # numpy/bc_encode only for conversion, not for reading

    source = read_gen9_texture_dictionary(ytd)
    textures, trims = [], []
    for texture in source["textures"]:
        if texture["sourceMipLevels"] != texture["mipLevels"]:
            size = [texture["width"], texture["height"]]
            trims.append(
                {
                    "name": texture["name"],
                    "from": [*size, texture["sourceMipLevels"]],
                    "to": [*size, texture["mipLevels"]],
                    "droppedTopMips": 0,
                    "droppedTailMips": texture["sourceMipLevels"] - texture["mipLevels"],
                }
            )
        texture, trim = trim_texture_mips(texture, max_size)
        textures.append(texture)
        if trim:
            trims.append(trim)
    blob, rows = writer.convert(source | {"textures": textures}, templates)
    keys = ("name", "sourceFormat", "format", "width", "height", "mipLevels", "tileMode")
    report = {"textures": [dict(zip(keys, row, strict=True)) for row in rows], "mipTrims": trims}
    return blob, report | {"sourceFamily": "gen9-pc"}


def convert_any_ytd(ytd: bytes, templates: Path | None, max_size: int | None = None) -> tuple[bytes, dict]:
    """Route a Legacy (v13) or Enhanced PC (v5) .ytd to the PS5 texture writer; refuse anything else."""
    family = texture_family(ytd)
    if family == "gen9-pc":
        return convert_gen9_ytd(ytd, templates, max_size)
    if family == "legacy":
        import convert_pc_ytd_writer as writer

        blob, report = writer.convert_file(ytd, templates, max_size)
        return blob, report | {"sourceFamily": "legacy"}
    raise AssetError(f"not a PC texture dictionary this route converts (family {family})")


# ---- Gen9 drawable survey (assessment only; no conversion) ----------------------------------------


def survey_gen9_drawable_dictionary(blob: bytes) -> dict:
    """What separates an Enhanced PC .ydd from the PS5 .pdd layout native_drawable_dictionary models.

    The PS5 reader walks the Enhanced object graph with three tolerances, each counted here: arrays the
    exporter shares between LODs (bone ids), shader parameter blocks whose texture-pointer array is not
    inline, and class words that are host heap pointers. Shader parameter copies are reported per count.
    """
    import native_drawable_dictionary as ndd

    shared: Counter = Counter()
    copies: Counter = Counter()
    detached: list[str] = []

    class Reader(ndd._Reader):
        def add(self, at: int, size: int, kind: str, label: str) -> bool:
            if at and at in self.spans and self.spans[at][:2] == (size, kind):
                shared[kind] += 1
                return True
            return super().add(at, size, kind, label)

        def shader(self, at: int, label: str) -> None:
            self.add(at, 0x40, "shader", label)
            infos = self.u(at, "<Q", 0x20)
            buffers, count, copy = self.u(infos, "<B", 0), self.u(infos, "<B", 4), self.u(infos, "<B", 7)
            copies[copy] += 1
            self.add(infos, 8 + 8 * count, "param-infos", label)
            params, textures = self.u(at, "<Q", 0x08), self.u(at, "<Q", 0x10)
            if params and textures and textures < params:
                detached.append(label)
            if params and buffers:
                first = self.u(params, "<Q", 0)
                if first != params + 8 * buffers * copy:
                    detached.append(label + " buffers")

        def embedded_texture(self, at: int, label: str) -> None:
            self.add(at, OBJECT_BYTES + VIEW_BYTES, "txd-texture", label)

    reader = Reader(blob)
    reader.dictionary()
    kinds = Counter(kind for _, kind, _ in reader.spans.values())
    class_words = Counter()
    for at, (_, kind, _) in reader.spans.items():
        if kind in ("drawable", "shader-group", "skeleton", "model", "geometry", "vertex-buffer", "index-buffer"):
            word = reader.u(at, "<Q", 0)
            class_words["host-pointer" if word >> 40 else ("zero" if not word else "image")] += 1
    return {
        "version": reader.version,
        "objects": dict(sorted(kinds.items())),
        "sharedArrays": dict(shared),
        "shaderParameterCopies": dict(copies),
        "detachedParameterArrays": len(detached),
        "classWords": dict(class_words),
        "ps5ParameterCopies": 8,
    }


def _detect_paths(paths: list[Path]) -> list[tuple[Path, str]]:
    rows = []
    for path in paths:
        files = sorted(p for p in path.rglob("*") if p.is_file()) if path.is_dir() else [path]
        for file in files:
            suffix = PS5_SUFFIXES.get(file.suffix.lower(), file.suffix.lower())
            if suffix not in LEGACY_VERSIONS:
                continue
            with file.open("rb") as handle:
                head = handle.read(16)
            family = resource_family(head, suffix)
            if family == "gen9" and suffix == ".ytd":
                family = texture_family(file.read_bytes())
            rows.append((file, family))
    return rows


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    detect = commands.add_parser("detect", help="classify resources as legacy / gen9 / gen9-pc / ps5")
    detect.add_argument("paths", type=Path, nargs="+")
    ytd = commands.add_parser("ytd", help="convert a Legacy or Enhanced PC .ytd to a PS5 .ptd")
    ytd.add_argument("--ytd", type=Path, required=True)
    ytd.add_argument("--output", type=Path, required=True)
    ytd.add_argument("--templates", type=Path, help="retail template cache instead of build/assets")
    ytd.add_argument("--max-size", type=int, help="drop mips above this size (and always below one block)")
    survey = commands.add_parser("survey", help="Gen9 drawable dictionary differences from the PS5 layout")
    survey.add_argument("ydd", type=Path)
    args = parser.parse_args(argv)

    if args.command == "detect":
        rows = _detect_paths(args.paths)
        for file, family in rows:
            print(f"{family:8s} {file}")
        counts = Counter(family for _, family in rows)
        print("total " + " ".join(f"{key}={value}" for key, value in sorted(counts.items())))
        return 0
    if args.command == "survey":
        import json

        print(json.dumps(survey_gen9_drawable_dictionary(args.ydd.read_bytes()), indent=2))
        return 0
    if args.output.exists():
        raise SystemExit(f"refusing to overwrite {args.output}")
    blob, report = convert_any_ytd(args.ytd.read_bytes(), args.templates, args.max_size)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    rows = report["textures"]
    converted = sum(1 for row in rows if row["sourceFormat"] != row["format"])
    print(
        f"wrote {args.output} bytes={len(blob)} source={report['sourceFamily']} textures={len(rows)}"
        f" reencoded={converted}"
    )
    for trim in report["mipTrims"]:
        print(f"trimmed {trim['name']}: {trim['from']} -> {trim['to']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

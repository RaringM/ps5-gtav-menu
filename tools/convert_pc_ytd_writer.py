#!/usr/bin/env python3
"""Rebuild a PC Legacy .ytd as a PS5 .ptd or PS4 .otd.

PS5 route proven on hardware: writer-built dictionaries (MIX-run1/2) with AddrLib tiling and object
metadata words copied from a stock PS5 texture of the same format draw correctly. Every PC mip is
used as-is (BC blocks are platform-neutral); uncompressed textures (BGRA8, BGRX8, RGBA8, A8, L8), for
which no stock PS5 template of their format is available, are re-encoded to BC3 with
bc_encode.py, expanded the way D3D samples them (A8 -> (0, 0, 0, a), L8 -> (l, l, l, 1),
BGRX8 -> alpha 1). BC2 (DXT3) has no stock PS5 template either: it is transcoded to BC3 with
bc_encode.bc2_to_bc3 (colour blocks copied bit-exactly, explicit 4-bit alpha re-encoded as
interpolated BC3 alpha; worst pixel error <= 18/255). PS5 uses GFX10 standard swizzle
5 when both dimensions are >= 64, otherwise 1. PS4 uses GCN Thin1DThin 8x8 Morton
microtiles, sequential padded mips and an exact native format donor. PS4 output
carries one pitch/offset view per mip from the same layout as its pixels. The output
is re-parsed and every allocation and view entry compared. Names are kept exactly.

--max-size N first trims the PC mip chains (a new in-memory dictionary, the source file is untouched):
mips wider or taller than N are dropped (the texture starts at the first mip that fits) and so are
the mips below one compression block (2x2 and 1x1 of a BC texture, which exporters often add and
the Legacy layout cannot describe). Only width, height, stride, mip count and the data pointer of a
texture object change; the trimmed dictionary is read with the same whole-dictionary checks.
Without --max-size the conversion is unchanged.

--embedded FRAGMENT (repeatable) adds the textures a PC fragment embeds in its main drawable (the original
fragment repair_pc_embedded_textures.py keeps beside the repaired fixture): each texture whose name the
.ytd (or an earlier --embedded fragment) does not already hold, with the same --max-size trim (or
--embedded-max-size N for these textures only: livery parts carry 2048x2048 textures each); the names the
dictionary already has keep its texture and are reported (repair_pc_embedded_textures.py has renamed every such
name whose pixels differ, so these are the same texture).

Graphics pages: a dictionary whose textures fit one 16 MiB page keeps the single power-of-two page
(byte-identical to before). A larger one is written retail-style over 4 MiB pages (or the next power
of two above its largest texture), no texture crossing a page.

PS4 output copies its object metadata from stock 1.57 vehicles.rpf adder.otd and asea.otd in --templates (or
GTAV_PS4_TEMPLATES); without either it refuses.

  convert_pc_ytd_writer.py --ytd a80.ytd --output a80.ptd [--max-size 1024] [--embedded aventador_hi.yft]
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib
from dataclasses import replace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

import numpy as np  # noqa: E402
from bc_encode import bc2_to_bc3, encode  # noqa: E402
from gtavmenu_tools import ps4_texture_writer  # noqa: E402
from gtavmenu_tools import ps5_texture_writer as writer  # noqa: E402
from gtavmenu_tools.asset_formats import AssetError, Limits, decode_resource  # noqa: E402
from gtavmenu_tools.asset_textures import LEGACY_FORMATS, inspect_legacy_dictionary  # noqa: E402
from gtavmenu_tools.ps4_texture_reader import parse_ps4_texture_dictionary  # noqa: E402
from gtavmenu_tools.ps5_texture_reader import parse_ps5_texture_dictionary  # noqa: E402
from gtavmenu_tools.target_profile import target_platform  # noqa: E402
from gtavmenu_tools.texture_conversion import read_pc_texture_dictionary  # noqa: E402
from gtavmenu_tools.texture_layout import surface_layout, tile_mips  # noqa: E402
from gtavmenu_tools.texture_names import SOURCE_NAME_POLICY, texture_name_issue  # noqa: E402

PS5_TEMPLATES = {
    "BC1": ("build/assets/corpus/tornado6.ptd", "t6_tyre_diff"),
    "BC3": ("build/assets/corpus/tornado6.ptd", "tornado3_badges"),
    "BC5": ("build/assets/corpus/banshee-a.ptd", "generic_leather2_n"),
}
# Stock 1.57 vehicles.rpf members, read from the PS4 template directory (no built-in default).
PS4_TEMPLATES = {
    "BC1": ("adder.otd", "adder_badges_n"),
    "BC3": ("adder.otd", "adder_badges"),
    "BC5": ("asea.otd", "generic_leather2_n"),
}
PS4_TEMPLATES_ENV = "GTAV_PS4_TEMPLATES"


def _expand(fmt: str, raw: bytes, w: int, h: int) -> np.ndarray:
    """(H, W, 4) RGBA of one uncompressed mip, as D3D samples the format."""
    pixels = np.frombuffer(raw, dtype=np.uint8)
    if fmt in ("A8", "L8"):
        one = pixels.reshape(h, w)
        rgba = np.empty((h, w, 4), dtype=np.uint8)
        if fmt == "A8":
            rgba[..., :3], rgba[..., 3] = 0, one
        else:
            rgba[..., :3], rgba[..., 3] = one[..., None], 255
        return rgba
    four = pixels.reshape(h, w, 4)
    if fmt == "RGBA8":
        return four
    rgba = four[..., [2, 1, 0, 3]].copy()
    if fmt == "BGRX8":
        rgba[..., 3] = 255
    return rgba


UNCOMPRESSED = ("BGRA8", "BGRX8", "RGBA8", "A8", "L8")


def to_bc3(texture: dict) -> list[bytes]:
    """BC3 mips of an uncompressed texture (UNCOMPRESSED formats)."""
    mips = []
    for level, raw in enumerate(texture["mips"]):
        w, h = max(1, texture["width"] >> level), max(1, texture["height"] >> level)
        mips.append(encode("BC3", _expand(texture["format"], raw, w, h)))
    return mips


def bc7_to_bc3(texture: dict) -> list[bytes]:
    """Decode BC7 with Pillow and encode BC3 using the same encoder as other source formats.

    This is lossy, preserves every mip's dimensions and alpha channel, and never
    relabels BC7 bytes as BC3. Pillow is needed only for this source format.
    """
    try:
        from PIL import Image
    except ImportError:
        raise AssetError("BC7 conversion needs Pillow; install the project's [packs] dependencies") from None
    mips = []
    if len(texture["mips"]) != texture["mipLevels"]:
        raise AssetError(f"{texture['name']}: BC7 mip count differs")
    for level, raw in enumerate(texture["mips"]):
        width, height = max(1, texture["width"] >> level), max(1, texture["height"] >> level)
        expected = ((width + 3) // 4) * ((height + 3) // 4) * 16
        if len(raw) != expected:
            raise AssetError(f"{texture['name']}: BC7 mip {level} has an invalid block count")
        try:
            pixels = Image.frombytes("RGBA", (width, height), raw, "bcn", (7, "BC7"))
        except (OSError, ValueError) as error:
            raise AssetError(f"{texture['name']}: BC7 mip {level} cannot be decoded: {error}") from error
        blocks = encode("BC3", np.asarray(pixels))
        if len(blocks) != expected:
            raise AssetError(f"{texture['name']}: BC3 output mip {level} has an invalid block count")
        mips.append(blocks)
    return mips


SINGLE_PAGE_BYTES = 16 * 1024 * 1024  # largest hardware-proven single graphics page (a80.ptd)
GRAPHICS_PAGE_BYTES = 4 * 1024 * 1024  # retail vehicle dictionaries: graphics pages of <= 4 MiB (virgo2+hi)
MAX_DICTIONARY_TEXTURES = 256  # per written dictionary (convert_pc_map_models splits larger groups)


# Legacy texture object fields (asset_textures.inspect_legacy_dictionary reads "<4HIBB" at +80).
WIDTH, HEIGHT, STRIDE, LEVELS, DATA = 80, 82, 86, 93, 112


def trim_mips(blob: bytes, max_size: int) -> tuple[bytes, list[dict]]:
    """A copy of a Legacy .ytd whose mip chains start at <= max_size and end at one block."""
    if max_size < 4 or max_size & (max_size - 1):
        raise SystemExit("--max-size must be a power of two >= 4")
    header, payload = decode_resource(blob, 1 << 30)
    payload = bytearray(payload)
    trims = []
    for row in inspect_legacy_dictionary(bytes(payload), header, Limits())["textures"]:
        fmt = int(row["formatCode"], 16)
        if fmt not in LEGACY_FORMATS:
            continue
        _, edge, _ = LEGACY_FORMATS[fmt]
        width, height, levels, stride = row["width"], row["height"], row["mipLevels"], row["stride"]
        top = 0
        while top + 1 < levels and max(width, height) >> top > max_size:
            top += 1
        keep = 0
        while top + keep < levels and min(width, height) >> (top + keep) >= edge:
            keep += 1
        if keep == 0 or (top, keep) == (0, levels):
            continue
        dropped = sum(stride * height >> (2 * level) for level in range(top))
        at = row["systemOffset"]
        struct.pack_into("<HH", payload, at + WIDTH, width >> top, height >> top)
        struct.pack_into("<H", payload, at + STRIDE, stride >> top)
        payload[at + LEVELS] = keep
        pointer = struct.unpack_from("<Q", payload, at + DATA)[0]
        struct.pack_into("<Q", payload, at + DATA, pointer + dropped)
        trims.append(
            {
                "name": row["name"],
                "from": [width, height, levels],
                "to": [width >> top, height >> top, keep],
                "droppedTopMips": top,
                "droppedTailMips": levels - top - keep,
            }
        )
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    return blob[:16] + packer.compress(bytes(payload)) + packer.flush(), trims


def graphics_pages(inputs: list, limits: Limits) -> int | None:
    """None (one graphics page, as before) up to SINGLE_PAGE_BYTES; else the retail-style page size.

    A single power-of-two page above 16 MiB (the largest hardware-proven texture page, a80.ptd)
    would be a 32/64 MiB contiguous streaming allocation; retail vehicle dictionaries spread their
    textures over pages of at most 4-8 MiB (large-assets-re.md 7).
    """
    storage = [writer.Ps5TextureStorage(t.name, len(t.allocation), t.storage_alignment) for t in inputs]
    plan = writer.plan_ps5_texture_dictionary(storage, limits, name_policy=SOURCE_NAME_POLICY)
    if plan["graphicsBytes"] <= SINGLE_PAGE_BYTES:
        return None
    largest = max(len(t.allocation) for t in inputs)
    return max(GRAPHICS_PAGE_BYTES, 1 << (largest - 1).bit_length())


def convert(source: dict, templates: Path | None, target: str = "ppsa04264-01.010.002") -> tuple[bytes, list[tuple]]:
    """Target dictionary bytes and one report row per texture for a decoded PC dictionary."""
    limits = replace(Limits(), max_file_bytes=1 << 28, max_total_bytes=1 << 31, max_entries=4096)
    # The writer's 64-texture cap is a host resource bound, not an engine limit (retail vehshare is larger).
    writer.MAX_TEXTURES = MAX_DICTIONARY_TEXTURES

    ps4 = target_platform(target) == "ps4"
    if ps4 and templates is None:
        if not os.environ.get(PS4_TEMPLATES_ENV):
            raise SystemExit(
                f"PS4 output needs --templates DIR (or {PS4_TEMPLATES_ENV}): the directory holding the stock"
                " 1.57 vehicles.rpf adder.otd and asea.otd"
            )
        templates = Path(os.environ[PS4_TEMPLATES_ENV])
    for texture in source["textures"]:
        if issue := texture_name_issue(texture["name"], SOURCE_NAME_POLICY):
            raise AssetError(f"{texture['name']}: {issue}; repair source dictionary and fragment names together")
    meta = {}
    for fmt, (ptd, name) in (PS4_TEMPLATES if ps4 else PS5_TEMPLATES).items():
        path = (templates / (ptd if ps4 else ptd.removeprefix("build/assets/"))) if templates else ROOT / ptd
        if ps4:
            stock = parse_ps4_texture_dictionary(path.read_bytes(), limits)
            t = next(t for t in stock["textures"] if t["name"] == name)
            if t["formatCode"] & 0xFF != ps4_texture_writer.FORMAT_CODES[fmt]:
                raise AssetError(f"PS4 {fmt} donor {name} has the wrong native format")
            donor_layout = ps4_texture_writer.surface_layout(t["width"], t["height"], t["mipLevels"], fmt)
            if (
                t["tileMode"] != ps4_texture_writer.TILE_MODE
                or t["rawObject"][0x1E] != 13
                or len(t["allocation"]) != donor_layout.storage_bytes
                or t["rawView"] != ps4_texture_writer.texture_view(donor_layout)
            ):
                raise AssetError(f"PS4 {fmt} donor {name} is outside the reviewed native layout")
            meta[fmt] = (t["formatCode"], t["rawObject"], t["rawView"], t["tileMode"])
        else:
            stock = parse_ps5_texture_dictionary(path.read_bytes(), limits)
            t = next(t for t in stock["textures"] if t["name"] == name)
            meta[fmt] = (t["formatCode"], t["metadata"])

    inputs, expected, report = [], {}, []
    expected_ps4_mips = {}
    for texture in source["textures"]:
        fmt, mips = texture["format"], list(texture["mips"])
        if fmt in UNCOMPRESSED:
            mips, fmt = to_bc3(texture), "BC3"
        elif fmt == "BC2":
            mips, fmt = [bc2_to_bc3(m) for m in mips], "BC3"
        elif fmt == "BC7":
            mips, fmt = bc7_to_bc3(texture), "BC3"
        if fmt not in meta:
            raise SystemExit(f"{texture['name']}: unsupported format {texture['format']}")
        width, height, levels = texture["width"], texture["height"], texture["mipLevels"]
        tile = 5 if min(width, height) >= 64 else 1
        if ps4:
            layout = ps4_texture_writer.surface_layout(width, height, levels, fmt)
            allocation = ps4_texture_writer.tile_mips(tuple(mips), layout)
            expected_ps4_mips[texture["name"]] = (layout, tuple(mips))
            code, raw_object, _, tile_mode = meta[fmt]
            code = (code & 0xFFFF00FF) | (levels << 8)
            inputs.append(
                ps4_texture_writer.Ps4TextureInput(
                    texture["name"],
                    width,
                    height,
                    code,
                    levels,
                    tile_mode,
                    allocation,
                    layout.alignment_bytes,
                    raw_object,
                    ps4_texture_writer.texture_view(layout),
                )
            )
        else:
            layout = surface_layout(width, height, levels, fmt, tile)
            allocation = tile_mips(tuple(mips), layout)
            code, m = meta[fmt]
            inputs.append(
                writer.Ps5TextureInput(
                    texture["name"],
                    width,
                    height,
                    code,
                    levels,
                    tile,
                    allocation,
                    layout.alignment_bytes,
                    writer.Ps5TextureMetadata(
                        m["flags"],
                        m["metadataCandidate"],
                        m["referenceCount"],
                        m["viewType"],
                        "ordinary2d-serialized-v1",
                    ),
                )
            )
        expected[texture["name"]] = allocation
        report.append((texture["name"], texture["format"], fmt, width, height, levels, tile_mode if ps4 else tile))
    if ps4:
        blob = ps4_texture_writer.write_ps4_texture_dictionary(
            inputs,
            limits,
            name_policy=SOURCE_NAME_POLICY,
            graphics_page_bytes=graphics_pages(inputs, limits),
            max_textures=MAX_DICTIONARY_TEXTURES,
        )
        parsed = parse_ps4_texture_dictionary(blob, limits)["textures"]
    else:
        blob = writer.write_ps5_texture_dictionary(
            inputs, limits, name_policy=SOURCE_NAME_POLICY, graphics_page_bytes=graphics_pages(inputs, limits)
        )
        parsed = parse_ps5_texture_dictionary(blob, limits)["textures"]
    for t in parsed:
        if t["allocation"] != expected[t["name"]]:
            raise SystemExit(f"{t['name']}: written allocation differs")
        if ps4:
            layout, wanted_mips = expected_ps4_mips[t["name"]]
            if ps4_texture_writer.untile_mips(t["allocation"], layout) != wanted_mips:
                raise AssetError(f"{t['name']}: PS4 readback mip blocks differ from the source")
    # The writer emits canonical stored deflate; retail resources use real deflate. Recompress the
    # identical payload so large dictionaries stay under the archive's 24-bit member size field.
    stream = zlib.decompressobj(-15)
    payload = stream.decompress(blob[16:])
    if not stream.eof:
        raise SystemExit("writer output does not inflate")
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    blob = blob[:16] + packer.compress(payload) + packer.flush()
    if zlib.decompressobj(-15).decompress(blob[16:]) != payload:
        raise SystemExit("recompressed payload differs")
    return blob, report


def solid_texture(name: str, rgba: tuple[int, int, int, int], size: int = 16) -> dict:
    """A one-colour BGRA8 source texture (full mip chain down to one block) for convert()."""
    b, g, r, a = rgba[2], rgba[1], rgba[0], rgba[3]
    mips, edge = [], size
    while edge >= 4:
        mips.append(bytes((b, g, r, a)) * edge * edge)
        edge //= 2
    return {"name": name, "format": "BGRA8", "width": size, "height": size, "mipLevels": len(mips), "mips": mips}


def convert_file(
    ytd: bytes,
    templates: Path | None,
    max_size: int | None = None,
    extra: list[dict] | None = None,
    target: str = "ppsa04264-01.010.002",
) -> tuple[bytes, dict]:
    """convert() from .ytd bytes, with the optional mip trim and extra (solid_texture) entries."""
    limits = replace(Limits(), max_file_bytes=1 << 28, max_total_bytes=1 << 31, max_entries=4096)
    trims = []
    import pc_gen9_resources

    if pc_gen9_resources.resource_family(ytd, ".ytd") == "gen9":  # Enhanced PC build (.ytd v5)
        source = {"textures": pc_gen9_resources.gen9_ytd_textures(ytd, max_size)[0]}
    else:
        if max_size is not None:
            ytd, trims = trim_mips(ytd, max_size)
        source = read_pc_texture_dictionary(ytd, limits)
    names = {t["name"].lower() for t in source["textures"]}
    for texture in extra or []:
        if texture["name"].lower() in names:
            raise SystemExit(f"{texture['name']}: already in the dictionary")
        names.add(texture["name"].lower())
    source = source | {"textures": [*source["textures"], *(extra or [])]}
    blob, rows = convert(source, templates, target)
    keys = ("name", "sourceFormat", "format", "width", "height", "mipLevels", "tileMode")
    return blob, {"textures": [dict(zip(keys, row, strict=True)) for row in rows], "mipTrims": trims}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ytd", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--target",
        choices=("ppsa04264-01.010.002", "cusa00411-01.57"),
        default="ppsa04264-01.010.002",
        help="resource platform and object layout (default: PS5)",
    )
    parser.add_argument(
        "--templates",
        type=Path,
        help="PS5: retail template cache (tools/fetch_retail_templates.py) instead of build/assets;"
        f" PS4: the stock 1.57 vehicles.rpf member directory (default: ${PS4_TEMPLATES_ENV})",
    )
    parser.add_argument("--max-size", type=int, help="drop mips above this size and below one compression block")
    parser.add_argument(
        "--embedded", type=Path, action="append", default=[], help="PC fragment whose embedded textures are added"
    )
    parser.add_argument("--embedded-max-size", type=int, help="--max-size for the --embedded textures only")
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit(f"refusing to overwrite {args.output}")
    extra, duplicates = [], []
    if args.embedded:
        from convert_pc_drawable import trim_texture, ytd_textures
        from repair_pc_embedded_textures import embedded_textures

        ytd = args.ytd.read_bytes()
        names = {t["name"].lower() for t in ytd_textures(ytd)[0]}  # Legacy or Enhanced PC
        for fragment in args.embedded:
            for texture in embedded_textures(fragment.read_bytes()):
                if texture["name"].lower() in names:
                    duplicates.append(f"{texture['name']} ({fragment.name})")
                    continue
                names.add(texture["name"].lower())
                size = args.embedded_max_size or args.max_size
                extra.append(trim_texture(texture, size) if size else texture)
    try:
        blob, result = convert_file(args.ytd.read_bytes(), args.templates, args.max_size, extra, args.target)
    except AssetError as error:
        if "exceeds the writer" not in str(error):
            raise SystemExit(str(error)) from None
        # The Prowler bike: 46 textures up to 2480x3508 (98 MB of mips) over the writer's dictionary budget.
        smaller = min(args.max_size // 2, 1024) if args.max_size else 1024
        raise SystemExit(
            f"{error}: textures too large for one dictionary; add --max-size {smaller}"
            f" (convert-vehicle: --max-texture-size {smaller})"
        ) from None
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    rows = result["textures"]
    bc2 = sum(1 for row in rows if row["sourceFormat"] == "BC2")
    bc7 = sum(1 for row in rows if row["sourceFormat"] == "BC7")
    converted = sum(1 for row in rows if row["sourceFormat"] in UNCOMPRESSED)
    print(
        f"wrote {args.output} bytes={len(blob)} textures={len(rows)} uncompressed->bc3={converted} bc2->bc3={bc2} bc7->bc3={bc7}"
    )
    if args.embedded:
        print(f"embedded textures added={len(extra)}" + "".join(f" {t['name']}" for t in extra))
    for name in duplicates:
        print(f"embedded texture {name}: the dictionary already has this name; its texture is kept")
    for trim in result["mipTrims"]:
        print(f"trimmed {trim['name']}: {trim['from']} -> {trim['to']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

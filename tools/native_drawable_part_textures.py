#!/usr/bin/env python3
"""Restore a kit livery part's own one-texture dictionary.

The engine binds entry zero of this dictionary to the car's livery shader.
The original PC fragment must embed exactly one texture. The converted dictionary
is appended to the converted part, its graphics pages are retained, and page info
is rebuilt. This step precedes LOD filling and takes a single-page part.
Retail texture templates are read only from the caller's template directory.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

from gtavmenu_tools.resource_view import SYS, Resource  # noqa: E402

GFX = 0x60000000
ROOT_INFO, INFO_HEADER, INFO_RECORD, INFO_SYSTEM, INFO_GRAPHICS = 0x08, 16, 8, 8, 9
FRAGMENT_DRAWABLE, DRAWABLE_GROUP, GROUP_TEXTURES = 0x30, 0x10, 0x08
TXD_COUNT, TXD_TEXTURES = 0x28, 0x30
SPARE = 0x200  # left after the new objects: the LOD step's empty collections (16 bytes each) + its guard


class GraftError(RuntimeError):
    pass


def part_dictionary(fragment: bytes, max_size: int | None, templates: Path | None) -> bytes:
    """A standalone PS5 .ptd of the PC fragment's embedded textures (exactly one)."""
    from convert_pc_drawable import trim_texture
    from convert_pc_ytd_writer import convert
    from repair_pc_embedded_textures import embedded_textures

    textures = embedded_textures(fragment)
    if len(textures) != 1:
        names = ", ".join(t["name"] for t in textures) or "none"
        raise GraftError(f"a livery part must embed exactly one texture (has {len(textures)}: {names})")
    textures = [trim_texture(t, max_size) if max_size else t for t in textures]
    blob, _ = convert({"textures": textures}, templates)
    return blob


def graft(part: bytes, ptd: bytes) -> tuple[bytes, dict]:
    """The part .pft with the .ptd embedded in its main drawable's shader group."""
    import native_drawable_dictionary as ndd
    import resource_page_layout as layout

    r = Resource(part)
    sys_flags, gfx_flags = struct.unpack_from("<II", part, 8)
    if r.header["graphicsBytes"] or len(layout.pages_of(sys_flags)) != 1:
        raise GraftError("the part must be one system page without graphics pages (convert parts single-page)")
    group = r.u(r.u(SYS, "<Q", FRAGMENT_DRAWABLE), "<Q", DRAWABLE_GROUP)
    if r.u(group, "<Q", GROUP_TEXTURES):
        raise GraftError("the part already embeds a texture dictionary")
    textures = ndd.parse_texture_dictionary(ptd)
    _, ptd_payload = ndd.decode_resource(ptd, ndd.MAX_PAYLOAD)
    _, _, ptd_sys_flags, ptd_gfx_flags = struct.unpack_from("<4sIII", ptd)
    ptd_system = sum(ndd.pc_pages(ptd_sys_flags))
    graphics = bytes(ptd_payload[ptd_system:])
    gfx_pages = len(ndd.pc_pages(ptd_gfx_flags))
    if len(graphics) != sum(ndd.pc_pages(ptd_gfx_flags)) or not gfx_pages:
        raise GraftError("the dictionary's graphics pages do not match its flags")

    used = len(r.payload.rstrip(b"\0"))
    cursor = (used + 0x40 + 15) & ~15
    placed: dict[int, int] = {}
    sys_objects = [o for o in textures.objects if o.region == "sys" and o.kind != "page-info"]
    for obj in sys_objects:
        cursor = (cursor + obj.align - 1) & -obj.align
        placed[id(obj)] = cursor
        cursor += len(obj.data)
    info_at = (cursor + 15) & ~15
    info_bytes = INFO_HEADER + INFO_RECORD * (1 + gfx_pages)
    end = info_at + info_bytes
    size = max(r.system, 1 << (end + SPARE - 1).bit_length())
    payload = bytearray(r.payload[: r.system]) + bytes(size - r.system)
    for obj in sys_objects:
        at = placed[id(obj)]
        payload[at : at + len(obj.data)] = obj.data
        for slot, (target, delta) in obj.refs.items():
            if target.region == "gfx":
                value = target.src + delta
            elif target.kind == "page-info":
                value = 0  # the root's page info: an embedded dictionary has none
            else:
                value = SYS + placed[id(target)] + delta
            struct.pack_into("<Q", payload, at + slot, value)
    root = SYS + placed[id(textures.root)]
    struct.pack_into("<Q", payload, r.off(group) + GROUP_TEXTURES, root)
    old_info = r.u(SYS, "<Q", ROOT_INFO)
    old_off = r.off(old_info)
    old_bytes = INFO_HEADER + INFO_RECORD * (payload[old_off + INFO_SYSTEM] + payload[old_off + INFO_GRAPHICS])
    payload[info_at : info_at + INFO_HEADER] = payload[old_off : old_off + INFO_HEADER]
    payload[old_off : old_off + old_bytes] = bytes(old_bytes)
    payload[info_at + INFO_SYSTEM], payload[info_at + INFO_GRAPHICS] = 1, gfx_pages
    struct.pack_into("<Q", payload, ROOT_INFO, SYS + info_at)

    new_sys = sys_flags if size == r.system else (sys_flags & 0xF0000000) | layout.flags_for_pages([size])
    new_gfx = (gfx_flags & 0xF0000000) | (ptd_gfx_flags & 0x0FFFFFFF)
    from fill_drawable_lods import encode

    out = part[:8] + struct.pack("<II", new_sys, new_gfx) + encode(bytes(payload) + graphics)
    check(out, textures)
    name = textures.root.refs[TXD_TEXTURES][0].refs[0][0].refs[0x28][0].data.rstrip(b"\0").decode("ascii", "replace")
    return out, {"texture": name, "systemBytes": size, "graphicsBytes": len(graphics), "flags": (new_sys, new_gfx)}


def check(out: bytes, textures) -> None:
    """Re-read: the shader group's dictionary has count 1 and its entry 0 is the texture, data in graphics."""
    r = Resource(out)
    group = r.u(r.u(SYS, "<Q", FRAGMENT_DRAWABLE), "<Q", DRAWABLE_GROUP)
    txd = r.u(group, "<Q", GROUP_TEXTURES)
    if not txd or r.u(txd, "<H", TXD_COUNT) != 1 or r.u(txd, "<Q", 0x08):
        raise GraftError("readback: the embedded dictionary is missing, has a page info or not one texture")
    texture = r.u(r.u(txd, "<Q", TXD_TEXTURES), "<Q", 0)
    data = r.u(texture, "<Q", 0x38)
    if not GFX <= data < GFX + r.header["graphicsBytes"] or r.u(texture, "<Q", 0x30) != texture + 0x58:
        raise GraftError("readback: texture data or view pointer is wrong")
    info = r.u(SYS, "<Q", ROOT_INFO)
    if r.u(info, "<B", INFO_SYSTEM) != 1 or not r.u(info, "<B", INFO_GRAPHICS):
        raise GraftError("readback: page info counts")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", type=Path, required=True, help="converted part (.partial.pft, before LODs)")
    parser.add_argument("--embedded", type=Path, required=True, help="the original PC fragment (embedded-textures/)")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-size", type=int, help="drop mips above this size (convert-vehicle --part-texture-size)")
    parser.add_argument("--templates", type=Path, help="retail template cache (as convert_pc_ytd_writer.py)")
    args = parser.parse_args(argv)
    if args.output.exists():
        raise SystemExit(f"refusing to overwrite {args.output}")
    try:
        ptd = part_dictionary(args.embedded.read_bytes(), args.max_size, args.templates)
    except GraftError as error:  # this part's livery cannot show; the rest of the car converts
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(args.input.read_bytes())
        print(f"livery texture: {args.input.name} left unchanged: {error}")
        return 0
    try:
        out, info = graft(args.input.read_bytes(), ptd)
    except GraftError as error:
        raise SystemExit(f"livery texture: {error}") from None
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(out)
    print(
        f"livery texture {info['texture']} embedded: system {info['systemBytes']:#x}, graphics "
        f"{info['graphicsBytes']:#x}, flags {info['flags'][0]:#x}/{info['flags'][1]:#x}; wrote {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

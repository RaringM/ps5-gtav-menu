#!/usr/bin/env python3
"""Give fragment drawables independent empty medium/low/very-low LOD lists.

The engine requires each selected collection to exist, even when it is empty.
Sharing a collection between slots causes repeated relocation. Each missing list
is therefore allocated independently in unreferenced slack or a new resource page.
The result is re-parsed and checked for shared collections and models.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from collections import Counter
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

import resource_page_layout  # noqa: E402
from gtavmenu_tools.meta_resource import stored_deflate  # noqa: E402
from gtavmenu_tools.resource_view import SYS, Resource  # noqa: E402

LISTS = (0x50, 0x58, 0x60, 0x68)


def drawables(r: Resource) -> list[tuple[str, int]]:
    """Main, DrawableArray, cloth and every physics child's undamaged/damaged drawable."""
    out = [("main", r.u(SYS, "<Q", 0x30))]
    array, count = r.u(SYS, "<Q", 0x38), r.u(SYS, "<I", 0x48)
    out += [(f"array[{i}]", r.u(array, "<Q", 8 * i)) for i in range(count if array else 0)]
    out.append(("cloth", r.u(SYS, "<Q", 0xF8)))
    group = r.u(SYS, "<Q", 0xF0)
    for k, off in enumerate((0x10, 0x18, 0x20)):
        lod = r.u(group, "<Q", off) if group else 0
        if not lod:
            continue
        children, n = r.u(lod, "<Q", 0xD0), r.u(lod, "<B", 0x11D)
        for c in range(n):
            child = r.u(children, "<Q", 8 * c)
            out.append((f"phys{k + 1}.child{c}.d1", r.u(child, "<Q", 0xA0)))
            out.append((f"phys{k + 1}.child{c}.d2", r.u(child, "<Q", 0xA8)))
    seen, unique = set(), []
    for name, d in out:
        if d and d not in seen:
            seen.add(d)
            unique.append((name, d))
    return unique


def verify(r: Resource) -> list[str]:
    problems = []
    slots = []
    for name, d in drawables(r):
        lists = struct.unpack_from("<4Q", r.payload, r.off(d) + LISTS[0])
        if lists[0] and not all(lists):
            problems.append(f"{name}: null LOD slot {[hex(x) for x in lists]}")
        slots += [(name, k, p) for k, p in enumerate(lists) if p]
    owners = Counter(p for _, _, p in slots)
    for p, n in owners.items():
        if n > 1:
            problems.append(f"collection {p:#x} referenced by {n} LOD slots")
    # Census over every aligned qword of the system page. The only other reference a converted
    # drawable carries is DrawableModelsPointer (+0xA0, the models block = the high collection,
    # with blocks size 0 at +0x9A), which the hardware-proven Razor has too; it is not relocated as
    # a collection, so it is excluded.
    words = Counter(struct.unpack_from(f"<{r.system // 8}Q", r.payload, 0))
    for _, d in drawables(r):
        models_block = r.u(d, "<Q", 0xA0)
        if models_block:
            words[models_block] -= 1
    for p in owners:
        if words[p] != 1:
            problems.append(f"collection {p:#x} appears {words[p]} times in the system page")
    models = Counter()
    for _, _, p in slots:
        items, count = r.u(p, "<Q"), r.u(p, "<H", 8)
        for i in range(count):
            models[r.u(items, "<Q", 8 * i)] += 1
    for m, n in models.items():
        if n > 1:
            problems.append(f"model {m:#x} in {n} collections")
    return problems


STORED_MAX_BYTES = 16 * 1024 * 1024  # largest stored member size verified on hardware


def encode(system: bytes) -> bytes:
    """Stored deflate up to 16 MiB (unchanged outputs); above it raw deflate like copy_drawable_lods
    (a80-mp.pft): a heavy fragment (LaFerrari 24 MiB) would otherwise sit uncompressed in the pack
    archive, which stays resident in direct memory (the archive stays resident in direct memory)."""
    if len(system) <= STORED_MAX_BYTES:
        return stored_deflate(system)
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    return packer.compress(system) + packer.flush()


def fill_paged(args, blob: bytes, r: Resource, payload: bytearray, needed: list, sys_flags: int, gfx_flags: int) -> int:
    """Multi-page input (route B): collections go into the last page's tail (or a new page), never
    across a page boundary; the page list, flags and ResourcePagesInfo follow the allocation."""
    alloc = resource_page_layout.TailAllocator.after_used(payload, sys_flags)
    values = struct.unpack_from(f"<{r.system // 8}Q", payload, 0)
    used = len(payload.rstrip(b"\0"))
    if any(SYS + used <= v < SYS + r.system for v in values):
        raise SystemExit("a pointer targets the trailing zero region; refusing to use it as slack")
    for name, d, k in needed:
        at = alloc.alloc(16)  # {items 0, count 0, capacity 0}
        struct.pack_into("<Q", payload, r.off(d) + LISTS[k], SYS + at)
        print(f"{name}: slot {LISTS[k]:#x} -> empty collection at {SYS + at:#x}")
    system, sizes = alloc.finish()
    flags = (sys_flags & 0xF0000000) | resource_page_layout.flags_for_pages(sizes)
    out = blob[:8] + struct.pack("<II", flags, gfx_flags) + encode(system)
    back = Resource(out)
    if back.payload != system or back.system != sum(sizes) or back.header["graphicsBytes"] != r.header["graphicsBytes"]:
        raise SystemExit("readback mismatch")
    problems = verify(back)
    if problems:
        raise SystemExit("verification failed:\n  " + "\n  ".join(problems))
    if args.output.exists():
        raise SystemExit(f"refusing to overwrite {args.output}")
    args.output.write_bytes(out)
    print(
        f"wrote {args.output} bytes={len(out)} empty collections={len(needed)} flags={flags:#x}/{gfx_flags:#x} "
        f"pages={len(sizes)} ({r.system:#x} -> {back.system:#x} system bytes)"
    )
    return 0


def unchanged(blob: bytes, r: Resource, output: Path) -> int:
    """Every drawable with a high list already has its medium/low/very-low lists (the Gta5KoRn gmt400
    ships real LODs): the input is the output, once copy_drawable_lods.census shows that no collection,
    model, geometry, buffer or data range is referenced twice (aliased LOD slots are not safe to relocate)."""
    from copy_drawable_lods import census  # copy_drawable_lods imports this module

    problems = census(r)
    if problems:
        raise SystemExit("the source's own LOD lists share objects:\n  " + "\n  ".join(problems[:20]))
    if output.exists():
        raise SystemExit(f"refusing to overwrite {output}")
    output.write_bytes(blob)
    print(f"wrote {output} unchanged: every LOD slot is already set (the source's own LOD lists)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    blob = args.input.read_bytes()
    r = Resource(blob)
    payload = bytearray(r.payload)
    needed = []
    for name, d in drawables(r):
        lists = struct.unpack_from("<4Q", payload, r.off(d) + LISTS[0])
        if lists[0]:
            needed += [(name, d, k) for k in (1, 2, 3) if not lists[k]]
    if not needed:
        return unchanged(blob, r, args.output)

    sys_flags, gfx_flags = struct.unpack_from("<II", blob, 8)
    if len(resource_page_layout.pages_of(sys_flags)) > 1:
        return fill_paged(args, blob, r, payload, needed, sys_flags, gfx_flags)
    used = max(i for i in range(r.system) if payload[i]) + 1
    start = (used + 0x40 + 15) & ~15  # 64-byte guard after the last used byte
    end = start + 16 * len(needed)
    if end > r.system:
        raise SystemExit(f"no slack: need {end:#x} bytes of a {r.system:#x} system page")
    values = struct.unpack_from(f"<{r.system // 8}Q", payload, 0)
    if any(SYS + used <= v < SYS + r.system for v in values):
        raise SystemExit("a pointer targets the trailing zero region; refusing to use it as slack")

    for i, (name, d, k) in enumerate(needed):
        at = start + 16 * i
        payload[at : at + 16] = bytes(16)  # {items 0, count 0, capacity 0}
        struct.pack_into("<Q", payload, r.off(d) + LISTS[k], SYS + at)
        print(f"{name}: slot {LISTS[k]:#x} -> empty collection at {SYS + at:#x}")

    out = blob[:16] + stored_deflate(bytes(payload))
    check = zlib.decompressobj(-15)
    if check.decompress(out[16:]) != bytes(payload) or not check.eof:
        raise SystemExit("readback mismatch")
    back = Resource(out)
    if back.system != r.system or back.header["graphicsBytes"] != r.header["graphicsBytes"]:
        raise SystemExit("page sizes changed")
    problems = verify(back)
    if problems:
        raise SystemExit("verification failed:\n  " + "\n  ".join(problems))
    diff = sum(a != b for a, b in zip(r.payload, back.payload, strict=True))
    if args.output.exists():
        raise SystemExit(f"refusing to overwrite {args.output}")
    args.output.write_bytes(out)
    print(f"wrote {args.output} bytes={len(out)} empty collections={len(needed)} payload bytes changed={diff}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

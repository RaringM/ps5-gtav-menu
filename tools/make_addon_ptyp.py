#!/usr/bin/env python3
"""Make a one-archetype add-on .ptyp by renaming the archetype of a template typ.

--template is a loose RSC7 .ptyp (fwMapTypes, resource v2) taken from the user's own game; nothing
of it ships with this tool. The default layout is that of a one-archetype retail typ such as
icons13.ptyp, whose CBaseArchetypeDef name (+0x58, payload 0x1f8) and assetName (+0x70, payload
0x210) are joaat(--old-name). Patching both hashes to a new name changes no pointer or size; the
drawable member is renamed to match in the pack. Optional edits: texture dictionary, flags,
physicsDictionary, specialAttribute, bounds, or every scalar field copied from another archetype
(--def-from). Output is a loose RSC7 v2 resource (stored deflate) whose header flags equal the
template's, read back before it is written. Refuses to overwrite.

  make_addon_ptyp.py --template icons13.ptyp --new-name gmprop_thruster --output gmprop_thruster.ptyp
"""

from __future__ import annotations

import argparse
import struct
import zlib
from collections.abc import Callable
from pathlib import Path

from gtavmenu_tools import rpf7
from gtavmenu_tools.hashes import joaat
from gtavmenu_tools.meta_resource import stored_deflate

__all__ = ["build", "collision_problem", "main", "stored_deflate"]

NAME_OFFSET, ASSET_OFFSET, TXD_OFFSET = 0x1F8, 0x210, 0x1FC
FLAGS_OFFSET, PHYSICS_OFFSET = 0x1AC, 0x208  # CBaseArchetypeDef +0x0c flags, +0x68 physicsDictionary
ASSET_TYPE_OFFSET = NAME_OFFSET - 0x58 + 0x6C  # CBaseArchetypeDef +0x6c assetType
ASSET_TYPE_DRAWABLE = 2
DEFAULT_OLD_NAME = "prop_mk_transform_thruster"

# (spec "NAME=ARCHIVE:PATH") -> (name, stored, system flags, graphics flags, payload); developer hook.
RetailMember = Callable[[str], tuple[str, bytes, int, int, bytes]]


def collision_problem(payload: bytes) -> str | None:
    """Why the archetype would spawn and render without collision, or None.

    Every retail drawable archetype (assetType 2) names a physicsDictionary; the game builds the
    object's physics only when that word is non-zero (a dumpster clone with 0 had no collision).
    Fragments carry their own physics.
    """
    asset_type = struct.unpack_from("<I", payload, ASSET_TYPE_OFFSET)[0]
    physics = struct.unpack_from("<I", payload, PHYSICS_OFFSET)[0]
    if asset_type == ASSET_TYPE_DRAWABLE and physics == 0:
        return (
            "drawable archetype with physicsDictionary 0 spawns and renders without collision; "
            "copy a retail definition (--def-from STORED_PTYP ARCHETYPE), name one (--physics), "
            "or pass --allow-no-collision for a deliberately non-colliding prop"
        )
    return None


def _def_from(payload: bytearray, source: bytes, archetype: str, label: str) -> None:
    stream = zlib.decompressobj(-15)
    retail = stream.decompress(source[16:])
    wanted = joaat(archetype)
    base = next(
        (
            at - 0x58
            for at in range(0x58, len(retail) - 0x1C, 4)
            if struct.unpack_from("<I", retail, at)[0] == wanted
            and struct.unpack_from("<I", retail, at + 0x18)[0] == wanted
        ),
        None,
    )
    if base is None:
        raise SystemExit(f"{archetype} (name == assetName) not found in {label}")
    template = NAME_OFFSET - 0x58
    payload[template + 0x08 : template + 0x58] = retail[base + 0x08 : base + 0x58]
    struct.pack_into("<I", payload, template + 0x68, struct.unpack_from("<I", retail, base + 0x68)[0])
    struct.pack_into("<I", payload, template + 0x6C, struct.unpack_from("<I", retail, base + 0x6C)[0])


def build(
    raw: bytes,
    sys_flags: int,
    gfx_flags: int,
    *,
    new_name: str,
    old_name: str = DEFAULT_OLD_NAME,
    txd: str | None = None,
    bounds: list[float] | None = None,
    flags: int | None = None,
    physics: str | None = None,
    special_attribute: int | None = None,
    def_from: tuple[bytes, str, str] | None = None,
    allow_no_collision: bool = False,
) -> bytes:
    """The renamed (and optionally edited) typ as a loose RSC7 resource, read back once.

    `def_from` is (stored .ptyp member bytes, archetype name, label for messages).
    """
    old, new = joaat(old_name), joaat(new_name)
    payload = bytearray(raw)
    if len(payload) < ASSET_OFFSET + 4:
        raise SystemExit("template payload is too small for a one-archetype typ")
    for offset in (NAME_OFFSET, ASSET_OFFSET):
        if struct.unpack_from("<I", payload, offset)[0] != old:
            raise SystemExit(f"payload +{offset:#x} is not joaat({old_name})")
        struct.pack_into("<I", payload, offset, new)
    if def_from:
        _def_from(payload, def_from[0], def_from[1], def_from[2])
    if txd:
        struct.pack_into("<I", payload, TXD_OFFSET, joaat(txd))
    if special_attribute is not None:
        struct.pack_into("<I", payload, NAME_OFFSET - 0x58 + 0x10, special_attribute)
    if flags is not None:
        struct.pack_into("<I", payload, FLAGS_OFFSET, flags)
    if physics is not None:
        struct.pack_into("<I", payload, PHYSICS_OFFSET, 0 if physics in ("", "0") else joaat(physics))
    if bounds:
        lo, hi = bounds[:3], bounds[3:]
        centre = [(a + b) / 2 for a, b in zip(lo, hi, strict=True)]
        radius = sum(((b - a) / 2) ** 2 for a, b in zip(lo, hi, strict=True)) ** 0.5
        struct.pack_into("<3f", payload, 0x1C0, *lo)
        struct.pack_into("<3f", payload, 0x1D0, *hi)
        struct.pack_into("<3f", payload, 0x1E0, *centre)
        struct.pack_into("<f", payload, 0x1F0, radius)
    problem = collision_problem(bytes(payload))
    if problem and not allow_no_collision:
        raise SystemExit(f"{new_name}: {problem}")
    if bytes(payload).count(struct.pack("<I", old)):
        raise SystemExit("old name hash still present elsewhere in the payload")
    version = ((sys_flags >> 28) << 4) | (gfx_flags >> 28)
    resource = struct.pack("<4sIII", b"RSC7", version, sys_flags, gfx_flags) + stored_deflate(bytes(payload))
    check = zlib.decompressobj(-15)
    if check.decompress(resource[16:]) != bytes(payload) or not check.eof:
        raise SystemExit("readback mismatch")
    return resource


def main(
    argv: list[str] | None = None, *, retail_member: RetailMember | None = None, default_source: str | None = None
) -> int:
    """`retail_member` (a developer hook, never set by this tool) adds --source ARCHIVE:PATH, a typ
    read from a keyed retail archive with the game's table keys."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--template", type=Path, help="loose RSC7 .ptyp from the user's own game")
    if retail_member is not None:
        source.add_argument("--source", metavar="ARCHIVE:PATH", help=f"typ inside a retail archive ({default_source})")
    parser.add_argument("--old-name", default=DEFAULT_OLD_NAME)
    parser.add_argument("--new-name", required=True)
    parser.add_argument("--txd", help="texture dictionary name for the archetype (default: keep)")
    parser.add_argument(
        "--bounds",
        nargs=6,
        type=float,
        metavar=("MINX", "MINY", "MINZ", "MAXX", "MAXY", "MAXZ"),
        help="replace bbMin/bbMax and derive bsCentre/bsRadius (default: keep the template's)",
    )
    parser.add_argument("--flags", type=lambda v: int(v, 0), help="archetype flags word (default: keep)")
    parser.add_argument("--physics", help="physicsDictionary name (a .pbn bounds dictionary; default: keep)")
    parser.add_argument("--special-attribute", type=lambda v: int(v, 0), help="specialAttribute (default: keep)")
    parser.add_argument(
        "--def-from",
        nargs=2,
        metavar=("STORED_PTYP", "ARCHETYPE"),
        help="copy every scalar field of ARCHETYPE's CBaseArchetypeDef in a stored .ptyp member (16-byte "
        "prefix + raw deflate: lodDist, flags, specialAttribute, bounds, hdTextureDist, physicsDictionary, "
        "assetType) into the template archetype; name/assetName/txd still come from --new-name/--txd",
    )
    parser.add_argument(
        "--allow-no-collision",
        action="store_true",
        help="build a drawable archetype without a physicsDictionary (it will not collide)",
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)

    if args.template:
        try:
            _stored, sys_flags, gfx_flags, raw = rpf7.loose_resource(args.template.read_bytes(), str(args.template))
        except rpf7.Rpf7Error as error:
            raise SystemExit(str(error)) from None
    elif retail_member is not None and (args.source or default_source):
        _, _stored, sys_flags, gfx_flags, raw = retail_member(f"x.ptyp={args.source or default_source}")
    else:
        parser.error("--template is required")
    def_from = None
    if args.def_from:
        def_from = (Path(args.def_from[0]).read_bytes(), args.def_from[1], args.def_from[0])
    resource = build(
        raw,
        sys_flags,
        gfx_flags,
        new_name=args.new_name,
        old_name=args.old_name,
        txd=args.txd,
        bounds=args.bounds,
        flags=args.flags,
        physics=args.physics,
        special_attribute=args.special_attribute,
        def_from=def_from,
        allow_no_collision=args.allow_no_collision,
    )
    if args.output.exists():
        raise SystemExit(f"refusing to overwrite {args.output}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(resource)
    version = resource[4]
    old, new = joaat(args.old_name), joaat(args.new_name)
    print(
        f"wrote {args.output} version={version} flags={sys_flags:#x}/{gfx_flags:#x} "
        f"name {old:#010x} -> {new:#010x} payload={len(raw)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

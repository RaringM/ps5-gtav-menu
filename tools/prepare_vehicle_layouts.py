#!/usr/bin/env python3
"""Prepare a converted car's vehiclelayouts.meta for runtime loading (VEHICLE_LAYOUTS_FILE, type 165).

The runtime merge appends each item of the 20
CVehicleMetadataMgr arrays only when its name hash is not already registered ("first wins"), so a
mod item whose name collides with a retail one would be silently dropped and the car would use the
retail definition. This tool:
  1. collects every <Name> the mod file defines (any of the 20 arrays),
  2. renames each to PREFIX + name (default GTAVMENU_<MODEL>_),
  3. rewrites every reference to a renamed definition, in the layouts file (ref="..." and
     <Item>...</Item> / element text) and in the car's vehicles.meta (<layout>, <coverBoundOffsets>,
     <povTuningInfo>, <explosionInfo>, firstPersonDrivebyData items, ...),
  4. checks that every remaining reference resolves to a mod definition or a retail one
     (--retail retail vehiclelayouts.meta), and reports unresolved names.

  prepare_vehicle_layouts.py --model a80 --layouts a80/vehiclelayouts.meta --vehicles v1/vehicles.meta \\
      --retail retail_vehiclelayouts.meta --out-layouts out/vehiclelayouts.meta --out-vehicles out/vehicles.meta
"""

from __future__ import annotations

import argparse
import contextlib
import re
import sys
import xml.dom.minidom
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

NAME = re.compile(r"<Name>\s*([^<\s]+)\s*</Name>")
REF = re.compile(r'ref="([^"]+)"')
TEXT = re.compile(r">\s*([A-Za-z0-9_@]+)\s*<")


# Motorbikes: a bike's first-person drive-by set is one BIKE_*_FRONT item that
# matches its seat layout; the car default (LOW_BUCCANEER_FRONT_LEFT/RIGHT) would give a bike rider car
# drive-by animations. Each layout maps to the drive-by set of a retail bike that uses it (update.rpf's
# vehicles.meta, read-only from the game copy): bagger, daemon, akuma, bati, sanchez, policeb, faggio2, blazer.
BIKE_DRIVEBY = {
    "LAYOUT_BIKE_FREEWAY": "BIKE_BAGGER_FRONT",
    "LAYOUT_BIKE_CHOPPER": "BIKE_DAEMON_FRONT",
    "LAYOUT_BIKE_SPORT": "BIKE_AKUMA_FRONT",
    "LAYOUT_BIKE_SPORT_BATI": "BIKE_BATI_FRONT",
    "LAYOUT_BIKE_DIRT": "BIKE_SANCHEZ_FRONT",
    "LAYOUT_BIKE_POLICE": "BIKE_POLICEB_FRONT",
    "LAYOUT_BIKE_SCOOTER": "BIKE_FAGGIO_FRONT",
    "LAYOUT_BIKE_QUAD": "BIKE_BLAZER_FRONT",
}


def default_driveby(vehicles: str) -> tuple[str, ...] | None:
    """The retail drive-by set for a bike's undefined firstPersonDrivebyData (BIKE_DRIVEBY by its <layout>;
    another bike layout -> the freeway set), or None for anything that is not VEHICLE_TYPE_BIKE/QUADBIKE."""
    kind = re.search(r"<type>\s*(VEHICLE_TYPE_[A-Z_]+)\s*</type>", vehicles)
    if kind is None or kind.group(1) not in ("VEHICLE_TYPE_BIKE", "VEHICLE_TYPE_QUADBIKE"):
        return None
    layout = re.search(r"<layout>\s*([A-Za-z0-9_]+)\s*</layout>", vehicles)
    return (BIKE_DRIVEBY.get(layout.group(1).upper() if layout else "", "BIKE_BAGGER_FRONT"),)


RELATION = re.compile(r"(<Item>\s*<parent>\s*)([^<\s]+)(\s*</parent>\s*<child>\s*)([^<\s]+)(\s*</child>)")


def reparent_shipped_txds(vehicles: str, shipped: set[str]) -> tuple[str, list[str]]:
    """txdRelationships of a texture dictionary the pack ships (a mod's own interior dictionary):
    a parent the pack does not ship becomes vehshare, the resident root of every retail vehicle chain. The
    Dominator GTX names vehicles_tfdominator_interior -> vehicles_sup1_interior, which the PS5 game does not
    have; a parent slot without a file would leave the car's textures waiting on it."""
    shipped = {name.lower() for name in shipped}
    notes = []

    def item(match: re.Match) -> str:
        parent, child = match.group(2), match.group(4)
        if child.lower() not in shipped or parent.lower() in shipped or parent.lower() == "vehshare":
            return match.group(0)
        notes.append(f"txd parent of {child}: {parent} (not shipped) -> vehshare")
        return match.group(1) + "vehshare" + match.group(3) + child + match.group(5)

    return RELATION.sub(item, vehicles), notes


def defined_names(text: str) -> list[str]:
    return list(dict.fromkeys(NAME.findall(text)))


def rename(text: str, mapping: dict[str, str]) -> str:
    def ref(match: re.Match) -> str:
        return f'ref="{mapping.get(match.group(1), match.group(1))}"'

    def body(match: re.Match) -> str:
        token = match.group(1)
        return match.group(0).replace(token, mapping[token]) if token in mapping else match.group(0)

    return TEXT.sub(body, REF.sub(ref, text))


MAX_XML_BYTES = 16 * 1024 * 1024
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_@]*")


def read_xml(path: Path) -> str:
    """Bounded UTF-8 XML without DTD/entity expansion or redirected input paths."""
    if any(part.is_symlink() for part in (path, *path.parents)) or not path.is_file():
        raise SystemExit(f"XML input must be a regular non-symlink file: {path}")
    with path.open("rb") as stream:
        raw = stream.read(MAX_XML_BYTES + 1)
    if not raw or len(raw) > MAX_XML_BYTES:
        raise SystemExit(f"XML input is empty or exceeds {MAX_XML_BYTES} bytes: {path}")
    try:
        text = raw.decode("utf-8-sig").replace("\r\n", "\n")
        if re.search(r"<!\s*(?:DOCTYPE|ENTITY)\b", text, re.I):
            raise ValueError("DTD and entity declarations are forbidden")
        xml.dom.minidom.parseString(text.encode("utf-8"))
    except (ValueError, UnicodeError, xml.parsers.expat.ExpatError) as exc:
        raise SystemExit(f"invalid XML {path}: {exc}") from None
    return text


def check_output(path: Path) -> None:
    if path.exists() or any(part.is_symlink() for part in (path, *path.parents)):
        raise SystemExit(f"refusing existing or symlink output: {path}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--model", required=True)
    parser.add_argument("--prefix")
    parser.add_argument("--layouts", type=Path, required=True)
    parser.add_argument("--vehicles", type=Path, required=True)
    parser.add_argument("--retail", type=Path, required=True, help="retail vehiclelayouts.meta (decrypted)")
    parser.add_argument("--out-layouts", type=Path, required=True)
    parser.add_argument("--out-vehicles", type=Path, required=True)
    args = parser.parse_args(argv)

    prefix = args.prefix or f"GTAVMENU_{args.model.upper()}_"
    if len(prefix) > 96 or not IDENTIFIER.fullmatch(prefix):
        raise SystemExit("layout prefix must be an identifier of at most 96 characters")
    if args.out_layouts.resolve() == args.out_vehicles.resolve():
        raise SystemExit("layout and vehicle outputs must be distinct")
    check_output(args.out_layouts)
    check_output(args.out_vehicles)
    layouts, vehicles = read_xml(args.layouts), read_xml(args.vehicles)
    retail = set(NAME.findall(read_xml(args.retail)))
    names = defined_names(layouts)
    mapping = {n: prefix + n for n in names}
    collisions = sorted(n for n in names if n in retail)
    out_layouts, out_vehicles = rename(layouts, mapping), rename(vehicles, mapping)
    defined = set(mapping.values()) | retail
    unresolved = sorted({r for r in REF.findall(out_layouts) if r not in defined and r != "NULL"})
    if any(len(name) > 127 or not IDENTIFIER.fullmatch(name) for name in mapping.values()):
        raise SystemExit("renamed layout definitions must be identifiers of at most 127 characters")
    if retail & set(mapping.values()):
        raise SystemExit("renamed layout definitions still collide with retail names")
    if unresolved:
        raise SystemExit("UNRESOLVED references: " + ", ".join(unresolved))
    outputs = ((args.out_layouts, out_layouts), (args.out_vehicles, out_vehicles))
    for _, text in outputs:
        xml.dom.minidom.parseString(text.encode("utf-8"))
    with contextlib.ExitStack() as stack:
        streams = []
        for path, text in outputs:
            path.parent.mkdir(parents=True, exist_ok=True)
            streams.append((stack.enter_context(path.open("x", encoding="utf-8", newline="\n")), text))
        for stream, text in streams:
            stream.write(text)
    print(f"renamed {len(names)} definitions with prefix {prefix}; retail collisions avoided: {len(collisions)}")
    for name in collisions:
        print(f"  collision: {name}")
    vehicle_refs = sorted({v for v in mapping.values() if v in out_vehicles})
    print(f"vehicles.meta now references: {', '.join(vehicle_refs) or '(none)'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

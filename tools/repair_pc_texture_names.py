#!/usr/bin/env python3
"""Turn texture names with spaces or hyphens into identifiers in an imported PC fixture (F1.2 breadth).

The Gta5KoRn pack's silver94 names textures "Reese Towpower Logo", "hitch tag", "chevrolet-orange",
"seat2-normal". The PC game keys textures by joaat of the lowercased name, so any byte string works
there, but the converter's texture-reference reader (native_texture_references.name_at) and the PS5
texture writer (gtavmenu_tools.texture_names SOURCE_NAME_POLICY) admit only [A-Za-z0-9_] identifiers.
This tool writes a new fixture in which every such name has each other byte replaced by "_" (same
length, so every string is rewritten in place):
  - in each .ytd: the texture's name string, and the dictionary's hash table (joaat of the new
    lowercased name), re-sorted by hash with the pointer table in the same order;
  - in each .yft of the material report: the name string of every texture reference whose name is
    one of those (the report's textureSystemOffset; NamePointer at +0x28, as in the Legacy texture).
Both sides change together, so every binding keeps resolving: "reese towpower logo" in the fragment
and "Reese Towpower Logo" in the dictionary both become joaat("reese_towpower_logo"). Refused when a
name does not start with a letter or digit after the change, or when the new name collides with
another texture's name (hash) in the same dictionary. The original fixture stays intact and the
manifest records every rename.

  repair_pc_texture_names.py --fixture build/assets/convert-<id>/fixtures/silver94-import \\
      --material-report <its material report> --output <new fixture>   (convert_vehicle.py --repair texture-names)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
import zlib
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

from gtavmenu_tools.asset_formats import Limits, decode_resource  # noqa: E402
from gtavmenu_tools.asset_textures import SYSTEM_BASE, inspect_legacy_dictionary  # noqa: E402
from gtavmenu_tools.hashes import joaat  # noqa: E402
from gtavmenu_tools.vehicle_repair_inputs import (  # noqa: E402
    FixtureError,
    copy_fixture,
    resource_path,
    validate_fixture,
)

NAME_POINTER = 40  # Legacy texture object / texture reference: name string pointer
IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9_]*")
RESOURCE_LIMIT = 512 * 1024 * 1024


def fixed(name: str) -> str:
    """The identifier spelling of a texture name (same length); refuses one that cannot start right."""
    new = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not IDENTIFIER.fullmatch(new):
        raise SystemExit(f"texture name {name!r} does not become an identifier ({new!r})")
    return new


def string_at(payload: bytes | bytearray, pointer: int) -> tuple[int, str]:
    at = pointer - SYSTEM_BASE
    end = payload.index(b"\0", at)
    return at, payload[at:end].decode("ascii")


def rename_dictionary(payload: bytearray, header: dict) -> list[dict]:
    """Rename every non-identifier texture of a Legacy .ytd payload in place; returns the renames."""
    rows = inspect_legacy_dictionary(bytes(payload), header, Limits())["textures"]
    renames = []
    for row in rows:
        if IDENTIFIER.fullmatch(row["name"]):
            continue
        new = fixed(row["name"])
        at, name = string_at(payload, struct.unpack_from("<Q", payload, row["systemOffset"] + NAME_POINTER)[0])
        if name != row["name"]:
            raise SystemExit(f"{row['name']}: name string differs from the dictionary row")
        payload[at : at + len(new)] = new.encode("ascii")
        renames.append({"texture": row["name"], "name": new, "nameHash": f"0x{joaat(new.lower()):08x}"})
    if not renames:
        return renames
    hp, count = struct.unpack_from("<QH", payload, 32)
    tp = struct.unpack_from("<Q", payload, 48)[0]
    hp, tp = hp - SYSTEM_BASE, tp - SYSTEM_BASE
    entries = []
    for index in range(count):
        pointer = struct.unpack_from("<Q", payload, tp + 8 * index)[0]
        _, name = string_at(payload, struct.unpack_from("<Q", payload, pointer - SYSTEM_BASE + NAME_POINTER)[0])
        entries.append((joaat(name.lower()), pointer))
    if len({key for key, _ in entries}) != count:
        raise SystemExit("a renamed texture collides with another texture of its dictionary")
    for index, (key, pointer) in enumerate(sorted(entries)):
        struct.pack_into("<I", payload, hp + 4 * index, key)
        struct.pack_into("<Q", payload, tp + 8 * index, pointer)
    for row in inspect_legacy_dictionary(bytes(payload), header, Limits())["textures"]:
        if not IDENTIFIER.fullmatch(row["name"]):
            raise SystemExit(f"{row['name']}: still not an identifier after the rename")
    return renames


def rename_references(payload: bytearray, offsets: set[int], names: set[str]) -> list[dict]:
    """Rename, in place, the non-identifier name strings of the texture references at `offsets`;
    `names` (the fixture's dictionary texture names, lowercased, after renames) marks the external ones."""
    done, renames = set(), []
    for offset in sorted(offsets):
        at, name = string_at(payload, struct.unpack_from("<Q", payload, offset + NAME_POINTER)[0])
        if IDENTIFIER.fullmatch(name) or at in done:
            continue
        new = fixed(name)
        payload[at : at + len(new)] = new.encode("ascii")
        done.add(at)
        # external: no dictionary of the fixture has it (unresolved before and after, like any external)
        renames.append({"reference": name, "name": new, "systemOffset": offset, "external": new.lower() not in names})
    return renames


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--material-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.output.exists():
        raise SystemExit(f"refusing to overwrite {args.output}")
    manifest = validate_fixture(args.fixture)
    report = json.loads(args.material_report.read_bytes())
    offsets: dict[str, set[int]] = {}
    for resource in report["resources"]:
        for shader in resource["shaders"]:
            for parameter in shader["textureParameters"]:
                if parameter.get("textureName") and not IDENTIFIER.fullmatch(parameter["textureName"]):
                    offsets.setdefault(resource["member"], set()).add(parameter["textureSystemOffset"])
    planned, known = [], set()  # known: every dictionary texture name of the fixture, lowercased, after renames
    for row in manifest["resources"]:
        if not row["member"].endswith(".ytd"):
            continue
        path = resource_path(args.fixture, row["member"])
        blob = path.read_bytes()
        header, payload = decode_resource(blob, RESOURCE_LIMIT)
        payload = bytearray(payload)
        renames = rename_dictionary(payload, header)
        known |= {r["name"].lower() for r in inspect_legacy_dictionary(bytes(payload), header, Limits())["textures"]}
        if renames:
            planned.append((row, blob, payload, renames))
    for row in manifest["resources"]:
        if row["member"] not in offsets:
            continue
        path = resource_path(args.fixture, row["member"])
        blob = path.read_bytes()
        header, payload = decode_resource(blob, RESOURCE_LIMIT)
        payload = bytearray(payload)
        renames = rename_references(payload, offsets[row["member"]], known)
        if renames:
            planned.append((row, blob, payload, renames))
    if not planned:
        raise SystemExit("no texture names to repair")
    copy_fixture(args.fixture, args.output, manifest)
    records = []
    for row, blob, payload, renames in planned:
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
        new_blob = blob[:16] + compressor.compress(bytes(payload)) + compressor.flush()
        resource_path(args.output, row["member"]).write_bytes(new_blob)
        row["bytes"] = row["storedBytes"] = len(new_blob)
        row["sha256"] = hashlib.sha256(new_blob).hexdigest()
        records.append({"member": row["member"], "renames": renames})
    manifest["textureNameRepairs"] = {
        "tool": "tools/repair_pc_texture_names.py",
        "reason": "texture names with spaces/hyphens: other bytes -> '_' in dictionaries and references alike",
        "sourceFixtureManifestSha256": hashlib.sha256((args.fixture / "manifest.json").read_bytes()).hexdigest(),
        "materialReportSha256": hashlib.sha256(args.material_report.read_bytes()).hexdigest(),
        "resources": records,
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"output": str(args.output), "resources": [(r["member"], len(r["renames"])) for r in records]}))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FixtureError as error:
        raise SystemExit(f"fixture refused: {error}") from None

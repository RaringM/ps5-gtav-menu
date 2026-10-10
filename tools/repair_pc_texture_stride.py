#!/usr/bin/env python3
"""Repair texture strides an exporter wrote for another block size (PC .ytd in an imported fixture).

The Prototipo police car's protopolice.ytd stores generic_leather2_n as BC5 (ATI2, 16-byte blocks)
with the stride of an 8-byte-block format (128 instead of 256 at 256x256). The Legacy reader's mip
spans (stride * height, quartered per mip) then cover half of each mip, and the texture writer
refuses the whole dictionary ("Legacy stride/mip spans differ from block-rounded linear layout").
The full BC5 chain is in the file: its 87,376 bytes fill the 4 KiB-aligned gap before the next
texture's data. This tool writes a new fixture in which such a stride is set to the block-rounded
value, and only when the data proves it:
  - the format's block-rounded linear mip sizes form the quartered chain stride' * height, ...
    (stride' = first mip size / height), so the Legacy reader then reads exactly the linear mips;
  - the repaired chain stays inside the graphics pages and overlaps no other texture's declared
    span, so the bytes it covers belong to this texture (refused otherwise: data would be missing).
Only the u16 stride (texture object +86) changes; the dictionary is re-read and must validate.

Two more exporter quirks of the same gate are repaired here, each only when the data proves it:
  - mip chains the Legacy layout cannot describe past some level: mips below one compression block
    (the GT-R's 4x4/8x8 BC1 colour swatches store 2x2 and 1x1 mips the reader quarters to 2 and 0
    bytes) or a width that is not a power of two (the Skyline's 68x64 leather2_nrm: the quartered
    544-byte second mip is 576 bytes block-rounded, and the third would overlap the next texture).
    When no stride fixes the chain, the mip count is cut to the longest prefix whose quartered
    chain equals the block-rounded chain (at least the first mip), and only when that prefix stays
    inside the graphics pages and overlaps no other texture's data. Only the u8 mip count (+93)
    changes; the kept mips are the source bytes. A neighbour that was refused only because this
    texture's declared span overlapped it (the Skyline's leather2) reads clean afterwards.
  - render-target placeholders: a texture named script_rt_* (the Skyline embeds a blank 1024x512
    BGRA8 script_rt_dials_banshee). The texture writer refuses such names (target-native special
    handling). The entry is removed from the dictionary's hash and pointer tables (counts - 1), so
    the material binding stays an external render-target name, as for the GT-R and the Prototipo,
    which reference script_rt_dials_* without embedding it. Its bytes stay unreferenced.
Every .ytd member of the fixture is checked; other members and the original fixture are untouched,
and the new manifest records each repair (convert_vehicle.py --repair texture-stride).

  repair_pc_texture_stride.py --fixture build/assets/convert-<id>/fixtures/protopolice-import \\
      --material-report <its material report> --output build/assets/convert-<id>/fixtures/<new>
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

from gtavmenu_tools.asset_formats import Limits, decode_resource  # noqa: E402
from gtavmenu_tools.asset_textures import (  # noqa: E402
    LEGACY_FORMATS,
    SCRIPT_TEXTURE_MARKER,
    SYSTEM_BASE,
    inspect_legacy_dictionary,
    linear_mip_sizes,
)
from gtavmenu_tools.vehicle_repair_inputs import (  # noqa: E402
    FixtureError,
    copy_fixture,
    resource_path,
    validate_fixture,
)

STRIDE_FIELD = 86  # u16 in the Legacy texture object: width, height, depth, stride at +80..+87
LEVELS_FIELD = 93  # u8 mip count ("<4HIBB" at +80: format u32 at +88, a byte, then the levels)
ISSUE = "Legacy stride/mip spans differ from block-rounded linear layout"
SPECIAL = "texture name selects target-native special handling; ordinary static conversion is unsupported"
RESOURCE_LIMIT = 256 * 1024 * 1024


def repair_strides(payload: bytearray, header: dict) -> list[dict]:
    """Fix every repairable stride in a decoded Legacy .ytd payload in place; returns the repairs."""
    limits = Limits()
    rows = inspect_legacy_dictionary(bytes(payload), header, limits)["textures"]
    spans = {
        row["name"]: (row["graphicsOffset"], row["graphicsOffset"] + sum(row["legacyMipBytes"]))
        for row in rows
        if "graphicsOffset" in row
    }
    repairs = []
    for row in rows:
        if ISSUE not in row["issues"]:
            continue
        fmt = int(row["formatCode"], 16)
        _, edge, block_bytes = LEGACY_FORMATS[fmt]
        linear = linear_mip_sizes(row["width"], row["height"], row["mipLevels"], edge, block_bytes)
        stride, rest = divmod(linear[0], row["height"])
        chain, length = [], stride * row["height"]
        for _ in range(row["mipLevels"]):
            chain.append(length)
            length //= 4
        try:
            if rest or not 0 < stride < 0x10000 or chain != linear:
                raise SystemExit(f"{row['name']}: no stride reproduces the {row['format']} mip chain {linear}")
            _inside(row, row["graphicsOffset"], row["graphicsOffset"] + sum(linear), header, spans)
        except SystemExit as refused:
            trimmed = _trim(payload, row, linear, header, spans)
            if trimmed is None:
                raise refused from None
            repairs.append(trimmed)
            continue
        struct.pack_into("<H", payload, row["systemOffset"] + STRIDE_FIELD, stride)
        repairs.append(
            {
                "texture": row["name"],
                "format": row["format"],
                "size": [row["width"], row["height"]],
                "mipLevels": row["mipLevels"],
                "oldStride": row["stride"],
                "newStride": stride,
                "mipBytes": linear,
            }
        )
    if repairs:
        fixed = {row["name"]: row for row in inspect_legacy_dictionary(bytes(payload), header, limits)["textures"]}
        for repair in repairs:
            row = fixed[repair["texture"]]
            if SPECIAL in row["issues"]:  # a render-target placeholder (KoRn a45 script_rt_dials_race): dropped next
                continue
            if row["issues"] or not row["linearPayloadValidated"]:
                raise SystemExit(f"{repair['texture']}: still invalid after the stride repair: {row['issues']}")
    return repairs


def _inside(row: dict, start: int, end: int, header: dict, spans: dict) -> None:
    """Refuse a repaired chain [start, end) that leaves the graphics pages or reaches another texture's data."""
    if end > header["graphicsBytes"]:
        raise SystemExit(f"{row['name']}: the {row['format']} chain ends past the graphics pages; data missing")
    for name, (other_start, other_end) in spans.items():
        if name != row["name"] and start < other_end and other_start < end:
            raise SystemExit(f"{row['name']}: the {row['format']} chain overlaps {name}'s data; data missing")


def _trim(payload: bytearray, row: dict, linear: list[int], header: dict, spans: dict) -> dict | None:
    """Cut the mip count to the longest prefix the stored stride describes exactly (None: no such prefix)."""
    keep = 0
    while keep < row["mipLevels"] and row["legacyMipBytes"][keep] == linear[keep]:
        keep += 1
    if keep == 0:
        return None
    try:
        _inside(row, row["graphicsOffset"], row["graphicsOffset"] + sum(linear[:keep]), header, spans)
    except SystemExit:
        return None
    payload[row["systemOffset"] + LEVELS_FIELD] = keep
    spans[row["name"]] = (row["graphicsOffset"], row["graphicsOffset"] + sum(linear[:keep]))
    return {
        "texture": row["name"],
        "format": row["format"],
        "size": [row["width"], row["height"]],
        "mipLevels": row["mipLevels"],
        "keptMipLevels": keep,
        "stride": row["stride"],
        "mipBytes": linear[:keep],
        "droppedMipBytes": linear[keep:],
    }


def drop_render_targets(payload: bytearray, header: dict) -> list[dict]:
    """Remove script_rt_* placeholder textures from the dictionary tables in place; returns the drops."""
    rows = inspect_legacy_dictionary(bytes(payload), header, Limits())["textures"]
    special = {int(row["nameHash"], 16): row for row in rows if SPECIAL in row["issues"]}
    drops = []
    if not special:
        return drops
    hp, hc, _, _ = struct.unpack_from("<QHHI", payload, 32)
    tp = struct.unpack_from("<Q", payload, 48)[0]
    hp, tp = hp - SYSTEM_BASE, tp - SYSTEM_BASE  # table pointers -> payload offsets
    entries = [
        (struct.unpack_from("<I", payload, hp + 4 * i)[0], struct.unpack_from("<Q", payload, tp + 8 * i)[0])
        for i in range(hc)
    ]
    kept = [entry for entry in entries if entry[0] not in special]
    if len(kept) != hc - len(special) or not kept:
        raise SystemExit("render-target placeholder removal would leave the dictionary inconsistent or empty")
    for index in range(hc):
        name_hash, pointer = kept[index] if index < len(kept) else (0, 0)
        struct.pack_into("<I", payload, hp + 4 * index, name_hash)
        struct.pack_into("<Q", payload, tp + 8 * index, pointer)
    struct.pack_into("<H", payload, 40, len(kept))
    struct.pack_into("<H", payload, 56, len(kept))
    for _, row in sorted(special.items()):
        if SCRIPT_TEXTURE_MARKER not in row["name"].lower():
            raise SystemExit(f"{row['name']}: special handling without the {SCRIPT_TEXTURE_MARKER} marker")
        drops.append(
            {
                "texture": row["name"],
                "format": row.get("format", row["formatCode"]),
                "size": [row["width"], row["height"]],
                "reason": "render-target placeholder: the material keeps the external render-target name",
            }
        )
    return drops


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--material-report", type=Path, required=True, help="recorded (convert_vehicle repair chain)")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.output.exists():
        raise SystemExit(f"refusing to overwrite {args.output}")
    manifest = validate_fixture(args.fixture)
    planned = []
    for row in manifest["resources"]:
        if not row["member"].endswith(".ytd"):
            continue
        path = resource_path(args.fixture, row["member"])
        blob = path.read_bytes()
        header, payload = decode_resource(blob, RESOURCE_LIMIT)
        payload = bytearray(payload)
        fixed = repair_strides(payload, header)
        dropped = drop_render_targets(payload, header)
        if fixed or dropped:
            planned.append((row, blob, payload, fixed, dropped))
    if not planned:
        raise SystemExit("no texture stride to repair")
    copy_fixture(args.fixture, args.output, manifest)
    repairs = []
    for row, blob, payload, fixed, dropped in planned:
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
        new_blob = blob[:16] + compressor.compress(bytes(payload)) + compressor.flush()
        resource_path(args.output, row["member"]).write_bytes(new_blob)
        row["bytes"] = row["storedBytes"] = len(new_blob)
        row["sha256"] = hashlib.sha256(new_blob).hexdigest()
        repairs.append({"member": row["member"], "textures": fixed} | ({"dropped": dropped} if dropped else {}))
    manifest["textureStrideRepairs"] = {
        "tool": "tools/repair_pc_texture_stride.py",
        "reason": "stride stored for another block size; set to the block-rounded stride of the texture's format",
        "sourceFixtureManifestSha256": hashlib.sha256((args.fixture / "manifest.json").read_bytes()).hexdigest(),
        "materialReportSha256": hashlib.sha256(args.material_report.read_bytes()).hexdigest(),
        "resources": repairs,
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"output": str(args.output), "repairs": repairs}))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FixtureError as error:
        raise SystemExit(f"fixture refused: {error}") from None

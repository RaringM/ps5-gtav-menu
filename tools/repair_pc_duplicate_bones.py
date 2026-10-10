#!/usr/bin/env python3
"""Rename duplicate lettered bones in an imported PC fixture to the next free letter.

The Prototipo police car (protopolice.yft and _hi) has two bones named misc_a (indices 57 and 60,
both children of chassis, both tag 58614, separate name strings). The skeleton reader refuses
duplicate bone names or tags ("skeleton duplicate bone tags or names are ambiguous"). On PC a tag
lookup walks its tag bucket and takes the first node: bucket 10 holds bone 60 before bone 57, so
every lookup of misc_a (physics children, mods, scripts) reached bone 60 and bone 57 was reachable
only by index (drawing). This tool keeps that: the bone the tag chain reaches first keeps its name
and tag; each other bone of the group is renamed in place to the same name with the next free last
letter (misc_a -> misc_b, same length, its own name string) and given that name's tag, and its tag
node moves to the head of the new tag's bucket. Index-based bindings (geometry palettes, parents,
transforms) do not change. Refused unless:
  - the name is a generic lettered slot (misc_a .. misc_z) and a free letter exists; a semantic name
    (door_dside_f, neon_l) is never renamed,
  - the bone's name string is not shared with another bone,
  - the skeleton's tags follow ElfHash(UPPER(name)) % 0xFE8F + 0x170 for the duplicated name and the
    new tag is unused (the renamed bone gets the tag the game derives from its new name).
The repaired fixture must pass the skeleton reader; the original stays intact and the manifest
records each rename.

  repair_pc_duplicate_bones.py --fixture build/assets/convert-<id>/fixtures/protopolice-import \\
      --material-report <its material report> --output <new fixture>
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

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import inspect_pc_vehicle_pose as mesh_runner  # noqa: E402
from gtavmenu_tools import vehicle_contracts  # noqa: E402
from gtavmenu_tools.asset_formats import Limits, decode_resource  # noqa: E402
from gtavmenu_tools.vehicle_repair_inputs import FixtureError, copy_fixture, validate_fixture  # noqa: E402
from repair_pc_seat_mirror import SYSTEM_BASE, pc_walk_args  # noqa: E402

LETTERED = re.compile(r"(misc_)([a-z])")  # generic slots misc_a..misc_z; never semantic names (neon_l, door_dside_f)
RESOURCE_LIMIT = 256 * 1024 * 1024


def elf_hash(text: str) -> int:
    value = 0
    for byte in text.encode("ascii"):
        value = ((value << 4) + byte) & 0xFFFFFFFF
        high = value & 0xF0000000
        if high:
            value ^= high >> 24
        value &= ~high & 0xFFFFFFFF
    return value


def bone_tag(name: str) -> int:
    """The tag the game derives from a bone name (matches every non-root bone of the PC vehicles seen)."""
    return elf_hash(name.upper()) % 0xFE8F + 0x170


def read_fields(raw: bytes | bytearray, at: int, layout: dict) -> dict:
    out = {}
    for name, spec in layout["fields"].items():
        if spec["type"] in ("Single", "Vector3", "Vector4"):
            continue
        start, size = at + spec["offset"], spec["bytes"]
        if start < 0 or start + size > len(raw):
            raise SystemExit("skeleton field exceeds resource bounds")
        chunk = raw[start : start + size]
        out[name] = int.from_bytes(chunk, "little", signed=spec["type"] == "Int16")
    return out


def write_field(raw: bytearray, at: int, layout: dict, name: str, value: int) -> None:
    spec = layout["fields"][name]
    raw[at + spec["offset"] : at + spec["offset"] + spec["bytes"]] = value.to_bytes(spec["bytes"], "little")


def repair_skeleton(raw: bytearray, skeleton_at: int, layouts: dict) -> list[dict]:
    """Rename every duplicate bone except the one its tag chain reaches first; returns the renames."""
    skel = read_fields(raw, skeleton_at, layouts["Skeleton"])
    count, bone_bytes = skel["BonesCount"], layouts["Bone"]["bytes"]
    if not 0 < count <= Limits().max_entries:
        raise SystemExit("skeleton bone count is empty or exceeds limit")
    bones_at = skel["BonesPointer"] - SYSTEM_BASE
    bones = []
    for index in range(count):
        at = bones_at + index * bone_bytes
        bone = read_fields(raw, at, layouts["Bone"]) | {"at": at}
        name_at = bone["NamePointer"] - SYSTEM_BASE
        end = raw.find(0, max(0, name_at), name_at + 256)
        if name_at < 0 or end < name_at or end < 0:
            raise SystemExit("skeleton bone name is outside bounded resource bytes")
        try:
            bone["name"] = bytes(raw[name_at:end]).decode("ascii")
        except UnicodeError:
            raise SystemExit("skeleton bone name is not ASCII") from None
        bones.append(bone)
    capacity, node_layout = skel["BoneTagsCapacity"], layouts["SkeletonBoneTag"]
    if capacity == 0 and count == 1 and skel["BoneTagsPointer"] == 0 and skel.get("BoneTagsCount", 0) == 0:
        return []  # the ordinary skeleton reader admits a single bone without a tag table
    buckets_at = skel["BoneTagsPointer"] - SYSTEM_BASE
    if not 0 < capacity <= Limits().max_entries or buckets_at < 0 or buckets_at + capacity * 8 > len(raw):
        raise SystemExit("skeleton tag table exceeds resource bounds")
    chain_order, node_of, seen_nodes = [], {}, set()
    for bucket in range(capacity):
        pointer = struct.unpack_from("<Q", raw, buckets_at + bucket * 8)[0]
        while pointer:
            if pointer in seen_nodes or len(seen_nodes) >= count:
                raise SystemExit("skeleton tag chain cycles, shares nodes or exceeds bone count")
            seen_nodes.add(pointer)
            node = read_fields(raw, pointer - SYSTEM_BASE, node_layout)
            index = node["BoneIndex"]
            if (
                index >= count
                or index in node_of
                or node["BoneTag"] != bones[index]["Tag"]
                or node["BoneTag"] % capacity != bucket
            ):
                raise SystemExit("skeleton tag chain disagrees with bone identity or bucket")
            chain_order.append(index)
            node_of[node["BoneIndex"]] = (bucket, pointer)
            pointer = node["NextPointer"]
    if len(node_of) != count:
        raise SystemExit("skeleton tag chains do not uniquely cover bones")
    groups: dict[str, list[int]] = {}
    for index, bone in enumerate(bones):
        groups.setdefault(bone["name"], []).append(index)
    tags = {bone["Tag"] for bone in bones}
    names = {bone["name"] for bone in bones}
    renames = []
    for name, members in groups.items():
        if len(members) < 2:
            continue
        family = LETTERED.fullmatch(name)
        if family is None:
            raise SystemExit(f"duplicate bone {name!r} is not a generic lettered slot (misc_a .. misc_z)")
        if any(bones[i]["Tag"] != bone_tag(name) for i in members):
            raise SystemExit(
                f"duplicate bone {name!r}: stored tags do not follow the name hash; cannot derive new tags"
            )
        pointers = [bone["NamePointer"] for bone in bones]
        keep = min(members, key=chain_order.index)  # what a PC tag lookup reaches
        for index in sorted(set(members) - {keep}):
            if pointers.count(bones[index]["NamePointer"]) != 1:
                raise SystemExit(f"bone {index} ({name}) shares its name string with another bone")
            free = [
                family[1] + letter
                for letter in "abcdefghijklmnopqrstuvwxyz"
                if family[1] + letter not in names and bone_tag(family[1] + letter) not in tags
            ]
            if not free:
                raise SystemExit(f"no free name for duplicate bone {index} ({name})")
            new, tag = free[0], bone_tag(free[0])
            names.add(new)
            tags.add(tag)
            name_at = bones[index]["NamePointer"] - SYSTEM_BASE
            raw[name_at : name_at + len(new)] = new.encode("ascii")
            write_field(raw, bones[index]["at"], layouts["Bone"], "Tag", tag)
            # unlink the bone's tag node, retag it, push it on the new bucket's chain
            bucket, pointer = node_of[index]
            node_at = pointer - SYSTEM_BASE
            following = read_fields(raw, node_at, node_layout)["NextPointer"]
            link_at = buckets_at + bucket * 8
            while struct.unpack_from("<Q", raw, link_at)[0] != pointer:
                previous = struct.unpack_from("<Q", raw, link_at)[0] - SYSTEM_BASE
                link_at = previous + node_layout["fields"]["NextPointer"]["offset"]
            struct.pack_into("<Q", raw, link_at, following)
            target = buckets_at + (tag % capacity) * 8
            write_field(raw, node_at, node_layout, "BoneTag", tag)
            write_field(raw, node_at, node_layout, "NextPointer", struct.unpack_from("<Q", raw, target)[0])
            struct.pack_into("<Q", raw, target, pointer)
            renames.append(
                {"boneIndex": index, "oldName": name, "newName": new, "oldTag": bone_tag(name), "newTag": tag}
            )
            renames[-1]["keptBoneIndex"] = keep
    return renames


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--material-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.output.exists() or args.output.is_symlink():
        raise SystemExit(f"refusing to overwrite {args.output}")
    manifest = validate_fixture(args.fixture)
    mesh = mesh_runner.inspect(
        pc_walk_args(args.fixture, args.material_report), stage="mesh"
    )  # PC-side replay, no native input
    layouts = vehicle_contracts.load()["pc"]["skeleton"]["layouts"]
    geometry = mesh["freshGeometryBindings"]
    pointer_spec = geometry["pcLegacyGeometryContract"]["drawable"]["fields"]["SkeletonPointer"]
    rows = {row["member"]: row for row in manifest["resources"]}
    planned = []
    for geom in geometry["resources"]:
        member = geom["member"]
        path = args.fixture.joinpath("resources", *member.replace("!/", "/").split("/"))
        blob = path.read_bytes()
        _, payload = decode_resource(blob, RESOURCE_LIMIT)
        raw = bytearray(payload)
        (drawable,) = [r for r in geom["claimedSystemSpans"] if r["kind"] == "drawable"]
        at = drawable["start"] + pointer_spec["offset"]
        pointer = int.from_bytes(raw[at : at + pointer_spec["bytes"]], "little")
        if not pointer:
            continue
        renames = repair_skeleton(raw, pointer - SYSTEM_BASE, layouts)
        if renames:
            planned.append((member, blob, raw, renames))
    if not planned:
        raise SystemExit("no duplicate bones to repair")
    copy_fixture(args.fixture, args.output, manifest)
    repairs = []
    for member, blob, raw, renames in planned:
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
        new_blob = blob[:16] + compressor.compress(bytes(raw)) + compressor.flush()
        args.output.joinpath("resources", *member.replace("!/", "/").split("/")).write_bytes(new_blob)
        row = rows[member]
        row["bytes"] = row["storedBytes"] = len(new_blob)
        row["sha256"] = hashlib.sha256(new_blob).hexdigest()
        repairs.append({"member": member, "bones": renames})
    manifest["duplicateBoneRepairs"] = {
        "tool": "tools/repair_pc_duplicate_bones.py",
        "reason": "duplicate lettered bone names/tags; the bone a PC tag lookup reaches keeps the name",
        "sourceFixtureManifestSha256": hashlib.sha256((args.fixture / "manifest.json").read_bytes()).hexdigest(),
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

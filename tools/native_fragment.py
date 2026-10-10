"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import struct
from itertools import pairwise

import native_collision as collision
import native_drawables as drawable
import native_physics as physics
from gtavmenu_tools.asset_formats import AssetError
from native_resource_builder import ObjectArena


def read_glass(system: bytes, base: int, pointer: int, contract: dict) -> dict | None:
    if not pointer:
        return None
    at = pointer - base
    header, raw = collision.typed(system, at, contract["glassHeader"])
    count, length = header["ItemCount"], header["TotalLength"]
    if (
        at < 0
        or at % 16
        or not 0 < count <= 255
        or at + length > len(system)
        or header["Unknown_0h"] != contract["glassSignature"]
        or header["Unknown_4h"] != contract["window"]["bytes"]
    ):
        raise AssetError("fragment glass header/count/extent is invalid")
    padded_count = count + (count & 1)
    table_at = at + contract["glassHeader"]["bytes"]
    table_end = table_at + padded_count * struct.calcsize("<II")
    if table_end + 4 > at + length:
        raise AssetError("fragment glass table exceeds block")
    offsets = [list(row) for row in struct.iter_unpack("<II", system[table_at:table_end])]
    trailer = struct.unpack_from("<I", system, table_end)[0]
    if trailer or (padded_count > count and offsets[-1] != [0, 0]):
        raise AssetError("fragment glass table extension/padding is populated")
    cursor, windows, identifiers = table_end + 4, [], set()
    end = at + length
    for item_id, relative in offsets[:count]:
        if cursor != at + relative or item_id in identifiers or cursor + contract["window"]["bytes"] > end:
            raise AssetError("fragment glass window offset/identity differs")
        identifiers.add(item_id)
        values, fields = collision.typed(system, cursor, contract["window"])
        if values["UnkUint1"] != contract["windowSignature"] or values["ItemID"] != item_id:
            raise AssetError("fragment glass window signature/identity differs")
        cursor += contract["window"]["bytes"]
        data_start = cursor
        data_end = data_start + values["ItemDataByteLength"]
        rows, row_offsets, data_padding = [], [], 0
        if data_end > end:
            raise AssetError("fragment glass shatter map exceeds block")
        if values["ItemDataByteLength"]:
            row_count = values["ItemDataCount"]
            if not 0 < row_count <= 4096 or cursor + row_count * 2 > data_end:
                raise AssetError("fragment glass shatter row count is invalid")
            row_offsets = list(struct.unpack_from("<" + str(row_count) + "H", system, cursor))
            cursor += row_count * 2
            row_start = cursor

            def take(n, limit=data_end):
                nonlocal cursor
                if n < 0 or cursor + n > limit:
                    raise AssetError("fragment glass shatter row exceeds declared data")
                value = system[cursor : cursor + n]
                cursor += n
                return value

            for offset in row_offsets:
                if offset != cursor - row_start:
                    raise AssetError("fragment glass shatter row offset differs")
                first, last = take(2)
                row = {"start1": first, "end1": last, "data1": "", "start2": None, "end2": None, "data2": ""}
                n = last - first + 1
                if n > 0:
                    if last >= values["ShatterMapWidth"]:
                        raise AssetError("fragment glass shatter interval exceeds width")
                    row["data1"] = take(n).hex()
                    row["start2"] = take(1)[0]
                    if row["start2"] != 255:
                        row["end2"] = take(1)[0]
                        n2 = row["end2"] - row["start2"] + 1
                        if n2 <= 0 or row["end2"] >= values["ShatterMapWidth"]:
                            raise AssetError("fragment glass second shatter interval invalid")
                        row["data2"] = take(n2).hex()
                rows.append(row)
            # Razor counts terminal 16-byte window alignment in this length;
            # the retail references exclude it. Account for both explicitly.
            data_padding = data_end - cursor
            if data_padding and (data_padding != (-(cursor - at)) % 16 or any(system[cursor:data_end])):
                raise AssetError("fragment glass shatter data has unaccounted bytes")
            cursor = data_end
        elif values["ItemDataCount"]:
            raise AssetError("fragment glass row count without data is unsupported")
        padding = (16 - ((cursor - at) % 16)) % 16
        if cursor + padding > end or any(system[cursor : cursor + padding]):
            raise AssetError("fragment glass padding is invalid")
        cursor += padding
        windows.append(
            {
                "header": values,
                "fieldBytes": fields,
                "rowOffsets": row_offsets,
                "rows": rows,
                "dataPadding": data_padding,
                "padding": padding,
            }
        )
    if cursor != end:
        raise AssetError("fragment glass total length differs")
    return {"header": header, "fieldBytes": raw, "offsets": offsets, "windows": windows}


def encode_glass(glass: dict, contract: dict) -> bytes:
    out = bytearray(drawable.source_fields(glass["fieldBytes"], contract["glassHeader"], ()))
    for item, offset in glass["offsets"]:
        out.extend(struct.pack("<II", item, offset))
    out.extend(struct.pack("<I", 0))
    for window in glass["windows"]:
        out.extend(drawable.source_fields(window["fieldBytes"], contract["window"], ()))
        for offset in window["rowOffsets"]:
            out.extend(struct.pack("<H", offset))
        for row in window["rows"]:
            out.extend(bytes([row["start1"], row["end1"]]))
            if row["start2"] is not None:
                out.extend(bytes.fromhex(row["data1"]))
                out.append(row["start2"])
                if row["start2"] != 255:
                    out.append(row["end2"])
                    out.extend(bytes.fromhex(row["data2"]))
        out.extend(bytes(window["dataPadding"] + window["padding"]))
    if len(out) != glass["header"]["TotalLength"]:
        raise AssetError("encoded fragment glass extent differs")
    return bytes(out)


def read(system: bytes, base: int, root: int, contract: dict) -> dict:
    if root < 0 or root % 16:
        raise AssetError("fragment root alignment is invalid")
    header, raw = collision.typed(system, root, contract["root"])
    allowed = (
        "DrawablePointer",
        "NamePointer",
        "BoneTransformsPointer",
        "PhysicsLODGroupPointer",
        "VehicleGlassWindowsPointer",
        "FilePagesInfoPointer",
    )
    for name, value in header.items():
        if (
            (name.endswith("Pointer") and name not in allowed)
            or (name.startswith("Unknown_") and contract["root"]["fields"][name]["type"] == "UInt64")
            or name.startswith(("Cloths.", "LightAttributes."))
            or name in ("DrawableArrayCount", "GlassWindowsCount")
        ) and value:
            raise AssetError("fragment root optional extension is populated: " + name)
    name_at = header["NamePointer"] - base
    name_end = system.find(b"\0", name_at, name_at + 1024) if 0 <= name_at < len(system) else -1
    if name_end < 0:
        raise AssetError("fragment root name is absent or unterminated")
    at = header["BoneTransformsPointer"] - base
    if at < 0 or at % 16:
        raise AssetError("fragment bone transform pointer is invalid")
    bone_header, bone_fields = collision.typed(system, at, contract["boneHeader"])
    count = bone_header["ItemCount1"]
    if (
        not count
        or count != bone_header["ItemCount2"]
        or any(v for n, v in bone_header.items() if n.startswith("Unknown_") and n != "Unknown_12h")
    ):
        raise AssetError("fragment bone transform count/extension differs")
    matrices = [
        collision.typed(
            system, at + contract["boneHeader"]["bytes"] + i * contract["boneMatrix"]["bytes"], contract["boneMatrix"]
        )[1]
        for i in range(count)
    ]
    glass = read_glass(system, base, header["VehicleGlassWindowsPointer"], contract)
    # Populated root members must be disjoint; external graph members are checked
    # independently by their typed readers and explicit composition fixups.
    ranges = [
        (root, root + contract["root"]["bytes"]),
        (name_at, name_end + 1),
        (at, at + contract["boneHeader"]["bytes"] + count * contract["boneMatrix"]["bytes"]),
    ]
    if glass:
        glass_at = header["VehicleGlassWindowsPointer"] - base
        ranges.append((glass_at, glass_at + glass["header"]["TotalLength"]))
    ordered = sorted(ranges)
    if any(a[1] > b[0] for a, b in pairwise(ordered)):
        raise AssetError("fragment root members overlap")
    return {
        "header": header,
        "fieldBytes": raw,
        "nameHex": system[name_at : name_end + 1].hex(),
        "boneHeader": bone_fields,
        "matrices": matrices,
        "glass": glass,
    }


def semantics(decoded: dict) -> dict:
    return {
        "fields": {
            k: v
            for k, v in decoded["fieldBytes"].items()
            if not k.endswith("Pointer") and k not in ("FileVFT", "FileUnknown")
        },
        **{k: decoded[k] for k in ("nameHex", "boneHeader", "matrices", "glass")},
    }


def build(system: bytes, source_base: int, blob: bytes, graph: dict, contract: dict) -> tuple[bytes, dict]:
    decoded = read(system, source_base, 0, contract)
    arena = ObjectArena()
    pointer_fields = tuple(n for n in contract["root"]["fields"] if n.endswith("Pointer"))
    root = arena.add(
        "fragment.root",
        drawable.source_fields(decoded["fieldBytes"], contract["root"], (*pointer_fields, "FileVFT", "FileUnknown")),
    )
    delta = arena.include("physics-graph", blob, graph)
    moved = drawable.shifted(graph, delta)
    moved["collision"] = collision.shifted(graph["collision"], delta)
    moved["physics"] = physics.shifted(graph["physics"], delta)
    h = decoded["header"]
    main = one([r for r in moved["drawables"] if r["sourcePointer"] == h["DrawablePointer"]], "fragment main drawable")
    physics_root = (
        one(
            [r for r in moved["physics"]["nodes"] if r["sourcePointer"] == h["PhysicsLODGroupPointer"]],
            "fragment physics root",
        )
        if h["PhysicsLODGroupPointer"]
        else {"systemOffset": None}  # mod-kit part fragment
    )
    bones = drawable.source_fields(decoded["boneHeader"], contract["boneHeader"], ())
    bones += b"".join(drawable.source_fields(row, contract["boneMatrix"], ()) for row in decoded["matrices"])
    targets = {
        "DrawablePointer": main["systemOffset"],
        "PhysicsLODGroupPointer": physics_root["systemOffset"],
        "NamePointer": arena.add("fragment.name", bytes.fromhex(decoded["nameHex"]), alignment=1),
        "BoneTransformsPointer": arena.add("fragment.bone-transforms", bones),
        "VehicleGlassWindowsPointer": None,
    }
    if decoded["glass"]:
        source_physics = graph["physics"]["sourceSemantics"]
        lod_index = source_physics["nodes"][source_physics["rootIndex"]]["links"]["PhysicsLOD1Pointer"]
        if lod_index is None:
            raise AssetError("fragment vehicle glass requires the first physics LOD")
        child_count = int.from_bytes(
            bytes.fromhex(source_physics["nodes"][lod_index]["fields"]["ChildrenCount"]), "little"
        )
        if any(w["header"]["ItemID"] >= child_count for w in decoded["glass"]["windows"]):
            raise AssetError("fragment vehicle glass child index is out of range")
        targets["VehicleGlassWindowsPointer"] = arena.add(
            "fragment.vehicle-glass", encode_glass(decoded["glass"], contract)
        )
    for name in pointer_fields:
        arena.pointer(root + contract["root"]["fields"][name]["offset"], targets.get(name))
    result, placement = arena.render(graph["sourceBase"])
    moved.update(placement)
    moved["fragment"] = {
        "sourceBase": graph["sourceBase"],
        "rootOffset": root,
        "sourceSemantics": semantics(decoded),
        "targets": targets,
        "pageInfoPointerOffset": root + contract["root"]["fields"]["FilePagesInfoPointer"]["offset"],
    }
    moved.update(
        fragmentRootAvailable=True,
        completeNativeResource=False,
        resourcePagesResolved=False,
        vehicleGlassConsumerSemanticsQualified=False,
    )
    verify(result, moved["fragment"], contract)
    return result, moved


def verify(blob: bytes, placement: dict, contract: dict) -> dict:
    base = placement["sourceBase"]
    decoded = read(blob, base, placement["rootOffset"], contract)
    if semantics(decoded) != placement["sourceSemantics"]:
        raise AssetError("fragment source fields, transforms or glass changed")
    page_info = placement["targets"].get("FilePagesInfoPointer")
    if any(decoded["header"][n] for n in ("FileVFT", "FileUnknown")) or decoded["header"]["FilePagesInfoPointer"] != (
        0 if page_info is None else base + page_info
    ):
        raise AssetError("fragment serialized class/page info profile differs")
    for name, target in placement["targets"].items():
        if decoded["header"][name] != (0 if target is None else base + target):
            raise AssetError("fragment composed graph link differs")
    glass = decoded["glass"]
    return {
        "boneMatrices": len(decoded["matrices"]),
        "windows": len(glass["windows"]) if glass else 0,
        "shatterRows": sum(len(w["rows"]) for w in glass["windows"]) if glass else 0,
        "sourceFieldsAndArraysPreserved": True,
        "nativeCodeExecuted": False,
    }


def shifted(placement: dict, delta: int) -> dict:
    return placement | {
        "rootOffset": placement["rootOffset"] + delta,
        "pageInfoPointerOffset": placement["pageInfoPointerOffset"] + delta,
        "targets": {n: None if off is None else off + delta for n, off in placement["targets"].items()},
    }


def one(rows, label):
    if len(rows) != 1:
        raise AssetError(f"vehicle {label} is missing or ambiguous")
    return rows[0]

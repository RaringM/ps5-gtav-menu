"""Gen9 (PS5) drawable models: geometry pointer arrays, shader mappings and bounds, written and read back.

The build/read half of native_models.py, split from the derivation of its contract: the model layout
arrives as data (`contract`: the `model` entry of the drawable contracts, frozen in data/drawable_contracts
and loaded by gtavmenu_tools.drawable_contracts). Nothing here reads a reference source, a game
executable or a third-party package.
"""

from __future__ import annotations

import math
import struct

import gen9_mesh as mesh
from gtavmenu_tools.asset_formats import AssetError
from native_resource_builder import ObjectArena


def read(system: bytes, base: int, root: int, contract: dict, max_geometries: int = 16384) -> dict:
    header = mesh.get(system, root, contract["layout"])
    count = header["GeometriesCount1"]
    if not 0 < count <= max_geometries or header["GeometriesCount2"] != count or header["GeometriesCount3"] != count:
        raise AssetError("model geometry counts are empty, unequal or excessive")
    bounds_count = count + (1 if count > 1 else 0)
    ranges = [(root, root + contract["layout"]["bytes"])]
    arrays = {}
    for name, length, stride in (
        ("GeometriesPointer", count, contract["geometryPointerBytes"]),
        ("ShaderMappingPointer", count, contract["mappingElementBytes"]),
        ("BoundsPointer", bounds_count, contract["boundsBytes"]),
    ):
        at, size = header[name] - base, length * stride
        if (
            at < 0
            or at % min(stride, 16)
            or at + size > len(system)
            or any(a < at + size and at < b for a, b in ranges)
        ):
            raise AssetError("model array is out of bounds, unaligned or overlapping")
        ranges.append((at, at + size))
        arrays[name] = system[at : at + size]
    geometry_pointers = list(struct.unpack("<" + "Q" * count, arrays["GeometriesPointer"]))
    if any(p < base or p >= base + len(system) or p % 16 for p in geometry_pointers):
        raise AssetError("model geometry reference exceeds system storage")
    boxes = []
    for offset in range(0, len(arrays["BoundsPointer"]), contract["boundsBytes"]):
        values = struct.unpack_from("<8f", arrays["BoundsPointer"], offset)
        if any(
            not math.isfinite(values[i]) or not math.isfinite(values[i + 4]) or values[i] > values[i + 4]
            for i in range(3)
        ):
            raise AssetError("model bounds have non-finite or inverted spatial axes")
        boxes.append({"minimum": list(values[:3]), "maximum": list(values[4:7])})
    return {
        "header": header,
        "geometryPointers": geometry_pointers,
        "shaderMapping": list(struct.unpack("<" + "H" * count, arrays["ShaderMappingPointer"])),
        "bounds": boxes,
        "boundsHex": arrays["BoundsPointer"].hex(),
        "boundsSha256": mesh.digest(arrays["BoundsPointer"]),
    }


def semantics(value: dict) -> dict:
    pointer_fields = {"VFT", "Unknown_4h", "GeometriesPointer", "ShaderMappingPointer", "BoundsPointer"}
    return {
        "fields": {k: v for k, v in value["header"].items() if k not in pointer_fields},
        "shaderMapping": value["shaderMapping"],
        "boundsSha256": value["boundsSha256"],
    }


def build(
    payload: bytes, groups: list, mesh_blob: bytes, mesh_placement: dict, contract: dict, source_base: int
) -> tuple[bytes, dict]:
    arena = ObjectArena()
    displacement = arena.include("meshes", mesh_blob, mesh_placement)
    geometry_rows = [row | {"systemOffset": row["systemOffset"] + displacement} for row in mesh_placement["geometries"]]
    models, consumed = [], set()
    for group in groups:
        for lod in group["geometry"]["lods"]:
            for model in lod["models"]:
                original = read(payload, source_base, model["systemOffset"], contract)
                expected_geometries = [source_base + g["systemOffset"] for g in model["geometries"]]
                shaders = [g["shaderIndex"] for g in model["geometries"]]
                if original["geometryPointers"] != expected_geometries or original["shaderMapping"] != shaders:
                    raise AssetError("model reference arrays differ from validated source mesh bindings")
                indices = [
                    i
                    for i, g in enumerate(geometry_rows)
                    if g["owner"] == group["owner"] and g["lod"] == lod["lod"] and g["modelIndex"] == model["index"]
                ]
                if len(indices) != len(expected_geometries) or any(i in consumed for i in indices):
                    raise AssetError("model composition has missing or multiply-owned geometries")
                if [geometry_rows[i]["sourceGeometryOffset"] for i in indices] != [
                    g["systemOffset"] for g in model["geometries"]
                ]:
                    raise AssetError("model composition geometry order changed")
                consumed.update(indices)
                tag = f"model-{len(models)}"
                header = bytearray(contract["layout"]["bytes"])
                for name, value in semantics(original)["fields"].items():
                    mesh.put(header, contract["layout"], name, value)
                root = arena.add(tag, header)
                geom_array = arena.add(tag + ".geometries", bytes(len(indices) * contract["geometryPointerBytes"]))
                mapping = arena.add(tag + ".shaders", struct.pack("<" + "H" * len(shaders), *shaders))
                bounds = arena.add(tag + ".bounds", bytes.fromhex(original["boundsHex"]))
                for name, target in (
                    ("GeometriesPointer", geom_array),
                    ("ShaderMappingPointer", mapping),
                    ("BoundsPointer", bounds),
                ):
                    arena.pointer(root + contract["layout"]["fields"][name]["offset"], target)
                for index, geometry_index in enumerate(indices):
                    arena.pointer(
                        geom_array + index * contract["geometryPointerBytes"],
                        geometry_rows[geometry_index]["systemOffset"],
                    )
                models.append(
                    {
                        "systemOffset": root,
                        "owner": group["owner"],
                        "lod": lod["lod"],
                        "modelIndex": model["index"],
                        "geometryIndices": indices,
                        "sourceModelOffset": model["systemOffset"],
                        "semantics": semantics(original),
                    }
                )
    if consumed != set(range(len(geometry_rows))):
        raise AssetError("model composition leaves unowned geometry")
    blob, placement = arena.render(mesh_placement["sourceBase"])
    placement.update(models=models, geometries=geometry_rows)
    verify(blob, placement, contract)
    return blob, placement


def verify(blob: bytes, placement: dict, contract: dict) -> dict:
    count = 0
    for model in placement["models"]:
        restored = read(blob, placement["sourceBase"], model["systemOffset"], contract)
        targets = [
            placement["sourceBase"] + placement["geometries"][i]["systemOffset"] for i in model["geometryIndices"]
        ]
        if semantics(restored) != model["semantics"] or restored["geometryPointers"] != targets:
            raise AssetError("native model readback differs from source fields, bounds, shaders or geometry order")
        if restored["header"]["VFT"] or restored["header"]["Unknown_4h"]:
            raise AssetError("native model serialized runtime class was not cleared")
        count += len(restored["bounds"])
    return {
        "modelCount": len(placement["models"]),
        "boundsCount": count,
        "geometryCount": len(placement["geometries"]),
        "sourceFieldsAndBoundsPreserved": True,
        "nativeCodeExecuted": False,
    }

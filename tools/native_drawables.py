"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import json
import math
import struct

import gen9_mesh as mesh
import pc_drawable_geometry as geometry
from gtavmenu_tools.asset_formats import AssetError
from native_resource_builder import ObjectArena

FORMATS = {
    "UInt64": "Q",
    "UInt32": "I",
    "UInt16": "H",
    "Single": "f",
    "Vector3": "3f",
    "Matrix4F_s": "3fI3fI3fI3fI",
}


POINTERS = (
    "FilePagesInfoPointer",
    "ShaderGroupPointer",
    "SkeletonPointer",
    "JointsPointer",
    "DrawableModelsHighPointer",
    "DrawableModelsMediumPointer",
    "DrawableModelsLowPointer",
    "DrawableModelsVeryLowPointer",
    "DrawableModelsPointer",
    "Unknown_0A8h",
    "BoundPointer",
    "FragMatricesIndsPointer",
    "FragMatricesPointer",
    "Unknown_118h",
    "NamePointer",
    "Unknown_138h",
)


ZERO_EXTENSION = (
    "FilePagesInfoPointer",
    "Unknown_0A8h",
    "FragMatricesIndsPointer",
    "FragMatricesIndsCount",
    "FragMatricesCapacity",
    "FragMatricesPointer",
    "FragMatricesCount",
    "Unknown_104h",
    "Unknown_114h",
    "Unknown_118h",
    "Unknown_120h",
    "Unknown_128h",
    "Unknown_138h",
    "Unknown_140h",
    "Unknown_148h",
)


JOINT_POINTERS = ("RotationLimitsPointer", "TranslationLimitsPointer", "Unknown_20h", "Unknown_28h")


def typed(data: bytes, at: int, layout: dict) -> tuple[dict, dict]:
    if at < 0 or at + layout["bytes"] > len(data):
        raise AssetError("drawable typed record exceeds system storage")
    values, raw = {}, {}
    for name, field in layout["fields"].items():
        start = at + field["offset"]
        raw[name] = data[start : start + field["bytes"]].hex()
        v = struct.unpack_from("<" + FORMATS[field["type"]], data, start)
        if any(isinstance(x, float) and not math.isfinite(x) for x in v):
            raise AssetError("drawable typed record has non-finite spatial values")
        values[name] = v[0] if len(v) == 1 else list(v)
    return values, raw


def source_fields(data: dict, layout: dict, exclude: tuple) -> bytes:
    output = bytearray(layout["bytes"])
    for name, field in layout["fields"].items():
        if name not in exclude:
            raw = bytes.fromhex(data[name])
            if len(raw) != field["bytes"]:
                raise AssetError("drawable typed source byte count differs")
            output[field["offset"] : field["offset"] + len(raw)] = raw
    return bytes(output)


def read_joints(system: bytes, base: int, root: int, contract: dict) -> dict:
    if root % 16:
        raise AssetError("joint record is not aligned")
    header, raw = typed(system, root, contract["joints"])
    if any(header[n] for n in ("Unknown_8h", "Unknown_20h", "Unknown_28h", "Unknown_38h")):
        raise AssetError("joint optional extension is unsupported")
    arrays = {}
    ranges = [(root, root + contract["joints"]["bytes"])]
    for label, key in (("Rotation", "rotationLimit"), ("Translation", "translationLimit")):
        count, pointer = header[label + "LimitsCount"], header[label + "LimitsPointer"]
        layout = contract[key]
        if count > 1024 or bool(count) != bool(pointer):
            raise AssetError("joint array count/pointer is invalid")
        start, size = pointer - base, count * layout["bytes"]
        if count and (
            start < 0
            or start % 16
            or start + size > len(system)
            or any(a < start + size and start < b for a, b in ranges)
        ):
            raise AssetError("joint array span is invalid")
        if count:
            ranges.append((start, start + size))
        rows = [typed(system, start + i * layout["bytes"], layout) for i in range(count)]
        if len({row[0]["BoneId"] for row in rows}) != count:
            raise AssetError("joint limits contain duplicate bone tags")
        arrays[label] = [{"fields": v, "fieldBytes": r} for v, r in rows]
    return {"header": header, "fieldBytes": raw, "arrays": arrays}


def read(system: bytes, base: int, root: int, contract: dict) -> dict:
    if root % 16:
        raise AssetError("drawable record is not aligned")
    header, raw = typed(system, root, contract["drawable"])
    if any(header[n] for n in ZERO_EXTENSION) or header["DrawableModelsBlocksSize"]:
        raise AssetError("drawable optional extension or nonzero model block size is unsupported")
    if header["BoundingSphereRadius"] < 0 or any(
        a > b for a, b in zip(header["BoundingBoxMin"], header["BoundingBoxMax"], strict=True)
    ):
        raise AssetError("drawable bounding region is invalid")
    for name in POINTERS:
        p = header[name]
        if p and (not base <= p < base + len(system) or (name != "NamePointer" and p % 8)):
            raise AssetError("drawable pointer exceeds system storage: " + name)
    lods = []
    for label, suffix in geometry.LODS.items():
        p = header[f"DrawableModels{suffix}Pointer"]
        if not p:
            lods.append({"lod": label, "header": None, "models": []})
            continue
        at = p - base
        h, _ = typed(system, at, contract["listHeader"])
        count, cap, array = h["Count"], h["Capacity"], h["Pointer"] - base
        if at % 16 or not 0 < count == cap <= 1024 or array < 0 or array % 8 or array + cap * 8 > len(system):
            raise AssetError("drawable model list count/capacity/span is invalid")
        pointers = list(struct.unpack_from("<" + "Q" * cap, system, array))
        if any(p < base or p + 48 > base + len(system) or p % 16 for p in pointers):
            raise AssetError("drawable model target is invalid")
        lods.append({"lod": label, "header": h, "models": pointers})
    if header["DrawableModelsPointer"] not in (0, header["DrawableModelsHighPointer"]):
        raise AssetError("extra drawable model list is unsupported")
    name = None
    if header["NamePointer"]:
        start = header["NamePointer"] - base
        end = system.find(b"\0", start, min(start + 1024, len(system)))
        if end < 0:
            raise AssetError("drawable name is unterminated")
        name = system[start:end].decode("utf-8")
    joints = read_joints(system, base, header["JointsPointer"] - base, contract) if header["JointsPointer"] else None
    return {"header": header, "fieldBytes": raw, "lods": lods, "name": name, "joints": joints}


def semantics(value: dict) -> dict:
    fields = {k: v for k, v in value["fieldBytes"].items() if k not in (*POINTERS, "FileVFT", "FileUnknown")}
    joints = value["joints"]
    return {
        "fieldBytes": fields,
        "name": value["name"],
        "lods": [
            {"lod": r["lod"], "count": len(r["models"]), "unknown": r["header"]["Unknown"] if r["header"] else None}
            for r in value["lods"]
        ],
        "joints": (
            None
            if joints is None
            else {
                "fieldBytes": {
                    k: v for k, v in joints["fieldBytes"].items() if k not in (*JOINT_POINTERS, "VFT", "Unknown_4h")
                },
                "arrays": joints["arrays"],
            }
        ),
    }


def build(
    system: bytes, source_base: int, main: dict, children: dict, components: dict, contract: dict, bone_tags: set[int]
) -> tuple[bytes, dict]:
    expected_links = [
        (lod["index"], child["index"], field, int(child[field], 0), child["boneTag"])
        for lod in children["physics"]["lods"]
        if lod is not None
        for child in lod["children"]
        for field in ("drawable1Pointer", "drawable2Pointer")
    ]
    actual_links = [
        (r["physicsLod"], r["childIndex"], r["field"], r["drawablePointer"], r["boneTag"]) for r in children["links"]
    ]
    if (
        actual_links != expected_links
        or any(r["present"] != bool(r["drawablePointer"]) for r in children["links"])
        or {r["drawablePointer"] for r in children["links"] if r["present"]}
        != {r["pointer"] for r in children["drawables"]}
    ):
        raise AssetError("drawable physics child slot coverage differs")
    arena, offsets = ObjectArena(), {}
    for key in ("skeleton", "materials", "models"):
        blob, placement = components[key]
        offsets[key] = arena.include(key, blob, placement)
    base = components["models"][1]["sourceBase"]
    if any(p["sourceBase"] != base for _, p in components.values()):
        raise AssetError("drawable component bases disagree")
    mp, sp, matp = (components[k][1] for k in ("models", "skeleton", "materials"))
    models = [r | {"systemOffset": r["systemOffset"] + offsets["models"]} for r in mp["models"]]
    geometries = [r | {"systemOffset": r["systemOffset"] + offsets["models"]} for r in mp["geometries"]]
    root = main["drawable"]["systemOffset"]
    main_read = read(system, source_base, root, contract)
    inputs = [(root, {"kind": "main"})] + [
        (r["pointer"] - source_base, {"kind": "physics-child", "sourceDrawablePointer": r["pointer"]})
        for r in children["drawables"]
    ]
    if len({at for at, _ in inputs}) != len(inputs):
        raise AssetError("drawable source roots duplicate")
    fields, written, external, consumed = contract["drawable"]["fields"], [], [], set()
    for source_at, owner in inputs:
        value = read(system, source_base, source_at, contract)
        h = value["header"]
        if (
            h["SkeletonPointer"] != main_read["header"]["SkeletonPointer"]
            or not h["SkeletonPointer"]
            or (owner["kind"] != "main" and h["ShaderGroupPointer"])
            or (owner["kind"] == "main" and h["ShaderGroupPointer"] != int(main["drawable"]["shaderGroupPointer"], 0))
        ):
            raise AssetError("drawable source material/skeleton ownership differs")
        tag = f"drawable-{len(written)}"
        at = arena.add(
            tag, source_fields(value["fieldBytes"], contract["drawable"], (*POINTERS, "FileVFT", "FileUnknown"))
        )
        targets = dict.fromkeys(POINTERS)
        targets["SkeletonPointer"] = offsets["skeleton"] + sp["rootOffset"]
        if owner["kind"] == "main":
            targets["ShaderGroupPointer"] = offsets["materials"] + matp["rootOffset"]
        indices = []
        for source_lod, suffix in zip(value["lods"], geometry.LODS.values(), strict=True):
            selected = [i for i, m in enumerate(models) if m["owner"] == owner and m["lod"] == source_lod["lod"]]
            if [models[i]["sourceModelOffset"] + source_base for i in selected] != source_lod["models"] or any(
                i in consumed for i in selected
            ):
                raise AssetError("drawable model composition order/coverage differs")
            consumed.update(selected)
            indices.append(selected)
            if source_lod["header"] is None:
                continue
            count = len(selected)
            raw = bytearray(contract["listHeader"]["bytes"] + count * 8)
            lh = contract["listHeader"]["fields"]
            for n in ("Count", "Capacity", "Unknown"):
                struct.pack_into("<" + FORMATS[lh[n]["type"]], raw, lh[n]["offset"], source_lod["header"][n])
            list_at = arena.add(tag + "." + source_lod["lod"], raw)
            array = list_at + contract["listHeader"]["bytes"]
            arena.pointer(list_at + lh["Pointer"]["offset"], array)
            for i, index in enumerate(selected):
                arena.pointer(array + i * 8, models[index]["systemOffset"])
            targets[f"DrawableModels{suffix}Pointer"] = list_at
        if h["DrawableModelsPointer"]:
            targets["DrawableModelsPointer"] = targets["DrawableModelsHighPointer"]
        if value["name"] is not None:
            targets["NamePointer"] = arena.add(tag + ".name", value["name"].encode("utf-8") + b"\0")
        if h["BoundPointer"]:
            external.append(
                {
                    "offset": at + fields["BoundPointer"]["offset"],
                    "kind": "collision-bound",
                    "sourcePointer": h["BoundPointer"],
                    "drawableIndex": len(written),
                }
            )
        if value["joints"]:
            joints = value["joints"]
            j = arena.add(
                tag + ".joints",
                source_fields(joints["fieldBytes"], contract["joints"], (*JOINT_POINTERS, "VFT", "Unknown_4h")),
            )
            targets["JointsPointer"] = j
            jf = contract["joints"]["fields"]
            for label, key in (("Rotation", "rotationLimit"), ("Translation", "translationLimit")):
                rows = joints["arrays"][label]
                if any(r["fields"]["BoneId"] not in bone_tags for r in rows):
                    raise AssetError("joint limit bone tag is absent from skeleton")
                data = b"".join(source_fields(row["fieldBytes"], contract[key], ()) for row in rows)
                target = arena.add(tag + ".joints." + label, data) if data else None
                arena.pointer(j + jf[label + "LimitsPointer"]["offset"], target)
            for n in ("Unknown_20h", "Unknown_28h"):
                arena.pointer(j + jf[n]["offset"], None)
        for name in POINTERS:
            arena.pointer(at + fields[name]["offset"], targets[name])
        written.append(
            {
                "systemOffset": at,
                "sourcePointer": source_base + source_at,
                "owner": owner,
                "sourceCollisionPointer": h["BoundPointer"],
                "semantics": semantics(value),
                "modelIndicesByLod": indices,
                "targets": targets,
            }
        )
    if consumed != set(range(len(models))):
        raise AssetError("drawable composition leaves unowned models")
    source_to_native = {r["sourcePointer"]: r["systemOffset"] for r in written}
    links = [
        r | {"drawableOffset": source_to_native[r["drawablePointer"]] if r["present"] else None}
        for r in children["links"]
    ]
    blob, placement = arena.render(base)
    placement.update(
        rootOffset=written[0]["systemOffset"],
        drawables=written,
        models=models,
        geometries=geometries,
        componentOffsets=offsets,
        childLinks=links,
        sourceChildLinksSha256=child_link_digest(links),
        externalCollisionLinks=external,
        externalTextureLinks=[r | {"offset": r["offset"] + offsets["materials"]} for r in matp["externalTextureLinks"]],
        externalLinksResolved=False,
        completeNativeResource=False,
    )
    verify(blob, placement, contract)
    return blob, placement


def child_link_digest(links: list) -> str:
    source = [{k: v for k, v in row.items() if k != "drawableOffset"} for row in links]
    return mesh.digest(json.dumps(source, sort_keys=True, separators=(",", ":"), allow_nan=False).encode())


def verify(blob: bytes, placement: dict, contract: dict) -> dict:
    if child_link_digest(placement["childLinks"]) != placement["sourceChildLinksSha256"]:
        raise AssetError("drawable source child slot coverage changed")
    base, model_count, joint_count = placement["sourceBase"], 0, 0
    expected_external = {}
    for row in [*placement["externalCollisionLinks"], *placement.get("resolvedCollisionLinks", [])]:
        key = (row["drawableIndex"], row["offset"])
        if key in expected_external or row["kind"] != "collision-bound" or not row["sourcePointer"]:
            raise AssetError("drawable symbolic collision site invalid or duplicated")
        expected_external[key] = row
    for index, row in enumerate(placement["drawables"]):
        value = read(blob, base, row["systemOffset"], contract)
        if semantics(value) != row["semantics"] or value["header"]["FileVFT"] or value["header"]["FileUnknown"]:
            raise AssetError("drawable readback differs from typed source fields")
        for field, target in row["targets"].items():
            if value["header"][field] != (0 if target is None else base + target):
                raise AssetError("drawable composed pointer differs: " + field)
        for lod, indices in zip(value["lods"], row["modelIndicesByLod"], strict=True):
            if lod["models"] != [base + placement["models"][i]["systemOffset"] for i in indices]:
                raise AssetError("drawable composed model order differs")
            model_count += len(indices)
        if value["joints"]:
            if value["joints"]["header"]["VFT"] or value["joints"]["header"]["Unknown_4h"]:
                raise AssetError("joint serialized runtime class is not cleared")
            joint_count += sum(len(v) for v in value["joints"]["arrays"].values())
        link = expected_external.pop(
            (index, row["systemOffset"] + contract["drawable"]["fields"]["BoundPointer"]["offset"]), None
        )
        if (None if link is None else link["sourcePointer"]) != (row["sourceCollisionPointer"] or None):
            raise AssetError("drawable source collision reference is missing or changed")
        if link is not None:
            target = link.get("targetOffset")
            if value["header"]["BoundPointer"] != (0 if target is None else base + target):
                raise AssetError("drawable collision binding differs from its declared resolution")
    roots = {r["sourcePointer"]: r["systemOffset"] for r in placement["drawables"]}
    for link in placement["childLinks"]:
        if link["drawableOffset"] != (roots.get(link["drawablePointer"]) if link["present"] else None):
            raise AssetError("drawable child source alias mapping differs")
    if expected_external or model_count != len(placement["models"]):
        raise AssetError("drawable model or collision coverage differs")
    return {
        "drawableCount": len(placement["drawables"]),
        "modelCount": model_count,
        "jointLimitCount": joint_count,
        "externalCollisionLinks": len(placement["externalCollisionLinks"]),
        "resolvedCollisionLinks": len(placement.get("resolvedCollisionLinks", [])),
        "externalTextureLinks": len(placement["externalTextureLinks"]),
        "childPointerSlots": len(placement["childLinks"]),
        "sourceFieldsAndRelationshipsPreserved": True,
        "nativeCodeExecuted": False,
    }


def shifted(placement: dict, displacement: int) -> dict:
    """Reference-reader positions for a graph composed at a different offset."""
    return placement | {
        "rootOffset": placement["rootOffset"] + displacement,
        "componentOffsets": {k: v + displacement for k, v in placement["componentOffsets"].items()},
        "drawables": [
            r
            | {
                "systemOffset": r["systemOffset"] + displacement,
                "targets": {k: None if v is None else v + displacement for k, v in r["targets"].items()},
            }
            for r in placement["drawables"]
        ],
        "models": [r | {"systemOffset": r["systemOffset"] + displacement} for r in placement["models"]],
        "geometries": [r | {"systemOffset": r["systemOffset"] + displacement} for r in placement["geometries"]],
        "childLinks": [
            r | {"drawableOffset": None if r["drawableOffset"] is None else r["drawableOffset"] + displacement}
            for r in placement["childLinks"]
        ],
        **{
            k: [r | {"offset": r["offset"] + displacement} for r in placement[k]]
            for k in ("externalCollisionLinks", "externalTextureLinks")
        },
        "resolvedCollisionLinks": [
            r | {"offset": r["offset"] + displacement, "targetOffset": r["targetOffset"] + displacement}
            for r in placement.get("resolvedCollisionLinks", [])
        ],
    }


def component_view(placement: dict, name: str, original: dict) -> dict:
    displacement = placement["componentOffsets"][name]
    view = original | {"sourceBase": placement["sourceBase"]}
    if "rootOffset" in original:
        view["rootOffset"] = original["rootOffset"] + displacement
    for key, offset_key in (
        ("models", "systemOffset"),
        ("geometries", "systemOffset"),
        ("shaders", "systemOffset"),
        ("externalTextureLinks", "offset"),
    ):
        if key in original:
            view[key] = [r | {offset_key: r[offset_key] + displacement} for r in original[key]]
    return view

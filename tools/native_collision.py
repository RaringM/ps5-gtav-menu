"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import math
import struct
from collections import Counter

import gen9_mesh as mesh
import native_drawables as drawable
from gtavmenu_tools.asset_formats import AssetError
from native_resource_builder import ObjectArena

FORMATS = drawable.FORMATS | {"Byte": "B", "Int16": "h", "Int32": "i", "Vector4": "4f"}


GEOMETRY_POINTERS = (
    "VerticesShrunkPointer",
    "PolygonsPointer",
    "VerticesPointer",
    "VertexColoursPointer",
    "OctantsPointer",
    "OctantItemsPointer",
    "MaterialsPointer",
    "MaterialColoursPointer",
    "PolygonMaterialIndicesPointer",
)


COMPOSITE_POINTERS = (
    "ChildrenPointer",
    "ChildrenTransformation1Pointer",
    "ChildrenTransformation2Pointer",
    "ChildrenBoundingBoxesPointer",
    "ChildrenFlags1Pointer",
    "ChildrenFlags2Pointer",
    "BVHPointer",
)


CAPSULE_TAIL = ("Unknown_70h", "Unknown_74h", "Unknown_78h", "Unknown_7Ch")


def typed(data: bytes, at: int, layout: dict) -> tuple[dict, dict]:
    if at < 0 or at + layout["bytes"] > len(data):
        raise AssetError("collision record exceeds storage")
    values, raw = {}, {}
    for name, field in layout["fields"].items():
        pos = at + field["offset"]
        v = struct.unpack_from("<" + FORMATS[field["type"]], data, pos)
        spatial = v[:3] if field["type"] == "Vector4" else v
        if any(isinstance(x, float) and not math.isfinite(x) for x in spatial):
            raise AssetError("collision record has non-finite spatial value")
        values[name] = (
            {"xyz": list(v[:3]), "wBits": struct.unpack_from("<I", data, pos + 12)[0]}
            if field["type"] == "Vector4"
            else v[0] if len(v) == 1 else list(v)
        )
        raw[name] = data[pos : pos + field["bytes"]].hex()
    return values, raw


def scalar(data, base, pointer, layout, field):
    at = pointer - base + layout["fields"][field]["offset"]
    if at < 0 or at + layout["fields"][field]["bytes"] > len(data):
        raise AssetError("collision owner pointer exceeds storage")
    return struct.unpack_from("<" + FORMATS[layout["fields"][field]["type"]], data, at)[0]


def roots(system: bytes, base: int, graph: dict, contract: dict) -> list[dict]:
    result = [
        {"owner": "drawable", "ownerIndex": link["drawableIndex"], "pointer": link["sourcePointer"]}
        for link in graph["externalCollisionLinks"]
    ]
    c = contract["sourceLinks"]
    group = scalar(system, base, base, c["fragment"], "PhysicsLODGroupPointer")
    if not group:
        return result  # mod-kit part fragment: drawable bounds only
    for i in range(1, 4):
        lod = scalar(system, base, group, c["lodGroup"], f"PhysicsLOD{i}Pointer")
        if not lod:
            continue
        bound = scalar(system, base, lod, c["lod"], "BoundPointer")
        result.append({"owner": "physics-lod", "ownerIndex": i, "pointer": bound})
        for j in range(1, 3):
            archetype = scalar(system, base, lod, c["lod"], f"Archetype{j}Pointer")
            if archetype:
                bound = scalar(system, base, archetype, c["archetype"], "BoundPointer")
                result.append({"owner": f"archetype-{j}", "ownerIndex": i, "pointer": bound})
    return result


def read(system: bytes, base: int, root_rows: list, contract: dict) -> dict:
    layouts, structs = contract["layouts"], contract["structs"]
    visited, nodes, claims = {}, [], {}

    def claim(p, size, alignment, kind):
        at = p - base
        if not p or at < 0 or p % alignment or size <= 0 or at + size > len(system):
            raise AssetError("collision span invalid: " + kind)
        if p in claims:
            if claims[p] != (size, kind):
                raise AssetError("collision alias has different layout or extent")
        else:
            if any(q < p + size and p < q + n for q, (n, _) in claims.items()):
                raise AssetError("collision spans overlap")
            claims[p] = (size, kind)
        return at

    def array(p, count, layout, kind, alignment=1, optional=False):
        if not 0 <= count <= 65536:
            raise AssetError("collision array count exceeds bound")
        if not p:
            if count and not optional:
                raise AssetError("collision required array missing: " + kind)
            return None
        if not count:
            raise AssetError("collision nonnull empty array unsupported: " + kind)
        at = claim(p, count * layout["bytes"], alignment, kind)
        rows = [typed(system, at + i * layout["bytes"], layout) for i in range(count)]
        return {
            "sourcePointer": p,
            "layout": layout,
            "kind": kind,
            "count": count,
            "rawHex": system[at : at + count * layout["bytes"]].hex(),
            "values": [v for v, _ in rows],
        }

    def integers(p, count, kind, fmt="UInt32", optional=False):
        layout = {
            "bytes": struct.calcsize("<" + FORMATS[fmt]),
            "fields": {"value": {"offset": 0, "bytes": struct.calcsize("<" + FORMATS[fmt]), "type": fmt}},
        }
        return array(p, count, layout, kind, min(layout["bytes"], 8), optional)

    def visit(p, explicit=None, leaf_limit=None, stack=()):
        if not p:
            return None
        if p in stack or len(stack) > 32 or len(nodes) >= 512:
            raise AssetError("collision graph cyclic or excessive")
        if p in visited:
            previous = nodes[visited[p]]
            if (explicit is not None and (previous["kind"] != explicit or previous.get("leafLimit") != leaf_limit)) or (
                explicit is None and previous["kind"] == "BVH"
            ):
                raise AssetError("collision object alias has a different type or BVH context")
            return visited[p]
        tag = explicit
        if tag is None:
            number = scalar(system, base, p, layouts["Box"], "Type")
            tag = one([k for k in layouts if contract["types"].get(k) == number], "supported collision type")
        layout = layouts[tag]
        at = claim(p, layout["bytes"], 16, tag)
        h, raw = typed(system, at, layout)
        index = len(nodes)
        visited[p] = index
        node = {
            "sourcePointer": p,
            "kind": tag,
            "header": h,
            "fieldBytes": raw,
            "arrays": {},
            "links": {},
            "octants": [],
        }
        if tag == "BVH":
            node["leafLimit"] = leaf_limit
        nodes.append(node)
        arrays, links = node["arrays"], node["links"]
        if tag != "BVH" and (
            h["FilePagesInfoPointer"]
            or h["SphereRadius"] < 0
            or h["Margin"] < 0
            or any(a > b for a, b in zip(h["BoxMin"], h["BoxMax"], strict=True))
        ):
            raise AssetError("collision base fields unsupported or invalid")
        if tag == "Capsule" and any(h[n] for n in CAPSULE_TAIL):
            raise AssetError("collision capsule tail extension unsupported")
        if tag == "Geometry":
            nv, np = h["VerticesCount"], h["PolygonsCount"]
            if not 0 < nv <= 32768 or not 0 < np <= 32767 or h["VerticesShrunkCount"] not in (0, nv):
                raise AssetError("collision geometry vertex/polygon counts invalid")
            if any(
                h[n]
                for n in (
                    "Unknown_80h",
                    "Unknown_82h",
                    "Unknown_100h",
                    "Unknown_104h",
                    "Unknown_108h",
                    "Unknown_10Ch",
                    "Unknown_110h",
                    "Unknown_114h",
                )
            ):
                raise AssetError("collision geometry auxiliary extension unsupported")
            if any(x <= 0 for x in h["Quantum"]):
                raise AssetError("collision geometry quantum is not positive")
            for name, count, st, optional in (
                ("VerticesPointer", nv, "BoundVertex_s", False),
                ("VerticesShrunkPointer", h["VerticesShrunkCount"], "BoundVertex_s", True),
                ("VertexColoursPointer", nv, "BoundMaterialColour", True),
                ("MaterialsPointer", max(h["MaterialsCount"], contract["minimumMaterials"]), "BoundMaterial_s", False),
                ("MaterialColoursPointer", h["MaterialColoursCount"], "BoundMaterialColour", True),
            ):
                arrays[name] = array(h[name], count, structs[st], st, 2 if st == "BoundVertex_s" else 4, optional)
            arrays["PolygonsPointer"] = array(h["PolygonsPointer"], np, contract["triangle"], "triangle", 4)
            polys = arrays["PolygonsPointer"]
            for i, row in enumerate(polys["values"]):
                first = system[h["PolygonsPointer"] - base + i * contract["triangle"]["bytes"]]
                if first & contract["polygonTypeMask"] or row["triArea"] < 0:
                    raise AssetError("collision polygon is not a supported triangle")
                if any(row[f"triIndex{j}"] & contract["triangleIndexMask"] >= nv for j in range(1, 4)):
                    raise AssetError("collision polygon vertex index exceeds array")
                if any(row[f"edgeIndex{j}"] != 0xFFFF and row[f"edgeIndex{j}"] >= np for j in range(1, 4)):
                    raise AssetError("collision polygon adjacency index exceeds array")
            arrays["PolygonMaterialIndicesPointer"] = integers(
                h["PolygonMaterialIndicesPointer"], np, "polygon-material", "Byte"
            )
            if any(r["value"] >= h["MaterialsCount"] for r in arrays["PolygonMaterialIndicesPointer"]["values"]):
                raise AssetError("collision polygon material index exceeds array")
            if bool(h["OctantsPointer"]) != bool(h["OctantItemsPointer"]):
                raise AssetError("collision octant arrays disagree")
            if h["OctantsPointer"]:
                arrays["OctantsPointer"] = integers(h["OctantsPointer"], contract["octantCount"], "octant-counts")
                ptrs = integers(h["OctantItemsPointer"], contract["octantCount"], "octant-pointers", "UInt64")
                arrays["OctantItemsPointer"] = ptrs
                for count, target in zip(arrays["OctantsPointer"]["values"], ptrs["values"], strict=True):
                    items = integers(target["value"], count["value"], "octant-items")
                    if items and any(r["value"] >= nv for r in items["values"]):
                        raise AssetError("collision octant vertex exceeds array")
                    node["octants"].append(items)
            else:
                arrays["OctantsPointer"] = arrays["OctantItemsPointer"] = None
        elif tag == "Composite":
            count = h["ChildrenCount1"]
            if not 0 < count == h["ChildrenCount2"] <= 512:
                raise AssetError("collision composite child counts disagree")
            arrays["ChildrenPointer"] = integers(h["ChildrenPointer"], count, "bound-pointers", "UInt64")
            links["children"] = [visit(r["value"], stack=(*stack, p)) for r in arrays["ChildrenPointer"]["values"]]
            for name, st in (
                ("ChildrenTransformation1Pointer", "Matrix4F_s"),
                ("ChildrenTransformation2Pointer", "Matrix4F_s"),
                ("ChildrenBoundingBoxesPointer", "AABB_s"),
                ("ChildrenFlags1Pointer", "BoundCompositeChildrenFlags"),
                ("ChildrenFlags2Pointer", "BoundCompositeChildrenFlags"),
            ):
                arrays[name] = array(
                    h[name], count, structs[st], st, 16 if st != "BoundCompositeChildrenFlags" else 4, True
                )
            links["BVHPointer"] = visit(h["BVHPointer"], "BVH", count, (*stack, p))
        elif tag == "BVH":
            if h["Trees.Padding"] or leaf_limit is None:
                raise AssetError("collision BVH tree padding/context unsupported")
            for label, st in (("Nodes", "BVHNode_s"), ("Trees", "BVHTreeInfo_s")):
                count, cap = h[label + ".EntriesCount"], h[label + ".EntriesCapacity"]
                if not 0 < count <= cap <= 65536 or (label == "Trees" and cap != count):
                    raise AssetError("collision BVH list counts unsupported")
                arrays[label + ".EntriesPointer"] = array(h[label + ".EntriesPointer"], cap, structs[st], st, 16)
            active = arrays["Nodes.EntriesPointer"]["values"][: h["Nodes.EntriesCount"]]
            leaves = []
            for i, row in enumerate(active):
                if any(row["Min" + a] > row["Max" + a] for a in "XYZ"):
                    raise AssetError("collision BVH node bounds inverted")
                item, count = row["ItemId"], row["ItemCount"]
                if count > 0:
                    if item < 0 or item + count > leaf_limit:
                        raise AssetError("collision BVH leaf exceeds children")
                    leaves.extend(range(item, item + count))
                elif count != 0 or item <= 1 or i + item > len(active):
                    raise AssetError("collision BVH branch span invalid")
            if sorted(leaves) != list(range(leaf_limit)):
                raise AssetError("collision BVH leaf coverage differs")
            for tree in arrays["Trees.EntriesPointer"]["values"]:
                if not 0 <= tree["NodeIndex1"] < tree["NodeIndex2"] <= len(active):
                    raise AssetError("collision BVH tree node range invalid")
        return index

    roots_out = [r | {"nodeIndex": visit(r["pointer"])} for r in root_rows]
    return {"roots": roots_out, "nodes": nodes}


def pointers(kind: str) -> tuple:
    if kind == "BVH":
        return ("Nodes.EntriesPointer", "Trees.EntriesPointer")
    return ("FilePagesInfoPointer",) + (
        (*GEOMETRY_POINTERS, "Unknown_108h")
        if kind == "Geometry"
        else COMPOSITE_POINTERS if kind == "Composite" else ()
    )


def semantics(value: dict) -> dict:
    rows = []
    for node in value["nodes"]:
        kind = node["kind"]
        arrays = {}
        for field, array in node["arrays"].items():
            if array is None:
                arrays[field] = None
            elif field in ("ChildrenPointer", "OctantItemsPointer"):
                arrays[field] = {"count": array["count"]}
            else:
                arrays[field] = {"count": array["count"], "sha256": mesh.digest(bytes.fromhex(array["rawHex"]))}
        rows.append(
            {
                "kind": kind,
                "fieldBytes": {
                    k: v for k, v in node["fieldBytes"].items() if k not in (*pointers(kind), "FileVFT", "FileUnknown")
                },
                "arrays": arrays,
                "links": node["links"],
                "octants": [
                    None if a is None else {"count": a["count"], "sha256": mesh.digest(bytes.fromhex(a["rawHex"]))}
                    for a in node["octants"]
                ],
                "transformArraysAlias": kind == "Composite"
                and node["header"]["ChildrenTransformation1Pointer"]
                == node["header"]["ChildrenTransformation2Pointer"],
            }
        )
    return {"roots": [{k: v for k, v in row.items() if k != "pointer"} for row in value["roots"]], "nodes": rows}


def build(system: bytes, source_base: int, source_roots: list, contract: dict, base: int) -> tuple[bytes, dict]:
    decoded = read(system, source_base, source_roots, contract)
    arena, objects, array_cache = ObjectArena(), [], {}
    for i, node in enumerate(decoded["nodes"]):
        layout = contract["layouts"][node["kind"]]
        objects.append(
            arena.add(
                f"bound-{i}",
                drawable.source_fields(node["fieldBytes"], layout, (*pointers(node["kind"]), "FileVFT", "FileUnknown")),
            )
        )
    arrays_placed = []

    def add_array(array):
        if array is None:
            return None
        key = array["sourcePointer"]
        if key in array_cache:
            return array_cache[key]
        data = bytes.fromhex(array["rawHex"])
        if array["kind"] in ("bound-pointers", "octant-pointers"):
            data = bytes(len(data))
        else:
            # Re-encode each independently typed source element; never forward a
            # retail object template or unparsed pointer-bearing array.
            reconstructed = b"".join(
                drawable.source_fields(
                    typed(data, i * array["layout"]["bytes"], array["layout"])[1], array["layout"], ()
                )
                for i in range(array["count"])
            )
            if reconstructed != data:
                raise AssetError("collision typed array does not cover all bytes")
        at = arena.add(f"array-{len(array_cache)}", data)
        array_cache[key] = at
        arrays_placed.append({"sourcePointer": key, "systemOffset": at, "bytes": len(data), "kind": array["kind"]})
        return at

    for i, node in enumerate(decoded["nodes"]):
        layout = contract["layouts"][node["kind"]]
        targets = {name: add_array(array) for name, array in node["arrays"].items()}
        if node["kind"] == "Composite":
            bvh = node["links"]["BVHPointer"]
            targets["BVHPointer"] = objects[bvh] if bvh is not None else None
            for j, child in enumerate(node["links"]["children"]):
                arena.pointer(targets["ChildrenPointer"] + j * 8, objects[child] if child is not None else None)
        elif node["kind"] == "Geometry" and node["octants"]:
            for j, array in enumerate(node["octants"]):
                arena.pointer(targets["OctantItemsPointer"] + j * 8, add_array(array))
        for name in pointers(node["kind"]):
            arena.pointer(objects[i] + layout["fields"][name]["offset"], targets.get(name))
    blob, placement = arena.render(base)
    placement.update(
        nodes=[
            {"systemOffset": off, "sourcePointer": node["sourcePointer"], "kind": node["kind"]}
            for off, node in zip(objects, decoded["nodes"], strict=True)
        ],
        arrays=arrays_placed,
        sourceSemantics=semantics(decoded),
        roots=[
            row | {"systemOffset": None if row["nodeIndex"] is None else objects[row["nodeIndex"]]}
            for row in decoded["roots"]
        ],
        collisionConsumerSemanticsQualified=False,
    )
    verify(blob, placement, contract)
    return blob, placement


def verify(blob: bytes, placement: dict, contract: dict) -> dict:
    base = placement["sourceBase"]
    roots_in = [
        {k: v for k, v in r.items() if k not in ("nodeIndex", "systemOffset")}
        | {"pointer": 0 if r["systemOffset"] is None else base + r["systemOffset"]}
        for r in placement["roots"]
    ]
    decoded = read(blob, base, roots_in, contract)
    if semantics(decoded) != placement["sourceSemantics"]:
        raise AssetError("collision readback changed source fields, arrays, topology or alias relationships")
    if [n["sourcePointer"] - base for n in decoded["nodes"]] != [n["systemOffset"] for n in placement["nodes"]]:
        raise AssetError("collision placement traversal differs")
    for node in decoded["nodes"]:
        if node["kind"] != "BVH" and (node["header"]["FileVFT"] or node["header"]["FileUnknown"]):
            raise AssetError("collision serialized class word is not cleared")
    counts = Counter(n["kind"] for n in decoded["nodes"])
    return {
        "nodeTypes": dict(sorted(counts.items())),
        "rootReferences": len(decoded["roots"]),
        "vertices": sum(n["header"]["VerticesCount"] for n in decoded["nodes"] if n["kind"] == "Geometry"),
        "triangles": sum(n["header"]["PolygonsCount"] for n in decoded["nodes"] if n["kind"] == "Geometry"),
        "sourceFieldsAndArraysPreserved": True,
        "nativeCodeExecuted": False,
    }


def shifted(placement: dict, delta: int) -> dict:
    return placement | {
        key: [
            r | {"systemOffset": None if r["systemOffset"] is None else r["systemOffset"] + delta}
            for r in placement[key]
        ]
        for key in ("nodes", "arrays", "roots")
    }


def compose(drawable_blob: bytes, graph: dict, collision_blob: bytes, collision: dict) -> tuple[bytes, dict]:
    if graph["sourceBase"] != collision["sourceBase"] or graph.get("resolvedCollisionLinks"):
        raise AssetError("collision composition base/resolution profile differs")
    arena = ObjectArena()
    graph_offset = arena.include("drawables", drawable_blob, graph)
    # A mod-kit part fragment without bounds composes an empty collision component.
    collision_offset = arena.include("collision", collision_blob, collision) if collision["blocks"] else 0
    moved_graph = drawable.shifted(graph, graph_offset)
    moved_collision = shifted(collision, collision_offset)
    targets = {r["sourcePointer"]: r["systemOffset"] for r in moved_collision["nodes"]}
    resolved = []
    roots_by_owner = {r["ownerIndex"]: r for r in collision["roots"] if r["owner"] == "drawable"}
    for link in moved_graph["externalCollisionLinks"]:
        source = roots_by_owner.pop(link["drawableIndex"], None)
        if source is None or source["pointer"] != link["sourcePointer"] or link["sourcePointer"] not in targets:
            raise AssetError("drawable collision root is missing or changed")
        target = targets[link["sourcePointer"]]
        arena.bind(link["offset"], target)
        index = link["drawableIndex"]
        moved_graph["drawables"][index]["targets"]["BoundPointer"] = target
        resolved.append(link | {"targetOffset": target})
    if roots_by_owner:
        raise AssetError("collision component has extra drawable root bindings")
    blob, placement = arena.render(graph["sourceBase"])
    moved_graph.update(placement)
    moved_graph.update(
        externalCollisionLinks=[],
        resolvedCollisionLinks=resolved,
        collision={k: moved_collision[k] for k in ("sourceBase", "nodes", "arrays", "roots", "sourceSemantics")},
        collisionLinksResolved=True,
        collisionConsumerSemanticsQualified=False,
    )
    return blob, moved_graph


def one(rows, label):
    if len(rows) != 1:
        raise AssetError(f"vehicle {label} is missing or ambiguous")
    return rows[0]

"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import struct
from collections import Counter

import native_collision as collision
import native_drawables as drawable
import physics_group_hierarchy as hierarchy
from gtavmenu_tools.asset_formats import AssetError
from native_resource_builder import ObjectArena

CLASS_FIELDS = ("VFT", "Unknown_04h")


def pointers(kind: str, layout: dict) -> tuple[str, ...]:
    return tuple(n for n in layout["fields"] if n.endswith("Pointer"))


def root_pointer(system: bytes, base: int, contract: dict) -> int:
    return collision.typed(system, 0, contract["fragment"])[0]["PhysicsLODGroupPointer"]


def read(system: bytes, base: int, root: int, contract: dict) -> dict:
    """Read the complete supported physics graph without consulting placement."""
    layouts = contract["layouts"]
    nodes, identities, ranges = [], {}, []

    def claim(pointer, size, identity, alignment=8):
        at = pointer - base
        if not pointer or size <= 0 or at < 0 or at % alignment or at + size > len(system):
            raise AssetError("physics object extent/alignment is invalid: " + str(identity))
        if any(at < end and start < at + size for start, end in ranges):
            raise AssetError("physics objects overlap: " + str(identity))
        ranges.append((at, at + size))
        return at

    def array(pointer, count, kind, trailer=0):
        if not count:
            if pointer:
                raise AssetError("physics empty array has a nonnull pointer")
            return None
        if count > 4096:
            raise AssetError("physics array exceeds finite budget")
        identity = (pointer, kind, count, trailer)
        if pointer in identities:
            index = identities[pointer]
            if nodes[index]["identity"] != identity:
                raise AssetError("physics array alias type/count differs")
            return index
        stride = 8 if kind in ("groupPointers", "childPointers", "namePointers") else layouts[kind]["bytes"]
        at = claim(pointer, count * stride + trailer, identity, 4 if kind == "Single" else 8)
        node = {"kind": kind, "identity": identity, "sourcePointer": pointer, "count": count, "links": {}}
        index = len(nodes)
        nodes.append(node)
        identities[pointer] = index
        if kind.endswith("Pointers"):
            targets = struct.unpack_from("<" + str(count) + "Q", system, at)
            if not all(targets):
                raise AssetError("physics pointer array contains a null element")
            for i, value in enumerate(targets):
                node["links"][str(i)] = visit(
                    value, {"groupPointers": "group", "childPointers": "child", "namePointers": "name"}[kind]
                )
            node["trailerHex"] = system[at + count * stride : at + count * stride + trailer].hex()
        else:
            node["rows"] = [collision.typed(system, at + i * stride, layouts[kind])[1] for i in range(count)]
        return index

    def visit(pointer, kind):
        if not pointer:
            return None
        if pointer in identities:
            index = identities[pointer]
            if nodes[index]["kind"] != kind:
                raise AssetError("physics object alias changes type")
            return index
        layout = layouts[kind]
        at = pointer - base
        header, raw = collision.typed(system, at, layout)
        size = layout["bytes"]
        if kind == "transforms":
            count = header["MatricesCount"]
            if not 0 < count <= 255:
                raise AssetError("physics transform count is invalid")
            size += count * layouts["matrix"]["bytes"]
        claim(pointer, size, kind, 4 if kind == "name" else 8)
        node = {
            "kind": kind,
            "sourcePointer": pointer,
            "header": header,
            "fieldBytes": raw,
            "links": {},
            "external": {},
        }
        index = len(nodes)
        nodes.append(node)
        identities[pointer] = index
        zero = [
            n
            for n, row in layout["fields"].items()
            if n.startswith("Unknown_") and row["type"] == "UInt64" and not (kind == "archetype" and n == "Unknown_10h")
        ]
        if any(header[n] for n in zero):
            raise AssetError("physics unknown extension is populated: " + kind)
        if kind == "lodGroup":
            for name in pointers(kind, layout):
                node["links"][name] = visit(header[name], "lod")
        elif kind == "lod":
            if any(
                header[n]
                for n in (
                    "ArticulatedBodyTypePointer",
                    "UnknownData1Pointer",
                    "UnknownData2Pointer",
                    "UnknownData1Count",
                    "UnknownData2Count",
                )
            ):
                raise AssetError("physics articulated/unknown extension is unsupported")
            gc, cc = header["GroupsCount"], header["ChildrenCount"]
            if not 0 < gc <= 255 or not 0 < cc <= 255 or cc != header["ChildrenCount2"]:
                raise AssetError("physics child/group counts differ")
            for name, targetkind, count, trailer in (
                ("GroupsPointer", "groupPointers", gc, 0),
                ("GroupNamesPointer", "namePointers", gc, contract["groupNamesTrailerBytes"]),
                ("ChildrenPointer", "childPointers", cc, 0),
                ("ChildrenUnkFloatsPointer", "Single", cc, 0),
                ("ChildrenInertiaTensorsPointer", "Vector4", cc, 0),
                ("ChildrenUnkVecsPointer", "Vector4", cc, 0),
            ):
                node["links"][name] = array(header[name], count, targetkind, trailer)
            for name, targetkind in (
                ("Archetype1Pointer", "archetype"),
                ("Archetype2Pointer", "archetype"),
                ("FragTransformsPointer", "transforms"),
            ):
                node["links"][name] = visit(header[name], targetkind)
            for name in ("ArticulatedBodyTypePointer", "UnknownData1Pointer", "UnknownData2Pointer"):
                node["links"][name] = None
            node["external"]["BoundPointer"] = header["BoundPointer"]
            groups = [nodes[v]["header"] for v in nodes[node["links"]["GroupsPointer"]]["links"].values()]
            children = [nodes[v]["header"] for v in nodes[node["links"]["ChildrenPointer"]]["links"].values()]
            if sum(g["ParentIndex"] == 255 for g in groups) != header["RootGroupsCount"]:
                raise AssetError("physics root group count differs")
            node["hierarchyAnomalies"] = []
            for gi, group in enumerate(groups):
                parent, seen = gi, set()
                while parent != 255:
                    if parent >= gc or parent in seen:
                        raise AssetError("physics group ancestry invalid or cyclic")
                    seen.add(parent)
                    parent = groups[parent]["ParentIndex"]
                child_indices = [i for i, c in enumerate(children) if c["GroupIndex"] == gi]
                group_indices = [i for i, g in enumerate(groups) if g["ParentIndex"] == gi]
                if child_indices != list(range(group["ChildIndex"], group["ChildIndex"] + group["ChildCount"])):
                    raise AssetError("physics child membership/range differs")
                first, count = group["ChildGroupIndex"], group["ChildGroupCount"]
                if len(group_indices) != count or (count and (first != group_indices[0] or first + count > gc)):
                    raise AssetError("physics group membership/count differs")
                if group_indices != list(range(first, first + count)):
                    node["hierarchyAnomalies"].append(
                        {
                            "groupIndex": gi,
                            "declaredFirst": first,
                            "declaredCount": count,
                            "actualChildren": group_indices,
                            "reason": "Direct child groups are not contiguous; source values preserved",
                        }
                    )
            if any(c["GroupIndex"] >= gc for c in children):
                raise AssetError("physics child group is out of range")
            transforms = node["links"]["FragTransformsPointer"]
            if transforms is None or nodes[transforms]["header"]["MatricesCount"] != cc:
                raise AssetError("physics child transform count differs")
        elif kind == "child":
            node["links"]["EvtSetPointer"] = visit(header["EvtSetPointer"], "event")
            node["external"] = {n: header[n] for n in ("Drawable1Pointer", "Drawable2Pointer")}
        elif kind == "archetype":
            if header["Unknown_10h"] != contract["archetypeKind"]:
                raise AssetError("physics archetype kind is unsupported")
            node["external"]["BoundPointer"] = header["BoundPointer"]
            p = header["NamePointer"]
            start = p - base
            end = system.find(b"\0", start, start + 1024) if 0 <= start < len(system) else -1
            if not p or end < 0:
                raise AssetError("physics archetype name is missing or unterminated")
            node["nameHex"] = system[start : end + 1].hex()
        elif kind == "transforms":
            node["matrices"] = [
                collision.typed(system, at + layout["bytes"] + i * layouts["matrix"]["bytes"], layouts["matrix"])[1]
                for i in range(header["MatricesCount"])
            ]
        return index

    root_index = visit(root, "lodGroup")
    if root_index is None:
        raise AssetError("physics LOD group is absent")
    return {"nodes": nodes, "rootIndex": root_index}


def semantics(decoded: dict, external_identities: dict | None = None) -> dict:
    rows = []
    for node in decoded["nodes"]:
        kind = node["kind"]
        row = {"kind": kind, "links": node["links"]}
        if "fieldBytes" in node:
            classes = CLASS_FIELDS if "VFT" in node["header"] else ()
            row["fields"] = {
                n: v for n, v in node["fieldBytes"].items() if n not in classes and not n.endswith("Pointer")
            }
            row["external"] = {
                n: (p if external_identities is None or p == 0 else external_identities[p])
                for n, p in node["external"].items()
            }
        for name in ("rows", "matrices", "nameHex", "count", "hierarchyAnomalies"):
            if name in node:
                row[name] = node[name]
        rows.append(row)
    return {"rootIndex": decoded["rootIndex"], "nodes": rows}


def build(
    system: bytes, source_base: int, drawing: bytes, graph: dict, contract: dict, hierarchy_contract=None
) -> tuple[bytes, dict]:
    if not root_pointer(system, source_base, contract):
        # Mod-kit part fragment: no physics LOD group; the fragment root keeps a null pointer.
        arena = ObjectArena()
        delta = arena.include("geometry-collision", drawing, graph)
        result = drawable.shifted(graph, delta)
        result["collision"] = collision.shifted(graph["collision"], delta)
        blob, placement = arena.render(graph["sourceBase"])
        result.update(placement)
        result["physics"] = {"sourceBase": graph["sourceBase"], "rootOffset": None, "nodes": [], "bindings": []}
        result.update(physicsLinksResolved=True, physicsConsumerSemanticsQualified=False)
        return blob, result
    decoded = read(system, source_base, root_pointer(system, source_base, contract), contract)
    source_semantics = semantics(decoded)
    native_semantics, hierarchy_plan = (
        hierarchy.native_semantics(source_semantics) if hierarchy_contract is not None else (source_semantics, None)
    )
    arena = ObjectArena()
    delta = arena.include("geometry-collision", drawing, graph)
    result = drawable.shifted(graph, delta)
    result["collision"] = collision.shifted(graph["collision"], delta)
    targets = {row["sourcePointer"]: row["systemOffset"] for row in result["drawables"]}
    targets.update({row["sourcePointer"]: row["systemOffset"] for row in result["collision"]["nodes"]})
    layouts = contract["layouts"]
    # Reassemble the native contiguous public/retail layout entirely from the
    # source core and pointer-reached typed name tail. No new scalar defaults.
    group_names = {}
    for node in decoded["nodes"]:
        if node["kind"] != "lod":
            continue
        groups = decoded["nodes"][node["links"]["GroupsPointer"]]["links"]
        names = decoded["nodes"][node["links"]["GroupNamesPointer"]]["links"]
        for key, group in groups.items():
            if group in group_names and group_names[group] != names[key]:
                raise AssetError("physics shared group has conflicting name tails")
            group_names[group] = names[key]
    if len(set(group_names.values())) != len(group_names):
        raise AssetError("physics shared name tails need a separate native placement profile")
    offsets = [None] * len(decoded["nodes"])
    for i, node in enumerate(decoded["nodes"]):
        if offsets[i] is not None:
            continue
        kind = node["kind"]
        if "fieldBytes" in node:
            excluded = (CLASS_FIELDS if "VFT" in node["header"] else ()) + pointers(kind, layouts[kind])
            fields = node["fieldBytes"] | native_semantics["nodes"][i]["fields"]
            raw = drawable.source_fields(fields, layouts[kind], excluded)
            if kind == "transforms":
                raw += b"".join(drawable.source_fields(r, layouts["matrix"], ()) for r in node["matrices"])
            elif kind == "group":
                name_node = decoded["nodes"][group_names[i]]
                raw += drawable.source_fields(name_node["fieldBytes"], layouts["name"], ())
                if len(raw) != contract["contiguousGroup"]["bytes"]:
                    raise AssetError("physics reconstructed native group extent differs")
        elif kind.endswith("Pointers"):
            raw = bytes(node["count"] * 8 + (contract["groupNamesTrailerBytes"] if kind == "namePointers" else 0))
        else:
            raw = b"".join(drawable.source_fields(row, layouts[kind], ()) for row in node["rows"])
        offsets[i] = arena.add(f"physics.{i}.{kind}", raw)
        if kind == "group":
            offsets[group_names[i]] = offsets[i] + contract["groupNameOffset"]
    bindings = []
    for i, node in enumerate(decoded["nodes"]):
        kind, at = node["kind"], offsets[i]
        for name, link in native_semantics["nodes"][i]["links"].items():
            slot = at + (int(name) * 8 if kind.endswith("Pointers") else layouts[kind]["fields"][name]["offset"])
            target = None if link is None else offsets[link]
            arena.pointer(slot, target)
        for name, pointer in node.get("external", {}).items():
            if pointer and pointer not in targets:
                raise AssetError("physics drawable/collision dependency is missing")
            target = targets[pointer] if pointer else None
            slot = at + layouts[kind]["fields"][name]["offset"]
            arena.pointer(slot, target)
            bindings.append({"offset": slot, "sourcePointer": pointer, "targetOffset": target, "kind": name})
        if kind == "archetype":
            name_at = arena.add(f"physics.{i}.archetype-name", bytes.fromhex(node["nameHex"]), alignment=1)
            arena.pointer(at + layouts[kind]["fields"]["NamePointer"]["offset"], name_at)
    blob, placement = arena.render(graph["sourceBase"])
    result.update(placement)
    result["physics"] = {
        "sourceBase": graph["sourceBase"],
        "rootOffset": offsets[decoded["rootIndex"]],
        "nodes": [
            {"systemOffset": off, "sourcePointer": n["sourcePointer"], "kind": n["kind"]}
            for off, n in zip(offsets, decoded["nodes"], strict=True)
        ],
        "sourceSemantics": source_semantics,
        "bindings": bindings,
    }
    if hierarchy_plan is not None:
        result["physics"]["hierarchyConversion"] = hierarchy_plan
        result["physics"]["hierarchyContract"] = hierarchy_contract
    result.update(physicsLinksResolved=True, physicsConsumerSemanticsQualified=False)
    verify(blob, result["physics"], contract)
    return blob, result


def verify(blob: bytes, placement: dict, contract: dict) -> dict:
    base = placement["sourceBase"]
    if placement["rootOffset"] is None:
        if placement["nodes"] or placement["bindings"]:
            raise AssetError("absent physics graph has nodes")
        return {
            "physicsAbsent": True,
            "nodeTypes": {},
            "childDrawableLinks": 0,
            "boundLinks": 0,
            "nativeCodeExecuted": False,
        }
    decoded = read(blob, base, base + placement["rootOffset"], contract)
    identities = {}
    for row in placement["bindings"]:
        pointer = 0 if row["targetOffset"] is None else base + row["targetOffset"]
        if struct.unpack_from("<Q", blob, row["offset"])[0] != pointer:
            raise AssetError("physics external link changed")
        if pointer in identities and identities[pointer] != row["sourcePointer"]:
            raise AssetError("physics external alias identity differs")
        identities[pointer] = row["sourcePointer"]
    expected = placement["sourceSemantics"]
    if "hierarchyConversion" in placement:
        expected, plan = hierarchy.native_semantics(expected)
        if plan != placement["hierarchyConversion"]:
            raise AssetError("physics hierarchy permutation plan changed")
        decoded = hierarchy.canonical_nodes(decoded, placement)
    if semantics(decoded, identities) != expected:
        raise AssetError("physics source fields, arrays, hierarchy or aliases changed")
    if [n["sourcePointer"] - base for n in decoded["nodes"]] != [n["systemOffset"] for n in placement["nodes"]]:
        raise AssetError("physics placement traversal differs")
    for node in decoded["nodes"]:
        classes = CLASS_FIELDS if "VFT" in node.get("header", {}) else ()
        if any(node["header"][n] for n in classes) or any(bytes.fromhex(node.get("trailerHex", ""))):
            raise AssetError("physics serialized class word was not cleared")
    result = {
        "nodeTypes": dict(sorted(Counter(n["kind"] for n in decoded["nodes"]).items())),
        "childDrawableLinks": sum(
            r["kind"].startswith("Drawable") and r["targetOffset"] is not None for r in placement["bindings"]
        ),
        "boundLinks": sum(r["kind"] == "BoundPointer" and r["targetOffset"] is not None for r in placement["bindings"]),
        "hierarchyAnomalies": [a for n in decoded["nodes"] for a in n.get("hierarchyAnomalies", [])],
        "sourceFieldsAndArraysPreserved": True,
        "nativeCodeExecuted": False,
    }
    if "hierarchyConversion" in placement:
        result["groupHierarchyConversion"] = {
            "lods": len(plan["lods"]),
            "groups": sum(r["groups"] for r in plan["lods"]),
            "changedLods": sum(r["changed"] for r in plan["lods"]),
            "changedFields": len(plan["fieldChanges"]),
            "nativeContiguousHierarchyVerified": not result["hierarchyAnomalies"],
            "childOrderPreserved": True,
            "glassIdentifiersPreserved": True,
            "sourceRelationshipsRecovered": True,
        }
    return result


def shifted(placement: dict, delta: int) -> dict:
    return placement | {
        "rootOffset": None if placement["rootOffset"] is None else placement["rootOffset"] + delta,
        "nodes": [r | {"systemOffset": r["systemOffset"] + delta} for r in placement["nodes"]],
        "bindings": [
            r
            | {
                "offset": r["offset"] + delta,
                "targetOffset": None if r["targetOffset"] is None else r["targetOffset"] + delta,
            }
            for r in placement["bindings"]
        ],
    }

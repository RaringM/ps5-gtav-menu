"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import copy

from gtavmenu_tools.asset_formats import AssetError


def native_semantics(source):
    """Derive a stable permutation and all changed typed fields from source links."""
    result = copy.deepcopy(source)
    nodes = result["nodes"]
    changed_fields, changed_tables, lods = {}, {}, []

    def number(node, name):
        return int.from_bytes(bytes.fromhex(node["fields"][name]), "little")

    def elements(index):
        row = source["nodes"][index]
        if set(row["links"]) != {str(i) for i in range(row["count"])}:
            raise AssetError("physics permutation pointer-table coverage differs")
        return [row["links"][str(i)] for i in range(row["count"])]

    def field(index, name, value):
        old = source["nodes"][index]["fields"][name]
        new = value.to_bytes(len(bytes.fromhex(old)), "little").hex()
        key = (index, name)
        if key in changed_fields and changed_fields[key] != new:
            raise AssetError("shared physics group has incompatible index mappings")
        changed_fields[key] = new
        nodes[index]["fields"][name] = new

    def table(index, order):
        old = source["nodes"][index]["links"]
        new = {str(k): old[str(i)] for k, i in enumerate(order)}
        if index in changed_tables and changed_tables[index] != new:
            raise AssetError("shared physics table has incompatible index mappings")
        changed_tables[index] = new
        nodes[index]["links"] = new

    for lod_index, lod in enumerate(source["nodes"]):
        if lod["kind"] != "lod":
            continue
        group_table, name_table, child_table = (
            lod["links"][n] for n in ("GroupsPointer", "GroupNamesPointer", "ChildrenPointer")
        )
        groups = elements(group_table)
        names = elements(name_table)
        children = elements(child_table)
        if len(set(groups)) != len(groups) or len(names) != len(groups) or len(set(names)) != len(names):
            raise AssetError("physics group/name alias profile cannot be permuted")
        parents = [number(source["nodes"][g], "ParentIndex") for g in groups]
        roots = [i for i, p in enumerate(parents) if p == 255]
        if not roots or len(roots) != number(lod, "RootGroupsCount"):
            raise AssetError("physics permutation root coverage differs")
        order = list(roots)
        for index in order:
            direct = [i for i, p in enumerate(parents) if p == index]
            if any(i in order for i in direct):
                raise AssetError("physics permutation hierarchy is cyclic")
            order.extend(direct)
        if sorted(order) != list(range(len(groups))):
            raise AssetError("physics permutation hierarchy is disconnected")
        # Preserve already valid native ordering, including valid non-BFS trees.
        if not lod["hierarchyAnomalies"]:
            order = list(range(len(groups)))
        inverse = {old: new for new, old in enumerate(order)}
        table(group_table, order)
        table(name_table, order)
        for old, g in enumerate(groups):
            direct = sorted(inverse[i] for i, p in enumerate(parents) if p == old)
            if direct and direct != list(range(direct[0], direct[0] + len(direct))):
                raise AssetError("physics permutation did not form contiguous child groups")
            if len(direct) != number(source["nodes"][g], "ChildGroupCount"):
                raise AssetError("physics permutation changes child-group count")
            field(g, "ParentIndex", 255 if parents[old] == 255 else inverse[parents[old]])
            field(g, "ChildGroupIndex", direct[0] if direct else number(source["nodes"][g], "ChildGroupIndex"))
        for child in children:
            old = number(source["nodes"][child], "GroupIndex")
            if old not in inverse:
                raise AssetError("physics permutation child references an absent group")
            field(child, "GroupIndex", inverse[old])
        nodes[lod_index]["hierarchyAnomalies"] = []
        lods.append(
            {
                "lodNode": lod_index,
                "groupTableNode": group_table,
                "nameTableNode": name_table,
                "childTableNode": child_table,
                "newToOld": order,
                "oldToNew": [inverse[i] for i in range(len(groups))],
                "groups": len(groups),
                "children": len(children),
                "changed": order != list(range(len(groups))),
            }
        )
    changes = [
        {"node": index, "field": name, "before": source["nodes"][index]["fields"][name], "after": value}
        for (index, name), value in sorted(changed_fields.items())
        if source["nodes"][index]["fields"][name] != value
    ]
    return result, {
        "lods": lods,
        "fieldChanges": changes,
        "childOrderPreserved": True,
        "glassIdentifiersPreserved": True,
    }


def canonical_nodes(decoded, placement):
    """Restore logical node identity after reading permuted pointer tables."""
    base = placement["sourceBase"]
    expected = {row["systemOffset"]: i for i, row in enumerate(placement["nodes"])}
    offsets = [n["sourcePointer"] - base for n in decoded["nodes"]]
    if len(expected) != len(offsets) or set(expected) != set(offsets):
        raise AssetError("physics permuted placement node coverage differs")
    indices = {i: expected[off] for i, off in enumerate(offsets)}
    nodes = [None] * len(offsets)
    for old, node in enumerate(decoded["nodes"]):
        index = indices[old]
        if node["kind"] != placement["nodes"][index]["kind"]:
            raise AssetError("physics permuted placement node kind differs")
        nodes[index] = node | {"links": {k: None if v is None else indices[v] for k, v in node["links"].items()}}
    return {"nodes": nodes, "rootIndex": indices[decoded["rootIndex"]]}

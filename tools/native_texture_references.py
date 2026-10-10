"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import re
import struct

import gen9_materials as materials
import gen9_mesh as mesh
from gtavmenu_tools.asset_formats import AssetError
from gtavmenu_tools.hashes import joaat
from native_resource_builder import ObjectArena, relocate


def name_at(system: bytes, pointer: int, base: int) -> str:
    at = pointer - base
    if not 0 <= at < len(system):
        raise AssetError("texture reference name pointer leaves system storage")
    end = system.find(b"\0", at, min(at + 256, len(system)))
    if end < 0:
        raise AssetError("texture reference name has no bounded terminator")
    name = system[at:end].decode("ascii")
    if not re.fullmatch(r"[A-Za-z0-9_]+", name):
        raise AssetError("texture reference name leaves the admitted ASCII identifier domain")
    return name


def native_read(system: bytes, base: int, pointers: list[int], public: dict, native: dict) -> list[dict]:
    expected, result = prototype(public, native), []
    for pointer in sorted(set(pointers)):
        at = pointer - base
        if pointer % 16 or not 0 <= at <= len(system) - public["native"]["bytes"]:
            raise AssetError("texture reference object pointer is unaligned or out of bounds")
        fields = materials.get(system, at, public["native"])
        name = name_at(system, fields["NamePointer"], base)
        observed = bytearray(system[at : at + public["native"]["bytes"]])
        observed[:8] = bytes(8)
        materials.put(observed, public["native"], "NamePointer", 0)
        if bytes(observed) != expected:
            raise AssetError("texture reference fields differ from the derived finite native profile")
        result.append({"systemOffset": at, "name": name, "nameHash": f"0x{joaat(name.lower()):08x}", "fields": fields})
    return result


def source_read(system: bytes, base: int, material: dict, links: list[dict], public: dict) -> list[dict]:
    sources, result, sites = {}, [], set()
    for shader in material["shaders"]:
        for param in shader["textureParameters"]:
            if param["textureName"] is not None:
                sources[(shader["index"], param["index"])] = param
    for link in links:
        key = (link["shaderIndex"], link["sourceIndex"])
        if key not in sources or key in sites:
            raise AssetError("texture reference source site is absent or duplicated")
        sites.add(key)
        src = sources[key]
        at = src["textureSystemOffset"]
        if at % 16 or int(src["dataPointer"], 0) != base + at:
            raise AssetError("texture reference source pointer/offset disagrees")
        fields = materials.get(system, at, public["legacy"])
        name = name_at(system, fields["NamePointer"], base)
        if (
            name != src["textureName"]
            or name != link["textureName"]
            or f"0x{joaat(name.lower()):08x}" != link["textureNameHash"]
        ):
            raise AssetError("texture reference source name/hash differs from material site")
        handled = {"VFT", "Unknown_4h", "NamePointer", "Unknown_30h", "Unknown_32h"}
        if (
            fields["Unknown_30h"] != public["legacyReferenceCount"]
            or fields["Unknown_32h"] != public["legacyReferenceTag"]
            or any(v for n, v in fields.items() if n not in handled)
        ):
            raise AssetError("texture reference source is not the zero-metadata Legacy name profile")
        result.append(
            {
                "sourceOffset": at,
                "name": name,
                "nameHash": link["textureNameHash"],
                "fields": fields,
                "sourceHeaderSha256": mesh.digest(system[at : at + public["legacy"]["bytes"]]),
                "link": link,
            }
        )
    if sites != sources.keys():
        raise AssetError("texture reference conversion does not cover all nonnull source sites")
    return result


def verify(blob: bytes, graph: dict, public: dict, native: dict) -> dict:
    metadata, base = graph["textureReferences"], graph["sourceBase"]
    if not 0 < metadata["componentBytes"] <= metadata["bytes"] <= len(blob):
        raise AssetError("texture reference component extents are invalid")
    pointers, seen, restored = [], set(), bytearray(blob[: metadata["componentBytes"]])
    if "vehicleResource" in graph:
        slot = graph["fragment"]["pageInfoPointerOffset"]
        if struct.unpack_from("<Q", blob, slot)[0] != base + graph["vehicleResource"]["pageInfoOffset"]:
            raise AssetError("texture reference enclosing page-info pointer changed")
        restored[slot : slot + 8] = bytes(8)
    for row in metadata["sites"]:
        link, target = row["link"], row["targetOffset"]
        slot = link["offset"]
        if slot in seen or not 0 <= slot <= metadata["componentBytes"] - 8 or target < metadata["componentBytes"]:
            raise AssetError("texture reference site is duplicated or outside its component")
        seen.add(slot)
        value = struct.unpack_from("<Q", blob, slot)[0]
        if value != base + target:
            raise AssetError("texture reference shader pointer differs from its owned object")
        pointers.append(value)
        restored[slot : slot + 8] = bytes(8)
    if [r["link"] for r in metadata["sites"]] != graph["externalTextureLinks"]:
        raise AssetError("texture reference symbolic dependency coverage changed")
    if mesh.digest(bytes(restored)) != metadata["componentSha256"]:
        raise AssetError("texture reference composition changed the fragment preimage")
    decoded = native_read(blob, base, pointers, public, native)
    by_at = {r["systemOffset"]: r for r in decoded}
    source_objects, cursor = {}, metadata["componentBytes"]
    fixups = {r["offset"]: r["targetOffset"] for r in graph["pointerSlots"]}
    for row in metadata["sites"]:
        actual = by_at[row["targetOffset"]]
        if actual["name"] != row["name"] or actual["nameHash"] != row["nameHash"]:
            raise AssetError("texture reference serialized name differs from source")
        if actual["fields"]["VFT"] or actual["fields"]["Unknown_4h"]:
            raise AssetError("texture reference serialized class placeholder is nonzero")
        source_at = row["sourceOffset"]
        if source_at in source_objects:
            if source_objects[source_at] != (row["targetOffset"], row["sourceHeaderSha256"], row["name"]):
                raise AssetError("texture reference source alias identity changed")
            continue
        source_objects[source_at] = (row["targetOffset"], row["sourceHeaderSha256"], row["name"])
        at = (cursor + 15) & -16
        name_offset = at + public["native"]["bytes"]
        end = name_offset + len(row["name"]) + 1
        if (
            row["targetOffset"] != at
            or actual["fields"]["NamePointer"] != base + name_offset
            or end > metadata["bytes"]
            or any(blob[cursor:at])
        ):
            raise AssetError("texture reference owned placement or padding changed")
        for key, target in (("NamePointer", name_offset), ("G9_SRVPointer", None), ("DataPointer", None)):
            slot = at + public["native"]["fields"][key]["offset"]
            if slot not in fixups or fixups[slot] != target:
                raise AssetError("texture reference pointer ownership differs")
        for offset, size, alignment in ((at, public["native"]["bytes"], 16), (name_offset, end - name_offset, 1)):
            if (
                sum(b == {"offset": offset, "bytes": size, "alignment": alignment} for b in graph["blocks"].values())
                != 1
            ):
                raise AssetError("texture reference object/name block ownership differs")
        cursor = end
    if cursor != metadata["bytes"] or len(source_objects) != len(decoded):
        raise AssetError("texture reference appended bytes or object coverage differs")
    return {
        "boundShaderSites": len(pointers),
        "referenceObjects": len(decoded),
        "uniqueNames": len({r["name"] for r in decoded}),
        "componentPreimageRecovered": True,
        "serializedNameReferencesAvailable": True,
        "textureDependenciesResolved": False,
        "lookupServiceExecuted": False,
        "runtimeSemanticsQualified": False,
    }


def build(blob: bytes, graph: dict, system: bytes, source_base: int, material: dict, public: dict, native: dict):
    if "textureReferences" in graph or graph.get("resourcePagesResolved"):
        raise AssetError("texture references require an unresolved fragment component")
    source = source_read(system, source_base, material, graph["externalTextureLinks"], public)
    arena = ObjectArena()
    if arena.include("fragment", blob, graph) != 0:
        raise AssetError("texture reference composition moved the fragment root")
    objects, sites = {}, []
    for row in source:
        source_at = row["sourceOffset"]
        if source_at not in objects:
            prefix = f"texture-reference-{len(objects)}"
            at = arena.add(prefix, prototype(public, native))
            name = arena.add(prefix + ".name", row["name"].encode("ascii") + b"\0", 1)
            for key, target in (("NamePointer", name), ("G9_SRVPointer", None), ("DataPointer", None)):
                arena.pointer(at + public["native"]["fields"][key]["offset"], target)
            objects[source_at] = at
        target = objects[source_at]
        arena.bind(row["link"]["offset"], target)
        sites.append(row | {"targetOffset": target})
    output, placed = arena.render(graph["sourceBase"])
    result = (
        graph
        | placed
        | {
            "textureReferences": {
                "componentBytes": len(blob),
                "componentSha256": mesh.digest(blob),
                "bytes": len(output),
                "sites": sites,
            },
            "serializedTextureReferencesAvailable": True,
            "textureDependenciesResolved": False,
        }
    )
    relocate(output, result, result["sourceBase"])
    verify(output, result, public, native)
    return output, result


def prototype(public: dict, native: dict) -> bytes:
    if public["native"]["bytes"] != native["objectBytes"]:
        raise AssetError("texture reference object size differs from reviewed layout")
    data = bytearray(native["objectBytes"])
    for name, value in native["fields"].items():
        materials.put(data, public["native"], name, value)
    return bytes(data)

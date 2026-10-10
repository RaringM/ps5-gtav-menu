"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import struct

import pc_drawable_geometry as geometry
import pc_mesh_buffers as mesh
import pc_physics_damage as physics
import pc_skeleton_bindings as skeleton
import pc_skinning_bindings as skinning
from gtavmenu_tools.asset_formats import AssetError, Limits
from gtavmenu_tools.asset_textures import ResourceView


def inspect(
    payload: bytes,
    header: dict,
    material: dict,
    source_skeleton: dict,
    contracts: dict,
    child_contract: dict,
    limits: Limits,
) -> dict:
    view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
    group_at = child_contract["physics"]["layout"]["fragmentPhysicsLodGroupPointer"]
    if struct.unpack_from("<Q", view.system, group_at)[0] == 0:
        # Mod-kit part fragments (vehiclemods/<model>_*.yft) carry no physics LOD group: no
        # physics children, no child drawables. Everything else still goes through its checks.
        physics_data = {"absent": True, "lods": [], "counts": {"children": 0, "groups": 0}}
    else:
        physics_data = physics.inspect_payload(payload, header, child_contract["physics"], limits)
    gc, mc, sc = (contracts[name] for name in ("geometry", "mesh", "skeleton"))
    parent_bones = source_skeleton["bones"]
    tags = {bone["Tag"]: index for index, bone in enumerate(parent_bones)}
    if len(tags) != len(parent_bones):
        raise AssetError("child skeleton tag lookup is ambiguous")
    occurrences, drawables = [], {}
    visited = 0
    for lod in physics_data["lods"]:
        if lod is None:
            continue
        for child in lod["children"]:
            if child["boneTag"] not in tags:
                raise AssetError("physics child bone tag is absent from the shared skeleton")
            for field in ("drawable1Pointer", "drawable2Pointer"):
                pointer = int(child[field], 0)
                link = {
                    "physicsLod": lod["index"],
                    "childIndex": child["index"],
                    "field": field,
                    "boneTag": child["boneTag"],
                    "boneIndex": tags[child["boneTag"]],
                    "drawablePointer": pointer,
                    "present": bool(pointer),
                }
                occurrences.append(link)
                if not pointer or pointer in drawables:
                    continue
                if len(drawables) >= limits.max_entries or pointer % 16:
                    raise AssetError("child drawable count/alignment exceeds supported limits")
                at = view.offset(pointer, gc["fragmentDrawableBytes"])
                shader_offset = gc["drawable"]["fields"]["ShaderGroupPointer"]["offset"]
                shader_pointer = struct.unpack_from("<Q", payload, at + shader_offset)[0]
                if shader_pointer != 0:
                    raise AssetError("child has its own shader group; selected inherited-material path refuses it")
                context = {"root": material["root"], "drawable": {"systemOffset": at}, "shaders": material["shaders"]}
                geom = geometry.inspect(payload, header, context, gc, limits)
                buffers = mesh.inspect(payload, header, geom, gc, mc, limits)
                if buffers["rejections"]:
                    codes = sorted({row["code"] for row in buffers["rejections"]})
                    raise AssetError(
                        f"physics-child mesh topology has rejected geometry ({', '.join(codes)}; "
                        "triangle-declaration-unqualified: --repair triangle-counts)"
                    )
                skel = skeleton.inspect(payload, header, geom, buffers, gc, sc, limits)
                if skel["rejections"] or skel["skeletonPointer"] != source_skeleton["skeletonPointer"]:
                    raise AssetError("child skeleton does not reference the verified shared skeleton")
                if any(
                    skel[key] != source_skeleton[key]
                    for key in ("header", "bones", "parents", "childIndices", "tagNodes")
                ):
                    raise AssetError("child skeleton decoding differs from shared skeleton")
                models = {(row["lod"], row["modelIndex"]): row for row in skel["models"]}
                skin_rows = []
                for geom_buffer in buffers["geometries"]:
                    model = models[(geom_buffer["lod"], geom_buffer["modelIndex"])]
                    streams = []
                    if model["HasSkin"]:
                        for stream in geom_buffer["vertexStreams"]:
                            visited += stream["bytes"]
                            if visited > limits.max_total_bytes:
                                raise AssetError("child skinning stream work budget exceeded")
                            start = stream["systemOffset"]
                            raw = view.system[start : start + stream["bytes"]]
                            streams.append(
                                {
                                    "field": stream["field"],
                                    **skinning.inspect_stream(
                                        raw, geom_buffer, len(parent_bones), contracts["skinning"], limits
                                    ),
                                }
                            )
                    skin_rows.append(
                        {
                            "lod": geom_buffer["lod"],
                            "modelIndex": geom_buffer["modelIndex"],
                            "geometryIndex": geom_buffer["geometryIndex"],
                            "modelBinding": model,
                            "streams": streams,
                        }
                    )
                drawables[pointer] = {
                    "pointer": pointer,
                    "sourceSkeletonPointer": skel["skeletonPointer"],
                    "shaderOwnership": "main-drawable-group-by-public-fragment-consumer",
                    "geometry": geom,
                    "mesh": buffers,
                    "skinning": skin_rows,
                }
    rows = list(drawables.values())
    geometries = [g for row in rows for g in row["mesh"]["geometries"]]
    return {
        "physics": physics_data,
        "links": occurrences,
        "drawables": rows,
        "summary": {
            "physicsChildCount": physics_data["counts"]["children"],
            "drawableOccurrences": sum(row["present"] for row in occurrences),
            "nullDrawableSlots": sum(not row["present"] for row in occurrences),
            "uniqueDrawables": len(rows),
            "sharedSkeletonDrawables": len(rows),
            "geometryOccurrences": len(geometries),
            "vertexOccurrences": sum(g["vertexCount"] for g in geometries),
            "indexOccurrences": sum(g["indexCount"] for g in geometries),
        },
        "remaining": [
            "Child drawable extension matrices, bounds, event sets and archetypes require further decoding",
            "Native material/geometry/physics serialization and final fragment references remain unimplemented",
        ],
    }

#!/usr/bin/env python3
"""Inspect vehicle geometry, skeletons, skin bindings and pose with public format data."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

import inspect_pc_vehicle_materials as material_runner
import pc_drawable_geometry as geometry
import pc_mesh_buffers as mesh
import pc_mesh_draw_context as draw_context
import pc_skeleton_bindings as skeleton
import pc_skinning_bindings as skinning
import pc_transforms_pose as transforms
from gtavmenu_tools import drawable_contracts, vehicle_contracts, vehicle_materials
from gtavmenu_tools.asset_formats import AssetError, Limits, decode_resource
from gtavmenu_tools.asset_textures import SYSTEM_BASE, ResourceView
from gtavmenu_tools.assets import Budget, read_file
from gtavmenu_tools.io import sha256_file


def tool_hashes():
    return (
        material_runner.tool_hashes()
        | {
            name: sha256_file(Path(module.__file__))
            for name, module in (
                ("vehiclePoseInspector", __import__(__name__)),
                ("geometryReader", geometry),
                ("meshReader", mesh),
                ("meshDrawContext", draw_context),
                ("skeletonReader", skeleton),
                ("skinningReader", skinning),
                ("poseReader", transforms),
            )
        }
        | {
            "vehicleContracts": sha256_file(vehicle_contracts.DEFAULT),
            "drawableContracts": sha256_file(drawable_contracts.DEFAULT),
        }
    )


def inspect_skinning(payload, header, observed_mesh, observed_skeleton, contract, budget):
    """Replay every stream occurrence, retaining the strict palette and weight checks."""
    if observed_skeleton["state"] != "selected-skeleton-relationships-verified":
        raise AssetError("skinning requires freshly verified local skeleton relationships")
    view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
    models = {(m["lod"], m["modelIndex"]): m for m in observed_skeleton["models"]}
    if len(models) != len(observed_skeleton["models"]):
        raise AssetError("skinning duplicate model keys")
    rows = []
    for geom in observed_mesh["geometries"]:
        key = (geom["lod"], geom["modelIndex"])
        if key not in models or models[key]["HasSkin"] != 1:
            raise AssetError("skinning selected geometry lacks the verified skin-model binding")
        streams = []
        for stream in geom["vertexStreams"]:
            budget.charge(stream["bytes"])
            at = view.offset(SYSTEM_BASE + stream["systemOffset"], stream["bytes"])
            raw = view.system[at : at + stream["bytes"]]
            if hashlib.sha256(raw).hexdigest() != stream["sha256"]:
                raise AssetError("skinning stream differs from freshly verified mesh")
            result = skinning.inspect_stream(raw, geom, len(observed_skeleton["bones"]), contract, budget.limits)
            streams.append({key: stream[key] for key in ("field", "systemOffset", "bytes", "sha256")} | result)
        rows.append(
            {
                key: geom[key]
                for key in ("lod", "modelIndex", "geometryIndex", "declarationFlags", "declarationTypes", "boneIds")
            }
            | {"streams": streams}
        )
    return {"geometries": rows}


def inspect(args, stage="pose"):
    """Read each source resource once and build the checked dependency reports.

    The optional mesh stage is used before repairing malformed skeletons. It has
    the same geometry and mesh admission checks and never skips a requested stage.
    """
    if stage not in {"mesh", "pose"}:
        raise AssetError("vehicle inspection stage must be mesh or pose")
    before = tool_hashes()
    limits = Limits()
    budget, stream_budget = Budget(limits), Budget(limits)
    source = material_runner.open_pc_source(args, budget)
    prior_blob = read_file(args.material_report, budget)
    prior = json.loads(prior_blob)
    if (
        prior.get("kind") != "gtavmenu-pc-legacy-drawable-material-binding-observation"
        or prior.get("schemaVersion") != 2
        or prior.get("tools") != material_runner.tool_hashes()
    ):
        raise AssetError("vehicle material report kind, schema or tool identities are stale")
    if not material_runner.source_matches_report(source, prior["sources"]):
        raise AssetError("vehicle source differs from its material report")
    public, formats = drawable_contracts.load(), vehicle_contracts.load()["pc"]
    material = vehicle_materials.material_contract()
    if {key: value for key, value in material.items() if key != "parameterNames"} != prior["pcLegacyContract"]:
        raise AssetError("vehicle material contract differs from reviewed public layouts")
    sources = material_runner.source_evidence(source) | {"materialReportSha256": hashlib.sha256(prior_blob).hexdigest()}
    identities = {row["member"]: row for row in prior["sources"]["resources"] if row["kind"] == "fragment"}
    members = [row["member"] for row in prior["resources"]]
    if not members or len(members) != len(set(members)) or set(members) != set(identities):
        raise AssetError("vehicle material resource set is empty, duplicated or incomplete")
    geometry_rows, mesh_rows, skeleton_rows, skin_rows, pose_rows = [], [], [], [], []
    for old in prior["resources"]:
        member = old["member"]
        blob = material_runner.source_resource(source, member, budget, ".yft")
        header, payload = decode_resource(blob, limits.max_file_bytes)
        budget.charge(len(payload))
        identity = {
            "member": member,
            "resourceSha256": hashlib.sha256(blob).hexdigest(),
            "payloadSha256": hashlib.sha256(payload).hexdigest(),
        }
        if any(identity[key] != identities[member][key] for key in ("resourceSha256", "payloadSha256")):
            raise AssetError("vehicle resource differs from its material evidence")
        fresh = vehicle_materials.inspect_legacy_yft(payload, header, material, limits) | {"member": member}
        if fresh != old:
            raise AssetError("vehicle material evidence differs from freshly decoded source")
        try:
            geom = identity | geometry.inspect(payload, header, fresh, public["geometry"], limits)
            meshed = identity | mesh.inspect(payload, header, geom, public["geometry"], public["pcMesh"], limits)
            meshed["publicRendererContext"] = draw_context.inspect(
                meshed["geometries"], fresh["shaders"], formats["meshDrawContext"]
            )
            geometry_rows.append(geom)
            mesh_rows.append(meshed)
            if stage == "mesh":
                continue
            skel = identity | skeleton.inspect(
                payload, header, geom, meshed, public["geometry"], formats["skeleton"], limits
            )
            skeleton_rows.append(skel)
            skin_rows.append(
                identity | inspect_skinning(payload, header, meshed, skel, formats["skinning"], stream_budget)
            )
            pose_rows.append(
                identity
                | transforms.inspect(payload, header, skel, meshed, formats["skeleton"], formats["transform"], limits)
            )
        except AssetError as exc:
            raise AssetError(f"{member}: {exc}") from exc
    material_runner.verify_pc_source(source)
    if tool_hashes() != before or sha256_file(args.material_report) != sources["materialReportSha256"]:
        raise AssetError("vehicle inspection tool or input changed during inspection")
    common = {"schemaVersion": 2, "tools": before, "sources": sources}
    geometry_report = common | {
        "kind": "gtavmenu-pc-legacy-geometry-shader-bindings",
        "pcLegacyGeometryContract": public["geometry"],
        "resources": geometry_rows,
    }
    rejected_mesh = sum(row["state"] == "rejected" for row in mesh_rows)
    mesh_report = common | {
        "kind": "gtavmenu-pc-legacy-mesh-buffer-observation",
        "pcLegacyMeshContract": public["pcMesh"],
        "publicRendererContract": formats["meshDrawContext"],
        "freshGeometryBindings": geometry_report,
        "resources": mesh_rows,
        "summary": {"resourceCount": len(mesh_rows), "rejectedResourceCount": rejected_mesh},
    }
    if stage == "mesh":
        return mesh_report
    skeleton_report = common | {
        "kind": "gtavmenu-pc-legacy-skeleton-relationships",
        "pcSkeletonContract": formats["skeleton"],
        "freshMeshEvidence": mesh_report,
        "resources": skeleton_rows,
        "summary": {"resourceCount": len(skeleton_rows), "upstreamMeshRejectedResources": rejected_mesh},
    }
    skin_report = common | {
        "kind": "gtavmenu-pc-packed-skin-bindings",
        "pcSkinningContract": formats["skinning"],
        "freshSkeletonEvidence": skeleton_report,
        "resources": skin_rows,
        "summary": {"resourceCount": len(skin_rows), "upstreamMeshRejectedResources": rejected_mesh},
    }
    return common | {
        "kind": "gtavmenu-pc-transform-array-pose-observation",
        "pcTransformContract": formats["transform"],
        "freshSkinningEvidence": skin_report,
        "resources": pose_rows,
        "summary": {
            "resourceCount": len(pose_rows),
            "rejectedPoseResources": sum(bool(row["rejections"]) for row in pose_rows),
            "rejectedPoseBones": sum(len(row["rejections"]) for row in pose_rows),
            "upstreamMeshRejectedResources": rejected_mesh,
        },
    }

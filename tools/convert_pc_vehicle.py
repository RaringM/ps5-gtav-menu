#!/usr/bin/env python3
"""Build vehicle component graphs from a verified PC source and runtime retail templates.

Only reviewed field layouts and verified user-supplied resources are consumed.
Every emitted component is independently read back, including relocated copies.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import gen9_materials as native_materials  # noqa: E402
import gen9_models as native_models  # noqa: E402
import inspect_pc_vehicle_materials as material_runner  # noqa: E402
import inspect_pc_vehicle_pose as pose_runner  # noqa: E402
import native_collision  # noqa: E402
import native_collision_ownership  # noqa: E402
import native_drawable_geometry  # noqa: E402
import native_drawables  # noqa: E402
import native_fragment  # noqa: E402
import native_mesh  # noqa: E402
import native_physics  # noqa: E402
import native_resource_builder  # noqa: E402
import native_skeleton  # noqa: E402
import native_texture_references  # noqa: E402
import native_vehicle_resource  # noqa: E402
import pc_fragment_children  # noqa: E402
import shader_constant_defaults  # noqa: E402
import shader_database  # noqa: E402
import shader_texture_policies  # noqa: E402
import vehicle_material_checks  # noqa: E402
from gtavmenu_tools import drawable_contracts, vehicle_contracts, vehicle_templates  # noqa: E402
from gtavmenu_tools.asset_formats import AssetError, Limits, decode_resource  # noqa: E402
from gtavmenu_tools.assets import Budget, read_file  # noqa: E402
from gtavmenu_tools.host_paths import assets_dir, build_dir  # noqa: E402
from gtavmenu_tools.io import sha256_file  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
TARGET_ID = "PPSA04264_01.010.002_DISC"
REFERENCE_CACHES = (
    "corpus/tornado6.pft",
    "corpus/banshee-a.pft",
    "corpus/decal-schema-a.pft",
    "corpus/vigero.pft",
    "corpus/trailersmall.pft",
    "corpus/trailers3.pft",
    "corpus/buc2_dl_camo2.pft",
    "corpus/cheb_skirts.pft",
    "corpus/nemesis.pft",
    "corpus/armytrailer_hi.pft",
)
NATIVE_EXECUTION_NOT_PERFORMED = {
    "skipped": True,
    "reason": "public conversion performs bounded serialized-data readback; no native execution",
}
STAGES = (
    "skeletons",
    "meshes",
    "models",
    "materials",
    "drawables",
    "collision",
    "physics",
    "fragment",
    "resources",
    "references",
)


def canonical(value):
    return (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + "\n").encode()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def tool_hashes():
    modules = (
        native_materials,
        native_models,
        native_collision,
        native_collision_ownership,
        native_drawable_geometry,
        native_drawables,
        native_fragment,
        native_mesh,
        native_physics,
        native_resource_builder,
        native_skeleton,
        native_texture_references,
        native_vehicle_resource,
        pc_fragment_children,
        shader_constant_defaults,
        shader_database,
        shader_texture_policies,
        vehicle_material_checks,
        vehicle_templates,
    )
    return (
        pose_runner.tool_hashes()
        | {module.__name__: sha256_file(Path(module.__file__)) for module in modules}
        | {"vehicleConverter": sha256_file(Path(__file__))}
    )


def template_references(args, templates, main):
    paths = [main, *(getattr(args, "shader_reference", None) or [templates / cache for cache in REFERENCE_CACHES[1:]])]
    seen = set()
    result = []
    for path in paths:
        cache = "corpus/" + path.name
        if cache not in REFERENCE_CACHES or cache in seen:
            raise AssetError("native shader reference is duplicated or outside the reviewed vehicle corpus")
        seen.add(cache)
        result.append((path, vehicle_templates.verified_input(path, cache)))
    if not set(REFERENCE_CACHES[:3]) <= seen:
        raise AssetError("materials requires the pinned primary vehicle shader corpus")
    return result


def build_skeletons(args):
    stage = getattr(args, "stage", "references")
    if stage not in STAGES:
        raise AssetError("unsupported vehicle conversion stage")
    rank = STAGES.index(stage)
    include_meshes, include_models, include_materials = rank >= 1, rank >= 2, rank >= 3
    include_drawables, include_collision, include_physics = rank >= 4, rank >= 5, rank >= 6
    include_fragment, include_resources, include_references = rank >= 7, rank >= 8, rank >= 9
    before = tool_hashes()
    formats, public = vehicle_contracts.load(), drawable_contracts.load()
    pc_formats, native_formats = formats["pc"], formats["native"]
    target_blob = read_file(args.target, Budget(Limits()))
    vehicle_contracts.check_target(json.loads(target_blob), formats)
    templates = getattr(args, "templates", None) or build_dir(ROOT) / "retail-templates"
    native_path = getattr(args, "native_reference", None) or templates / REFERENCE_CACHES[0]
    accepted = vehicle_templates.geometry_observation(native_path)
    input_paths = {"target": args.target, "nativeFragment": native_path}
    identities = {name: sha256_file(path) for name, path in input_paths.items()}
    if identities["nativeFragment"] != accepted["sources"]["nativeFragment"]:
        raise AssetError("native fragment changed during template observation")
    pc = pose_runner.inspect(args)
    skeletons = pc["freshSkinningEvidence"]["freshSkeletonEvidence"]
    sc, tc = pc_formats["skeleton"], pc_formats["transform"]
    child_contract = pc_formats["children"]
    contracts = {
        "geometry": public["geometry"],
        "mesh": public["pcMesh"],
        "skeleton": sc,
        "skinning": pc_formats["skinning"],
    }
    budget = Budget(Limits())
    source = material_runner.open_pc_source(args, budget)
    if not material_runner.source_matches_report(source, pc["sources"]):
        raise AssetError("child fixture changed after skeleton reproduction")
    material_report_blob = read_file(args.material_report, budget)
    if digest(material_report_blob) != pc["sources"]["materialReportSha256"]:
        raise AssetError("child material evidence changed after skeleton reproduction")
    materials = {row["member"]: row for row in json.loads(material_report_blob)["resources"]}
    geometry, native = native_formats["geometry"], native_formats["skeleton"]
    base = accepted["freshFragmentEvidence"]["contract"]["rootPlacement"]["sourceBase"]
    reference_blob = vehicle_templates.verified_input(native_path, REFERENCE_CACHES[0])
    if digest(reference_blob) != identities["nativeFragment"]:
        raise AssetError("native reference changed during component conversion")
    header, payload = decode_resource(reference_blob, 32 * 1024 * 1024)
    expected = accepted["freshFragmentEvidence"]["nativeReference"]
    if header != expected["header"] or digest(payload) != expected["payloadSha256"]:
        raise AssetError("native reference decoding differs from fresh observation")
    at = expected["observation"]["drawableSystemOffset"] + native["drawableSkeletonOffset"]
    root = struct.unpack_from("<Q", payload, at)[0] - base
    retail_skeleton, retail_pose = native_skeleton.reference_read(payload[: header["systemBytes"]], root, base, sc, tc)
    if retail_skeleton["rejections"] or retail_pose["rejections"]:
        raise AssetError("native retail skeleton fails the shared layout/pose checks")
    mesh_contract, native_mesh_contract = public["mesh"], native_formats["mesh"]
    model_contract = public["model"]
    material_contract, native_material_contract = public["shader"], native_formats["material"]
    drawable_contract, collision_contract = pc_formats["drawable"], pc_formats["collision"]
    physics_contract, native_physics_contract = pc_formats["physics"], native_formats["physics"]
    fragment_contract = pc_formats["fragment"]
    resource_contract, native_resource_contract = pc_formats["resource"], native_formats["resource"]
    reference_contract, native_reference_contract = pc_formats["textureReference"], native_formats["textureReference"]
    resource_numeric_checks = {}
    mesh_reference = None
    model_reference = {"modelCount": 0, "boundsCount": 0, "geometryCount": 0, "nativeCodeExecuted": False}
    if include_meshes:
        observation = native_drawable_geometry.read_geometry(
            payload[: header["systemBytes"]],
            base,
            expected["observation"]["drawableSystemOffset"],
            expected["observation"] | {"drawableObservationBytes": public["geometry"]["fragmentDrawableBytes"]},
            geometry,
        )
        leaves = [g for slot in observation["slots"] for model in slot["models"] for g in model["geometries"]]
        mesh_reference = native_mesh.retail_check(
            payload[: header["systemBytes"]],
            payload[header["systemBytes"] :],
            accepted["freshFragmentEvidence"]["pages"]["sourceBases"],
            leaves,
            mesh_contract,
            native_mesh_contract,
        )
        if include_models:
            for slot in observation["slots"]:
                for model in slot["models"]:
                    decoded = native_models.read(
                        payload[: header["systemBytes"]], base, model["systemOffset"], model_contract
                    )
                    if decoded["geometryPointers"] != [base + g["systemOffset"] for g in model["geometries"]]:
                        raise AssetError("native full model reader differs from template geometry links")
                    model_reference["modelCount"] += 1
                    model_reference["boundsCount"] += len(decoded["bounds"])
                    model_reference["geometryCount"] += len(decoded["geometryPointers"])
    material_bank = {}
    reference_checks = []
    shader_database_observations = None
    if include_materials:
        if (
            native_material_contract["drawableShaderGroupOffset"]
            != public["geometry"]["drawable"]["fields"]["ShaderGroupPointer"]["offset"]
        ):
            raise AssetError("native material group root disagrees with public drawable")
        references, supplementary = [], set()
        for path, raw in template_references(args, templates, native_path):
            identity = digest(raw)
            input_paths["nativeShader-" + identity] = path
            identities["nativeShader-" + identity] = identity
            if "corpus/" + path.name in REFERENCE_CACHES[3:]:
                supplementary.add(identity)
            rh, rp = decode_resource(raw, 32 * 1024 * 1024)
            if rh["version"] != header["version"]:
                raise AssetError("native shader reference resource version differs")
            contract = accepted["freshFragmentEvidence"]["contract"]
            offset = contract["rootPlacement"]["sourceOffset"] + contract["link"]["drawableOffset"]
            drawable_at = struct.unpack_from("<Q", rp, offset)[0] - base
            group_slot = drawable_at + native_material_contract["drawableShaderGroupOffset"]
            if drawable_at < 0 or group_slot + 8 > rh["systemBytes"]:
                raise AssetError("native shader reference drawable pointer exceeds system storage")
            group_at = struct.unpack_from("<Q", rp, group_slot)[0] - base
            system = rp[: rh["systemBytes"]]
            check = {"resourceSha256": identity}
            if include_drawables:
                check["drawable"] = native_drawables.semantics(
                    native_drawables.read(system, base, drawable_at, drawable_contract)
                )
            if include_collision:
                roots = native_collision.roots(system, base, {"externalCollisionLinks": []}, collision_contract)
                check["collision"] = native_collision.semantics(
                    native_collision.read(system, base, roots, collision_contract)
                )
            if include_physics:
                check["physics"] = native_physics.semantics(
                    native_physics.read(
                        system, base, native_physics.root_pointer(system, base, physics_contract), physics_contract
                    )
                )
            if include_fragment:
                check["fragment"] = native_fragment.semantics(native_fragment.read(system, base, 0, fragment_contract))
            if include_resources:
                check["resource"] = native_vehicle_resource.reference_check(
                    raw, resource_contract, native_resource_contract
                )
                check["ownership"] = native_collision_ownership.reference_check(
                    system, base, collision_contract, physics_contract, drawable_contract
                )
            rows = native_materials.read_group(system, base, group_at, material_contract)
            references.append((identity, rows))
            if include_references:
                check["textureReferences"] = native_texture_references.native_read(
                    system,
                    base,
                    [p for row in rows for p in row["texturePointers"] if p],
                    reference_contract,
                    native_reference_contract,
                )
            reference_checks.append(check)
        material_bank = native_materials.schema_bank(references, frozenset(supplementary))
        wanted = {str(int(s["nameHash"], 0)) for resource in materials.values() for s in resource["shaders"]}
        if not wanted <= material_bank.keys():
            missing = ", ".join(f"0x{int(k):08x}" for k in sorted(wanted - material_bank.keys(), key=int))
            raise AssetError(f"native shader corpus does not cover every source shader type (name hashes {missing})")
        material_bank = {k: material_bank[k] for k in sorted(wanted)}
        material_contract["mappings"] = {k: material_contract["mappings"][k] for k in sorted(wanted)}
        if include_references:
            shader_database_observations = shader_database.load(
                templates, material_contract, native_formats["shaderDatabase"]
            )
            shader_constant_defaults.attach(
                templates,
                shader_database_observations,
                material_bank,
                material_contract,
                native_formats["constantDefaults"],
            )
            shader_texture_policies.attach(
                templates,
                shader_database_observations,
                material_bank,
                material_contract,
                native_formats["texturePolicies"],
            )
    files, resources = ({}, [])
    for resource_index, (skeleton, pose) in enumerate(zip(skeletons["resources"], pc["resources"], strict=True)):
        if skeleton["member"] != pose["member"]:
            raise AssetError("skeleton/pose member order differs")
        blob, placement = native_skeleton.build(skeleton, pose, sc, base)
        restored, restored_pose = native_skeleton.reference_read(blob, placement["rootOffset"], base, sc, tc)
        semantics = native_skeleton.semantics(skeleton, pose)
        if restored_pose["rejections"] or canonical(native_skeleton.semantics(restored, restored_pose)) != canonical(
            semantics
        ):
            raise AssetError("written skeleton differs from source under the reference reader")
        displacement = len(blob) + 15 & -16
        moved = native_resource_builder.relocate(blob, placement, base + displacement)
        relocated, relocated_pose = native_skeleton.reference_read(
            bytes(displacement) + moved, displacement + placement["rootOffset"], base, sc, tc
        )
        if relocated_pose["rejections"] or canonical(native_skeleton.semantics(relocated, relocated_pose)) != canonical(
            semantics
        ):
            raise AssetError("relocated skeleton differs from source under the reference reader")
        stem = Path(skeleton["member"].split("!/")[-1]).stem
        binary_name, placement_name = (f"{stem}.skeleton.bin", f"{stem}.skeleton.placement.json")
        if binary_name in files or not stem or Path(stem).name != stem:
            raise AssetError("skeleton output member name is unsafe or collides")
        files[binary_name], files[placement_name] = (blob, canonical(placement))
        original_blob = material_runner.source_resource(source, skeleton["member"], budget, ".yft")
        source_header, source_payload = decode_resource(original_blob, budget.limits.max_file_bytes)
        if digest(original_blob) != skeleton["resourceSha256"] or digest(source_payload) != skeleton["payloadSha256"]:
            raise AssetError("child source bytes differ from skeleton input")
        children = pc_fragment_children.inspect(
            source_payload,
            source_header,
            materials[skeleton["member"]],
            skeleton,
            contracts,
            child_contract,
            budget.limits,
        )
        children["skeletonComponent"] = binary_name
        child_name = f"{stem}.children.json"
        files[child_name] = canonical(children)
        resources.append(
            {
                "member": skeleton["member"],
                "resourceSha256": skeleton["resourceSha256"],
                "payloadSha256": skeleton["payloadSha256"],
                "component": binary_name,
                "placement": placement_name,
                "boneCount": len(skeleton["bones"]),
                "tagNodeCount": len(skeleton["tagNodes"]),
                "pointerSlotCount": len(placement["pointerSlots"]),
                "matrixCount": len(pose["bones"]) * 2,
                "semanticSha256": digest(canonical(semantics)),
                "preservedSourceHashFields": {
                    name: skeleton["header"][name] for name in ("Unknown_50h", "Unknown_54h", "Unknown_58h")
                },
                "maximumInverseBindAbsoluteError": restored_pose["maximumInverseBindAbsoluteError"],
                "compositionDisplacement": displacement,
                "children": child_name,
                "childCoverage": children["summary"],
            }
        )
        if include_meshes:
            mesh_evidence = skeletons["freshMeshEvidence"]
            main_mesh = mesh_evidence["resources"][resource_index]
            main_geometry = mesh_evidence["freshGeometryBindings"]["resources"][resource_index]
            if any(r["member"] != skeleton["member"] for r in (main_mesh, main_geometry)):
                raise AssetError("mesh input resource order differs")
            groups = [{"owner": {"kind": "main"}, "mesh": main_mesh, "geometry": main_geometry}]
            groups += [
                {
                    "owner": {"kind": "physics-child", "sourceDrawablePointer": row["pointer"]},
                    "mesh": row["mesh"],
                    "geometry": row["geometry"],
                }
                for row in children["drawables"]
            ]
            mesh_blob, mesh_placement = native_mesh.build(source_payload, groups, mesh_contract, base)
            graphics_base = accepted["freshFragmentEvidence"]["pages"]["sourceBases"][1]
            verified = native_mesh.verify(
                mesh_blob,
                mesh_placement,
                mesh_contract,
                native_mesh_contract["buffers"],
                native_mesh_contract["formats"],
                graphics_base,
            )
            mesh_displacement = len(mesh_blob) + 15 & -16
            moved = native_resource_builder.relocate(mesh_blob, mesh_placement, base + mesh_displacement)
            shifted = mesh_placement | {
                "geometries": [
                    row | {"systemOffset": row["systemOffset"] + mesh_displacement}
                    for row in mesh_placement["geometries"]
                ]
            }
            moved_check = native_mesh.verify(
                bytes(mesh_displacement) + moved,
                shifted,
                mesh_contract,
                native_mesh_contract["buffers"],
                native_mesh_contract["formats"],
                graphics_base,
            )
            if moved_check != verified:
                raise AssetError("mesh composition readback differs")
            mesh_name, fixups_name = (f"{stem}.meshes.bin", f"{stem}.meshes.placement.json")
            files[mesh_name], files[fixups_name] = (mesh_blob, canonical(mesh_placement))
            resources[-1]["mesh"] = {
                "component": mesh_name,
                "placement": fixups_name,
                "verification": verified,
                "compositionDisplacement": mesh_displacement,
                "mainGeometryOccurrences": len(main_mesh["geometries"]),
                "childGeometryOccurrences": children["summary"]["geometryOccurrences"],
            }
            if include_models:
                model_blob, model_placement = native_models.build(
                    source_payload,
                    groups,
                    mesh_blob,
                    mesh_placement,
                    model_contract,
                    pc_fragment_children.geometry.SYSTEM_BASE,
                )
                model_check = native_models.verify(model_blob, model_placement, model_contract)
                mesh_check = native_mesh.verify(
                    model_blob,
                    model_placement,
                    mesh_contract,
                    native_mesh_contract["buffers"],
                    native_mesh_contract["formats"],
                    graphics_base,
                )
                if mesh_check != verified:
                    raise AssetError("model composition changed mesh contents")
                model_displacement = len(model_blob) + 15 & -16
                shifted = model_placement | {
                    key: [
                        row | {"systemOffset": row["systemOffset"] + model_displacement} for row in model_placement[key]
                    ]
                    for key in ("models", "geometries")
                }
                moved = bytes(model_displacement) + native_resource_builder.relocate(
                    model_blob, model_placement, base + model_displacement
                )
                if (
                    native_models.verify(moved, shifted, model_contract) != model_check
                    or native_mesh.verify(
                        moved,
                        shifted,
                        mesh_contract,
                        native_mesh_contract["buffers"],
                        native_mesh_contract["formats"],
                        graphics_base,
                    )
                    != verified
                ):
                    raise AssetError("relocated model/mesh composition changed relationships")
                model_name, model_fixups = (f"{stem}.models.bin", f"{stem}.models.placement.json")
                files[model_name], files[model_fixups] = (model_blob, canonical(model_placement))
                resources[-1]["models"] = {
                    "component": model_name,
                    "placement": model_fixups,
                    "verification": model_check,
                    "compositionDisplacement": model_displacement,
                }
        if include_materials:
            material_blob, material_placement = native_materials.build(
                source_payload, materials[skeleton["member"]], material_bank, material_contract, base
            )
            material_check = native_materials.verify(
                material_blob, material_placement, material_bank, material_contract
            )
            displacement = len(material_blob) + 15 & -16
            shifted = material_placement | {
                "rootOffset": material_placement["rootOffset"] + displacement,
                "shaders": [
                    r | {"systemOffset": r["systemOffset"] + displacement} for r in material_placement["shaders"]
                ],
                "externalTextureLinks": [
                    r | {"offset": r["offset"] + displacement} for r in material_placement["externalTextureLinks"]
                ],
            }
            moved = bytes(displacement) + native_resource_builder.relocate(
                material_blob, material_placement, base + displacement
            )
            if native_materials.verify(moved, shifted, material_bank, material_contract) != material_check:
                raise AssetError("material relocation changed fields or symbolic links")
            material_name, material_fixups = (f"{stem}.materials.bin", f"{stem}.materials.placement.json")
            files[material_name], files[material_fixups] = (material_blob, canonical(material_placement))
            resources[-1]["materials"] = {
                "component": material_name,
                "placement": material_fixups,
                "verification": material_check,
                "compositionDisplacement": displacement,
            }
        if include_drawables:
            components = {
                "skeleton": (blob, placement),
                "models": (model_blob, model_placement),
                "materials": (material_blob, material_placement),
            }
            drawable_blob, drawable_placement = native_drawables.build(
                source_payload[: source_header["systemBytes"]],
                pc_fragment_children.geometry.SYSTEM_BASE,
                materials[skeleton["member"]],
                children,
                components,
                drawable_contract,
                {row["Tag"] for row in skeleton["bones"]},
            )
            drawable_check = native_drawables.verify(drawable_blob, drawable_placement, drawable_contract)
            displacement = len(drawable_blob) + 15 & -16
            moved = bytes(displacement) + native_resource_builder.relocate(
                drawable_blob, drawable_placement, base + displacement
            )
            for data, graph in (
                (drawable_blob, drawable_placement),
                (moved, native_drawables.shifted(drawable_placement, displacement)),
            ):
                if native_drawables.verify(data, graph, drawable_contract) != drawable_check:
                    raise AssetError("drawable composition changed fields or relationships")
                model_view = native_drawables.component_view(graph, "models", model_placement)
                material_view = native_drawables.component_view(graph, "materials", material_placement)
                skeleton_view = native_drawables.component_view(graph, "skeleton", placement)
                restored, restored_pose = native_skeleton.reference_read(
                    data, skeleton_view["rootOffset"], base, sc, tc
                )
                if (
                    restored_pose["rejections"]
                    or graph["externalTextureLinks"] != material_view["externalTextureLinks"]
                    or native_skeleton.semantics(restored, restored_pose) != semantics
                    or (native_models.verify(data, model_view, model_contract) != model_check)
                    or (
                        native_mesh.verify(
                            data,
                            model_view,
                            mesh_contract,
                            native_mesh_contract["buffers"],
                            native_mesh_contract["formats"],
                            graphics_base,
                        )
                        != verified
                    )
                    or (
                        native_materials.verify(data, material_view, material_bank, material_contract) != material_check
                    )
                ):
                    raise AssetError("drawable composition changed an imported component")
            name, fixups = (f"{stem}.drawables.bin", f"{stem}.drawables.placement.json")
            files[name], files[fixups] = (drawable_blob, canonical(drawable_placement))
            resources[-1]["drawables"] = {
                "component": name,
                "placement": fixups,
                "verification": drawable_check,
                "compositionDisplacement": displacement,
            }
        if include_collision:
            roots = native_collision.roots(
                source_payload[: source_header["systemBytes"]],
                pc_fragment_children.geometry.SYSTEM_BASE,
                drawable_placement,
                collision_contract,
            )
            bounds_blob, bounds_placement = native_collision.build(
                source_payload[: source_header["systemBytes"]],
                pc_fragment_children.geometry.SYSTEM_BASE,
                roots,
                collision_contract,
                base,
            )
            collision_blob, collision_placement = native_collision.compose(
                drawable_blob, drawable_placement, bounds_blob, bounds_placement
            )
            collision_check = native_collision.verify(
                collision_blob, collision_placement["collision"], collision_contract
            )
            draw_check = native_drawables.verify(collision_blob, collision_placement, drawable_contract)
            displacement = len(collision_blob) + 15 & -16
            moved = bytes(displacement) + native_resource_builder.relocate(
                collision_blob, collision_placement, base + displacement
            )
            shifted = native_drawables.shifted(collision_placement, displacement)
            if (
                native_drawables.verify(moved, shifted, drawable_contract) != draw_check
                or native_collision.verify(
                    moved, native_collision.shifted(collision_placement["collision"], displacement), collision_contract
                )
                != collision_check
            ):
                raise AssetError("collision/drawable relocated graph differs")
            bname, bfix = (f"{stem}.bounds.bin", f"{stem}.bounds.placement.json")
            name, fixups = (f"{stem}.collision.bin", f"{stem}.collision.placement.json")
            files.update(
                {
                    bname: bounds_blob,
                    bfix: canonical(bounds_placement),
                    name: collision_blob,
                    fixups: canonical(collision_placement),
                }
            )
            resources[-1]["collision"] = {
                "component": name,
                "placement": fixups,
                "boundsComponent": bname,
                "boundsPlacement": bfix,
                "verification": collision_check,
                "drawableVerification": draw_check,
                "compositionDisplacement": displacement,
            }
        if include_physics:
            physics_blob, physics_placement = native_physics.build(
                source_payload[: source_header["systemBytes"]],
                pc_fragment_children.geometry.SYSTEM_BASE,
                collision_blob,
                collision_placement,
                physics_contract,
                native_physics_contract.get("groupHierarchy"),
            )
            physics_check = native_physics.verify(physics_blob, physics_placement["physics"], physics_contract)
            displacement = len(physics_blob) + 15 & -16
            moved = bytes(displacement) + native_resource_builder.relocate(
                physics_blob, physics_placement, base + displacement
            )
            for data, delta in ((physics_blob, 0), (moved, displacement)):
                if (
                    native_physics.verify(
                        data, native_physics.shifted(physics_placement["physics"], delta), physics_contract
                    )
                    != physics_check
                    or native_collision.verify(
                        data, native_collision.shifted(physics_placement["collision"], delta), collision_contract
                    )
                    != collision_check
                    or native_drawables.verify(
                        data, native_drawables.shifted(physics_placement, delta), drawable_contract
                    )
                    != draw_check
                ):
                    raise AssetError("physics composition/relocation changed component relationships")
            name, fixups = (f"{stem}.physics.bin", f"{stem}.physics.placement.json")
            files.update({name: physics_blob, fixups: canonical(physics_placement)})
            resources[-1]["physics"] = {
                "component": name,
                "placement": fixups,
                "verification": physics_check,
                "compositionDisplacement": displacement,
            }
            if "groupHierarchy" in native_physics_contract and physics_placement["physics"]["rootOffset"] is not None:
                native_physics.read(
                    physics_blob, base, base + physics_placement["physics"]["rootOffset"], physics_contract
                )
                native_fragment.read(
                    source_payload[: source_header["systemBytes"]],
                    pc_fragment_children.geometry.SYSTEM_BASE,
                    0,
                    fragment_contract,
                )["glass"]
                resources[-1]["physics"]["hierarchyChecks"] = dict(NATIVE_EXECUTION_NOT_PERFORMED)
        if include_fragment:
            fragment_blob, fragment_placement = native_fragment.build(
                source_payload[: source_header["systemBytes"]],
                pc_fragment_children.geometry.SYSTEM_BASE,
                physics_blob,
                physics_placement,
                fragment_contract,
            )
            fragment_check = native_fragment.verify(fragment_blob, fragment_placement["fragment"], fragment_contract)
            if fragment_check["boneMatrices"] != len(skeleton["bones"]):
                raise AssetError("fragment bone transforms differ from skeleton bone count")
            displacement = len(fragment_blob) + 15 & -16
            moved = bytes(displacement) + native_resource_builder.relocate(
                fragment_blob, fragment_placement, base + displacement
            )
            for data, delta in ((fragment_blob, 0), (moved, displacement)):
                if (
                    native_fragment.verify(
                        data, native_fragment.shifted(fragment_placement["fragment"], delta), fragment_contract
                    )
                    != fragment_check
                    or native_physics.verify(
                        data, native_physics.shifted(fragment_placement["physics"], delta), physics_contract
                    )
                    != physics_check
                    or native_collision.verify(
                        data, native_collision.shifted(fragment_placement["collision"], delta), collision_contract
                    )
                    != collision_check
                    or (
                        native_drawables.verify(
                            data, native_drawables.shifted(fragment_placement, delta), drawable_contract
                        )
                        != draw_check
                    )
                ):
                    raise AssetError("fragment composition/relocation changed component relationships")
            name, fixups = (f"{stem}.fragment.bin", f"{stem}.fragment.placement.json")
            files.update({name: fragment_blob, fixups: canonical(fragment_placement)})
            resources[-1]["fragment"] = {
                "component": name,
                "placement": fixups,
                "verification": fragment_check,
                "compositionDisplacement": displacement,
            }
        if include_references:
            fragment_blob, fragment_placement = native_texture_references.build(
                fragment_blob,
                fragment_placement,
                source_payload[: source_header["systemBytes"]],
                pc_fragment_children.geometry.SYSTEM_BASE,
                materials[skeleton["member"]],
                reference_contract,
                native_reference_contract,
            )
            reference_check = native_texture_references.verify(
                fragment_blob, fragment_placement, reference_contract, native_reference_contract
            )
            name, fixups = (f"{stem}.references.bin", f"{stem}.references.placement.json")
            files.update({name: fragment_blob, fixups: canonical(fragment_placement)})
            resources[-1]["textureReferences"] = {
                "component": name,
                "placement": fixups,
                "verification": reference_check,
            }
        if include_resources:
            envelope, graph = native_vehicle_resource.build(
                fragment_blob,
                fragment_placement,
                resource_contract,
                native_resource_contract,
                multi_page=getattr(args, "multi_page", False),
            )
            graph = json.loads(canonical(graph))
            system, check = native_vehicle_resource.readback(envelope, graph, native_resource_contract)
            for key in (
                "boneBindings",
                "bindPose",
                "collisionVertices",
                "collisionMaterials",
                "collisionTopology",
                "collisionCentroid",
                "collisionLoading",
                "bufferLoading",
                "collisionOwnership",
                "vertexLoading",
                "indexLoading",
            ):
                resources[-1][key] = dict(NATIVE_EXECUTION_NOT_PERFORMED)
            if (
                include_references
                and native_texture_references.verify(system, graph, reference_contract, native_reference_contract)
                != reference_check
            ):
                raise AssetError("resource envelope changed serialized texture references")
            if (
                native_fragment.verify(system, graph["fragment"], fragment_contract) != fragment_check
                or native_physics.verify(system, graph["physics"], physics_contract) != physics_check
                or native_collision.verify(system, graph["collision"], collision_contract) != collision_check
                or (native_drawables.verify(system, graph, drawable_contract) != draw_check)
            ):
                raise AssetError("resource envelope changed typed graph relationships")
            flags = graph["vehicleResource"]["flags"]
            flags_key = ":".join(hex(f) for f in flags)
            if flags_key not in resource_numeric_checks:
                resource_numeric_checks[flags_key] = dict(NATIVE_EXECUTION_NOT_PERFORMED)
            name, fixups = (f"{stem}.partial.pft", f"{stem}.resource.placement.json")
            files.update({name: envelope, fixups: canonical(graph)})
            resources[-1]["resource"] = {
                "component": name,
                "placement": fixups,
                "verification": check,
                "numericCheckKey": flags_key,
                "partialResource": True,
            }
    material_runner.verify_pc_source(source)
    if tool_hashes() != before or any(sha256_file(path) != identities[name] for name, path in input_paths.items()):
        raise AssetError("component tool or template input changed during conversion")
    manifest = {
        "schemaVersion": 2,
        "kind": (
            "gtavmenu-native-vehicle-partial-resources" if include_resources else "gtavmenu-native-vehicle-components"
        ),
        "targetId": TARGET_ID,
        "sources": identities | pc["sources"],
        "tools": before,
        "nativeContract": native,
        "childContract": child_contract,
        "meshContract": mesh_contract,
        "nativeMeshContract": native_mesh_contract,
        "meshReferenceCheck": mesh_reference,
        "modelContract": model_contract,
        "modelReferenceCheck": model_reference,
        "materialContract": material_contract,
        "nativeMaterialContract": native_material_contract,
        "materialSchemas": material_bank,
        "drawableContract": drawable_contract,
        "collisionContract": collision_contract,
        "physicsContract": physics_contract,
        "nativePhysicsContract": native_physics_contract,
        "fragmentContract": fragment_contract,
        "resourceContract": resource_contract,
        "nativeResourceContract": native_resource_contract,
        "textureReferenceContract": reference_contract,
        "nativeTextureReferenceContract": native_reference_contract,
        "templateReadbacks": reference_checks,
        "referenceCheck": {
            "nativeBoneCount": len(retail_skeleton["bones"]),
            "nativePoseRejections": len(retail_pose["rejections"]),
            "nativeSemanticSha256": digest(canonical(native_skeleton.semantics(retail_skeleton, retail_pose))),
            "nativeCodeExecuted": False,
        },
        "resources": resources,
        "files": {name: {"bytes": len(data), "sha256": digest(data)} for name, data in sorted(files.items())},
        "qualification": {
            "referenceLayoutAndPoseChecksPassed": True,
            "internalPointerCompositionChecked": True,
            "resourceTypedReadbackPassed": include_resources,
            "serializedTextureReferencesAvailable": include_references,
            "nativeCodeExecuted": False,
            "completeNativeResource": False,
            "textureDependenciesResolved": False,
            "resourceLoaderExecuted": False,
            "nativeGpuLayoutValidated": False,
            "hardwareQualified": False,
        },
        "remaining": [
            "Package texture and metadata dependencies before loading; converter readbacks do not replace hardware validation."
        ],
    }
    if include_references:
        manifest["materialShaderSelectionObservations"] = vehicle_material_checks.observe_bindings(manifest, files)
        manifest["materialParameterCacheObservations"] = vehicle_material_checks.observe_cache(
            manifest, files, native_material_contract["parameterCache"]
        )
        manifest["materialUpdateObservations"] = vehicle_material_checks.observe_materials(manifest, files)
        manifest["shaderDatabaseObservations"] = shader_database_observations
        manifest["compiledConstantDefaultObservations"] = shader_constant_defaults.observe(manifest, files)
        manifest["introducedTexturePolicyObservations"] = shader_texture_policies.observe(manifest, files)
        manifest["constantRemappingObservations"] = vehicle_material_checks.observe_remapping(manifest, files)
    return files, manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=STAGES)
    material_runner.add_pc_source_arguments(parser)
    parser.add_argument("--material-report", type=Path, required=True)
    parser.add_argument("--target", type=Path, default=ROOT / "data/targets/ppsa04264-01.010.002.json")
    parser.add_argument("--templates", type=Path, default=build_dir(ROOT) / "retail-templates")
    parser.add_argument("--native-reference", type=Path)
    parser.add_argument("--shader-reference", type=Path, action="append")
    parser.add_argument("--multi-page", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if assets_dir(ROOT).resolve() not in args.output.resolve().parents:
            raise AssetError("vehicle component output must be a new directory under the host build assets directory")
        if args.output.exists() or args.output.is_symlink():
            raise AssetError("vehicle component output already exists; choose an unused path")
        files, manifest = build_skeletons(args)
        args.output.mkdir(parents=True, exist_ok=False)
        for name, blob in files.items():
            with (args.output / name).open("xb") as stream:
                stream.write(blob)
            if (args.output / name).read_bytes() != blob:
                raise AssetError("component disk readback differs")
        with (args.output / "manifest.json").open("xb") as stream:
            stream.write(canonical(manifest))
        print(
            json.dumps(
                {
                    "stage": args.stage,
                    "resources": len(manifest["resources"]),
                    "files": len(files),
                    "qualification": manifest["qualification"],
                }
            )
        )
        return 0
    except (AssetError, OSError, ValueError, KeyError, TypeError, struct.error) as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

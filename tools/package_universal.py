#!/usr/bin/env python3
"""Stage a self-contained, fully inventoried universal production delivery."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
from pathlib import Path

from gtavmenu_tools.etahen import HEADER_SIZE, inspect_container, validate_elf, validate_metadata
from gtavmenu_tools.target_profile import expected_build_config
from gtavmenu_tools.universal_package import (
    LAYOUTS,
    REPO_ROOT,
    UniversalPackageError,
    embedding,
    expected_contract,
    integer,
    inventory_sha256,
    load_json,
    mapped_span,
    sha256,
    validate_targets,
    verify_package,
)

PackageError = UniversalPackageError


def source_commit() -> str:
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO_ROOT, text=True).strip()


def package_manifest_path(output: Path) -> Path:
    return output.with_name(f"{output.name}.package-manifest.json")


def render_readme(delivery: str) -> str:
    opening = """Automatically selects **PPSA04264 01.005.000**, **PPSA04264 01.010.002**, or
**PPSA04263 01.010.002**. Other title/build combinations refuse. Custom asset packs
require 01.010.002 and the console `/data` shared with GTA through ShadowMountPlus or the HEN setup.
Without shared game `/data`, the regular menu remains usable, with custom assets unavailable.
All three workers are embedded; no separate worker upload is needed.
Selection has been tested on both 01.010.002 titles; 01.005.000 selection remains untested on hardware.
"""
    steps = {
        "standalone": """1. Deploy `gtav-menu-daemon.elf` with a PS5 payload manager before starting GTA V.
2. Start Story Mode. After the injection notification, open the menu with **R1 + D-pad Left**.

The daemon watches for relaunches and selects the current title/version each time. To stop it,
create an empty `/data/GTAVMenu/daemon.stop` using FTP or a file manager, then wait for
`/data/GTAVMenu/daemon.lock` to disappear before launching another delivery.
""",
        "onionhen": """1. Copy `GTAV00001.elf` to `/data/OnionHEN/plugins/` (OnionHEN v0.0.13).
2. In Toolbox, open **Payloads & Kernel → Plugins → GTAV Menu** and enable **Running**.
   Enable **Auto-start** if wanted.
3. Start Story Mode. After the injection notification, open the menu with **R1 + D-pad Left**.

Disable the plugin in Toolbox before updating/removing it. Wait for
`/data/GTAVMenu/daemon.lock` to disappear before launching another delivery.
""",
        "etahen": """1. Copy `GTAV00001.plugin` to `/data/etaHEN/plugins/` (targets the etaHEN 2.5B plugin interface).
2. In Toolbox → **Plugins**, enable `GTAV00001` under **Running**; enable **Auto-start** if wanted.
3. Start Story Mode. After the injection notification, open the menu with **R1 + D-pad Left**.

To stop/update, disable **Auto-start** and **Running**. The embedded helper retires the menu after
Toolbox stops its visible supervisor. Wait for `/data/GTAVMenu/daemon.lock` to disappear before
replacing/deleting the plugin or launching another delivery. Do not install a separate `GTAV00002`.
After replacing the plugin, reboot and reload etaHEN before enabling it; Toolbox can cache the old code.

Console checks used Beta2.6A with a custom lifecycle implementation. Compatibility with stock 2.5B
has not been verified on hardware.
""",
    }
    names = {"standalone": "Standalone", "onionhen": "OnionHEN", "etahen": "etaHEN"}
    return (
        f"# GTAV-Menu — Universal {names[delivery]}\n\n{opening}\n{steps[delivery]}\n"
        "Use D-pad Up/Down to move, Left/Right to change values, Cross to select, and Circle to go back.\n"
    )


def _input(record: dict, base: Path, label: str) -> bytes:
    raw = record.get("path")
    if not isinstance(raw, str) or not raw:
        raise PackageError(f"missing {label} input path")
    path = Path(raw)
    if not path.is_absolute():
        path = base / path
    if path.is_symlink() or not path.is_file():
        raise PackageError(f"missing or symlinked {label}: {path}")
    data = path.read_bytes()
    if sha256(data) != record.get("sha256"):
        raise PackageError(f"{label} input hash mismatch")
    return data


def stage_package(
    *,
    delivery: str,
    runtime: Path,
    artifact: Path,
    inventory: Path,
    build_config: Path,
    output: Path,
    version: str | None = None,
) -> dict:
    if delivery not in LAYOUTS:
        raise PackageError("unknown universal delivery")
    if delivery != "standalone":
        if not isinstance(version, str):
            raise PackageError("plugin delivery requires an explicit version")
        validate_metadata("GTAV00001", version)
    registry, config = load_json(inventory), load_json(build_config)
    commit = source_commit()
    required_registry = {"schemaVersion": 1, "kind": "gtavmenu-universal-registry", "sourceCommit": commit}
    if any(registry.get(key) != value for key, value in required_registry.items()):
        raise PackageError("universal registry schema/kind/source mismatch")
    required_config = {
        "schemaVersion": 1,
        "target": "universal",
        "profile": "production",
        "delivery": delivery,
        "sourceCommit": commit,
        "universal": 1,
        "persistent": 1,
        "embed_worker": 1,
        "target_ptrace": 0,
        "target_elevation": 0,
        "loader_wait": 1,
        "loader_sp_ready": 1,
        "worker_context": 1,
        "verify_writes": 1,
        "native_pins": 1,
        "registrySha256": sha256(inventory.read_bytes()),
    }
    for key, value in required_config.items():
        actual = integer(config.get(key), key) if isinstance(value, int) else config.get(key)
        if actual != value:
            raise PackageError(f"unsafe universal build config: {key}")
    targets = validate_targets(registry.get("supportedTargets"))
    runtime_bytes, artifact_bytes = runtime.read_bytes(), artifact.read_bytes()
    validate_elf(runtime_bytes)
    supported = []
    for entry in targets:
        target = entry["target"]
        worker = _input(entry["worker"], inventory.parent, f"{target} worker")
        if len(worker) != entry["worker"].get("size"):
            raise PackageError(f"{target} worker input size mismatch")
        worker_config_bytes = _input(entry["workerBuildConfig"], inventory.parent, f"{target} worker config")
        worker_config = json.loads(worker_config_bytes)
        target_manifest = load_json(REPO_ROOT / "data/targets" / f"{target}.json")
        if entry.get("contract") != expected_contract(target_manifest):
            raise PackageError(f"universal loader contract mismatch: {target}")
        expected = {
            **expected_build_config(target_manifest, "standalone"),
            "profile": "production",
            "delivery": "standalone",
            "target": target,
            "require_context": 1,
            "target_manifest_sha256": entry["targetManifestSha256"],
        }
        for key, value in expected.items():
            actual = integer(worker_config.get(key), key) if isinstance(value, int) else worker_config.get(key)
            if actual != value:
                raise PackageError(f"unsafe worker config: {target} {key}")
        span = mapped_span(worker)
        if span != integer(entry["worker"].get("mappedSpan"), "mappedSpan"):
            raise PackageError(f"{target} worker mapped span mismatch")
        supported.append(
            {
                **{
                    key: entry[key]
                    for key in (
                        "target",
                        "targetId",
                        "titleId",
                        "contentId",
                        "contentVersion",
                        "targetManifestSha256",
                        "customPacks",
                        "contract",
                    )
                },
                "worker": {**embedding(runtime_bytes, worker, f"{target} worker"), "mappedSpan": span},
                "workerBuildConfigSha256": sha256(worker_config_bytes),
            }
        )
    if delivery == "etahen":
        inspect_container(artifact_bytes, expect_id="GTAV00001", expect_version=version)
        outer = artifact_bytes[HEADER_SIZE:]
    else:
        validate_elf(artifact_bytes)
        outer = artifact_bytes
    runtime_proof = embedding(outer, runtime_bytes, "universal runtime")
    if delivery == "standalone" and runtime_bytes != artifact_bytes:
        raise PackageError("standalone artifact must be the universal runtime")
    name = next(name for name in LAYOUTS[delivery] if name != "README.md")
    readme = render_readme(delivery).encode()
    files = {name: artifact_bytes, "README.md": readme}
    manifest = {
        "schemaVersion": 2,
        "kind": f"gtavmenu-universal-{delivery}-production",
        "target": "universal",
        "profile": "production",
        "delivery": delivery,
        "releaseChannel": "local-publication-candidate",
        "requiresHardwareValidation": True,
        "selfContained": True,
        "sourceCommit": commit,
        "supportedTargets": supported,
        "registrySha256": sha256(inventory.read_bytes()),
        "inventorySha256": inventory_sha256(commit, supported),
        "buildConfigSha256": sha256(build_config.read_bytes()),
        "embeddedRuntime": runtime_proof,
        "ps5debugDependency": False,
        "loaderLog": "/data/GTAVMenu/gtav-menu.log",
        "files": [
            {"path": key, "role": LAYOUTS[delivery][key], "size": len(data), "sha256": sha256(data)}
            for key, data in files.items()
        ],
    }
    if delivery == "standalone":
        manifest.update(daemonElfName=name, primaryLaunchSurface="a generic PS5 payload launcher")
    else:
        manifest.update(pluginId="GTAV00001", version=version)
    if delivery == "etahen":
        manifest.update(
            minimumEtaHENVersion="2.5B",
            runtimeVersion=version,
            toolboxShutdown="supervisor-lease-cooperative-retirement",
        )
    # Finish all input checks before replacing this generated staging directory.
    from gtavmenu_tools.universal_package import verify_artifact

    verify_artifact(artifact_bytes, manifest)
    if output.is_symlink():
        raise PackageError("refusing symlinked package output")
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    for relative, data in files.items():
        (output / relative).write_bytes(data)
    (output / name).chmod(0o755)
    verify_package(output, manifest, delivery=delivery, source_commit=commit)
    package_manifest_path(output).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--delivery", choices=LAYOUTS, required=True)
    for name in ("runtime", "artifact", "inventory", "build-config", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--version")
    args = parser.parse_args()
    try:
        manifest = stage_package(**vars(args))
    except (OSError, KeyError, TypeError, ValueError, subprocess.CalledProcessError) as exc:
        parser.error(str(exc))
    print(f"staged {len(manifest['files'])} files for universal {args.delivery} to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

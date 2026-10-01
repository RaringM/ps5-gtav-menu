#!/usr/bin/env python3
"""Stage the stock etaHEN Toolbox plugin as a minimal install bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

from gtavmenu_tools.etahen import inspect_container
from gtavmenu_tools.target_profile import expected_build_config, validate_manifest

REPO_ROOT = Path(__file__).resolve().parents[1]


def render_readme(content_version: str) -> str:
    return f"""# GTAV-Menu — etaHEN

Toolbox-managed plugin for stock **etaHEN 2.5B or newer**, GTA V Enhanced
**PPSA04264, version {content_version}**.

1. Copy `GTAV00001.plugin` to `/data/etaHEN/plugins/GTAV00001.plugin` on the PS5.
2. Open etaHEN Toolbox, select **Plugins**, and enable `GTAV00001` under **Running**. Enable
   **Auto-start** too if the menu should watch for GTA V after every etaHEN launch.
3. Start Story Mode in GTA V Enhanced.
4. When the “GTAVMenu injected” notification appears, press **R1 + D-pad Left** to open the menu.

Use D-pad Up/Down to move, Left/Right to change values, Cross to select, and Circle to go back.

To stop or update the plugin, disable **Auto-start** and **Running** in Toolbox. Toolbox stops its
visible process immediately; the embedded runtime then detects that shutdown, asks the menu worker
to stop, verifies the render callback and frame hook were restored, and exits. Wait until
`/data/GTAVMenu/daemon.lock` disappears before replacing or deleting the plugin file.

The helper runtime is embedded in `GTAV00001.plugin`; do not install a separate `GTAV00002` file.
"""


class PackageError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_commit() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=REPO_ROOT,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def package_manifest_path(output: Path) -> Path:
    return output.with_name(f"{output.name}.package-manifest.json")


def _integer(config: dict[str, object], key: str) -> int:
    value = config.get(key)
    if not isinstance(value, (str, int)):
        raise PackageError(f"build config is missing {key}")
    try:
        return int(value, 0) if isinstance(value, str) else value
    except ValueError as exc:
        raise PackageError(f"build config has invalid {key}: {value}") from exc


def validate_build_config(
    config: dict[str, object], target_manifest: dict[str, object], target: str
) -> dict[str, object]:
    profile = validate_manifest(target_manifest)
    if config.get("profile") != "production" or config.get("delivery") != "etahen" or config.get("target") != target:
        raise PackageError("refusing to package a non-production or mismatched etaHEN build")
    if profile["stem"] != target:
        raise PackageError(f"target manifest identifies {profile['stem']}, expected {target}")
    mismatches: list[str] = []
    for key, expected in expected_build_config(target_manifest, "etahen").items():
        actual = _integer(config, key) if isinstance(expected, int) else config.get(key)
        if actual != expected:
            mismatches.append(f"{key}={actual!r} (expected {expected!r})")
    if mismatches:
        raise PackageError("unsafe etaHEN build config: " + "; ".join(mismatches))
    return profile


def _file_entry(path: Path, *, role: str) -> dict[str, object]:
    return {
        "path": path.name,
        "role": role,
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def stage_package(
    *,
    plugin: Path,
    output: Path,
    plugin_id: str,
    version: str,
    target: str,
    target_manifest: Path,
    build_config: Path,
) -> dict[str, object]:
    if not plugin.is_file() or not build_config.is_file() or not target_manifest.is_file():
        raise PackageError("required etaHEN package input is missing")
    try:
        inspect_container(plugin.read_bytes(), expect_id=plugin_id, expect_version=version)
    except ValueError as exc:
        raise PackageError(f"invalid etaHEN plugin: {exc}") from exc
    config = json.loads(build_config.read_text(encoding="utf-8"))
    manifest_data = json.loads(target_manifest.read_text(encoding="utf-8"))
    profile = validate_build_config(config, manifest_data, target)

    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    staged_plugin = output / f"{plugin_id}.plugin"
    shutil.copy2(plugin, staged_plugin)
    readme = output / "README.md"
    readme.write_text(render_readme(str(profile["contentVersion"])), encoding="utf-8")

    manifest = {
        "schemaVersion": 1,
        "kind": "gtavmenu-etahen-production",
        "target": target,
        "targetId": manifest_data["targetId"],
        "contentVersion": manifest_data["contentVersion"],
        "profile": "production",
        "delivery": "etahen",
        "pluginId": plugin_id,
        "version": version,
        "minimumEtaHENVersion": "2.5B",
        "toolboxShutdown": "supervisor-lease-cooperative-retirement",
        "releaseChannel": (
            "local-publication-candidate" if profile["channel"] == "primary" else "local-development-candidate"
        ),
        "targetChannel": profile["channel"],
        "requiresHardwareValidation": True,
        "sourceCommit": source_commit(),
        "targetManifestSha256": sha256_file(target_manifest),
        "buildConfigSha256": sha256_file(build_config),
        "files": [
            _file_entry(staged_plugin, role="etahen-plugin"),
            _file_entry(readme, role="documentation"),
        ],
    }
    package_manifest_path(output).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plugin", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--plugin-id", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--target-manifest", required=True, type=Path)
    parser.add_argument("--build-config", required=True, type=Path)
    args = parser.parse_args()
    try:
        manifest = stage_package(
            plugin=args.plugin,
            output=args.output,
            plugin_id=args.plugin_id,
            version=args.version,
            target=args.target,
            target_manifest=args.target_manifest,
            build_config=args.build_config,
        )
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError, PackageError) as exc:
        parser.error(str(exc))
    print(f"staged {len(manifest['files'])} files to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Stage the minimal standalone GTAVMenu production bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
TARGET_ID = "PPSA04264_01.010.002_DISC"
CONTENT_VERSION = "01.010.002"
EXPECTED_NATIVE_ADDRESSES = {
    "drawRect": "0x1aa8900",
    "beginTextCommandDisplayText": "0x1ac5d60",
}

README = """# GTAV-Menu — Standalone

For the disc release of GTA V **PPSA04264 / 01.010.002** only.

1. Start an FTP server on the PS5.
2. Copy this archive's `data/GTAVMenu` folder into `/data` on the PS5, preserving the directory layout.
3. Start GTA V, enter Story Mode, and wait until you have player control.
4. Launch `/data/GTAVMenu/payloads/gtav-menu-payload-loader.elf` with a compatible PS5 payload launcher.
5. Press **R1 + D-pad Left** to open or hide the menu.

Use D-pad Up/Down to move, Left/Right to change values, Cross to select, and Circle to go back.
Launch the loader again after each fresh GTA V process. Do not launch it while the game is loading.
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
    """Internal verification metadata kept beside, not inside, the user bundle."""
    return output.with_name(f"{output.name}.package-manifest.json")


def copy_entry(
    source: Path,
    destination: Path,
    package_root: Path,
    entries: list[dict[str, object]],
    *,
    role: str,
    remote: str | None = None,
) -> None:
    if not source.is_file():
        raise PackageError(f"required package input is missing: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    entry: dict[str, object] = {
        "path": destination.relative_to(package_root).as_posix(),
        "role": role,
        "size": destination.stat().st_size,
        "sha256": sha256_file(destination),
    }
    if remote is not None:
        entry["remote"] = remote
    entries.append(entry)


def stage_package(
    *,
    output: Path,
    loader: Path,
    worker: Path,
    target_manifest: Path,
    build_config: Path,
) -> dict[str, object]:
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)

    target = json.loads(target_manifest.read_text(encoding="utf-8"))
    config = json.loads(build_config.read_text(encoding="utf-8"))
    if (
        config.get("profile") != "production"
        or config.get("delivery") != "standalone"
        or config.get("target") != "ppsa04264-01.010.002"
    ):
        raise PackageError("refusing to package a non-production or non-01.010.002 build")
    if target.get("targetId") != TARGET_ID or target.get("contentVersion") != CONTENT_VERSION:
        raise PackageError("refusing to package a mismatched target manifest")
    bridge = target.get("nativeBridge")
    if (
        not isinstance(bridge, dict)
        or bridge.get("addressAcceptanceSemantics") != "exact_target_and_hardware_validation"
        or bridge.get("runtimeInvocationValidated") is not True
        or bridge.get("authorizedForInjection") is not True
    ):
        raise PackageError("target manifest lacks the production native-address policy")
    addresses = bridge.get("addresses")
    if not isinstance(addresses, dict):
        raise PackageError("target manifest lacks production native addresses")
    for name, expected in EXPECTED_NATIVE_ADDRESSES.items():
        if str(addresses.get(name) or "").lower() != expected:
            raise PackageError(f"target manifest has an unexpected {name} address")

    entries: list[dict[str, object]] = []
    copy_entry(
        loader,
        output / "data/GTAVMenu/payloads/gtav-menu-payload-loader.elf",
        output,
        entries,
        role="payload-loader",
        remote="/data/GTAVMenu/payloads/gtav-menu-payload-loader.elf",
    )
    copy_entry(
        worker,
        output / "data/GTAVMenu/gtav-menu-feature-menu.elf",
        output,
        entries,
        role="menu-worker",
        remote="/data/GTAVMenu/gtav-menu-feature-menu.elf",
    )
    readme = output / "README.md"
    readme.write_text(README, encoding="utf-8")
    entries.append(
        {
            "path": "README.md",
            "role": "documentation",
            "size": readme.stat().st_size,
            "sha256": sha256_file(readme),
        }
    )

    manifest = {
        "schemaVersion": 1,
        "kind": "gtavmenu-standalone-production",
        "releaseChannel": "local-publication-candidate",
        "targetId": target["targetId"],
        "contentVersion": target["contentVersion"],
        "profile": config["profile"],
        "delivery": config["delivery"],
        "sourceCommit": source_commit(),
        "loaderElfPath": "/data/GTAVMenu/payloads/gtav-menu-payload-loader.elf",
        "workerElfPath": "/data/GTAVMenu/gtav-menu-feature-menu.elf",
        "primaryLaunchSurface": "generic PS5 payload launcher",
        "ps5debugDependency": False,
        "requiresHardwareValidation": True,
        "loaderLog": "/data/GTAVMenu/payload-loader.log",
        "targetManifestSha256": sha256_file(target_manifest),
        "buildConfigSha256": sha256_file(build_config),
        "files": entries,
    }
    package_manifest_path(output).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loader", required=True, type=Path)
    parser.add_argument("--worker", required=True, type=Path)
    parser.add_argument("--target-manifest", required=True, type=Path)
    parser.add_argument("--build-config", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        manifest = stage_package(
            output=args.output,
            loader=args.loader,
            worker=args.worker,
            target_manifest=args.target_manifest,
            build_config=args.build_config,
        )
    except (OSError, KeyError, json.JSONDecodeError, PackageError) as exc:
        parser.error(str(exc))
    print(f"staged {len(manifest['files'])} files to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

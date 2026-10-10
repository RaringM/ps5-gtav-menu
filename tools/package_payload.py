#!/usr/bin/env python3
"""Stage the minimal standalone GTAVMenu production bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

from gtavmenu_tools.target_profile import expected_build_config, validate_manifest

REPO_ROOT = Path(__file__).resolve().parents[1]
DAEMON_NAME = "gtav-menu-daemon.elf"


def render_readme(title_id: str, content_version: str) -> str:
    return f"""# GTAV-Menu — Standalone

GTA V Enhanced (**{title_id}, version {content_version}**) mod menu daemon.
`{DAEMON_NAME}` watches for GTA V launches and injects the menu after Story Mode loads.

1. Copy the `GTAVMenu` folder to `/data/` on the PS5.
2. Deploy `{DAEMON_NAME}` with a PS5 payload manager before starting GTA V.
3. Start Story Mode in GTA V Enhanced.
4. When the “GTAVMenu injected” notification appears, press **R1 + D-pad Left** to open the menu.

Use D-pad Up/Down to move, Left/Right to change values, Cross to select, and Circle to go back.

Custom assets require 01.010.002 and the console `/data` shared with GTA through ShadowMountPlus
or the HEN setup. Without it the regular menu remains usable, with custom assets unavailable.
The daemon stays running and injects again after each GTA V relaunch. To stop it, create an empty
`/data/GTAVMenu/daemon.stop` file using FTP or a file manager; it exits after safely retiring the menu.
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


def _integer(config: dict[str, object], key: str) -> int:
    value = config.get(key)
    if not isinstance(value, (str, int)):
        raise PackageError(f"build config is missing {key}")
    try:
        return int(value, 0) if isinstance(value, str) else value
    except ValueError as exc:
        raise PackageError(f"build config has invalid {key}: {value}") from exc


def validate_build_config(config: dict[str, object], target: dict[str, object]) -> dict[str, object]:
    profile = validate_manifest(target)
    if (
        config.get("profile") != "production"
        or config.get("delivery") != "standalone"
        or config.get("target") != profile["stem"]
    ):
        raise PackageError("refusing to package a non-production or mismatched standalone build")
    mismatches: list[str] = []
    for key, expected in expected_build_config(target, "standalone").items():
        actual = _integer(config, key) if isinstance(expected, int) else config.get(key)
        if actual != expected:
            mismatches.append(f"{key}={actual!r} (expected {expected!r})")
    if mismatches:
        raise PackageError("unsafe standalone daemon build config: " + "; ".join(mismatches))
    return profile


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
    target = json.loads(target_manifest.read_text(encoding="utf-8"))
    config = json.loads(build_config.read_text(encoding="utf-8"))
    profile = validate_build_config(config, target)

    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    entries: list[dict[str, object]] = []
    copy_entry(
        loader,
        output / DAEMON_NAME,
        output,
        entries,
        role="daemon",
    )
    copy_entry(
        worker,
        output / "GTAVMenu/gtav-menu-feature-menu.elf",
        output,
        entries,
        role="menu-worker",
        remote="/data/GTAVMenu/gtav-menu-feature-menu.elf",
    )
    readme_path = output / "README.md"
    readme_path.write_text(render_readme(str(profile["titleId"]), str(profile["contentVersion"])), encoding="utf-8")
    entries.append(
        {
            "path": "README.md",
            "role": "documentation",
            "size": readme_path.stat().st_size,
            "sha256": sha256_file(readme_path),
        }
    )

    manifest = {
        "schemaVersion": 1,
        "kind": "gtavmenu-standalone-production",
        "releaseChannel": (
            "local-publication-candidate" if profile["channel"] == "primary" else "local-development-candidate"
        ),
        "target": profile["stem"],
        "targetChannel": profile["channel"],
        "targetId": target["targetId"],
        "titleId": target["titleId"],
        "contentId": target["contentId"],
        "contentVersion": target["contentVersion"],
        "profile": config["profile"],
        "delivery": config["delivery"],
        "sourceCommit": source_commit(),
        "daemonElfName": DAEMON_NAME,
        "workerElfPath": "/data/GTAVMenu/gtav-menu-feature-menu.elf",
        "primaryLaunchSurface": "generic PS5 payload launcher",
        "ps5debugDependency": False,
        "requiresHardwareValidation": True,
        "loaderLog": "/data/GTAVMenu/gtav-menu.log",
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
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError, PackageError) as exc:
        parser.error(str(exc))
    print(f"staged {len(manifest['files'])} files to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

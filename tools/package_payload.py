#!/usr/bin/env python3
"""Stage the complete standalone GTAVMenu production bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]


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
    if config.get("profile") != "production" or config.get("target") != "ppsa04264-01.010.002":
        raise PackageError("refusing to package a non-production or non-01.010.002 build")

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
    target_name = target_manifest.name
    copy_entry(
        target_manifest,
        output / f"data/GTAVMenu/targets/{target_name}",
        output,
        entries,
        role="target-manifest",
        remote=f"/data/GTAVMenu/targets/{target_name}",
    )
    copy_entry(build_config, output / "build-config.json", output, entries, role="build-config")
    copy_entry(REPO_ROOT / "tools/install_release.py", output / "install_release.py", output, entries, role="installer")

    readme = output / "README.txt"
    readme.write_text(
        "GTAVMenu standalone production bundle\n\n"
        "Install: python3 install_release.py install --host <PS5-IP>\n"
        "Launch /data/GTAVMenu/payloads/gtav-menu-payload-loader.elf only after Story Mode is ready.\n"
        "The menu starts hidden. Open it with R1 + D-pad Left.\n",
        encoding="utf-8",
    )
    entries.append(
        {
            "path": "README.txt",
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
        "files": entries,
    }
    (output / "package-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
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

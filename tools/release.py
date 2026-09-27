#!/usr/bin/env python3
"""Assemble, verify, and publish deterministic GTAV-Menu release assets."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath
from typing import NoReturn

REPO_ROOT = Path(__file__).resolve().parents[1]
TARGET = "ppsa04264-01.010.002"
TARGET_ID = "PPSA04264_01.010.002_DISC"
CONTENT_VERSION = "01.010.002"
TAG_RE = re.compile(r"v[A-Za-z0-9][A-Za-z0-9._-]*\Z")
ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)
PACKAGE_LAYOUTS = {
    "standalone": {
        "gtav-menu-daemon.elf": "daemon",
        "GTAVMenu/gtav-menu-feature-menu.elf": "menu-worker",
        "README.md": "documentation",
    },
    "onionhen": {
        "GTAV00001.elf": "onionhen-plugin",
        "README.md": "documentation",
    },
    "etahen": {
        "GTAV00001.plugin": "etahen-plugin",
        "README.md": "documentation",
    },
}


class ReleaseError(RuntimeError):
    pass


def fail(message: str) -> NoReturn:
    raise ReleaseError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot read {path}: {exc}")
    if not isinstance(value, dict):
        fail(f"expected a JSON object: {path}")
    return value


def package_manifest_path(package_root: Path) -> Path:
    return package_root.with_name(f"{package_root.name}.package-manifest.json")


def safe_path(raw: object) -> str:
    if not isinstance(raw, str):
        fail("package file entry has no string path")
    path = PurePosixPath(raw)
    if not raw or path.is_absolute() or ".." in path.parts or raw != path.as_posix():
        fail(f"unsafe package path: {raw!r}")
    return raw


def verify_package(package_root: Path, *, kind: str, delivery: str, source_commit: str) -> dict[str, object]:
    manifest = load_json(package_manifest_path(package_root))
    required = {
        "schemaVersion": 1,
        "kind": kind,
        "targetId": TARGET_ID,
        "contentVersion": CONTENT_VERSION,
        "profile": "production",
        "delivery": delivery,
        "releaseChannel": "local-publication-candidate",
        "requiresHardwareValidation": True,
        "sourceCommit": source_commit,
    }
    for key, expected in required.items():
        if manifest.get(key) != expected:
            fail(f"{delivery} package has {key}={manifest.get(key)!r}, expected {expected!r}")

    entries = manifest.get("files")
    if not isinstance(entries, list) or not entries:
        fail(f"{delivery} package has no file inventory")
    declared: set[str] = set()
    for raw_entry in entries:
        if not isinstance(raw_entry, dict):
            fail(f"{delivery} package contains a malformed file entry")
        relative = safe_path(raw_entry.get("path"))
        if relative in declared:
            fail(f"{delivery} package declares {relative} more than once")
        declared.add(relative)
        candidate = package_root / relative
        if candidate.is_symlink() or not candidate.is_file():
            fail(f"{delivery} package member is missing: {relative}")
        if raw_entry.get("size") != candidate.stat().st_size:
            fail(f"{delivery} package size mismatch: {relative}")
        if raw_entry.get("sha256") != sha256_file(candidate):
            fail(f"{delivery} package hash mismatch: {relative}")

    actual = {
        path.relative_to(package_root).as_posix()
        for path in package_root.rglob("*")
        if path.is_file() or path.is_symlink()
    }
    expected = declared
    if actual != expected:
        fail(
            f"{delivery} package inventory mismatch; "
            f"missing={sorted(expected - actual)} extra={sorted(actual - expected)}"
        )
    expected_layout = PACKAGE_LAYOUTS[delivery]
    actual_layout = {safe_path(entry["path"]): entry.get("role") for entry in entries}
    if actual_layout != expected_layout:
        fail(f"{delivery} package has an unexpected release layout: {actual_layout}")
    return manifest


def deterministic_zip(source: Path, output: Path, *, archive_root: str | None, members: list[str]) -> None:
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for relative in sorted(members):
            member = source / relative
            if member.is_symlink() or not member.is_file():
                fail(f"archive member is missing: {relative}")
            info = zipfile.ZipInfo(f"{archive_root}/{relative}" if archive_root else relative, ZIP_TIMESTAMP)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = (stat.S_IFREG | stat.S_IMODE(member.stat().st_mode)) << 16
            archive.writestr(info, member.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def package_members(manifest: dict[str, object]) -> list[str]:
    entries = manifest["files"]
    assert isinstance(entries, list)
    return [safe_path(entry.get("path")) for entry in entries if isinstance(entry, dict)]


def git_output(*args: str) -> str:
    result = subprocess.run(["git", *args], cwd=REPO_ROOT, check=True, text=True, capture_output=True)
    return result.stdout.strip()


def validate_tag(tag: str, *, require_head: bool = True) -> None:
    if not TAG_RE.fullmatch(tag):
        fail("release tag must begin with v and contain only letters, digits, dots, underscores, or hyphens")
    if require_head:
        head = git_output("rev-parse", "HEAD")
        tagged = git_output("rev-list", "-n", "1", tag)
        if tagged != head:
            fail(f"tag {tag} resolves to {tagged}, but HEAD is {head}")


def assemble(tag: str, output: Path, standalone: Path, onionhen: Path, etahen: Path) -> Path:
    validate_tag(tag)
    if output.exists() and any(output.iterdir()):
        fail(f"release output must be empty: {output}")
    output.mkdir(parents=True, exist_ok=True)
    commit = git_output("rev-parse", "HEAD")
    standalone_manifest = verify_package(
        standalone,
        kind="gtavmenu-standalone-production",
        delivery="standalone",
        source_commit=commit,
    )
    onionhen_manifest = verify_package(
        onionhen,
        kind="gtavmenu-onionhen-production",
        delivery="onionhen",
        source_commit=commit,
    )
    etahen_manifest = verify_package(
        etahen,
        kind="gtavmenu-etahen-production",
        delivery="etahen",
        source_commit=commit,
    )

    prefix = f"GTAV-Menu-{tag}"
    artifacts = [
        (output / f"{prefix}-standalone.zip", standalone, standalone_manifest, None),
        (output / f"{prefix}-onionhen.zip", onionhen, onionhen_manifest, None),
        (output / f"{prefix}-etahen.zip", etahen, etahen_manifest, None),
    ]
    for archive, package_root, manifest, archive_root in artifacts:
        deterministic_zip(package_root, archive, archive_root=archive_root, members=package_members(manifest))

    release_manifest = {
        "schemaVersion": 1,
        "tag": tag,
        "target": TARGET,
        "targetId": TARGET_ID,
        "contentVersion": CONTENT_VERSION,
        "sourceCommit": commit,
        "artifacts": [
            {"file": archive.name, "size": archive.stat().st_size, "sha256": sha256_file(archive)}
            for archive, _, _, _ in artifacts
        ],
    }
    manifest_path = output / "release-manifest.json"
    manifest_path.write_text(json.dumps(release_manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    checksum_paths = [*(archive for archive, _, _, _ in artifacts), manifest_path]
    (output / "SHA256SUMS").write_text(
        "".join(f"{sha256_file(path)}  {path.name}\n" for path in sorted(checksum_paths)),
        encoding="utf-8",
    )
    return output


def request_json(
    url: str,
    *,
    token: str,
    method: str = "GET",
    payload: dict[str, object] | None = None,
    data: bytes | None = None,
    content_type: str = "application/json",
) -> dict[str, object]:
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(url, data=data, method=method)
    request.add_header("Authorization", f"Bearer {token}")
    request.add_header("Accept", "application/json")
    if data is not None:
        request.add_header("Content-Type", content_type)
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            raw = response.read()
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        fail(f"release API {method} {url} failed ({exc.code}): {detail}")
    value = json.loads(raw or b"{}")
    if not isinstance(value, dict):
        fail(f"release API returned a non-object for {method} {url}")
    return value


def release_body(tag: str) -> str:
    return (
        f"GTAV-Menu {tag} for PPSA04264 01.010.002.\n\n"
        "The standalone, OnionHEN, and etaHEN archives contain only their installable runtime and setup README. "
        "SHA-256 checksums are provided separately. Rebuilt artifacts require on-hardware validation "
        "before distribution."
    )


def publish(platform: str, tag: str, assets: Path) -> None:
    validate_tag(tag)
    files = sorted(path for path in assets.iterdir() if path.is_file())
    if not files or not (assets / "SHA256SUMS").is_file():
        fail(f"release assets are incomplete: {assets}")

    if platform == "github":
        token = os.environ.get("GITHUB_TOKEN", "")
        repository = os.environ.get("GITHUB_REPOSITORY", "")
        server = os.environ.get("GITHUB_API_URL", "https://api.github.com").rstrip("/")
    else:
        token = os.environ.get("FORGEJO_TOKEN", "")
        repository = os.environ.get("FORGEJO_REPOSITORY") or os.environ.get("GITHUB_REPOSITORY", "")
        base = os.environ.get("FORGEJO_API_URL")
        if not base:
            host = os.environ.get("FORGEJO_SERVER_URL") or os.environ.get("GITHUB_SERVER_URL", "")
            base = f"{host.rstrip('/')}/api/v1" if host else ""
        server = base.rstrip("/")
    if not token or not repository or not server:
        fail(f"missing {platform} token, repository, or API URL environment")

    endpoint = f"{server}/repos/{repository}/releases"
    release = request_json(
        endpoint,
        token=token,
        method="POST",
        payload={"tag_name": tag, "name": tag, "body": release_body(tag), "draft": True, "prerelease": False},
    )
    release_id = release.get("id")
    if not isinstance(release_id, int):
        fail("release API response has no numeric id")

    if platform == "github":
        raw_upload = release.get("upload_url")
        if not isinstance(raw_upload, str):
            fail("GitHub release response has no upload_url")
        upload_base = raw_upload.split("{", 1)[0]
        for path in files:
            url = f"{upload_base}?{urllib.parse.urlencode({'name': path.name})}"
            request_json(
                url, token=token, method="POST", data=path.read_bytes(), content_type="application/octet-stream"
            )
    else:
        for path in files:
            url = f"{endpoint}/{release_id}/assets?{urllib.parse.urlencode({'name': path.name})}"
            request_json(
                url, token=token, method="POST", data=path.read_bytes(), content_type="application/octet-stream"
            )

    request_json(
        f"{endpoint}/{release_id}",
        token=token,
        method="PATCH",
        payload={"draft": False, "name": tag, "body": release_body(tag), "prerelease": False},
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    assemble_parser = sub.add_parser("assemble")
    assemble_parser.add_argument("--tag", required=True)
    assemble_parser.add_argument("--output", type=Path, default=REPO_ROOT / "build/release")
    assemble_parser.add_argument("--standalone", type=Path, default=REPO_ROOT / "build/pkg/gtavmenu-payload")
    assemble_parser.add_argument("--onionhen", type=Path, default=REPO_ROOT / "build/pkg/onionhen")
    assemble_parser.add_argument("--etahen", type=Path, default=REPO_ROOT / "build/pkg/etahen")
    publish_parser = sub.add_parser("publish")
    publish_parser.add_argument("--platform", choices=("forgejo", "github"), required=True)
    publish_parser.add_argument("--tag", required=True)
    publish_parser.add_argument("--assets", type=Path, default=REPO_ROOT / "build/release")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.command == "assemble":
            output = assemble(
                args.tag,
                args.output.expanduser().resolve(),
                args.standalone.expanduser().resolve(),
                args.onionhen.expanduser().resolve(),
                args.etahen.expanduser().resolve(),
            )
            print(f"release assets assembled: {output}")
        else:
            publish(args.platform, args.tag, args.assets.expanduser().resolve())
            print(f"published {args.tag} to {args.platform}")
    except (OSError, KeyError, ValueError, ReleaseError, subprocess.CalledProcessError) as exc:
        print(f"release failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

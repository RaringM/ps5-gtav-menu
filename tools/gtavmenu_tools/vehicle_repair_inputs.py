"""Validate complete imported fixtures before a repair copies or patches any file."""

from __future__ import annotations

import hashlib
import json
import shutil
import stat
from pathlib import Path, PurePosixPath

from .asset_formats import Limits, safe_name


class FixtureError(ValueError):
    """The supplied repair fixture is not a bounded, self-contained file tree."""


def _no_links(path: Path) -> None:
    for part in (path, *path.parents):
        if part.is_symlink():
            raise FixtureError(f"fixture path traverses a symbolic link: {part}")


def resource_path(fixture: Path, member: str) -> Path:
    """Require the importer's canonical nested-container member syntax."""
    if not isinstance(member, str) or not member or len(member) > 512:
        raise FixtureError("fixture resource member is missing or exceeds its name bound")
    parts = []
    for segment in member.split("!/"):
        path = PurePosixPath(segment)
        if (
            not segment
            or path.is_absolute()
            or any(part in {"", ".", ".."} for part in segment.split("/"))
            or str(path) != segment
            or "!" in segment
            or "\\" in segment
        ):
            raise FixtureError(f"fixture resource member is not canonical: {member!r}")
        try:
            safe_name(segment)
        except ValueError as error:
            raise FixtureError(str(error)) from error
        parts.extend(path.parts)
    result = fixture / "resources" / Path(*parts)
    if not result.is_relative_to(fixture / "resources"):
        raise FixtureError("fixture resource member escapes resources/")
    return result


def _unique(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise FixtureError(f"fixture manifest has duplicate key: {name}")
        result[name] = value
    return result


def validate_fixture(fixture: Path, limits: Limits | None = None) -> dict:
    """Check every tree entry and listed resource, including unselected members.

    Repairs may copy metadata or resources beyond the material report's selected
    members. A selected-member proof therefore cannot authorize this whole-tree
    copy. No links, special files, escaping names, aliasing paths, or stale resource
    identities are accepted here.
    """
    limits = limits or Limits()
    fixture = fixture.absolute()
    _no_links(fixture)
    fixture = fixture.resolve()
    if not fixture.is_dir():
        raise FixtureError("repair fixture must be a real directory")
    pending = [(fixture, 0)]
    total = entries = 0
    files: dict[Path, int] = {}
    aliases = set()
    while pending:
        directory, depth = pending.pop()
        if depth > limits.max_depth * 4:
            raise FixtureError("fixture directory tree exceeds depth limit")
        for path in directory.iterdir():
            entries += 1
            if entries > limits.max_entries:
                raise FixtureError("fixture directory tree exceeds entry limit")
            key = str(path.relative_to(fixture)).casefold()
            if key in aliases:
                raise FixtureError("fixture tree has case-colliding paths")
            aliases.add(key)
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode):
                raise FixtureError(f"fixture contains a symbolic link: {path}")
            if stat.S_ISDIR(info.st_mode):
                pending.append((path, depth + 1))
            elif stat.S_ISREG(info.st_mode):
                if info.st_size > limits.max_file_bytes:
                    raise FixtureError(f"fixture file exceeds byte limit: {path}")
                total += info.st_size
                if total > limits.max_total_bytes:
                    raise FixtureError("fixture file tree exceeds total byte limit")
                files[path] = info.st_size
            else:
                raise FixtureError(f"fixture contains a special file: {path}")
    manifest_path = fixture / "manifest.json"
    if not 0 < files.get(manifest_path, 0) <= limits.max_metadata_bytes:
        raise FixtureError("fixture manifest is missing, empty or exceeds metadata limit")
    with manifest_path.open("rb") as source:
        raw = source.read(limits.max_metadata_bytes + 1)
    if len(raw) != files[manifest_path]:
        raise FixtureError("fixture manifest changed during validation")
    try:
        manifest = json.loads(raw, object_pairs_hook=_unique)
    except (ValueError, UnicodeError) as error:
        raise FixtureError(f"invalid fixture manifest: {error}") from None
    if (
        not isinstance(manifest, dict)
        or manifest.get("kind") != "gtavmenu-pc-asset-import"
        or manifest.get("operation") != "vehicle"
        or not isinstance(manifest.get("resources"), list)
        or not 0 < len(manifest["resources"]) <= limits.max_entries
    ):
        raise FixtureError("fixture manifest must describe a nonempty vehicle import")
    members, paths = set(), set()
    for row in manifest["resources"]:
        if not isinstance(row, dict):
            raise FixtureError("fixture resource row is not an object")
        member = row.get("member")
        path = resource_path(fixture, member)
        if member.casefold() in members or path in paths:
            raise FixtureError("fixture resource members collide or alias the same file")
        members.add(member.casefold())
        paths.add(path)
        expected_size, expected_hash = row.get("bytes"), row.get("sha256")
        if (
            type(expected_size) is not int
            or not 0 < expected_size <= limits.max_file_bytes
            or files.get(path) != expected_size
            or not isinstance(expected_hash, str)
            or len(expected_hash) != 64
            or any(char not in "0123456789abcdef" for char in expected_hash)
        ):
            raise FixtureError(f"fixture resource is missing or has invalid size/identity: {member}")
        digest, count = hashlib.sha256(), 0
        with path.open("rb") as source:
            while chunk := source.read(min(1024 * 1024, expected_size - count + 1)):
                count += len(chunk)
                if count > expected_size:
                    raise FixtureError(f"fixture resource grew during validation: {member}")
                digest.update(chunk)
        if count != expected_size or digest.hexdigest() != expected_hash:
            raise FixtureError(f"fixture resource differs from manifest: {member}")
    return manifest


def copy_fixture(source: Path, output: Path, expected: dict | None = None) -> dict:
    """Recheck the entire input, reserve a fresh output and reject redirected copies."""
    source, output = source.absolute(), output.absolute()
    _no_links(output)
    _no_links(source)
    source, output = source.resolve(), output.resolve()
    if output.exists() or output == source or output.is_relative_to(source):
        raise FixtureError(f"refusing existing or nested repair output: {output}")
    manifest = validate_fixture(source)
    if expected is not None and manifest != expected:
        raise FixtureError("fixture manifest changed before copy")
    # Preserve links if a source entry races the check. The copied-tree validation
    # rejects them before a caller can open a resource path for writing.
    shutil.copytree(source, output, symlinks=True)
    copied = validate_fixture(output)
    if copied != manifest:
        raise FixtureError("fixture manifest changed during copy")
    return copied

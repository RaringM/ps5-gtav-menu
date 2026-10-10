"""Generic strict schema for inactive additive loose-resource packages."""

from __future__ import annotations

import ctypes
import errno
import hashlib
import json
import math
import os
import re
import shutil
import stat
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

SCHEMA_VERSION = 1
PACK_KIND = "gtavmenu-loose-resource-pack"
CATALOG_KIND = "gtavmenu-loose-resource-catalog-entry"
CONSOLE_ROOT = "/data/GTAVMenu/custom"
CONSOLE_CATALOG = f"{CONSOLE_ROOT}/catalog.json"
MAX_RESOURCE_BYTES = 512 * 1024 * 1024
MAX_JSON_BYTES = 1024 * 1024
MAX_JSON_DEPTH = 64
MAX_JSON_NODES = 65536
MAX_JSON_STRING_CHARS = 256 * 1024
MAX_JSON_INTEGER_BITS = 1024

_PACK_ID_RE = re.compile(r"[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?\Z")
_LOGICAL_NAME_RE = re.compile(r"[a-z0-9][a-z0-9_]{0,63}\Z")
_CLASS_RE = re.compile(r"[a-z][a-z0-9-]{0,63}\Z")
_CONTAINER_RE = re.compile(r"[A-Z0-9]{2,8}\Z")
_SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")
_PATH_COMPONENT_RE = re.compile(r"[A-Za-z0-9_ .()+@-]+\Z")

_MANIFEST_KEYS = {
    "schemaVersion",
    "kind",
    "packId",
    "packVersion",
    "target",
    "policy",
    "resources",
    "qualification",
}
_TARGET_KEYS = {
    "targetId",
    "titleId",
    "contentId",
    "contentVersion",
    "manifestSha256",
    "normalizedElfSha256",
}
_RESOURCE_KEYS = {
    "path",
    "consolePath",
    "class",
    "container",
    "logicalName",
    "bytes",
    "sha256",
    "source",
    "oracle",
}
_CATALOG_KEYS = {
    "schemaVersion",
    "kind",
    "packId",
    "packVersion",
    "installPath",
    "manifest",
    "resources",
    "activation",
}
_POLICY = {
    "activationCatalog": CONSOLE_CATALOG,
    "additiveOnly": True,
    "containerMode": "loose",
    "freshProcessRequired": True,
    "retailOverwriteAllowed": False,
    "customRoot": CONSOLE_ROOT,
}
_QUALIFICATION = {
    "offlinePackageVerified": True,
    "packagedResourceByteIdentityVerified": True,
    "resourceSemanticCompatibilityQualified": False,
    "engineMountRouteQualified": False,
    "resourceRegistrationQualified": False,
    "requestCompletionQualified": False,
    "gpuInterpretationQualified": False,
    "runtimeEnabled": False,
    "liveTestReady": False,
    "hardwareQualified": False,
}
_ACTIVATION = {
    "enabled": False,
    "qualified": False,
    "freshProcessRequired": True,
    "route": None,
    "reason": "loose-resource registration and loading are not qualified",
}


class CustomPackSchemaError(ValueError):
    """The loose-resource package is unsafe, inconsistent, or malformed."""


@dataclass(frozen=True)
class LooseResource:
    """One exact resource and the evidence that identifies it."""

    path: str
    resource_class: str
    container: str
    logical_name: str
    data: bytes
    source: dict
    oracle: dict


def sha256_bytes(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def _json_string_bytes(value: str) -> int:
    """Count ensure_ascii=True output without creating an escaped copy."""
    if len(value) > MAX_JSON_STRING_CHARS:
        raise CustomPackSchemaError("JSON string exceeds the character limit")
    count = 2  # Quotes.
    for char in value:
        number = ord(char)
        if char in '\\"\b\t\n\f\r':
            count += 2
        elif number < 32 or 127 <= number <= 0xFFFF:
            count += 6
        elif number > 0xFFFF:
            count += 12
        else:
            count += 1
    return count


def _admit_json(value: object) -> None:
    """Bound tree work and exact canonical size before recursive serialization.

    Shared, acyclic containers are allowed and counted at every appearance, as
    JSON serializes them. Ancestor identities detect cycles without mistaking
    an ordinary repeated subobject for a cycle. Keys count toward the node
    budget, and only exact JSON types are admitted: no coercion of keys,
    tuples, subclasses or arbitrary objects.
    """
    pending = [(value, 0, False)]
    active: set[int] = set()
    nodes, encoded_bytes = 1, 1  # The root and final newline.
    while pending:
        item, depth, leaving = pending.pop()
        if leaving:
            active.remove(id(item))
            continue
        if depth > MAX_JSON_DEPTH:
            raise CustomPackSchemaError("JSON nesting exceeds the depth limit")
        item_type = type(item)
        if item_type is dict or item_type is list:
            if id(item) in active:
                raise CustomPackSchemaError("JSON contains a container cycle")
            count = len(item)
            nodes += count * (2 if item_type is dict else 1)
            if nodes > MAX_JSON_NODES:
                raise CustomPackSchemaError("JSON exceeds the node limit")
            # Brackets/braces, commas, newlines, indentation and (for dicts)
            # colon-space separators. Child scalar/container sizes follow.
            encoded_bytes += 2 if not count else 2 * count * (depth + (3 if item_type is dict else 2)) + 2 * depth + 2
            active.add(id(item))
            pending.append((item, depth, True))
            if item_type is dict:
                for key, child in reversed(item.items()):
                    if type(key) is not str:
                        raise CustomPackSchemaError("JSON object keys must be exact strings")
                    pending.append((child, depth + 1, False))
                    pending.append((key, depth + 1, False))
            else:
                pending.extend((child, depth + 1, False) for child in reversed(item))
        elif item_type is str:
            encoded_bytes += _json_string_bytes(item)
        elif item is None:
            encoded_bytes += 4
        elif item_type is bool:
            encoded_bytes += 4 if item else 5
        elif item_type is int:
            if item.bit_length() > MAX_JSON_INTEGER_BITS:
                raise CustomPackSchemaError("JSON integer exceeds the bit limit")
            encoded_bytes += len(str(item))
        elif item_type is float:
            if not math.isfinite(item):
                raise CustomPackSchemaError("JSON numbers must be finite")
            encoded_bytes += len(repr(item))
        else:
            raise CustomPackSchemaError("value contains a non-JSON type")
        if encoded_bytes > MAX_JSON_BYTES:
            raise CustomPackSchemaError("canonical JSON exceeds the serialized byte limit")


def canonical_json(value: object) -> bytes:
    _admit_json(value)
    try:
        return (json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n").encode("ascii")
    except (TypeError, ValueError, UnicodeEncodeError, RecursionError) as exc:
        raise CustomPackSchemaError(f"value is not canonical ASCII JSON: {exc}") from exc


def _unique_json_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise CustomPackSchemaError("JSON object contains a duplicate key")
        result[key] = value
    return result


def _check_json_nesting(text: str) -> None:
    """Bound parser nesting before invoking the recursive JSON implementation.

    This is not a second JSON parser: grammar, escape validity and matching
    delimiter types remain json.loads' responsibility. Quotes and backslashes
    suffice to exclude string contents from the container-depth count.
    """
    depth, quoted, escaped = 0, False, False
    for char in text:
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
        elif char in "[{":
            depth += 1
            # The root container has tree depth zero. The exact tree check
            # also checks scalar depths after the bounded parse succeeds.
            if depth > MAX_JSON_DEPTH + 1:
                raise CustomPackSchemaError("JSON nesting exceeds the depth limit")
        elif char in "]}":
            depth -= 1
            if depth < 0:
                raise CustomPackSchemaError("JSON closing delimiter has no matching container")


def load_json(blob: bytes, label: str) -> dict:
    if type(blob) is not bytes or not 0 < len(blob) <= MAX_JSON_BYTES:
        raise CustomPackSchemaError(f"{label} must contain bounded immutable JSON bytes")
    try:
        text = blob.decode("utf-8")
        _check_json_nesting(text)
        value = json.loads(text, object_pairs_hook=_unique_json_object)
    except (UnicodeDecodeError, ValueError, RecursionError) as exc:
        raise CustomPackSchemaError(f"{label} is not valid UTF-8 JSON: {exc}") from exc
    if type(value) is not dict:
        raise CustomPackSchemaError(f"{label} root must be an object")
    _admit_json(value)
    return value


def read_regular(path: Path, *, maximum: int = MAX_RESOURCE_BYTES) -> bytes:
    if path.is_symlink():
        raise CustomPackSchemaError(f"expected a regular non-symlink file: {path}")
    try:
        fd = os.open(
            path,
            os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0),
        )
    except OSError as exc:
        raise CustomPackSchemaError(f"cannot open a regular non-symlink file: {path}") from exc
    with os.fdopen(fd, "rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= maximum:
            raise CustomPackSchemaError(f"file is empty, special, or exceeds {maximum} bytes: {path}")
        blob = stream.read(maximum + 1)
        after = os.fstat(stream.fileno())
    before_identity = (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
    after_identity = (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
    if len(blob) != before.st_size or before_identity != after_identity:
        raise CustomPackSchemaError(f"file changed while being read: {path}")
    return blob


def _require_exact_keys(value: object, keys: set[str], label: str) -> dict:
    if not isinstance(value, dict) or set(value) != keys:
        actual = sorted(value) if isinstance(value, dict) else type(value).__name__
        raise CustomPackSchemaError(f"{label} fields differ: {actual}")
    return value


def _require_sha256(value: object, label: str) -> str:
    if not isinstance(value, str) or not _SHA256_RE.fullmatch(value):
        raise CustomPackSchemaError(f"{label} must be a lowercase SHA-256")
    return value


def _pack_id(value: object) -> str:
    if not isinstance(value, str) or not _PACK_ID_RE.fullmatch(value):
        raise CustomPackSchemaError("packId must be a conservative lowercase identifier")
    return value


def _resource_path(value: object) -> str:
    if not isinstance(value, str) or len(value) > 160 or not value.isascii():
        raise CustomPackSchemaError("resource path must be bounded ASCII")
    path = PurePosixPath(value)
    if path.is_absolute() or len(path.parts) < 2 or path.parts[0] != "resources":
        raise CustomPackSchemaError("resource path must be relative below resources/")
    if any(part in ("", ".", "..") or part.endswith((" ", ".")) for part in path.parts):
        raise CustomPackSchemaError("resource path contains traversal or a non-portable component")
    if any(not _PATH_COMPONENT_RE.fullmatch(part) for part in path.parts):
        raise CustomPackSchemaError("resource path contains a non-portable character")
    if str(path) != value:
        raise CustomPackSchemaError("resource path must already be canonical")
    return value


def target_identity(target_blob: bytes) -> dict:
    """Extract the publication identity from an exact target-manifest blob."""

    target = load_json(target_blob, "target manifest")
    binary = target.get("binaryIdentity")
    normalized = binary.get("normalizedElf") if isinstance(binary, dict) else None
    fields = {key: target.get(key) for key in ("targetId", "titleId", "contentId", "contentVersion")}
    if any(not isinstance(value, str) or not value for value in fields.values()):
        raise CustomPackSchemaError("target manifest identity fields are missing")
    normalized_sha = normalized.get("sha256") if isinstance(normalized, dict) else None
    return {
        **fields,
        "manifestSha256": sha256_bytes(target_blob),
        "normalizedElfSha256": _require_sha256(normalized_sha, "normalized ELF identity"),
    }


def _resource_record(pack_id: str, resource: LooseResource) -> dict:
    path = _resource_path(resource.path)
    if not isinstance(resource.resource_class, str) or not _CLASS_RE.fullmatch(resource.resource_class):
        raise CustomPackSchemaError("resource class is invalid")
    if not isinstance(resource.container, str) or not _CONTAINER_RE.fullmatch(resource.container):
        raise CustomPackSchemaError("resource container is invalid")
    if not isinstance(resource.logical_name, str) or not _LOGICAL_NAME_RE.fullmatch(resource.logical_name):
        raise CustomPackSchemaError("resource logical name is invalid")
    if not isinstance(resource.source, dict) or not isinstance(resource.oracle, dict):
        raise CustomPackSchemaError("resource source and oracle must be objects")
    if not resource.data or len(resource.data) > MAX_RESOURCE_BYTES:
        raise CustomPackSchemaError("resource is empty or exceeds the package bound")
    # Prove source/oracle values are deterministic JSON before publishing anything.
    canonical_json(resource.source)
    canonical_json(resource.oracle)
    return {
        "path": path,
        "consolePath": f"{CONSOLE_ROOT}/packs/{pack_id}/{path}",
        "class": resource.resource_class,
        "container": resource.container,
        "logicalName": resource.logical_name,
        "bytes": len(resource.data),
        "sha256": sha256_bytes(resource.data),
        "source": resource.source,
        "oracle": resource.oracle,
    }


def build_manifest(pack_id: str, target: dict, resources: list[LooseResource], *, pack_version: int = 1) -> dict:
    pack_id = _pack_id(pack_id)
    _validate_target(target)
    if not isinstance(pack_version, int) or isinstance(pack_version, bool) or not 1 <= pack_version <= 0xFFFFFFFF:
        raise CustomPackSchemaError("packVersion must be a positive 32-bit integer")
    if not 1 <= len(resources) <= 256:
        raise CustomPackSchemaError("package must contain one to 256 resources")
    records = sorted((_resource_record(pack_id, resource) for resource in resources), key=lambda record: record["path"])
    paths = [record["path"] for record in records]
    logical_names = [record["logicalName"] for record in records]
    if len(set(paths)) != len(paths) or len(set(logical_names)) != len(logical_names):
        raise CustomPackSchemaError("resource paths and logical names must be unique")
    return {
        "schemaVersion": SCHEMA_VERSION,
        "kind": PACK_KIND,
        "packId": pack_id,
        "packVersion": pack_version,
        "target": target,
        "policy": dict(_POLICY),
        "resources": records,
        "qualification": dict(_QUALIFICATION),
    }


def build_catalog_entry(manifest_blob: bytes, manifest: dict) -> dict:
    pack_id = _pack_id(manifest.get("packId"))
    resources = manifest.get("resources")
    if not isinstance(resources, list):
        raise CustomPackSchemaError("manifest resources must be an array")
    return {
        "schemaVersion": SCHEMA_VERSION,
        "kind": CATALOG_KIND,
        "packId": pack_id,
        "packVersion": manifest.get("packVersion"),
        "installPath": f"{CONSOLE_ROOT}/packs/{pack_id}",
        "manifest": {
            "path": "manifest.json",
            "bytes": len(manifest_blob),
            "sha256": sha256_bytes(manifest_blob),
        },
        "resources": [
            {
                "class": record["class"],
                "container": record["container"],
                "logicalName": record["logicalName"],
                "path": record["path"],
                "bytes": record["bytes"],
                "sha256": record["sha256"],
            }
            for record in resources
        ],
        "activation": dict(_ACTIVATION),
    }


def expected_package(manifest: dict, resources: dict[str, bytes]) -> dict[str, bytes]:
    _validate_manifest(manifest)
    expected_paths = {record["path"] for record in manifest["resources"]}
    if set(resources) != expected_paths:
        raise CustomPackSchemaError("resource blobs differ from the manifest file set")
    for record in manifest["resources"]:
        blob = resources[record["path"]]
        if len(blob) != record["bytes"] or sha256_bytes(blob) != record["sha256"]:
            raise CustomPackSchemaError(f"resource blob identity differs: {record['path']}")
    manifest_blob = canonical_json(manifest)
    return {
        **resources,
        "manifest.json": manifest_blob,
        "catalog-entry.json": canonical_json(build_catalog_entry(manifest_blob, manifest)),
    }


def _validate_target(target: object) -> dict:
    target = _require_exact_keys(target, _TARGET_KEYS, "target identity")
    for key in ("targetId", "titleId", "contentId", "contentVersion"):
        if not isinstance(target[key], str) or not target[key] or not target[key].isascii():
            raise CustomPackSchemaError(f"target identity {key} is invalid")
    _require_sha256(target["manifestSha256"], "target manifest identity")
    _require_sha256(target["normalizedElfSha256"], "normalized ELF identity")
    return target


def _validate_resource_record(record: object, pack_id: str) -> dict:
    record = _require_exact_keys(record, _RESOURCE_KEYS, "resource record")
    path = _resource_path(record["path"])
    if record["consolePath"] != f"{CONSOLE_ROOT}/packs/{pack_id}/{path}":
        raise CustomPackSchemaError("resource console path is not derived from its package path")
    if not isinstance(record["class"], str) or not _CLASS_RE.fullmatch(record["class"]):
        raise CustomPackSchemaError("resource class is invalid")
    if not isinstance(record["container"], str) or not _CONTAINER_RE.fullmatch(record["container"]):
        raise CustomPackSchemaError("resource container is invalid")
    if not isinstance(record["logicalName"], str) or not _LOGICAL_NAME_RE.fullmatch(record["logicalName"]):
        raise CustomPackSchemaError("resource logical name is invalid")
    if (
        not isinstance(record["bytes"], int)
        or isinstance(record["bytes"], bool)
        or not 1 <= record["bytes"] <= MAX_RESOURCE_BYTES
    ):
        raise CustomPackSchemaError("resource byte count is invalid")
    _require_sha256(record["sha256"], "resource identity")
    if not isinstance(record["source"], dict) or not isinstance(record["oracle"], dict):
        raise CustomPackSchemaError("resource source and oracle must be objects")
    canonical_json(record["source"])
    canonical_json(record["oracle"])
    return record


def _validate_manifest(manifest: object) -> dict:
    manifest = _require_exact_keys(manifest, _MANIFEST_KEYS, "manifest")
    if type(manifest["schemaVersion"]) is not int or manifest["schemaVersion"] != SCHEMA_VERSION:
        raise CustomPackSchemaError("unsupported loose-resource package schema or kind")
    if not isinstance(manifest["kind"], str) or manifest["kind"] != PACK_KIND:
        raise CustomPackSchemaError("unsupported loose-resource package schema or kind")
    pack_id = _pack_id(manifest["packId"])
    if (
        not isinstance(manifest["packVersion"], int)
        or isinstance(manifest["packVersion"], bool)
        or not 1 <= manifest["packVersion"] <= 0xFFFFFFFF
    ):
        raise CustomPackSchemaError("packVersion must be a positive 32-bit integer")
    _validate_target(manifest["target"])
    if canonical_json(manifest["policy"]) != canonical_json(_POLICY):
        raise CustomPackSchemaError("package policy is not the fixed additive loose-resource policy")
    if canonical_json(manifest["qualification"]) != canonical_json(_QUALIFICATION):
        raise CustomPackSchemaError("package qualification must keep every runtime and hardware gate closed")
    resources = manifest["resources"]
    if not isinstance(resources, list) or not 1 <= len(resources) <= 256:
        raise CustomPackSchemaError("package must contain one to 256 resources")
    records = [_validate_resource_record(record, pack_id) for record in resources]
    paths = [record["path"] for record in records]
    logical_names = [record["logicalName"] for record in records]
    if paths != sorted(paths) or len(set(paths)) != len(paths) or len(set(logical_names)) != len(logical_names):
        raise CustomPackSchemaError("resources must have sorted unique paths and unique logical names")
    return manifest


def _expected_dirs(files: set[str]) -> set[str]:
    result: set[str] = set()
    for name in files:
        parent = PurePosixPath(name).parent
        while str(parent) != ".":
            result.add(str(parent))
            parent = parent.parent
    return result


def _walk_package(package: Path, expected_files: set[str]) -> None:
    if package.is_symlink() or not package.is_dir():
        raise CustomPackSchemaError("custom package must be a real non-symlink directory")
    found_files: set[str] = set()
    found_dirs: set[str] = set()
    expected_dirs = _expected_dirs(expected_files)
    for path in package.rglob("*"):
        if path.is_symlink():
            raise CustomPackSchemaError(f"custom package contains a symlink: {path}")
        relative = path.relative_to(package).as_posix()
        if path.is_file():
            if relative not in expected_files:
                raise CustomPackSchemaError(f"custom package tree differs: unexpected file {relative}")
            found_files.add(relative)
        elif path.is_dir():
            if relative not in expected_dirs:
                raise CustomPackSchemaError(f"custom package tree differs: unexpected directory {relative}")
            found_dirs.add(relative)
        else:
            raise CustomPackSchemaError(f"custom package contains a special file: {path}")
    if found_files != expected_files or found_dirs != expected_dirs:
        raise CustomPackSchemaError(
            f"custom package tree differs: files={sorted(found_files)}, dirs={sorted(found_dirs)}"
        )


def _validate_package_limits(max_resources: int, max_resource_bytes: int) -> None:
    if type(max_resources) is not int or not 1 <= max_resources <= 256:
        raise CustomPackSchemaError("resource count limit must be an integer from 1 to 256")
    if type(max_resource_bytes) is not int or not 1 <= max_resource_bytes <= MAX_RESOURCE_BYTES:
        raise CustomPackSchemaError(f"resource byte limit must be an integer from 1 to {MAX_RESOURCE_BYTES}")


def verify_package(
    package: Path,
    *,
    expected_target: dict,
    expected_manifest: dict | None = None,
    max_resources: int = 256,
    max_resource_bytes: int = MAX_RESOURCE_BYTES,
) -> dict:
    """Verify a package within explicit per-resource and resource-count bounds.

    Manifest declarations are checked before walking resource directories or
    opening their files. Each actual read is additionally bounded by its exact
    declared length, including when that declaration understates the file size.
    """
    _validate_package_limits(max_resources, max_resource_bytes)
    if package.is_symlink() or not package.is_dir():
        raise CustomPackSchemaError("custom package must be a real non-symlink directory")
    manifest_blob = read_regular(package / "manifest.json", maximum=MAX_JSON_BYTES)
    catalog_blob = read_regular(package / "catalog-entry.json", maximum=MAX_JSON_BYTES)
    manifest = _validate_manifest(load_json(manifest_blob, "manifest"))
    if manifest_blob != canonical_json(manifest):
        raise CustomPackSchemaError("manifest must use canonical sorted JSON")
    if manifest["target"] != _validate_target(expected_target):
        raise CustomPackSchemaError("custom package target identity mismatch")
    if expected_manifest is not None:
        reviewed_manifest = _validate_manifest(expected_manifest)
        if canonical_json(manifest) != canonical_json(reviewed_manifest):
            raise CustomPackSchemaError("custom package manifest differs from the reviewed inputs")
    if len(manifest["resources"]) > max_resources:
        raise CustomPackSchemaError("custom package exceeds the resource count limit")
    if any(record["bytes"] > max_resource_bytes for record in manifest["resources"]):
        raise CustomPackSchemaError("custom package exceeds the resource byte limit")
    expected_files = {"manifest.json", "catalog-entry.json", *(row["path"] for row in manifest["resources"])}
    _walk_package(package, expected_files)
    for record in manifest["resources"]:
        blob = read_regular(package / record["path"], maximum=record["bytes"])
        if len(blob) != record["bytes"] or sha256_bytes(blob) != record["sha256"]:
            raise CustomPackSchemaError(f"packaged resource identity differs: {record['path']}")
    catalog = _require_exact_keys(load_json(catalog_blob, "catalog entry"), _CATALOG_KEYS, "catalog entry")
    if catalog_blob != canonical_json(catalog):
        raise CustomPackSchemaError("catalog entry must use canonical sorted JSON")
    if canonical_json(catalog) != canonical_json(build_catalog_entry(manifest_blob, manifest)):
        raise CustomPackSchemaError("catalog entry is inconsistent with the package manifest")
    return manifest


def _encoded_publication_path(path: Path) -> bytes:
    encoded = os.fsencode(path)
    if b"\0" in encoded:
        raise CustomPackSchemaError("package publication paths must not contain NUL bytes")
    return encoded


def _rename_directory_noreplace(source: Path, destination: Path) -> None:
    """Atomically promote a directory without replacing any destination entry.

    POSIX rename/replace alone cannot provide this contract. Missing native
    support is an error, never permission to fall back to a check-then-rename.
    Linux renameat2: https://man7.org/linux/man-pages/man2/rename.2.html
    Darwin renamex_np: Apple's rename(2), RENAME_EXCL = 0x00000004.
    Windows os.rename raises FileExistsError for any existing destination.
    """
    source_bytes, destination_bytes = _encoded_publication_path(source), _encoded_publication_path(destination)
    if sys.platform == "win32":
        os.rename(source, destination)
        return
    if sys.platform.startswith("linux"):
        symbol = "renameat2"
        argument_types = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        # AT_FDCWD = -100; RENAME_NOREPLACE = 1.
        arguments = (-100, source_bytes, -100, destination_bytes, 1)
    elif sys.platform == "darwin":
        symbol = "renamex_np"
        argument_types = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
        arguments = (source_bytes, destination_bytes, 4)
    else:
        raise CustomPackSchemaError(f"atomic no-replace package publication is unsupported on {sys.platform}")
    try:
        library = ctypes.CDLL(None, use_errno=True)
        rename = getattr(library, symbol)
    except (AttributeError, OSError) as exc:
        raise CustomPackSchemaError(f"atomic no-replace package publication requires native {symbol} support") from exc
    rename.argtypes = argument_types
    rename.restype = ctypes.c_int
    ctypes.set_errno(0)
    if rename(*arguments) != 0:
        code = ctypes.get_errno() or errno.EIO
        if code in (errno.ENOSYS, errno.EINVAL, errno.ENOTSUP, errno.EOPNOTSUPP):
            raise CustomPackSchemaError(
                f"atomic no-replace package publication is unsupported by this kernel/filesystem: {os.strerror(code)}"
            )
        raise OSError(code, os.strerror(code), os.fspath(destination))


def _cleanup_staging(temporary: Path, identity: tuple[int, int]) -> None:
    """Remove only the unique staging directory created by this invocation."""
    try:
        current = temporary.lstat()
    except FileNotFoundError:
        # Promotion may have completed immediately before an interruption.
        # The destination is never a cleanup target.
        return
    if not stat.S_ISDIR(current.st_mode) or (current.st_dev, current.st_ino) != identity:
        raise CustomPackSchemaError(f"staging identity changed; refusing cleanup: {temporary}")
    shutil.rmtree(temporary)


def publish_package(
    package: Path,
    files: dict[str, bytes],
    *,
    max_resources: int = 256,
    max_resource_bytes: int = MAX_RESOURCE_BYTES,
) -> None:
    """Verify complete private staging, then atomically publish create-new output.

    Staging is a unique sibling (owner-only on POSIX). Failures and catchable
    interruptions clean up this invocation's staging only. Neither process
    termination nor power-loss durability is guaranteed; a completed promotion
    is never rolled back by deleting the destination.
    """
    _validate_package_limits(max_resources, max_resource_bytes)
    _encoded_publication_path(package)
    manifest_blob = files.get("manifest.json")
    if manifest_blob is None:
        raise CustomPackSchemaError("package files are missing manifest.json")
    manifest = _validate_manifest(load_json(manifest_blob, "manifest"))
    if len(manifest["resources"]) > max_resources:
        raise CustomPackSchemaError("custom package exceeds the resource count limit")
    if any(record["bytes"] > max_resource_bytes for record in manifest["resources"]):
        raise CustomPackSchemaError("custom package exceeds the resource byte limit")
    if manifest_blob != canonical_json(manifest):
        raise CustomPackSchemaError("manifest must use canonical sorted JSON")
    expected_files = {"manifest.json", "catalog-entry.json", *(row["path"] for row in manifest["resources"])}
    if set(files) != expected_files:
        raise CustomPackSchemaError("internal package file set differs from the manifest")
    for record in manifest["resources"]:
        blob = files[record["path"]]
        if len(blob) != record["bytes"] or sha256_bytes(blob) != record["sha256"]:
            raise CustomPackSchemaError(f"resource blob identity differs: {record['path']}")
    catalog = load_json(files["catalog-entry.json"], "catalog entry")
    if files["catalog-entry.json"] != canonical_json(catalog) or canonical_json(catalog) != canonical_json(
        build_catalog_entry(manifest_blob, manifest)
    ):
        raise CustomPackSchemaError("catalog entry is inconsistent with the package manifest")
    if package.is_symlink() or package.exists():
        raise CustomPackSchemaError(f"refusing to replace an existing package: {package}")
    package.parent.mkdir(parents=True, exist_ok=True)
    # A fixed, bounded prefix also supports output basenames near NAME_MAX.
    temporary = Path(tempfile.mkdtemp(prefix=".gtavmenu-package-", suffix=".tmp", dir=package.parent))
    identity = None
    try:
        created = temporary.lstat()
        identity = (created.st_dev, created.st_ino)
        for directory in sorted(_expected_dirs(expected_files), key=lambda value: (value.count("/"), value)):
            (temporary / directory).mkdir()
        for relative, blob in files.items():
            path = temporary / relative
            with path.open("xb") as stream:
                stream.write(blob)
        verify_package(
            temporary,
            expected_target=manifest["target"],
            expected_manifest=manifest,
            max_resources=max_resources,
            max_resource_bytes=max_resource_bytes,
        )
        _rename_directory_noreplace(temporary, package)
    except BaseException as exc:
        if identity is None:
            exc.add_note(f"Could not establish staging ownership; retained staging: {temporary}")
            raise
        try:
            _cleanup_staging(temporary, identity)
        except BaseException as cleanup_error:
            exc.add_note(f"Could not clean staging {temporary}: {cleanup_error}")
        raise


def compare_package(package: Path, files: dict[str, bytes], *, expected_target: dict, expected_manifest: dict) -> dict:
    manifest = verify_package(package, expected_target=expected_target, expected_manifest=expected_manifest)
    if set(files) != {path.relative_to(package).as_posix() for path in package.rglob("*") if path.is_file()}:
        raise CustomPackSchemaError("existing package file set differs from expected output")
    for relative, expected in files.items():
        if read_regular(package / relative) != expected:
            raise CustomPackSchemaError(f"existing package differs from reviewed input: {relative}")
    return manifest

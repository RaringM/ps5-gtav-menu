"""Strict package format for additive GTAV-Menu custom resources."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path

SCHEMA_VERSION = 1
PACK_KIND = "gtavmenu-custom-resource-pack"
CATALOG_ENTRY_KIND = "gtavmenu-custom-resource-catalog-entry"
PACK_ID = "gtavmenu-authored-bc1-v1"
PACK_VERSION = 1
LOGICAL_NAME = "gtavmenu_authored_bc1"
NAME_KEY = 1920351082
RESOURCE_PATH = f"resources/{LOGICAL_NAME}.ptd"
CONSOLE_ROOT = "/data/GTAVMenu/custom"
CONSOLE_PACK_ROOT = f"{CONSOLE_ROOT}/packs/{PACK_ID}"
CONSOLE_CATALOG = f"{CONSOLE_ROOT}/catalog.json"
RESOURCE_BYTES = 16405
RESOURCE_SHA256 = "8a341af5ff55fb574846bb8a3b49619b63af2e869c7995457132331de48435d4"
BUNDLE_BYTES = 18096
BUNDLE_SHA256 = "b8fc61d889ed553371af3fb50e24884e5945718d68214e4410f60ece5e8f817d"
TARGET_MANIFEST_SHA256 = "ede4b7143305eb4f8a7c5090f21aa48d8c324bc69d347ce20375b2328e8412fc"
TARGET_ID = "PPSA04264_01.010.002_DISC"
TITLE_ID = "PPSA04264"
CONTENT_ID = "UP1004-PPSA04264_00-GTAVFULLGAMEDISC"
CONTENT_VERSION = "01.010.002"
NORMALIZED_ELF_SHA256 = "2a3419b404a5d10f2d36d8056916e65afad6352b78489365e5f1003f8e953c0d"

_PACKAGE_FILES = {"manifest.json", "catalog-entry.json", RESOURCE_PATH}
_MANIFEST_KEYS = {
    "schemaVersion",
    "kind",
    "packId",
    "packVersion",
    "target",
    "policy",
    "sourceBundle",
    "resource",
    "visualOracle",
    "qualification",
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


class CustomPackError(ValueError):
    """The custom pack is unsafe, inconsistent, or not the reviewed candidate."""


def sha256_bytes(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def canonical_json(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("ascii")


def read_regular(path: Path, *, maximum: int = 2 * 1024 * 1024) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise CustomPackError(f"expected a regular non-symlink file: {path}")
    blob = path.read_bytes()
    if not blob or len(blob) > maximum:
        raise CustomPackError(f"file is empty or exceeds {maximum} bytes: {path}")
    return blob


def _require_keys(value: object, keys: set[str], label: str) -> dict:
    if not isinstance(value, dict) or set(value) != keys:
        actual = sorted(value) if isinstance(value, dict) else type(value).__name__
        raise CustomPackError(f"{label} fields differ: {actual}")
    return value


def load_json(blob: bytes, label: str) -> dict:
    try:
        value = json.loads(blob)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise CustomPackError(f"{label} is not valid UTF-8 JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise CustomPackError(f"{label} root must be an object")
    return value


def _target(target_blob: bytes) -> dict:
    if len(target_blob) == 0 or sha256_bytes(target_blob) != TARGET_MANIFEST_SHA256:
        raise CustomPackError("target manifest is not the reviewed PPSA04264 01.010.002 manifest")
    target = load_json(target_blob, "target manifest")
    binary = target.get("binaryIdentity")
    normalized = binary.get("normalizedElf") if isinstance(binary, dict) else None
    expected = {
        "targetId": TARGET_ID,
        "titleId": TITLE_ID,
        "contentId": CONTENT_ID,
        "contentVersion": CONTENT_VERSION,
    }
    for key, value in expected.items():
        if target.get(key) != value:
            raise CustomPackError(f"target manifest {key} mismatch")
    if not isinstance(normalized, dict) or normalized.get("sha256") != NORMALIZED_ELF_SHA256:
        raise CustomPackError("target normalized ELF identity mismatch")
    return {
        **expected,
        "manifestSha256": TARGET_MANIFEST_SHA256,
        "normalizedElfSha256": NORMALIZED_ELF_SHA256,
    }


def _manifest(target: dict) -> dict:
    return {
        "schemaVersion": SCHEMA_VERSION,
        "kind": PACK_KIND,
        "packId": PACK_ID,
        "packVersion": PACK_VERSION,
        "target": target,
        "policy": {
            "additiveOnly": True,
            "retailOverwriteAllowed": False,
            "freshProcessRequired": True,
            "customRoot": CONSOLE_ROOT,
            "activationCatalog": CONSOLE_CATALOG,
        },
        "sourceBundle": {
            "bytes": BUNDLE_BYTES,
            "sha256": BUNDLE_SHA256,
            "purpose": "offline-review-only",
        },
        "resource": {
            "path": RESOURCE_PATH,
            "consolePath": f"{CONSOLE_PACK_ROOT}/{RESOURCE_PATH}",
            "class": "texture-dictionary",
            "container": "PTD",
            "surfaceFormat": "BC1_UNORM",
            "bytes": RESOURCE_BYTES,
            "sha256": RESOURCE_SHA256,
            "logicalDictionary": LOGICAL_NAME,
            "textureName": LOGICAL_NAME,
            "nameKey": NAME_KEY,
            "width": 16,
            "height": 16,
            "mipCount": 1,
        },
        "visualOracle": {
            "upperLeft": "red",
            "upperRight": "green",
            "lowerLeft": "blue",
            "lowerRight": "white",
        },
        "qualification": {
            "offlinePackageVerified": True,
            "engineMountRouteQualified": False,
            "requestCompletionQualified": False,
            "gameThreadContextQualified": False,
            "retainedOwnershipQualified": False,
            "cancellationQualified": False,
            "shutdownQualified": False,
            "freshProcessQualified": False,
            "gpuInterpretationQualified": False,
            "runtimeEnabled": False,
            "liveTestReady": False,
            "hardwareQualified": False,
        },
    }


def build_manifest(resource: bytes, bundle: bytes, target_blob: bytes) -> dict:
    if len(resource) != RESOURCE_BYTES or sha256_bytes(resource) != RESOURCE_SHA256:
        raise CustomPackError("authored PTD size or SHA-256 does not match the reviewed candidate")
    if len(bundle) != BUNDLE_BYTES or sha256_bytes(bundle) != BUNDLE_SHA256:
        raise CustomPackError("offline review bundle size or SHA-256 does not match the accepted bundle")
    return _manifest(_target(target_blob))


def build_catalog_entry(manifest_blob: bytes) -> dict:
    return {
        "schemaVersion": SCHEMA_VERSION,
        "kind": CATALOG_ENTRY_KIND,
        "packId": PACK_ID,
        "packVersion": PACK_VERSION,
        "installPath": CONSOLE_PACK_ROOT,
        "manifest": {
            "path": "manifest.json",
            "bytes": len(manifest_blob),
            "sha256": sha256_bytes(manifest_blob),
        },
        "resources": [
            {
                "class": "texture-dictionary",
                "logicalName": LOGICAL_NAME,
                "path": RESOURCE_PATH,
                "bytes": RESOURCE_BYTES,
                "sha256": RESOURCE_SHA256,
            }
        ],
        "activation": {
            "enabled": False,
            "qualified": False,
            "freshProcessRequired": True,
            "route": None,
            "reason": "engine mount/lifetime route is not qualified",
        },
    }


def _walk_package(package: Path) -> set[str]:
    if package.is_symlink() or not package.is_dir():
        raise CustomPackError("custom package must be a real non-symlink directory")
    found: set[str] = set()
    for path in package.rglob("*"):
        if path.is_symlink():
            raise CustomPackError(f"custom package contains a symlink: {path}")
        relative = path.relative_to(package).as_posix()
        if path.is_file():
            found.add(relative)
        elif not path.is_dir():
            raise CustomPackError(f"custom package contains a special file: {path}")
    if found != _PACKAGE_FILES:
        raise CustomPackError(f"custom package file set differs: {sorted(found)}")
    return found


def verify_package(package: Path) -> dict:
    _walk_package(package)
    resource = read_regular(package / RESOURCE_PATH)
    manifest_blob = read_regular(package / "manifest.json")
    entry_blob = read_regular(package / "catalog-entry.json")
    manifest = _require_keys(load_json(manifest_blob, "manifest"), _MANIFEST_KEYS, "manifest")
    entry = _require_keys(load_json(entry_blob, "catalog entry"), _CATALOG_KEYS, "catalog entry")

    if manifest_blob != canonical_json(manifest) or entry_blob != canonical_json(entry):
        raise CustomPackError("manifest and catalog entry must use canonical sorted JSON")
    if manifest.get("schemaVersion") != SCHEMA_VERSION or manifest.get("kind") != PACK_KIND:
        raise CustomPackError("unsupported custom pack schema or kind")
    if manifest.get("packId") != PACK_ID or manifest.get("packVersion") != PACK_VERSION:
        raise CustomPackError("custom pack identity mismatch")
    if manifest.get("target") != {
        "targetId": TARGET_ID,
        "titleId": TITLE_ID,
        "contentId": CONTENT_ID,
        "contentVersion": CONTENT_VERSION,
        "manifestSha256": TARGET_MANIFEST_SHA256,
        "normalizedElfSha256": NORMALIZED_ELF_SHA256,
    }:
        raise CustomPackError("custom pack target identity mismatch")

    if len(resource) != RESOURCE_BYTES or sha256_bytes(resource) != RESOURCE_SHA256:
        raise CustomPackError("packaged PTD identity mismatch")
    expected_manifest = _manifest(
        {
            "targetId": TARGET_ID,
            "titleId": TITLE_ID,
            "contentId": CONTENT_ID,
            "contentVersion": CONTENT_VERSION,
            "manifestSha256": TARGET_MANIFEST_SHA256,
            "normalizedElfSha256": NORMALIZED_ELF_SHA256,
        }
    )
    if manifest != expected_manifest:
        raise CustomPackError("manifest fields differ from the reviewed candidate")
    expected_entry = build_catalog_entry(manifest_blob)
    if entry != expected_entry:
        raise CustomPackError("catalog entry is inconsistent with the package manifest")
    if manifest.get("policy") != {
        "additiveOnly": True,
        "retailOverwriteAllowed": False,
        "freshProcessRequired": True,
        "customRoot": CONSOLE_ROOT,
        "activationCatalog": CONSOLE_CATALOG,
    }:
        raise CustomPackError("custom pack policy is not the fail-closed additive policy")
    qualification = manifest.get("qualification")
    if not isinstance(qualification, dict) or qualification.get("liveTestReady") is not False:
        raise CustomPackError("unqualified package must declare liveTestReady=false")
    if qualification.get("engineMountRouteQualified") is not False or qualification.get("runtimeEnabled") is not False:
        raise CustomPackError("unqualified package cannot enable the mount route or runtime")
    return manifest


def expected_package(resource: bytes, bundle: bytes, target_blob: bytes) -> dict[str, bytes]:
    manifest = build_manifest(resource, bundle, target_blob)
    manifest_blob = canonical_json(manifest)
    return {
        RESOURCE_PATH: resource,
        "manifest.json": manifest_blob,
        "catalog-entry.json": canonical_json(build_catalog_entry(manifest_blob)),
    }


def publish_package(package: Path, files: dict[str, bytes]) -> None:
    if set(files) != _PACKAGE_FILES:
        raise CustomPackError("internal package file set mismatch")
    if package.is_symlink() or package.exists():
        raise CustomPackError(f"refusing to replace an existing package: {package}")
    package.parent.mkdir(parents=True, exist_ok=True)
    temporary = package.with_name(f".{package.name}.tmp")
    if temporary.exists() or temporary.is_symlink():
        raise CustomPackError(f"temporary package path already exists: {temporary}")
    temporary.mkdir()
    try:
        (temporary / "resources").mkdir()
        for relative, blob in files.items():
            path = temporary / relative
            path.write_bytes(blob)
            if path.read_bytes() != blob:
                raise CustomPackError(f"package write readback failed: {relative}")
        os.replace(temporary, package)
    except Exception:
        for relative in _PACKAGE_FILES:
            path = temporary / relative
            if path.is_file() and not path.is_symlink():
                path.unlink()
        resources = temporary / "resources"
        if resources.is_dir() and not resources.is_symlink():
            resources.rmdir()
        if temporary.is_dir() and not temporary.is_symlink():
            temporary.rmdir()
        raise


def compare_package(package: Path, files: dict[str, bytes]) -> None:
    verify_package(package)
    for relative, expected in files.items():
        if read_regular(package / relative) != expected:
            raise CustomPackError(f"existing package differs from reviewed input: {relative}")

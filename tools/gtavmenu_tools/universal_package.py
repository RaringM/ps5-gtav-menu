"""Universal delivery identities, embedded-byte proofs, and package verification."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path, PurePosixPath

from gtavmenu_tools.etahen import (
    ELF_HEADER,
    HEADER_SIZE,
    PROGRAM_HEADER,
    inspect_container,
    validate_elf,
    validate_metadata,
)
from gtavmenu_tools.onionhen import inspect_plugin_bytes
from gtavmenu_tools.target_profile import validate_manifest

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGETS = ("ppsa04264-01.005.000", "ppsa04264-01.010.002", "ppsa04263-01.010.002")
LAYOUTS = {
    "standalone": {"gtav-menu-daemon.elf": "daemon", "README.md": "documentation"},
    "onionhen": {"GTAV00001.elf": "onionhen-plugin", "README.md": "documentation"},
    "etahen": {"GTAV00001.plugin": "etahen-plugin", "README.md": "documentation"},
}


class UniversalPackageError(ValueError):
    """A package does not satisfy the complete universal production contract."""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise UniversalPackageError(f"expected an object: {path}")
    return value


def expected_contract(manifest: dict) -> dict:
    """The exact reviewed loader/render inputs included in each generated registry row."""
    loader_keys = (
        "caveAddress",
        "caveAllocationBytes",
        "playerPedAnchor",
        "playerPedOffset",
        "versionSignatureAddr",
        "versionSignatureExpectedCompact",
        "frameHookTarget",
        "brokerContinuation",
        "brokerPatchLen",
        "brokerStolenLen",
        "brokerExpectedBytesCompact",
    )
    return {
        "loader": {key: manifest["loader"][key] for key in loader_keys},
        "text": {key: manifest["liveMapping"]["text"][key] for key in ("liveStart", "liveEnd")},
        "renderPhase": {
            key: manifest["renderPhase"][key]
            for key in ("root", "leafVtable", "groupVtable", "taskId", "original", "cycle", "fingerprints")
        },
    }


def target_identity(target: str, *, targets_dir: Path | None = None) -> dict:
    if target not in TARGETS:
        raise UniversalPackageError(f"unsupported universal target: {target}")
    path = (targets_dir or REPO_ROOT / "data/targets") / f"{target}.json"
    manifest = load_json(path)
    profile = validate_manifest(manifest)
    if profile["stem"] != target:
        raise UniversalPackageError(f"target contract identity mismatch: {target}")
    return {
        "target": target,
        **{key: manifest[key] for key in ("targetId", "titleId", "contentId", "contentVersion")},
        "targetManifestSha256": sha256(path.read_bytes()),
        "customPacks": bool(profile["features"]["customPacks"]),
    }


def validate_targets(entries: object, *, targets_dir: Path | None = None) -> list[dict]:
    if not isinstance(entries, list) or len(entries) != len(TARGETS):
        raise UniversalPackageError("universal inventory must contain all three supported targets")
    for target, entry in zip(TARGETS, entries, strict=True):
        if not isinstance(entry, dict):
            raise UniversalPackageError("malformed universal target entry")
        for key, value in target_identity(target, targets_dir=targets_dir).items():
            if entry.get(key) != value or (key == "customPacks" and not isinstance(entry.get(key), bool)):
                raise UniversalPackageError(f"universal target contract mismatch: {target} {key}")
    return entries


def inventory_sha256(source_commit: str, entries: list[dict]) -> str:
    """Bind each reviewed profile to its worker/config without local paths or ELF offsets."""
    portable = []
    for entry in entries:
        worker = entry["worker"]
        config_sha256 = entry.get("workerBuildConfigSha256")
        if config_sha256 is None:
            config_sha256 = entry["workerBuildConfig"]["sha256"]
        portable.append(
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
                "worker": {key: worker[key] for key in ("sha256", "size", "mappedSpan")},
                "workerBuildConfigSha256": config_sha256,
            }
        )
    data = {"schemaVersion": 1, "sourceCommit": source_commit, "supportedTargets": portable}
    return sha256((json.dumps(data, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8"))


def integer(value: object, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, (str, int)):
        raise UniversalPackageError(f"invalid integer: {label}")
    try:
        number = int(value, 0) if isinstance(value, str) else value
    except ValueError as exc:
        raise UniversalPackageError(f"invalid integer: {label}") from exc
    if number < 0:
        raise UniversalPackageError(f"negative integer: {label}")
    return number


def mapped_span(data: bytes) -> int:
    validate_elf(data)
    header = ELF_HEADER.unpack_from(data)
    if header[1:3] != (3, 62) or len(data) > 16 * 1024 * 1024:
        raise UniversalPackageError("worker must be an x86-64 ET_DYN ELF within the loader byte limit")
    phoff, phsize, phnum = header[5], header[9], header[10]
    loads = [PROGRAM_HEADER.unpack_from(data, phoff + i * phsize) for i in range(phnum)]
    loads = [entry for entry in loads if entry[0] == 1 and entry[6]]
    if not loads:
        raise UniversalPackageError("worker has no mapped load segments")
    if any(entry[3] + entry[6] >= 2**64 for entry in loads):
        raise UniversalPackageError("worker virtual address overflow")
    start = min(entry[3] for entry in loads) & ~0x3FFF
    end = (max(entry[3] + entry[6] for entry in loads) + 0x3FFF) & ~0x3FFF
    return end - start


def safe_path(raw: object) -> str:
    if not isinstance(raw, str):
        raise UniversalPackageError("package path is not a string")
    path = PurePosixPath(raw)
    if not raw or "\x00" in raw or path.is_absolute() or ".." in path.parts or path.as_posix() != raw:
        raise UniversalPackageError(f"unsafe package path: {raw!r}")
    return raw


def checked_slice(data: bytes, record: object, label: str) -> bytes:
    if not isinstance(record, dict):
        raise UniversalPackageError(f"missing {label} byte proof")
    offset, size = integer(record.get("offset"), label), integer(record.get("size"), label)
    if not size or offset > len(data) or size > len(data) - offset:
        raise UniversalPackageError(f"{label} byte range is outside its container")
    result = data[offset : offset + size]
    if sha256(result) != record.get("sha256"):
        raise UniversalPackageError(f"{label} embedded hash mismatch")
    return result


def embedding(container: bytes, data: bytes, label: str) -> dict:
    offset = container.find(data)
    if offset < 0:
        raise UniversalPackageError(f"{label} is not embedded byte-for-byte")
    if container.find(data, offset + 1) >= 0:
        raise UniversalPackageError(f"{label} is embedded more than once")
    return {"offset": offset, "size": len(data), "sha256": sha256(data)}


def require_loadable(data: bytes, offset: int, size: int, label: str) -> None:
    header = ELF_HEADER.unpack_from(data)
    for index in range(header[10]):
        segment = PROGRAM_HEADER.unpack_from(data, header[5] + index * header[9])
        if segment[0] == 1 and segment[2] <= offset and offset + size <= segment[2] + segment[5]:
            return
    raise UniversalPackageError(f"{label} is outside loadable ELF bytes")


def verify_artifact(data: bytes, manifest: dict, *, targets_dir: Path | None = None) -> None:
    entries = validate_targets(manifest.get("supportedTargets"), targets_dir=targets_dir)
    delivery = manifest.get("delivery")
    if delivery not in LAYOUTS:
        raise UniversalPackageError("unknown universal delivery")
    if delivery != "standalone":
        version = manifest.get("version")
        if manifest.get("pluginId") != "GTAV00001" or not isinstance(version, str):
            raise UniversalPackageError("missing plugin identity/version")
        validate_metadata("GTAV00001", version)
    if delivery == "etahen":
        if manifest.get("runtimeVersion") != manifest.get("version"):
            raise UniversalPackageError("etaHEN helper version mismatch")
        inspect_container(data, expect_id="GTAV00001", expect_version=manifest.get("version"))
        outer = data[HEADER_SIZE:]
    else:
        outer = data
        validate_elf(outer)
    if delivery == "onionhen":
        descriptor = inspect_plugin_bytes(outer)
        if descriptor["id"] != "GTAV00001" or descriptor["version"] != manifest.get("version"):
            raise UniversalPackageError("OnionHEN plugin descriptor identity mismatch")
    runtime = checked_slice(outer, manifest.get("embeddedRuntime"), "runtime")
    if delivery == "etahen":
        offset = integer(manifest["embeddedRuntime"]["offset"], "runtime offset")
        if offset < HEADER_SIZE:
            raise UniversalPackageError("etaHEN embedded runtime container is missing")
        inspect_container(
            outer[offset - HEADER_SIZE : offset + len(runtime)],
            expect_id="GTAV00002",
            expect_version=manifest.get("runtimeVersion"),
        )
    validate_elf(runtime)
    if delivery != "standalone":
        require_loadable(
            outer, integer(manifest["embeddedRuntime"]["offset"], "runtime offset"), len(runtime), "runtime"
        )
    for field, prefix in (
        ("registrySha256", "GTAVMENU_UNIVERSAL_REGISTRY_SHA256:"),
        ("buildConfigSha256", "GTAVMENU_UNIVERSAL_BUILD_SHA256:"),
        ("inventorySha256", "GTAVMENU_UNIVERSAL_INVENTORY_SHA256:"),
    ):
        digest = manifest.get(field)
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise UniversalPackageError(f"missing universal runtime identity: {field}")
        marker = (prefix + digest).encode("ascii") + b"\0"
        if runtime.count(prefix.encode("ascii")) != 1 or runtime.count(marker) != 1:
            raise UniversalPackageError(f"universal runtime identity mismatch: {field}")
        require_loadable(runtime, runtime.index(marker), len(marker), field)
    if manifest.get("inventorySha256") != inventory_sha256(manifest["sourceCommit"], entries):
        raise UniversalPackageError("portable universal inventory identity mismatch")
    if delivery == "standalone" and runtime != outer:
        raise UniversalPackageError("standalone artifact must be the self-contained runtime")
    ranges = []
    for entry in entries:
        target = entry["target"]
        record = entry.get("worker")
        worker = checked_slice(runtime, record, target)
        span = mapped_span(worker)
        contract = load_json((targets_dir or REPO_ROOT / "data/targets") / f"{target}.json")
        if entry.get("contract") != expected_contract(contract):
            raise UniversalPackageError(f"universal loader contract mismatch: {target}")
        limit = integer(contract["loader"]["caveAllocationBytes"], "caveAllocationBytes")
        if span != integer(record.get("mappedSpan"), "mappedSpan") or span > limit:
            raise UniversalPackageError(f"worker mapped span mismatch or overflow: {target}")
        start = integer(record["offset"], "worker offset")
        require_loadable(runtime, start, len(worker), target)
        ranges.append((start, start + len(worker)))
        if not re.fullmatch(r"[0-9a-f]{64}", str(entry.get("workerBuildConfigSha256", ""))):
            raise UniversalPackageError(f"missing worker build identity: {target}")
    for left, right in zip(sorted(ranges), sorted(ranges)[1:], strict=False):
        if left[1] > right[0]:
            raise UniversalPackageError("embedded worker ranges overlap")


def verify_package(
    root: Path,
    manifest: dict,
    *,
    delivery: str,
    source_commit: str | None = None,
    targets_dir: Path | None = None,
) -> None:
    required = {
        "schemaVersion": 2,
        "kind": f"gtavmenu-universal-{delivery}-production",
        "target": "universal",
        "profile": "production",
        "delivery": delivery,
        "requiresHardwareValidation": True,
        "selfContained": True,
    }
    if source_commit is not None:
        required["sourceCommit"] = source_commit
    for key, value in required.items():
        if manifest.get(key) != value:
            raise UniversalPackageError(f"universal package {key} mismatch")
    if not re.fullmatch(r"[0-9a-f]{40}", str(manifest.get("sourceCommit", ""))):
        raise UniversalPackageError("universal package has no valid source commit")
    files = manifest.get("files")
    if not isinstance(files, list) or len(files) != len(LAYOUTS[delivery]):
        raise UniversalPackageError("universal package file inventory mismatch")
    declared = {}
    for entry in files:
        if not isinstance(entry, dict):
            raise UniversalPackageError("malformed universal file entry")
        relative = safe_path(entry.get("path"))
        if relative in declared:
            raise UniversalPackageError("duplicate universal package file")
        declared[relative] = entry.get("role")
        path = root / relative
        if path.is_symlink() or not path.is_file():
            raise UniversalPackageError(f"universal package file missing: {relative}")
        data = path.read_bytes()
        if entry.get("size") != len(data) or entry.get("sha256") != sha256(data):
            raise UniversalPackageError(f"universal package hash/size mismatch: {relative}")
        if entry.get("remote") is not None:
            raise UniversalPackageError("universal standalone/plugin files have no implicit FTP upload destination")
    actual = {path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file() or path.is_symlink()}
    if declared != LAYOUTS[delivery] or actual != set(declared):
        raise UniversalPackageError("universal package inventory mismatch")
    runtime_name = next(path for path, role in declared.items() if role != "documentation")
    verify_artifact((root / runtime_name).read_bytes(), manifest, targets_dir=targets_dir)

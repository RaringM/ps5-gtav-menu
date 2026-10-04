#!/usr/bin/env python3
"""Build a reviewed GTAV-Menu map.cfg from a named-model JSON manifest.

The output uses only stock model names. The runtime still checks every hash with
IS_MODEL_VALID and IS_MODEL_IN_CDIMAGE before requesting or creating an entity.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
from collections import Counter
from pathlib import Path

from gtavmenu_tools.hashes import joaat

ROOT = Path(__file__).resolve().parents[1]
CATALOGS = {
    "vehicle": ROOT / "data/vehicles/gtav-vehicle-names.txt",
    "ped": ROOT / "data/peds/gtav-ped-names.txt",
    "object": ROOT / "data/objects/gtav-object-names.txt",
}
KINDS = {"vehicle": 0, "ped": 1, "object": 2}
MAX_PER_KIND = 32
MAX_ENTRIES = 96
MAX_SOURCE_BYTES = 1024 * 1024
MODEL_RE = re.compile(r"[a-z0-9_]{1,31}\Z")
TOP_KEYS = {"schemaVersion", "name", "description", "placement", "entries"}
ENTRY_KEYS = {"kind", "model", "position", "rotation", "frozen", "placeOnGround"}


class MapError(ValueError):
    pass


def digest(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def read_source(path: Path) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise MapError("map source must be a regular non-symlink file")
    blob = path.read_bytes()
    if not blob or len(blob) > MAX_SOURCE_BYTES:
        raise MapError("map source is empty or exceeds 1 MiB")
    return blob


def catalogue(path: Path) -> set[str]:
    names = set()
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line and not line.startswith("#"):
            names.add(line.split("\t", 1)[0].strip().lower())
    if not names:
        raise MapError(f"model catalogue is empty: {path}")
    return names


def vector(value: object, label: str, limit: float) -> tuple[float, float, float]:
    if not isinstance(value, list) or len(value) != 3:
        raise MapError(f"{label} must contain exactly three numbers")
    result = []
    for component in value:
        if isinstance(component, bool) or not isinstance(component, (int, float)):
            raise MapError(f"{label} contains a non-number")
        number = float(component)
        if not math.isfinite(number) or abs(number) > limit:
            raise MapError(f"{label} contains a non-finite or out-of-range value")
        result.append(number)
    return tuple(result)  # type: ignore[return-value]


def validate(source: dict, catalogues: dict[str, set[str]]) -> tuple[dict, bytes]:
    if set(source) - TOP_KEYS:
        raise MapError(f"unknown top-level fields: {sorted(set(source) - TOP_KEYS)}")
    if source.get("schemaVersion") != 1:
        raise MapError("schemaVersion must be 1")
    name = source.get("name")
    if not isinstance(name, str) or not name.isascii() or not 1 <= len(name) <= 64:
        raise MapError("name must be 1-64 ASCII characters")
    description = source.get("description", "")
    if not isinstance(description, str) or not description.isascii() or len(description) > 256:
        raise MapError("description must be at most 256 ASCII characters")
    placement = source.get("placement")
    if placement not in ("absolute", "relative"):
        raise MapError("placement must be absolute or relative")
    entries = source.get("entries")
    if not isinstance(entries, list) or not 1 <= len(entries) <= MAX_ENTRIES:
        raise MapError("entries must contain 1-96 records")

    counts: Counter[str] = Counter()
    prepared = []
    rows = [f"GTAVMAP,1,{placement}", f"# {name}"]
    for index, raw in enumerate(entries):
        if not isinstance(raw, dict) or set(raw) - ENTRY_KEYS:
            raise MapError(f"entry {index} is not an object or has unknown fields")
        kind = raw.get("kind")
        model = raw.get("model")
        if not isinstance(kind, str) or kind not in KINDS:
            raise MapError(f"entry {index} has an unsupported kind")
        if not isinstance(model, str) or not MODEL_RE.fullmatch(model) or model != model.lower():
            raise MapError(f"entry {index} has an invalid lowercase model name")
        if model not in catalogues[kind]:
            raise MapError(f"entry {index} model is absent from the curated {kind} catalogue: {model}")
        counts[kind] += 1
        if counts[kind] > MAX_PER_KIND:
            raise MapError(f"map exceeds the {MAX_PER_KIND}-entry {kind} ownership roster")
        position = vector(
            raw.get("position"), f"entry {index} position", 10000.0 if placement == "relative" else 100000.0
        )
        rotation = vector(raw.get("rotation", [0, 0, 0]), f"entry {index} rotation", 100000.0)
        frozen = raw.get("frozen", kind == "object")
        ground = raw.get("placeOnGround", False)
        if type(frozen) is not bool or type(ground) is not bool:
            raise MapError(f"entry {index} frozen/placeOnGround must be booleans")
        if ground and kind != "object":
            raise MapError(f"entry {index} ground placement is valid only for objects")
        model_hash = joaat(model)
        flags = int(frozen) | (int(ground) << 1)
        prepared.append(
            {
                "kind": kind,
                "kindCode": KINDS[kind],
                "model": model,
                "modelHash": f"0x{model_hash:08x}",
                "position": list(position),
                "rotation": list(rotation),
                "frozen": frozen,
                "placeOnGround": ground,
                "flags": flags,
                "catalogue": CATALOGS[kind].relative_to(ROOT).as_posix(),
            }
        )
        rows.append(f"# {index}: {kind} {model}")
        values = (*position, *rotation)
        rows.append(f"{KINDS[kind]},0x{model_hash:08X}," + ",".join(f"{value:.3f}" for value in values) + f",{flags}")
    cfg = ("\n".join(rows) + "\n").encode("ascii")
    return (
        {
            "name": name,
            "description": description,
            "placement": placement,
            "entryCount": len(prepared),
            "kindCounts": {kind: counts[kind] for kind in KINDS},
            "entries": prepared,
        },
        cfg,
    )


def write_atomic(path: Path, blob: bytes) -> None:
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise MapError(f"output is not a regular file: {path}")
    temporary = path.with_name(path.name + ".tmp")
    if temporary.exists() or temporary.is_symlink():
        raise MapError(f"temporary output already exists: {temporary}")
    try:
        temporary.write_bytes(blob)
        os.replace(temporary, path)
    finally:
        if temporary.exists() and not temporary.is_symlink():
            temporary.unlink()


def prepare(source_path: Path, output_dir: Path) -> dict:
    before = read_source(source_path)
    try:
        source = json.loads(before)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise MapError(f"map source is not valid UTF-8 JSON: {exc}") from exc
    if not isinstance(source, dict):
        raise MapError("map source root must be an object")
    catalogues = {kind: catalogue(path) for kind, path in CATALOGS.items()}
    map_info, cfg = validate(source, catalogues)
    if read_source(source_path) != before:
        raise MapError("map source changed during preparation")
    if output_dir.is_symlink() or (output_dir.exists() and not output_dir.is_dir()):
        raise MapError("output directory must be a real directory")
    output_dir.mkdir(parents=True, exist_ok=True)
    cfg_path = output_dir / "map.cfg"
    report_path = output_dir / "manifest.json"
    source_name = (
        source_path.resolve().relative_to(ROOT).as_posix()
        if source_path.resolve().is_relative_to(ROOT)
        else source_path.name
    )
    report = {
        "schemaVersion": 1,
        "kind": "gtavmenu-custom-map-package",
        "source": {"path": source_name, "bytes": len(before), "sha256": digest(before)},
        "map": map_info,
        "artifact": {
            "path": "map.cfg",
            "bytes": len(cfg),
            "sha256": digest(cfg),
            "consolePath": "/data/GTAVMenu/custom/maps/active.map.cfg",
            "format": "GTAVMAP,1",
        },
        "qualification": {
            "strictManifestValidated": True,
            "hashesDerivedFromModelNames": True,
            "curatedModelMembershipVerified": True,
            "runtimeModelValidationRequired": True,
            "usesStockModelsOnly": True,
            "customResourceLoadingRequired": False,
            "hardwareQualified": False,
        },
    }
    report_blob = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode("ascii")
    write_atomic(cfg_path, cfg)
    write_atomic(report_path, report_blob)
    if cfg_path.read_bytes() != cfg or report_path.read_bytes() != report_blob or read_source(source_path) != before:
        raise MapError("map package readback or source identity check failed")
    return report


def verify_package(output_dir: Path) -> dict:
    if output_dir.is_symlink() or not output_dir.is_dir():
        raise MapError("map package must be a real directory")
    report_path = output_dir / "manifest.json"
    cfg_path = output_dir / "map.cfg"
    if report_path.is_symlink() or cfg_path.is_symlink():
        raise MapError("map package files must not be symlinks")
    report_blob = read_source(report_path)
    cfg = read_source(cfg_path)
    try:
        report = json.loads(report_blob)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise MapError(f"map package manifest is invalid: {exc}") from exc
    artifact = report.get("artifact") if isinstance(report, dict) else None
    qualification = report.get("qualification") if isinstance(report, dict) else None
    if (
        report.get("schemaVersion") != 1
        or report.get("kind") != "gtavmenu-custom-map-package"
        or not isinstance(artifact, dict)
        or artifact.get("path") != "map.cfg"
        or artifact.get("bytes") != len(cfg)
        or artifact.get("sha256") != digest(cfg)
        or artifact.get("consolePath") != "/data/GTAVMenu/custom/maps/active.map.cfg"
        or artifact.get("format") != "GTAVMAP,1"
        or not isinstance(qualification, dict)
        or qualification.get("strictManifestValidated") is not True
        or qualification.get("hashesDerivedFromModelNames") is not True
        or qualification.get("curatedModelMembershipVerified") is not True
        or not cfg.startswith(b"GTAVMAP,1,")
    ):
        raise MapError("map package manifest, qualification, or artifact identity is inconsistent")
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, nargs="?")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--verify-package", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.verify_package:
            if args.source or args.output_dir:
                raise MapError("--verify-package cannot be combined with source/output options")
            report = verify_package(args.verify_package)
        else:
            if not args.source or not args.output_dir:
                raise MapError("source and --output-dir are required when preparing a map")
            report = prepare(args.source, args.output_dir)
    except (MapError, OSError, KeyError, TypeError) as exc:
        parser.exit(1, f"custom map preparation failed: {exc}\n")
    print(
        json.dumps(
            {
                "name": report["map"]["name"],
                "placement": report["map"]["placement"],
                "entries": report["map"]["entryCount"],
                "mapCfgSha256": report["artifact"]["sha256"],
                "verified": True,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

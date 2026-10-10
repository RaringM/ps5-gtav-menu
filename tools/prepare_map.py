#!/usr/bin/env python3
"""Build a stock-model scene from JSON or an explicit YMAP XML placement snapshot.

The output uses only stock model names. The runtime still checks every hash with
IS_MODEL_VALID and IS_MODEL_IN_CDIMAGE before requesting or creating an entity.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
import tempfile
from collections import Counter
from pathlib import Path

from gtavmenu_tools import custom_pack_schema as package_io
from gtavmenu_tools.asset_formats import AssetError
from gtavmenu_tools.asset_maps import read_ymap_placements, snapshot_import_record
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
QUALIFICATION = {
    "strictManifestValidated": True,
    "hashesDerivedFromModelNames": True,
    "curatedModelMembershipVerified": True,
    "runtimeModelValidationRequired": True,
    "usesStockModelsOnly": True,
    "customResourceLoadingRequired": False,
    "hardwareQualified": False,
}


class MapError(AssetError):
    pass


def digest(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def read_source(path: Path) -> bytes:
    try:
        return package_io.read_regular(path, maximum=MAX_SOURCE_BYTES)
    except package_io.CustomPackSchemaError as exc:
        raise MapError(str(exc)) from exc


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
        try:
            number = float(component)
        except OverflowError as exc:
            raise MapError(f"{label} contains an out-of-range value") from exc
        if not math.isfinite(number) or abs(number) > limit:
            raise MapError(f"{label} contains a non-finite or out-of-range value")
        result.append(number)
    return tuple(result)  # type: ignore[return-value]


def validate(source: dict, catalogues: dict[str, set[str]]) -> tuple[dict, bytes]:
    if type(source) is not dict:
        raise MapError("map source root must be an object")
    if set(source) - TOP_KEYS:
        raise MapError(f"unknown top-level fields: {sorted(set(source) - TOP_KEYS)}")
    if type(source.get("schemaVersion")) is not int or source["schemaVersion"] != 1:
        raise MapError("schemaVersion must be 1")
    name = source.get("name")
    if not isinstance(name, str) or not 1 <= len(name) <= 64 or any(not 32 <= ord(char) <= 126 for char in name):
        raise MapError("name must be 1-64 printable ASCII characters")
    description = source.get("description", "")
    if (
        not isinstance(description, str)
        or len(description) > 256
        or any(not 32 <= ord(char) <= 126 for char in description)
    ):
        raise MapError("description must be at most 256 printable ASCII characters")
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


def _report(source_record: dict, map_info: dict, cfg: bytes, *, imported: bool) -> dict:
    report = {
        "schemaVersion": 2 if imported else 1,
        "kind": "gtavmenu-custom-map-package",
        "source": source_record,
        "map": map_info,
        "artifact": {
            "path": "map.cfg",
            "bytes": len(cfg),
            "sha256": digest(cfg),
            "consolePath": "/data/GTAVMenu/custom/maps/active.map.cfg",
            "format": "GTAVMAP,1",
        },
        "qualification": dict(QUALIFICATION),
    }
    if imported:
        report["import"] = snapshot_import_record()
    return report


def _publish(output_dir: Path, files: dict[str, bytes]) -> None:
    # Reuse the strict package publisher's native no-replace and owned-cleanup
    # primitives, not its different resource schema. No unsafe rename fallback.
    package_io._encoded_publication_path(output_dir)
    if output_dir.exists() or output_dir.is_symlink():
        verify_package(output_dir)
        if any(read_source(output_dir / name) != blob for name, blob in files.items()):
            raise MapError("existing map package differs; choose a new output directory")
        return
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=".gtavmenu-map-", suffix=".tmp", dir=output_dir.parent))
    identity = None
    try:
        created = temporary.lstat()
        identity = (created.st_dev, created.st_ino)
        for name, blob in files.items():
            with (temporary / name).open("xb") as stream:
                stream.write(blob)
        verify_package(temporary)
        if any(read_source(temporary / name) != blob for name, blob in files.items()):
            raise MapError("staged map differs from the complete expected package")
        package_io._rename_directory_noreplace(temporary, output_dir)
    except BaseException as exc:
        if identity is None:
            exc.add_note(f"Could not establish staging ownership; retained staging: {temporary}")
            raise
        try:
            package_io._cleanup_staging(temporary, identity)
        except BaseException as cleanup_error:
            exc.add_note(f"Could not clean staging {temporary}: {cleanup_error}")
        raise


def prepare(
    source_path: Path, output_dir: Path, *, input_format: str = "json", acknowledge_placement_only: bool = False
) -> dict:
    if input_format not in ("json", "ymap-xml"):
        raise MapError("input format must be json or ymap-xml")
    if input_format == "json" and acknowledge_placement_only:
        raise MapError("placement-only acknowledgment is valid only with --input-format ymap-xml")
    before = read_source(source_path)
    catalogues = {kind: catalogue(path) for kind, path in CATALOGS.items()}
    source = (
        read_ymap_placements(before, catalogues["object"], acknowledge_placement_only=acknowledge_placement_only)
        if input_format == "ymap-xml"
        else package_io.load_json(before, "map source")
    )
    map_info, cfg = validate(source, catalogues)
    source_name = (
        source_path.resolve().relative_to(ROOT).as_posix()
        if source_path.resolve().is_relative_to(ROOT)
        else source_path.name
    )
    report = _report(
        {"path": source_name, "bytes": len(before), "sha256": digest(before)},
        map_info,
        cfg,
        imported=input_format == "ymap-xml",
    )
    files = {"map.cfg": cfg, "manifest.json": package_io.canonical_json(report)}
    if any(len(blob) > MAX_SOURCE_BYTES for blob in files.values()):
        raise MapError("map package exceeds the verification byte budget")
    if read_source(source_path) != before:
        raise MapError("map source changed during preparation")
    _publish(output_dir, files)
    return report


def verify_package(output_dir: Path) -> dict:
    if output_dir.is_symlink() or not output_dir.is_dir():
        raise MapError("map package must be a real directory")
    found = set()
    for child in output_dir.iterdir():
        if child.name not in {"map.cfg", "manifest.json"} or child.is_symlink() or not child.is_file():
            raise MapError("map package must contain exactly two regular files and no other entries")
        found.add(child.name)
    if found != {"map.cfg", "manifest.json"}:
        raise MapError("map package is missing required files")
    report_blob, cfg = read_source(output_dir / "manifest.json"), read_source(output_dir / "map.cfg")
    report = package_io.load_json(report_blob, "map package manifest")
    version = report.get("schemaVersion")
    if type(version) is not int or version not in (1, 2):
        raise MapError("unsupported map package version")
    source_record, recorded = report.get("source"), report.get("map")
    if (
        type(source_record) is not dict
        or set(source_record) != {"path", "bytes", "sha256"}
        or type(source_record["path"]) is not str
        or not 1 <= len(source_record["path"]) <= 512
        or not source_record["path"].isprintable()
        or type(source_record["bytes"]) is not int
        or not 1 <= source_record["bytes"] <= MAX_SOURCE_BYTES
        or type(source_record["sha256"]) is not str
        or not re.fullmatch(r"[0-9a-f]{64}", source_record["sha256"])
        or type(recorded) is not dict
        or set(recorded) != {"name", "description", "placement", "entries", "entryCount", "kindCounts"}
        or type(recorded["entries"]) is not list
        or not 1 <= len(recorded["entries"]) <= MAX_ENTRIES
    ):
        raise MapError("map source identity or prepared scene fields are invalid")
    entries = []
    for row in recorded["entries"]:
        if type(row) is not dict or set(row) != ENTRY_KEYS | {"kindCode", "modelHash", "flags", "catalogue"}:
            raise MapError("prepared map entry has missing or unknown fields")
        entries.append({key: row[key] for key in ENTRY_KEYS})
    scene = {
        "schemaVersion": 1,
        **{key: recorded[key] for key in ("name", "description", "placement")},
        "entries": entries,
    }
    info, expected_cfg = validate(scene, {kind: catalogue(path) for kind, path in CATALOGS.items()})
    if version == 2 and (
        info["placement"] != "absolute"
        or any(row["kind"] != "object" or not row["frozen"] or row["placeOnGround"] for row in info["entries"])
    ):
        raise MapError("YMAP snapshot must contain only absolute frozen object placements without grounding")
    expected = _report(source_record, info, expected_cfg, imported=version == 2)
    if cfg != expected_cfg or report_blob != package_io.canonical_json(expected):
        raise MapError("map package differs from independently rederived fields, policy or map.cfg")
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, nargs="?")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--verify-package", type=Path)
    parser.add_argument("--input-format", choices=("json", "ymap-xml"), default="json")
    parser.add_argument(
        "--acknowledge-placement-only",
        action="store_true",
        help="explicitly discard native YMAP behavior for a stock-object static snapshot",
    )
    args = parser.parse_args(argv)
    try:
        if args.verify_package:
            if args.source or args.output_dir or args.input_format != "json" or args.acknowledge_placement_only:
                raise MapError("--verify-package cannot be combined with source/output options")
            report = verify_package(args.verify_package)
        else:
            if not args.source or not args.output_dir:
                raise MapError("source and --output-dir are required when preparing a map")
            report = prepare(
                args.source,
                args.output_dir,
                input_format=args.input_format,
                acknowledge_placement_only=args.acknowledge_placement_only,
            )
    except KeyboardInterrupt as exc:
        print("custom map preparation interrupted", file=sys.stderr)
        for note in getattr(exc, "__notes__", ()):
            print(note, file=sys.stderr)
        return 130
    except (AssetError, package_io.CustomPackSchemaError, OSError) as exc:
        print(f"custom map preparation failed: {exc}", file=sys.stderr)
        for note in getattr(exc, "__notes__", ()):
            print(note, file=sys.stderr)
        return 1
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

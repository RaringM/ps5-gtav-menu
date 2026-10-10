#!/usr/bin/env python3
"""Inventory or import exact resources from large PC RPF7 add-on packs.

The importer reads archive tables through bounded file windows, so multi-gigabyte
packs do not need to fit in memory. Imports publish atomically and can be replayed
from the original archive to verify every fixture byte. Imported files are source
fixtures for later conversion; they are never marked loadable or copied to a
console by this tool.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import tempfile
import xml.etree.ElementTree as ET
from collections import Counter
from dataclasses import replace
from pathlib import Path, PurePosixPath

from gtavmenu_tools import asset_formats, asset_metadata, asset_textures, hashes
from gtavmenu_tools import custom_pack_schema as package_io
from gtavmenu_tools.asset_formats import (
    AssetError,
    Limits,
    RpfMember,
    decode_resource,
    read_rpf_member,
    resource_header,
    rpf_header,
    rpf_members,
    safe_name,
)
from gtavmenu_tools.asset_textures import inspect_legacy_dictionary

ROOT = Path(__file__).resolve().parents[1]
MAX_ARCHIVE_BYTES = 8 * 1024 * 1024 * 1024
MAX_TABLE_BYTES = 64 * 1024 * 1024
MAX_SELECTED_BYTES = 256 * 1024 * 1024
MAX_SELECTION_TOTAL = 512 * 1024 * 1024
MAX_METADATA_BYTES = 4 * 1024 * 1024
MAX_REPORT_BYTES = 64 * 1024 * 1024
MAX_ENTRIES = 100_000
MAX_DEPTH = 8
MODEL_RE = re.compile(r"[a-z0-9_]{1,64}\Z")
LIMITS = Limits(
    max_file_bytes=MAX_ARCHIVE_BYTES,
    max_total_bytes=MAX_ARCHIVE_BYTES,
    max_entries=MAX_ENTRIES,
    max_depth=MAX_DEPTH,
)


def digest(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def tool_hashes() -> dict[str, str]:
    paths = [
        Path(__file__),
        Path(asset_formats.__file__),
        Path(asset_metadata.__file__),
        Path(asset_textures.__file__),
        Path(hashes.__file__),
    ]
    return {path.relative_to(ROOT).as_posix(): digest(path.read_bytes()) for path in paths}


class RpfSource:
    def __init__(self, path: Path):
        if path.is_symlink() or not path.is_file():
            raise AssetError("source must be a regular non-symlink RPF")
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
        self.stream = os.fdopen(fd, "rb")
        self.path = path
        self.before = os.fstat(self.stream.fileno())
        if not stat.S_ISREG(self.before.st_mode) or not 0 < self.before.st_size <= MAX_ARCHIVE_BYTES:
            self.stream.close()
            raise AssetError("source is empty, special, or exceeds the 8 GiB archive bound")

    def close(self) -> None:
        self.stream.close()

    def read_at(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset > self.before.st_size or size > self.before.st_size - offset:
            raise AssetError("archive read leaves the source")
        self.stream.seek(offset)
        blob = self.stream.read(size)
        if len(blob) != size:
            raise AssetError("archive read was truncated")
        return blob

    def sha256(self) -> str:
        self.stream.seek(0)
        hashed = hashlib.sha256()
        while True:
            block = self.stream.read(8 * 1024 * 1024)
            if not block:
                break
            hashed.update(block)
        self.assert_unchanged()
        return hashed.hexdigest()

    def assert_unchanged(self) -> None:
        after = os.fstat(self.stream.fileno())
        current = self.path.stat()

        def identity(row: os.stat_result) -> tuple[int, int, int, int]:
            return (row.st_dev, row.st_ino, row.st_size, row.st_mtime_ns)

        if identity(self.before) != identity(after) or identity(self.before) != identity(current):
            raise AssetError("source archive changed during import")


class RpfWindow:
    def __init__(self, source: RpfSource, base: int, size: int, chain: str):
        self.source = source
        self.base = base
        self.size = size
        self.chain = chain
        self._header: dict | None = None
        self._members: list[RpfMember] | None = None
        self.table_sha256 = ""

    def read_at(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset > self.size or size > self.size - offset:
            raise AssetError("nested RPF read leaves its containing member")
        return self.source.read_at(self.base + offset, size)

    def members(self) -> list[RpfMember]:
        if self._members is not None:
            return self._members
        header = rpf_header(self.read_at(0, 16), self.size)
        if header["tableEnd"] > MAX_TABLE_BYTES:
            raise AssetError("RPF table exceeds the 64 MiB metadata bound")
        table = self.read_at(0, header["tableEnd"])
        self._members = rpf_members(table, LIMITS, file_size=self.size, read_at=self.read_at)
        self._header = header
        self.table_sha256 = digest(table)
        return self._members

    @property
    def header(self) -> dict:
        self.members()
        assert self._header is not None
        return self._header

    def child(self, member: RpfMember) -> RpfWindow:
        if member.resource or member.encrypted or member.compressed or not member.name.lower().endswith(".rpf"):
            raise AssetError("nested RPF must be an exact stored, unencrypted non-resource member")
        chain = f"{self.chain}!/{member.name}" if self.chain else member.name
        return RpfWindow(self.source, self.base + member.offset, member.size, chain)

    def read_member(self, member: RpfMember, maximum: int) -> bytes:
        if member.encrypted:
            raise AssetError(f"encrypted member is unsupported: {member.name}")
        if max(member.size, member.unpacked_size) > maximum:
            raise AssetError(f"selected member exceeds its byte bound: {member.name}")
        stored = self.read_at(member.offset, member.size)
        return read_rpf_member(stored, replace(member, offset=0), maximum)


def chain_name(window: RpfWindow, member: RpfMember) -> str:
    return f"{window.chain}!/{member.name}" if window.chain else member.name


def discover(root: RpfWindow) -> tuple[list[RpfWindow], list[tuple[RpfWindow, RpfMember]]]:
    windows: list[RpfWindow] = []
    entries: list[tuple[RpfWindow, RpfMember]] = []
    pending = [(root, 0)]
    while pending:
        window, depth = pending.pop()
        windows.append(window)
        for member in window.members():
            entries.append((window, member))
            if (
                depth < MAX_DEPTH
                and member.name.lower().endswith(".rpf")
                and not member.resource
                and not member.encrypted
                and not member.compressed
            ):
                pending.append((window.child(member), depth + 1))
    return windows, entries


def member_record(window: RpfWindow, member: RpfMember) -> dict:
    return {
        "member": chain_name(window, member),
        "container": window.chain or "<root>",
        "offsetInContainer": member.offset,
        "storedBytes": member.size,
        "decodedBytes": member.unpacked_size,
        "resource": member.resource,
        "compressed": member.compressed,
        "encrypted": member.encrypted,
        "flags": [f"0x{value:08x}" for value in member.flags],
    }


def source_name(path: Path) -> str:
    resolved = path.resolve()
    return resolved.relative_to(ROOT).as_posix() if resolved.is_relative_to(ROOT) else path.name


def base_report(source: RpfSource, windows: list[RpfWindow]) -> dict:
    return {
        "schemaVersion": 1,
        "kind": "gtavmenu-pc-asset-import",
        "tools": tool_hashes(),
        "limits": {
            "archiveBytes": MAX_ARCHIVE_BYTES,
            "tableBytes": MAX_TABLE_BYTES,
            "selectedMemberBytes": MAX_SELECTED_BYTES,
            "selectionTotalBytes": MAX_SELECTION_TOTAL,
            "entries": MAX_ENTRIES,
            "depth": MAX_DEPTH,
        },
        "source": {
            "path": source_name(source.path),
            "bytes": source.before.st_size,
            "sha256": source.sha256(),
            "containers": [
                {
                    "member": window.chain or "<root>",
                    "offset": window.base,
                    "bytes": window.size,
                    "tableSha256": window.table_sha256,
                    "header": window.header,
                }
                for window in windows
            ],
        },
        "consoleLayout": {
            "root": "/data/GTAVMenu/custom",
            "packRoot": "/data/GTAVMenu/custom/packs",
            "catalog": "/data/GTAVMenu/custom/catalog.json",
        },
        "qualification": {
            "sourceArchiveIdentityVerified": True,
            "boundedStreamingTablesVerified": True,
            "selectedMemberIdentityVerified": False,
            "ps5ResourceLayoutsValidated": False,
            "completeDependencyClosure": False,
            "mountContractValidated": False,
            "lifetimeContractValidated": False,
            "conversionAvailable": False,
            "uploadAllowed": False,
            "runtimeEnabled": False,
            "liveTestReady": False,
        },
    }


def inventory(path: Path) -> dict:
    source = RpfSource(path)
    try:
        root = RpfWindow(source, 0, source.before.st_size, "")
        windows, entries = discover(root)
        report = base_report(source, windows)
        suffixes = Counter(PurePosixPath(member.name).suffix.lower() or "<none>" for _, member in entries)
        report.update(
            operation="inventory",
            members=[member_record(window, member) for window, member in entries],
            counts={"containers": len(windows), "members": len(entries), "suffixes": dict(sorted(suffixes.items()))},
        )
        return report
    finally:
        source.close()


def resolve_chains(entries: list[tuple[RpfWindow, RpfMember]], chains: list[str]) -> list[tuple[RpfWindow, RpfMember]]:
    by_chain: dict[str, list[tuple[RpfWindow, RpfMember]]] = {}
    for window, member in entries:
        by_chain.setdefault(chain_name(window, member).lower(), []).append((window, member))
    selected = []
    for requested in chains:
        if len(requested) > 4096:
            raise AssetError("requested member chain exceeds 4096 characters")
        for part in requested.split("!/"):
            safe_name(part)
        matches = by_chain.get(requested.lower(), [])
        if len(matches) != 1 or chain_name(*matches[0]) != requested:
            raise AssetError(f"member is missing, duplicated, or a case alias: {requested}")
        selected.append(matches[0])
    if len({chain_name(*row).lower() for row in selected}) != len(selected):
        raise AssetError("selected member list contains duplicates")
    return selected


def selected_blob(window: RpfWindow, member: RpfMember) -> tuple[bytes, dict]:
    blob = window.read_member(member, MAX_SELECTED_BYTES)
    record = member_record(window, member) | {"bytes": len(blob), "sha256": digest(blob)}
    suffix = PurePosixPath(member.name).suffix.lower()
    if member.resource:
        header = resource_header(blob)
        record["resourceHeader"] = header
        record["pcLayoutOnly"] = True
        if suffix == ".ytd":
            decoded_header, payload = decode_resource(blob, MAX_SELECTED_BYTES)
            dictionary = inspect_legacy_dictionary(payload, decoded_header, LIMITS)
            record["textureDictionary"] = {
                "layout": dictionary["layout"],
                "textureCount": dictionary["textureCount"],
                "textures": [
                    {
                        key: row.get(key)
                        for key in (
                            "name",
                            "nameHash",
                            "width",
                            "height",
                            "depth",
                            "mipLevels",
                            "format",
                            "formatCode",
                            "linearPayloadValidated",
                            "ordinaryStaticConversionEligible",
                            "issues",
                        )
                    }
                    for row in dictionary["textures"]
                ],
                "ps5LayoutValidated": False,
            }
    return blob, record


def output_name_for_chain(chain: str) -> str:
    parts = []
    for segment in chain.split("!/"):
        parts.extend(PurePosixPath(segment).parts)
    output = PurePosixPath("resources", *parts)
    if output.is_absolute() or any(part in ("", ".", "..") for part in output.parts):
        raise AssetError("selected member output is not a safe relative fixture path")
    return output.as_posix()


def canonical_report(report: dict) -> bytes:
    try:
        return (json.dumps(report, indent=2, sort_keys=True, allow_nan=False) + "\n").encode("utf-8")
    except (TypeError, ValueError, UnicodeEncodeError, RecursionError) as exc:
        raise AssetError(f"fixture report is not canonical JSON: {exc}") from exc


def write_atomic(path: Path, blob: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    if path.exists() or path.is_symlink() or temporary.exists() or temporary.is_symlink():
        raise AssetError(f"refusing to overwrite fixture output: {path}")
    try:
        temporary.write_bytes(blob)
        os.replace(temporary, path)
    finally:
        if temporary.exists() and not temporary.is_symlink():
            temporary.unlink()


def item_value(item: ET.Element, field: str, *, attribute: bool = False) -> str:
    return asset_metadata.scalar(item, field, required=True, attribute=attribute)


def matching_items(
    roots: list[tuple[str, bytes, ET.Element]],
    root_tag: str,
    xpath: str,
    field: str,
    value: str,
    *,
    attribute: bool = False,
) -> tuple[list[tuple[str, bytes, ET.Element]], list[str]]:
    candidates = []
    aliases = []
    for name, blob, root in roots:
        if root.tag != root_tag:
            continue
        for item in root.findall(xpath):
            candidate = item_value(item, field, attribute=attribute)
            if candidate == value:
                candidates.append((name, blob, item))
            elif candidate.casefold() == value.casefold():
                aliases.append(candidate)
    return candidates, aliases


def unique_item(
    roots: list[tuple[str, bytes, ET.Element]],
    root_tag: str,
    xpath: str,
    field: str,
    value: str,
    *,
    attribute: bool = False,
    required: bool = False,
    fold_case: bool = False,
) -> tuple[str, bytes, ET.Element] | None:
    candidates, aliases = matching_items(roots, root_tag, xpath, field, value, attribute=attribute)
    if fold_case and not candidates and len(aliases) == 1:
        # Model, handling and layout names are case-insensitive joaat keys in the engine: the Skyline
        # add-on names "Skyline" (vehicles.meta) for skyline.yft and handling "SKYLINE" for handlingId
        # "Skyline". Exactly one other spelling is accepted; two or more still refuse.
        candidates, aliases = matching_items(roots, root_tag, xpath, field, aliases[0], attribute=attribute)
    if len(candidates) != 1:
        if not candidates and not aliases and not required:
            return None
        detail = f"{len(candidates)} exact matches"
        if aliases:
            detail += f" and case aliases {sorted(aliases)!r}"
        raise AssetError(f"metadata selection for {root_tag}:{field}={value!r} has {detail}")
    if aliases:
        raise AssetError(f"metadata selection for {root_tag}:{field}={value!r} also has case aliases")
    return candidates[0]


def selected_item_record(kind: str, identity: str, selected: tuple[str, bytes, ET.Element]) -> tuple[bytes, dict]:
    source_name, source_blob, item = selected
    # This is a deterministic parsed fragment for conversion work. The complete source metadata
    # remains preserved byte-for-byte beside it and is the authority for whitespace/comments.
    fragment = ET.tostring(item, encoding="utf-8", short_empty_elements=True) + b"\n"
    output = f"metadata-selection/{kind}.item.xml"
    return fragment, {
        "kind": kind,
        "identity": identity,
        "sourceMember": source_name,
        "sourceBytes": len(source_blob),
        "sourceSha256": digest(source_blob),
        "parsedFragment": output,
        "parsedFragmentBytes": len(fragment),
        "parsedFragmentSha256": digest(fragment),
    }


def select_vehicle_metadata(
    model: str, metadata: list[tuple[str, bytes, dict]]
) -> tuple[dict, list[tuple[str, bytes]]]:
    roots = [(name, blob, asset_metadata.parse_xml(blob, LIMITS)) for name, blob, _ in metadata]
    vehicle_match = unique_item(
        roots,
        "CVehicleModelInfo__InitDataList",
        "./InitDatas/Item",
        "modelName",
        model,
        required=True,
        fold_case=True,
    )
    assert vehicle_match is not None
    vehicle = vehicle_match[2]
    txd_name = item_value(vehicle, "txdName")
    handling_name = item_value(vehicle, "handlingId")
    audio_name = item_value(vehicle, "audioNameHash")
    layout_name = item_value(vehicle, "layout")
    variation_match = unique_item(
        roots,
        "CVehicleModelInfoVariation",
        "./variationData/Item",
        "modelName",
        model,
        required=True,
        fold_case=True,
    )
    assert variation_match is not None
    variation = variation_match[2]
    kits = []
    for item in variation.findall("./kits/Item"):
        if len(item):
            raise AssetError("selected vehicle has a nested modkit reference")
        kit = (item.text or "").strip()
        if kit:
            kits.append(asset_metadata.identifier(kit, "modkit"))
    if len(kits) != len(set(kits)):
        raise AssetError("selected vehicle repeats a modkit reference")
    light_id = item_value(variation, "lightSettings", attribute=True)
    siren_id = item_value(variation, "sirenSettings", attribute=True)
    asset_metadata.identifier(light_id, "light-setting")
    asset_metadata.identifier(siren_id, "siren-setting")

    selections: list[tuple[str, str, tuple[str, bytes, ET.Element]]] = [
        ("vehicle", model, vehicle_match),
        ("variation", model, variation_match),
    ]
    dependencies = {
        "textureDictionary": {"name": txd_name, "status": "selected-resource"},
        "handling": {"name": handling_name, "status": "external-unverified"},
        "audio": {"name": audio_name, "status": "external-unverified"},
        "layout": {"name": layout_name, "status": "external-unverified"},
        "modkits": [{"name": kit, "status": "external-unverified"} for kit in kits],
        "lightSetting": {"id": int(light_id), "status": "external-unverified"},
        "sirenSetting": {"id": int(siren_id), "status": "external-unverified"},
    }

    optional = [
        (
            "handling",
            handling_name,
            unique_item(
                roots, "CHandlingDataMgr", "./HandlingData/Item", "handlingName", handling_name, fold_case=True
            ),
            dependencies["handling"],
        ),
        (
            "layout",
            layout_name,
            unique_item(roots, "CVehicleMetadataMgr", "./VehicleLayoutInfos/Item", "Name", layout_name, fold_case=True),
            dependencies["layout"],
        ),
        (
            "light",
            light_id,
            unique_item(
                roots,
                "CVehicleModelInfoVarGlobal",
                "./Lights/Item",
                "id",
                light_id,
                attribute=True,
            ),
            dependencies["lightSetting"],
        ),
        (
            "siren",
            siren_id,
            unique_item(
                roots,
                "CVehicleModelInfoVarGlobal",
                "./Sirens/Item",
                "id",
                siren_id,
                attribute=True,
            ),
            dependencies["sirenSetting"],
        ),
    ]
    for kind, identity, match, dependency in optional:
        if match is not None:
            selections.append((kind, identity, match))
            dependency["status"] = "selected-pack-metadata"

    for dependency in dependencies["modkits"]:
        matches, aliases = matching_items(
            roots,
            "CVehicleModelInfoVarGlobal",
            "./Kits/Item",
            "kitName",
            dependency["name"],
        )
        dependency["candidateCount"] = len(matches)
        if aliases:
            dependency.update(status="ambiguous-case-alias", caseAliases=sorted(aliases))
        elif len(matches) == 1:
            selections.append((f"modkit-{len(selections)}", dependency["name"], matches[0]))
            dependency["status"] = "selected-pack-metadata"
        elif matches:
            dependency["status"] = "ambiguous-pack-metadata"

    fragments = []
    records = []
    for kind, identity, selected in selections:
        fragment, record = selected_item_record(kind, identity, selected)
        fragments.append((record["parsedFragment"], fragment))
        records.append(record)
    report = {
        "coverage": "selected-vehicle-records-and-direct-references",
        "model": model,
        "records": records,
        "dependencies": dependencies,
        "completeDependencyClosure": False,
        "metadataSemanticsValidated": False,
        "ps5ActivationValidated": False,
    }
    summary = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode("utf-8")
    report["summary"] = {
        "path": "metadata-selection/selection.json",
        "bytes": len(summary),
        "sha256": digest(summary),
    }
    fragments.append((report["summary"]["path"], summary))
    return report, fragments


def _add_fixture_file(files: dict[str, bytes], name: str, blob: bytes) -> None:
    relative = PurePosixPath(name)
    if (
        type(blob) is not bytes
        or relative.is_absolute()
        or str(relative) != name
        or any(part in ("", ".", "..") for part in relative.parts)
    ):
        raise AssetError("fixture output contains an invalid path or non-byte value")
    if name in files:
        raise AssetError(f"fixture outputs collide at {name}")
    files[name] = blob


def mod_kit_chains(model: str, chains: list[str], kit_parts: frozenset[str] = frozenset()) -> list[str]:
    """Mod-kit fragments of a vehicle, sorted: <model>_<part>.yft inside a vehiclemods/ archive, plus any
    .yft in an archive (not the vehicle's own) whose name the vehicle's carcols kit lists (kit_parts,
    lowercase model names): packs name parts freely (vans123: laf_*, raptor_* in tuning_mods.rpf)."""
    part = re.compile(re.escape(model) + r"_[a-z0-9_]+\.yft")
    own = {f"{model}.yft", f"{model}_hi.yft"}
    found = []
    for chain in chains:
        container, _, name = chain.rpartition("!/")
        if not container or name in own:
            continue
        if ("vehiclemods/" in container and part.fullmatch(name)) or (
            name.lower().endswith(".yft") and name[:-4].lower() in kit_parts
        ):
            found.append(chain)
    return sorted(found)


def kit_part_names(model: str, root: RpfWindow) -> frozenset[str]:
    """Lowercase part model names (visibleMods and linkMods) of the kits the model's variation names."""
    roots = []
    for member in root.members():
        if PurePosixPath(member.name).suffix.lower() in (".xml", ".meta"):
            roots.append(asset_metadata.parse_xml(root.read_member(member, MAX_METADATA_BYTES), LIMITS))
    kits = set()
    for tree in roots:
        if tree.tag == "CVehicleModelInfoVariation":
            for item in tree.findall("./variationData/Item"):
                if (item.findtext("modelName") or "").strip().casefold() == model:
                    kits |= {(kit.text or "").strip().casefold() for kit in item.findall("./kits/Item")}
    names = set()
    for tree in roots:
        if tree.tag == "CVehicleModelInfoVarGlobal":
            for kit in tree.findall("./Kits/Item"):
                if (kit.findtext("kitName") or "").strip().casefold() in kits:
                    for mod in (*kit.findall("./visibleMods/Item"), *kit.findall("./linkMods/Item")):
                        names.add((mod.findtext("modelName") or "").strip().lower())
    return frozenset(names - {""})


def txd_parent_names(model: str, root: RpfWindow) -> list[str]:
    """Lowercase texture dictionaries on the model's txdRelationships parent chain (vehicles.meta), nearest
    first: the Dominator GTX names tfdominator -> vehicles_tfdominator_interior (shipped beside the car)
    -> vehicles_sup1_interior. Which of them the archive ships is decided by the caller."""
    txd, pairs = None, {}
    for member in root.members():
        if PurePosixPath(member.name).suffix.lower() not in (".xml", ".meta"):
            continue
        tree = asset_metadata.parse_xml(root.read_member(member, MAX_METADATA_BYTES), LIMITS)
        if tree.tag != "CVehicleModelInfo__InitDataList":
            continue
        for item in tree.findall("./InitDatas/Item"):
            if (item.findtext("modelName") or "").strip().casefold() == model:
                txd = (item.findtext("txdName") or "").strip().lower()
        for item in tree.findall("./txdRelationships/Item"):
            child = (item.findtext("child") or "").strip().lower()
            pairs.setdefault(child, (item.findtext("parent") or "").strip().lower())
    start, chain = txd, []
    while txd in pairs and pairs[txd] and pairs[txd] not in (start, *chain) and len(chain) < 16:
        txd = pairs[txd]
        chain.append(txd)
    return chain


def build_selection(
    path: Path, chains: list[str], operation: str, model: str | None, mod_kit: bool = False
) -> tuple[dict, dict[str, bytes]]:
    if operation not in ("extract", "vehicle") or (operation == "extract") != (model is None):
        raise AssetError("fixture operation/model scope is invalid")
    if type(chains) is not list or any(type(chain) is not str for chain in chains):
        raise AssetError("fixture member selection must be a list of exact names")
    if operation == "extract" and not 1 <= len(chains) <= MAX_ENTRIES:
        raise AssetError("extract fixture needs one to 100000 selected members")
    if operation == "vehicle" and chains:
        raise AssetError("vehicle fixture derives its required member selection from the model")
    if mod_kit and operation != "vehicle":
        raise AssetError("mod-kit parts are only selected beside a vehicle")
    if model is not None and (type(model) is not str or not MODEL_RE.fullmatch(model)):
        raise AssetError("model must be a lowercase ASCII asset identifier")
    source = RpfSource(path)
    try:
        root = RpfWindow(source, 0, source.before.st_size, "")
        windows, entries = discover(root)
        if model is not None:
            required = (f"{model}.yft", f"{model}_hi.yft", f"{model}.ytd")
            found: list[str] = []
            for name in required:
                # Archive member names are case-insensitive in the engine (Prowler: x64/vehicles.rpf/Prowler.yft
                # for model prowler); the chain keeps the archive's spelling, two spellings still refuse.
                matches = [chain_name(window, member) for window, member in entries if member.name.lower() == name]
                if len(matches) != 1:
                    raise AssetError(f"vehicle requires one {name} (any letter case); found {len(matches)}")
                found.extend(matches)
            # Texture dictionaries on the car's txd parent chain that the mod ships (the Dominator GTX's
            # vehicles_tfdominator_interior.ytd holds its interior textures); retail parents stay external.
            for parent in txd_parent_names(model, root):
                matches = [
                    chain_name(window, member) for window, member in entries if member.name.lower() == f"{parent}.ytd"
                ]
                if len(matches) > 1:
                    raise AssetError(f"vehicle texture parent {parent}.ytd appears {len(matches)} times")
                found.extend(matches)
            parts = []
            if mod_kit:
                names = [chain_name(window, member) for window, member in entries]
                parts = mod_kit_chains(model, names, kit_part_names(model, root))
                if not parts:
                    raise AssetError(f"vehicle {model} has no vehiclemods/{model}_*.yft or kit-listed parts")
                found.extend(parts)
            chains = found
        selected = resolve_chains(entries, chains)
        prepared = []
        total = 0
        for window, member in selected:
            # A part with graphics pages carries its own texture dictionary (the Dominator GTX's 19 livery parts,
            # KoRn a45_livery1..5): it is imported as is; the vehicle converter moves those textures into the car's
            # .ptd (its embedded-texture repair step) and leaves out a part whose dictionary is malformed.
            blob, record = selected_blob(window, member)
            total += len(blob)
            if total > MAX_SELECTION_TOTAL:
                raise AssetError("selected decoded resources exceed the 512 MiB fixture bound")
            prepared.append((chain_name(window, member), blob, record))

        # Preserve the exact pack-level XML metadata beside a selected vehicle. It is context,
        # not a pruned or converted activation set, and remains explicitly unqualified.
        metadata = []
        metadata_selection = None
        derived_metadata = []
        if model is not None:
            for member in root.members():
                if PurePosixPath(member.name).suffix.lower() not in (".xml", ".meta"):
                    continue
                blob = root.read_member(member, MAX_METADATA_BYTES)
                total += len(blob)
                if total > MAX_SELECTION_TOTAL:
                    raise AssetError("selected resources and metadata exceed the fixture bound")
                metadata.append(
                    (member.name, blob, member_record(root, member) | {"bytes": len(blob), "sha256": digest(blob)})
                )
            metadata_selection, derived_metadata = select_vehicle_metadata(model, metadata)

        report = base_report(source, windows)
        report.update(
            operation=operation,
            model=model,
            modelHash=f"0x{hashes.joaat(model):08x}" if model else None,
            **({"modKit": True} if mod_kit else {}),
            resources=[row for _, _, row in prepared],
            metadata=[row for _, _, row in metadata],
            metadataSelection=metadata_selection,
            outputBytes=total,
            derivedMetadataBytes=sum(len(blob) for _, blob in derived_metadata),
            limitations=[
                "Imported resources retain PC RSC7 object and graphics layouts and are not PS5 assets",
                "Pack-level metadata is preserved verbatim as context; no activation subset is synthesized",
                "No stock dependency, numeric-ID, collision, material, skeleton, physics, map-archetype or tuning closure is claimed",
                "The /data/GTAVMenu/custom namespace is a staging contract only; no engine mount is implemented",
            ],
        )
        report["qualification"]["selectedMemberIdentityVerified"] = True
        source.assert_unchanged()
        files: dict[str, bytes] = {}
        for chain, blob, _ in prepared:
            _add_fixture_file(files, output_name_for_chain(chain), blob)
        for name, blob, _ in metadata:
            _add_fixture_file(files, PurePosixPath("metadata", name).as_posix(), blob)
        for name, blob in derived_metadata:
            _add_fixture_file(files, PurePosixPath(name).as_posix(), blob)
        manifest = canonical_report(report)
        if len(manifest) > MAX_REPORT_BYTES:
            raise AssetError("fixture manifest exceeds the 64 MiB verification bound")
        _add_fixture_file(files, "manifest.json", manifest)
        return report, files
    finally:
        source.close()


def _read_fixture_file(path: Path, maximum: int) -> bytes:
    if type(maximum) is not int or maximum < 0:
        raise AssetError("fixture file byte bound is invalid")
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0))
    except OSError as exc:
        raise AssetError(f"cannot open fixture regular file: {path}") from exc
    with os.fdopen(fd, "rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or before.st_size > maximum:
            raise AssetError(f"fixture file is special or exceeds {maximum} bytes: {path}")
        blob = stream.read(maximum + 1)
        after = os.fstat(stream.fileno())
    try:
        current = path.stat(follow_symlinks=False)
    except OSError as exc:
        raise AssetError(f"fixture file changed while being read: {path}") from exc

    def identity(row: os.stat_result) -> tuple[int, int, int, int]:
        return (row.st_dev, row.st_ino, row.st_size, row.st_mtime_ns)

    if len(blob) != before.st_size or identity(before) != identity(after) or identity(before) != identity(current):
        raise AssetError(f"fixture file changed while being read: {path}")
    return blob


def _fixture_directories(files: set[str]) -> set[str]:
    result = set()
    for name in files:
        parent = PurePosixPath(name).parent
        while str(parent) != ".":
            result.add(parent.as_posix())
            parent = parent.parent
    return result


def verify_fixture_files(fixture: Path, expected: dict[str, bytes]) -> None:
    if fixture.is_symlink() or not fixture.is_dir():
        raise AssetError("fixture must be a real non-symlink directory")
    expected_files = set(expected)
    expected_dirs = _fixture_directories(expected_files)
    found_files: set[str] = set()
    found_dirs: set[str] = set()
    for path in fixture.rglob("*"):
        if path.is_symlink():
            raise AssetError(f"fixture contains a symlink: {path}")
        relative = path.relative_to(fixture).as_posix()
        if path.is_file():
            if relative not in expected_files:
                raise AssetError(f"fixture tree differs: unexpected file {relative}")
            found_files.add(relative)
        elif path.is_dir():
            if relative not in expected_dirs:
                raise AssetError(f"fixture tree differs: unexpected directory {relative}")
            found_dirs.add(relative)
        else:
            raise AssetError(f"fixture contains a special file: {path}")
    if found_files != expected_files or found_dirs != expected_dirs:
        raise AssetError(f"fixture tree differs: files={sorted(found_files)}, dirs={sorted(found_dirs)}")
    for relative, blob in expected.items():
        if _read_fixture_file(fixture / PurePosixPath(relative), len(blob)) != blob:
            raise AssetError(f"fixture byte identity differs: {relative}")


def publish_fixture(output_dir: Path, files: dict[str, bytes]) -> None:
    # Pin fixture publication to the strict loose-package publisher's native
    # no-replace promotion and owned-staging cleanup contract. The schemas are
    # separate; no package/runtime qualification is inherited here.
    package_io._encoded_publication_path(output_dir)
    if output_dir.exists() or output_dir.is_symlink():
        raise AssetError("output directory already exists; choose a new fixture directory")
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=".gtavmenu-pc-fixture-", suffix=".tmp", dir=output_dir.parent))
    identity = None
    try:
        created = temporary.lstat()
        identity = (created.st_dev, created.st_ino)
        for directory in sorted(_fixture_directories(set(files)), key=lambda value: (value.count("/"), value)):
            (temporary / PurePosixPath(directory)).mkdir()
        for relative, blob in files.items():
            with (temporary / PurePosixPath(relative)).open("xb") as stream:
                stream.write(blob)
        verify_fixture_files(temporary, files)
        package_io._rename_directory_noreplace(temporary, output_dir)
    except BaseException as exc:
        if identity is None:
            exc.add_note(f"Could not establish fixture staging ownership; retained staging: {temporary}")
            raise
        try:
            package_io._cleanup_staging(temporary, identity)
        except BaseException as cleanup_error:
            exc.add_note(f"Could not clean fixture staging {temporary}: {cleanup_error}")
        raise


def import_selection(
    path: Path, chains: list[str], output_dir: Path, operation: str, model: str | None, mod_kit: bool = False
) -> dict:
    if output_dir.exists() or output_dir.is_symlink():
        raise AssetError("output directory already exists; choose a new fixture directory")
    report, files = build_selection(path, chains, operation, model, mod_kit)
    publish_fixture(output_dir, files)
    return report


def load_fixture_manifest(fixture: Path) -> tuple[dict, bytes]:
    if fixture.is_symlink() or not fixture.is_dir():
        raise AssetError("fixture must be a real non-symlink directory")
    blob = _read_fixture_file(fixture / "manifest.json", MAX_REPORT_BYTES)
    if not blob:
        raise AssetError("fixture manifest is empty")
    try:
        report = json.loads(blob)
    except (UnicodeDecodeError, ValueError, RecursionError) as exc:
        raise AssetError(f"fixture manifest is not valid UTF-8 JSON: {exc}") from exc
    if type(report) is not dict:
        raise AssetError("fixture manifest root must be an object")
    if canonical_report(report) != blob:
        raise AssetError("fixture manifest must use canonical sorted JSON")
    return report, blob


def _replay_request(report: dict) -> tuple[list[str], str, str | None, bool]:
    if type(report.get("schemaVersion")) is not int or report["schemaVersion"] != 1:
        raise AssetError("unsupported PC asset fixture schema version")
    if report.get("kind") != "gtavmenu-pc-asset-import":
        raise AssetError("unsupported PC asset fixture kind")
    operation = report.get("operation")
    if operation == "extract":
        if report.get("model") is not None:
            raise AssetError("extract fixture cannot declare a vehicle model")
        resources = report.get("resources")
        if type(resources) is not list or not 1 <= len(resources) <= MAX_ENTRIES:
            raise AssetError("extract fixture needs a bounded nonempty resource list")
        chains = []
        for row in resources:
            member = row.get("member") if type(row) is dict else None
            if type(member) is not str:
                raise AssetError("extract fixture resource member is invalid")
            chains.append(member)
        return chains, operation, None, False
    if operation == "vehicle":
        model = report.get("model")
        if type(model) is not str or not MODEL_RE.fullmatch(model):
            raise AssetError("vehicle fixture model is invalid")
        mod_kit = report.get("modKit", False)
        if mod_kit is not True and "modKit" in report:
            raise AssetError("vehicle fixture mod-kit flag is invalid")
        return [], operation, model, mod_kit
    raise AssetError("fixture operation is unsupported")


def verify_import_fixture(source: Path, fixture: Path) -> dict:
    stored, stored_blob = load_fixture_manifest(fixture)
    chains, operation, model, mod_kit = _replay_request(stored)
    fresh, expected = build_selection(source, chains, operation, model, mod_kit)
    if expected["manifest.json"] != stored_blob:
        raise AssetError("fixture manifest differs from a fresh source-archive replay")
    verify_fixture_files(fixture, expected)
    return {
        "schemaVersion": 1,
        "kind": "gtavmenu-pc-asset-import-verification",
        "fixture": str(fixture),
        "operation": operation,
        "model": model,
        "resourceCount": len(fresh["resources"]),
        "metadataCount": len(fresh["metadata"]),
        "fixtureFiles": len(expected),
        "fixtureBytes": sum(map(len, expected.values())),
        "verificationScope": "source-archive-replay-and-fixture-bytes",
        "sourceArchiveIdentityVerified": True,
        "sourceArchiveTablesReparsed": True,
        "selectedMemberIdentityVerified": True,
        "fixtureManifestReproduced": True,
        "fixtureByteIdentityVerified": True,
        "conversionPerformed": False,
        "ps5ResourceLayoutsValidated": False,
        "completeDependencyClosure": False,
        "uploadAllowed": False,
        "runtimeEnabled": False,
        "liveTestReady": False,
        "limitations": [
            "Fresh replay verifies exact imported PC bytes and metadata fragments, not PC-to-PS5 conversion.",
            "This verifier does not authorize packaging, upload, activation or runtime use.",
        ],
    }


def write_report(report: dict, output: Path) -> None:
    if output.exists() or output.is_symlink() or output.suffix.lower() != ".json":
        raise AssetError("inventory output must be a new JSON file")
    write_atomic(output, canonical_report(report))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    inv = commands.add_parser("inventory", help="record bounded tables for a large RPF and stored nested RPFs")
    inv.add_argument("source", type=Path)
    inv.add_argument("--output", type=Path, required=True)
    extract = commands.add_parser("extract", help="materialize exact member chains into a private fixture")
    extract.add_argument("source", type=Path)
    extract.add_argument("--member", action="append", required=True)
    extract.add_argument("--output-dir", type=Path, required=True)
    vehicle = commands.add_parser("vehicle", help="import model.yft, model_hi.yft, model.ytd and pack metadata")
    vehicle.add_argument("source", type=Path)
    vehicle.add_argument("--model", required=True)
    vehicle.add_argument("--output-dir", type=Path, required=True)
    vehicle.add_argument("--mod-kit", action="store_true", help="also import vehiclemods/<model>_*.yft parts")
    verify = commands.add_parser("verify", help="replay an import from its original RPF and compare every fixture byte")
    verify.add_argument("source", type=Path)
    verify.add_argument("--fixture", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "inventory":
            report = inventory(args.source)
            write_report(report, args.output)
            print(json.dumps(report["counts"], sort_keys=True))
        elif args.command == "extract":
            report = import_selection(args.source, args.member, args.output_dir, "extract", None)
            print(json.dumps({"resources": len(report["resources"]), "bytes": report["outputBytes"]}, sort_keys=True))
        elif args.command == "verify":
            print(json.dumps(verify_import_fixture(args.source, args.fixture), sort_keys=True))
        else:
            if not MODEL_RE.fullmatch(args.model):
                raise AssetError("model must be a lowercase ASCII asset identifier")
            report = import_selection(args.source, [], args.output_dir, "vehicle", args.model, args.mod_kit)
            print(
                json.dumps(
                    {
                        "model": args.model,
                        "resources": len(report["resources"]),
                        "bytes": report["outputBytes"],
                        "conversionAvailable": False,
                    },
                    sort_keys=True,
                )
            )
        return 0
    except (AssetError, OSError, KeyError, TypeError, ValueError) as exc:
        parser.exit(1, f"PC asset import failed: {exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())

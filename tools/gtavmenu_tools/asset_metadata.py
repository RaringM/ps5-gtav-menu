"""Bounded XML observations and scoped add-on dependency preflight.

Field names follow the supplied PC add-on XML, not a PS5 registration ABI.
This deliberately covers only selected references: locating a file/definition
does not validate its contents, activation order, or presence in the stock game.
"""

from __future__ import annotations

import io
import re
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import PurePosixPath

from .asset_formats import AssetError, Limits, safe_name
from .hashes import joaat

FILE_ROOTS = {
    "VEHICLE_METADATA_FILE": "CVehicleModelInfo__InitDataList",
    "HANDLING_FILE": "CHandlingDataMgr",
    "CARCOLS_FILE": "CVehicleModelInfoVarGlobal",
    "VEHICLE_VARIATION_FILE": "CVehicleModelInfoVariation",
    "TEXTFILE_METAFILE": "CExtraTextMetaFile",
}
NUMERIC_KINDS = {"modkit-id", "light-setting", "siren-setting"}


def parse_xml(data: bytes, limits: Limits) -> ET.Element:
    if len(data) > limits.max_metadata_bytes:
        raise AssetError("metadata exceeds inspection limit")
    text = data.decode("utf-8-sig")
    if "<!DOCTYPE" in text.upper() or "<!ENTITY" in text.upper():
        raise AssetError("XML DTDs and entities are forbidden")
    nodes = level = 0
    parser = ET.iterparse(io.StringIO(text), events=("start", "end"))
    for event, _ in parser:
        if event == "start":
            level += 1
            nodes += 1
            if level > limits.max_xml_depth or nodes > limits.max_xml_nodes:
                raise AssetError("XML structure exceeds inspection limit")
        else:
            level -= 1
    return parser.root


def scalar(item: ET.Element, field: str, *, required: bool = False, attribute: bool = False) -> str:
    found = item.findall(field)
    if len(found) > 1:
        raise AssetError(f"duplicate metadata field: {field}")
    if not found:
        if required:
            raise AssetError(f"missing metadata field: {field}")
        return ""
    element = found[0]
    if len(element):
        raise AssetError(f"expected scalar metadata field: {field}")
    value = (element.get("value", "") if attribute else element.text or "").strip()
    if len(value) > 512 or (required and not value):
        raise AssetError(f"empty or overlong metadata field: {field}")
    return value


def identifier(value: str, kind: str) -> str:
    if kind in NUMERIC_KINDS:
        if not re.fullmatch(r"[0-9]{1,10}", value) or int(value) > 0xFFFFFFFF:
            raise AssetError("metadata numeric identifier exceeds host report bounds")
        return str(int(value))
    if not re.fullmatch(r"[A-Za-z0-9_@.-]{1,128}", value) or value in (".", ".."):
        raise AssetError(f"unsupported metadata identifier: {value!r}")
    return value


def summarize_metadata(data: bytes, limits: Limits) -> dict:
    root = parse_xml(data, limits)
    definitions: list[dict] = []
    references: list[dict] = []
    result = {
        "rootElement": root.tag,
        "coverage": "selected-dependencies-only",
        "semanticsValidated": False,
        "definitions": definitions,
        "references": references,
    }

    def add(collection: list[dict], kind: str, value: str, field: str, requirement: str | None = None) -> None:
        if not value:
            return
        record = {"kind": kind, "name": identifier(value, kind), "field": field}
        if requirement is not None:
            record["requirement"] = requirement
        collection.append(record)
        if len(definitions) + len(references) > limits.max_entries:
            raise AssetError("metadata dependency count exceeds inspection limit")

    def reference(
        item: ET.Element,
        field: str,
        kind: str,
        *,
        required: bool = False,
        attribute: bool = False,
        requirement: str = "local-or-stock",
    ) -> None:
        value = scalar(item, field, required=required, attribute=attribute)
        if value.lower() == "null" and not required:
            return
        add(references, kind, value, field, requirement)

    if root.tag == "SSetupData":
        device = identifier(scalar(root, "deviceName", required=True), "device")
        dat_file = safe_name(scalar(root, "datFile", required=True))
        result["setup"] = {"deviceName": device, "datFile": dat_file}
    elif root.tag == "CDataFileMgr__ContentsOfDataFileXml":
        entries = []
        for item in root.findall("./dataFiles/Item"):
            entries.append(
                {
                    "filename": scalar(item, "filename", required=True),
                    "fileType": scalar(item, "fileType", required=True),
                }
            )
        if len(entries) > limits.max_entries:
            raise AssetError("data-file declaration count exceeds inspection limit")
        result["dataFiles"] = entries
        result["activationValidated"] = False
    elif root.tag == "CVehicleModelInfo__InitDataList":
        reference(root, "residentTxd", "texture-dictionary")
        for item in root.findall("./InitDatas/Item"):
            reference(item, "modelName", "model", required=True, requirement="pack-required")
            reference(item, "txdName", "texture-dictionary", required=True)
            reference(item, "handlingId", "handling", required=True)
            for field, kind in (
                ("audioNameHash", "vehicle-audio"),
                ("layout", "vehicle-layout"),
                ("expressionDictName", "expression-dictionary"),
                ("animConvRoofDictName", "animation-dictionary"),
                ("ptfxAssetName", "particle-asset"),
            ):
                reference(item, field, kind)
    elif root.tag == "CHandlingDataMgr":
        for item in root.findall("./HandlingData/Item"):
            add(definitions, "handling", scalar(item, "handlingName", required=True), "handlingName")
    elif root.tag == "CVehicleModelInfoVarGlobal":
        for item in root.findall("./Kits/Item"):
            add(definitions, "modkit", scalar(item, "kitName", required=True), "kitName")
            add(definitions, "modkit-id", scalar(item, "id", required=True, attribute=True), "id")
            for group in ("visibleMods", "linkMods"):
                for mod in item.findall(f"./{group}/Item"):
                    reference(mod, "modelName", "model", required=True)
                    for linked in mod.findall("./linkedModels/Item"):
                        if len(linked):
                            raise AssetError("unsupported nested linked-model declaration")
                        add(references, "model", (linked.text or "").strip(), "linkedModels/Item", "local-or-stock")
        for group, kind in (("Lights", "light-setting"), ("Sirens", "siren-setting")):
            for item in root.findall(f"./{group}/Item"):
                add(definitions, kind, scalar(item, "id", required=True, attribute=True), "id")
    elif root.tag == "CVehicleModelInfoVariation":
        for item in root.findall("./variationData/Item"):
            reference(item, "modelName", "model", required=True)
            for kit in item.findall("./kits/Item"):
                if len(kit):
                    raise AssetError("unsupported nested modkit reference")
                add(references, "modkit", (kit.text or "").strip(), "kits/Item", "local-or-stock")
            reference(item, "lightSettings", "light-setting", attribute=True)
            reference(item, "sirenSettings", "siren-setting", attribute=True)
    else:
        result["coverage"] = "syntax-only"
    return result


def resolve_virtual_path(value: str, device: str, prefix: str) -> str:
    """Resolve the explicit PC x64 placeholder only; never access the host FS."""
    match = re.fullmatch(r"([A-Za-z0-9_@.-]+):/(.+)", value)
    if not match or match[1].lower() != device.lower():
        raise AssetError("unsupported or foreign DLC device path")
    relative = match[2]
    if "%PLATFORM%" in relative:
        if not relative.startswith("%PLATFORM%/") or relative.count("%PLATFORM%") != 1:
            raise AssetError("unsupported platform placeholder position")
        relative = "x64/" + relative[len("%PLATFORM%/") :]
    return prefix + safe_name(relative)


def definition_key(record: dict) -> tuple[str, int]:
    kind, name = record["kind"], record["name"]
    return kind, int(name) if kind in NUMERIC_KINDS else joaat(name)


def dependency_report(files: list[dict]) -> dict:
    """Locate selected declarations within each setup-defined pack boundary.

    Only declared metadata and resources beneath declared RPFs are candidates.
    An unrelated loose file or a different DLC cannot satisfy a reference.
    """
    files = sorted(files, key=lambda row: row["path"])
    paths: dict[str, list[dict]] = defaultdict(list)
    setups: dict[str, list[dict]] = defaultdict(list)
    for row in files:
        paths[row["path"].lower()].append(row)
        if "setup" in row.get("metadata", {}) and row.get("disposition") != "rejected":
            prefix = row["path"].rpartition("/")[0]
            setups[(prefix + "/" if prefix else "").lower()].append(row)

    def owner(path: str) -> str | None:
        current = path.lower()
        while "/" in current:
            current = current.rpartition("/")[0]
            if current + "/" in setups:
                return current + "/"
        return "" if "" in setups else None

    owned: dict[str, list[dict]] = defaultdict(list)
    unscoped = []
    for row in files:
        prefix = owner(row["path"])
        if prefix is not None:
            owned[prefix].append(row)
        elif "metadata" in row:
            unscoped.append(row["path"])
    packs = []
    for prefix, entries in sorted(setups.items()):
        pack = {
            "root": prefix,
            "diagnostics": [],
            "declarations": [],
            "definitions": [],
            "references": [],
            "activationValidated": False,
            "stockConflictsChecked": False,
        }
        packs.append(pack)
        if len(entries) != 1:
            pack["diagnostics"].append("ambiguous setup in pack root")
            continue
        setup = entries[0]["metadata"]["setup"]
        pack.update(setupPath=entries[0]["path"], deviceName=setup["deviceName"])
        content_path = prefix + setup["datFile"]
        contents = paths.get(content_path.lower(), [])
        if (
            len(contents) != 1
            or contents[0].get("disposition") == "rejected"
            or "dataFiles" not in contents[0].get("metadata", {})
        ):
            pack["diagnostics"].append("setup datFile is missing, ambiguous, rejected, or not a content manifest")
            continue
        if owner(contents[0]["path"]) != prefix:
            pack["diagnostics"].append("setup datFile crosses a nested pack boundary")
            continue
        pack["contentPath"] = contents[0]["path"]
        metadata_paths: set[str] = set()
        archives: set[str] = set()
        declared: dict[str, dict] = {}
        for declaration in contents[0]["metadata"]["dataFiles"]:
            row = {**declaration, "status": "unresolved"}
            pack["declarations"].append(row)
            try:
                target = resolve_virtual_path(declaration["filename"], setup["deviceName"], prefix)
            except AssetError as exc:
                row.update(status="unsupported-path", detail=str(exc))
                continue
            key = target.lower()
            if key in declared:
                row["status"] = "duplicate-declaration"
                declared[key]["status"] = "duplicate-declaration"
                metadata_paths.discard(key)
                archives.discard(key)
                continue
            declared[key] = row
            matches = paths.get(key, [])
            if not matches:
                row["status"] = "missing-file"
                continue
            if len(matches) != 1 or owner(matches[0]["path"]) != prefix:
                row["status"] = "ambiguous-or-cross-pack"
                continue
            source = matches[0]
            row["target"] = source["path"]
            if source.get("disposition") == "rejected":
                row["status"] = "rejected-file"
                continue
            file_type = declaration["fileType"]
            if file_type == "RPF_FILE":
                if source.get("kind") != "container" or not key.endswith(".rpf"):
                    row["status"] = "file-type-mismatch"
                    continue
                archives.add(key)
            elif file_type in FILE_ROOTS:
                if source.get("metadata", {}).get("rootElement") != FILE_ROOTS[file_type]:
                    row["status"] = "file-type-mismatch"
                    continue
                metadata_paths.add(key)
            else:
                row["status"] = "unsupported-file-type"
                continue
            row["status"] = "located-not-qualified"
        definitions: dict[tuple[str, int], list[dict]] = defaultdict(list)
        for source in owned[prefix]:
            path = source["path"]
            if source.get("disposition") == "rejected":
                continue
            records = []
            if path.lower() in metadata_paths:
                records = source["metadata"]["definitions"]
                pack["references"].extend({**r, "source": path} for r in source["metadata"]["references"])
            elif source.get("kind") == "resource-envelope":
                parents = path.lower().split("!/")[:-1]
                if not any("!/".join(parents[: i + 1]) in archives for i in range(len(parents))):
                    continue
                suffix = PurePosixPath(path).suffix.lower()
                kind = "model" if suffix in (".yft", ".ydr") else "texture-dictionary" if suffix == ".ytd" else None
                if kind:
                    records = [{"kind": kind, "name": PurePosixPath(path).stem, "field": "resource-filename"}]
            for record in records:
                definition = {**record, "source": path}
                definitions[definition_key(record)].append(definition)
                pack["definitions"].append(definition)
        for matches in definitions.values():
            if len(matches) > 1:
                pack["diagnostics"].append(f"duplicate definition/hash: {matches[0]['kind']} {matches[0]['name']}")
        for reference in pack["references"]:
            matches = definitions.get(definition_key(reference), [])
            if len(matches) == 1 and matches[0]["name"].lower() == reference["name"].lower():
                reference.update(status="local-candidate", target=matches[0]["source"])
            elif matches:
                reference["status"] = "ambiguous-local"
            else:
                reference["status"] = (
                    "missing-pack-asset" if reference["requirement"] == "pack-required" else "external-unverified"
                )
    # Pack-local lookup must not conceal possible collisions when several
    # add-ons are inspected together. These are candidates, not a reviewed
    # model of the target's global registries; no IDs are rewritten here.
    bindings: dict[tuple[str, int], list[dict]] = defaultdict(list)
    for pack in packs:
        records = list(pack["definitions"])
        if "deviceName" in pack:
            records.append({"kind": "dlc-device", "name": pack["deviceName"], "source": pack["setupPath"]})
        for record in records:
            bindings[definition_key(record)].append(
                {"pack": pack["root"], "source": record["source"], "name": record["name"]}
            )
    conflicts = [
        {"kind": kind, "identity": value, "definitions": owners}
        for (kind, value), owners in sorted(bindings.items())
        if len({owner["pack"] for owner in owners}) > 1
    ]
    return {
        "coverage": "declared-files-and-selected-vehicle-references",
        "completeDependencyClosure": False,
        "registrationValidated": False,
        "unreviewedAreas": [
            "changeset activation and load order",
            "stock definitions and numeric-ID conflicts",
            "labels and locale coverage",
            "fragment/material/physics dependencies",
            "metadata fields outside the selected reference subset",
        ],
        "packs": packs,
        "crossPackCollisionCandidates": conflicts,
        "unscopedMetadata": sorted(unscoped),
    }

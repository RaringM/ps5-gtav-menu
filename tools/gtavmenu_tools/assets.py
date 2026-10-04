"""Read-only custom-asset intake and qualification reports (stdlib only).

Inspection is not conversion or approval to load into GTA. This initial tool
has no qualified PS5 writer or runtime registration backend. convert/package
therefore emit a rejection report and create no asset bundle. There is no force
switch and a pack cannot grant itself compatibility through a manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import stat
import struct
import sys
import unicodedata
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath

from .asset_formats import (
    AssetError,
    Limits,
    decode_resource,
    gxt2_entries,
    read_rpf_member,
    resource_header,
    rpf_header,
    rpf_members,
    safe_name,
)
from .asset_metadata import dependency_report, summarize_metadata
from .asset_textures import inspect_legacy_dictionary
from .hashes import joaat

TOOL_VERSION = 2
PC_RESOURCES = {".ydr", ".ydd", ".yft", ".ytd", ".ytyp", ".ybn"}
ANCILLARY = {".txt", ".md", ".png", ".jpg", ".jpeg", ".pdf"}
EXECUTABLE = {".asi", ".dll", ".exe", ".so", ".ysc", ".lua", ".js", ".cs", ".py", ".sh"}
BLOCKERS = (
    "PS5 resource layouts and writers have not passed independent round-trip qualification",
    "No reviewed PS5 metadata/dependency registration contract",
    "Engine-thread visibility and safe post-readiness registration are unqualified",
    "Process-lifetime ownership, menu shutdown, and hardware qualification are pending",
)
DEFAULT_LIMITS = Limits()


def source_name(name: str) -> str:
    """Allow localized readme basenames, never localized engine namespaces.

    These files remain not-loadable and are never extracted. Directories and
    all actual asset names still follow the conservative ASCII contract.
    """
    if PurePosixPath(name).suffix.lower() in ANCILLARY:
        parent, separator, leaf = name.rpartition("/")
        portable_leaf = "".join(
            "u" if ord(char) > 127 and unicodedata.category(char)[0] in ("L", "M", "N") else char for char in leaf
        )
        safe_name(parent + separator + portable_leaf)
        return name
    return safe_name(name)


def capabilities() -> dict:
    return {
        "kind": "gtavmenu-asset-capabilities",
        "schemaVersion": 1,
        "toolVersion": TOOL_VERSION,
        "policy": "additive-only; reject unsupported dependencies; restart required",
        "inspection": [
            "directory",
            "ZIP (non-ZIP64)",
            "PC RPF7 NONE/OPEN",
            "PC RSC7 envelope",
            "UTF-8 XML",
            "GXT2",
            "PC Legacy YTD preflight",
            "selected add-on metadata dependencies",
        ],
        "conversionAvailable": False,
        "packagingAvailable": False,
        "runtimeEnabled": False,
        "blockers": list(BLOCKERS),
    }


@dataclass
class Budget:
    limits: Limits
    bytes_read: int = 0
    entries: int = 0

    def charge(self, size: int) -> None:
        if size < 0 or size > self.limits.max_file_bytes or size > self.limits.max_total_bytes - self.bytes_read:
            raise AssetError("cumulative input/decompression budget exceeded")
        self.bytes_read += size

    def entry(self) -> None:
        self.entries += 1
        if self.entries > self.limits.max_entries:
            raise AssetError("cumulative entry count exceeds inspection limit")


def read_file(path: Path, budget: Budget) -> bytes:
    # Reject special files before open (opening a FIFO could block indefinitely).
    if not stat.S_ISREG(path.lstat().st_mode):
        raise AssetError("expected a regular file, not a symlink or special file")
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0))
    with os.fdopen(fd, "rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise AssetError("input changed to a special file")
        budget.charge(before.st_size)
        data = stream.read(before.st_size + 1)
        after = os.fstat(stream.fileno())
        if len(data) != before.st_size or (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise AssetError("input changed while being read")
        return data


def zip_entries(data: bytes, limits: Limits) -> zipfile.ZipFile:
    # Bound central-directory allocation before ZipFile creates ZipInfo objects.
    end = data.rfind(b"PK\x05\x06", max(0, len(data) - 65557))
    if end < 0 or end + 22 > len(data):
        raise AssetError("ZIP end-of-directory record is missing")
    disk, cd_disk, disk_count, count, size, offset, comment = struct.unpack_from("<4H2IH", data, end + 4)
    if disk or cd_disk or disk_count != count or count == 0xFFFF or size == 0xFFFFFFFF or offset == 0xFFFFFFFF:
        raise AssetError("multi-disk and ZIP64 inputs are not supported")
    if count > limits.max_entries or size > limits.max_entries * 1024:
        raise AssetError("ZIP directory exceeds inspection limit")
    if end + 22 + comment != len(data) or offset + size != end:
        raise AssetError("invalid ZIP directory bounds or trailing data")
    archive = zipfile.ZipFile(io.BytesIO(data))
    if len(archive.infolist()) != count:
        archive.close()
        raise AssetError("ZIP entry count mismatch")
    return archive


def inspect(source: Path, limits: Limits = DEFAULT_LIMITS) -> dict:
    budget = Budget(limits)
    rows: list[dict] = []
    issues: list[dict] = []
    identities: dict[tuple[str, int], str] = {}

    def issue(path: str, message: str) -> None:
        issues.append({"path": path, "message": message})

    def visit(name: str, data: bytes, depth: int) -> None:
        budget.entry()
        if depth > limits.max_depth:
            raise AssetError("nested archive depth exceeds inspection limit")
        row = {"path": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        rows.append(row)
        suffix = PurePosixPath(name).suffix.lower()
        try:
            if suffix == ".rpf":
                row["kind"] = "container"
                row["disposition"] = "inspected"
                row["header"] = rpf_header(data, len(data))
                members = rpf_members(data, limits)
                for member in members:
                    child = f"{name}!/{member.name}"
                    try:
                        if member.encrypted:
                            raise AssetError("encrypted member; supply decoded files")
                        expected = member.size if member.resource else member.unpacked_size
                        budget.charge(expected)
                        blob = read_rpf_member(data, member, limits.max_file_bytes)
                        visit(child, blob, depth + 1)
                    except AssetError as exc:
                        issue(child, str(exc))
            elif suffix == ".zip":
                row["kind"] = "container"
                row["disposition"] = "inspected"
                with zip_entries(data, limits) as archive:
                    seen: set[str] = set()
                    files: set[str] = set()
                    directories: set[str] = set()
                    for entry in sorted(archive.infolist(), key=lambda item: item.filename):
                        path = safe_name(entry.filename[:-1]) if entry.is_dir() else source_name(entry.filename)
                        if path.lower() in seen:
                            raise AssetError(f"duplicate ZIP path: {path}")
                        seen.add(path.lower())
                        parts = path.lower().split("/")
                        parents = {"/".join(parts[:index]) for index in range(1, len(parts))}
                        if parents & files or (not entry.is_dir() and path.lower() in directories):
                            raise AssetError(f"ZIP file/directory namespace collision: {path}")
                        directories.update(parents)
                        (directories if entry.is_dir() else files).add(path.lower())
                        mode = entry.external_attr >> 16
                        if stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR):
                            raise AssetError(f"ZIP symlink or special file: {path}")
                        if entry.flag_bits & 1:
                            raise AssetError(f"encrypted ZIP member: {path}")
                        if entry.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                            raise AssetError(f"unsupported ZIP compression: {path}")
                        if entry.is_dir():
                            budget.entry()
                            continue
                        budget.charge(entry.file_size)
                        with archive.open(entry) as stream:
                            blob = stream.read(entry.file_size + 1)
                        if len(blob) != entry.file_size:
                            raise AssetError(f"ZIP length mismatch: {path}")
                        visit(f"{name}!/{path}", blob, depth + 1)
            elif suffix in PC_RESOURCES:
                row["kind"] = "resource-envelope"
                row["disposition"] = "requires-layout-validation"
                # Charge declared pages before decompressing. Header inspection
                # alone cannot establish resource-object or platform validity.
                header = resource_header(data)
                budget.charge(header["systemBytes"] + header["graphicsBytes"])
                header, payload = decode_resource(data, limits.max_file_bytes)
                row["resource"] = {**header, "payloadSha256": hashlib.sha256(payload).hexdigest()}
                if suffix == ".ytd" and header["version"] == 13:
                    row["textureDictionary"] = inspect_legacy_dictionary(payload, header, limits)
                row["objectLayoutValidated"] = False
                row["platform"] = "unverified"
                name_hash = joaat(PurePosixPath(name).stem)
                row["nameHash"] = f"0x{name_hash:08x}"
                key = ("model" if suffix in (".ydr", ".yft") else suffix, name_hash)
                if key in identities:
                    raise AssetError(f"resource name/hash collision; also present at {identities[key]}")
                identities[key] = name
            elif suffix == ".gxt2":
                row["kind"] = "localization-table"
                row["disposition"] = "requires-semantic-validation"
                labels = gxt2_entries(data, limits)
                row["labelCount"] = len(labels)
                row["semanticsValidated"] = False
            elif suffix in (".meta", ".xml"):
                row["kind"] = "metadata-syntax"
                row["disposition"] = "requires-semantic-validation"
                row["metadata"] = summarize_metadata(data, limits)
                row["rootElement"] = row["metadata"]["rootElement"]
                row["semanticsValidated"] = False
            elif suffix in EXECUTABLE:
                raise AssetError("scripts/plugins are not supported and will never be executed")
            elif suffix in ANCILLARY:
                row["kind"] = "ancillary"
                row["disposition"] = "not-loadable"
            else:
                raise AssetError(f"unsupported asset type: {suffix or '(no extension)'}")
        except (AssetError, UnicodeError, ET.ParseError, zipfile.BadZipFile, RuntimeError, NotImplementedError) as exc:
            row["disposition"] = "rejected"
            issue(name, str(exc))

    def walk(directory: Path, prefix: str, depth: int) -> None:
        if depth > limits.max_depth:
            raise AssetError("directory depth exceeds inspection limit")
        seen: set[str] = set()
        # Bound enumeration before sorting, including empty directories.
        children = []
        for child in directory.iterdir():
            budget.entry()
            children.append(child)
        for child in sorted(children, key=lambda item: item.name):
            name = f"{prefix}/{child.name}" if prefix else child.name
            try:
                mode = child.lstat().st_mode
                (safe_name if stat.S_ISDIR(mode) else source_name)(name)
                if child.name.lower() in seen:
                    raise AssetError("case-insensitive path collision")
                seen.add(child.name.lower())
                if stat.S_ISDIR(mode):
                    walk(child, name, depth + 1)
                else:
                    visit(name, read_file(child, budget), 0)
            except (AssetError, OSError) as exc:
                issue(name, str(exc))

    try:
        mode = source.lstat().st_mode
        if stat.S_ISDIR(mode):
            walk(source, "", 0)
        else:
            source_name(source.name)
            visit(source.name, read_file(source, budget), 0)
    except (AssetError, OSError) as exc:
        issue(source.name, str(exc))
    if not rows:
        issue(source.name, "no files inspected")
    rows.sort(key=lambda item: item["path"])
    issues.sort(key=lambda item: (item["path"], item["message"]))
    fingerprint = hashlib.sha256(json.dumps(rows, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    return {
        "schemaVersion": 1,
        "kind": "gtavmenu-asset-intake",
        "toolVersion": TOOL_VERSION,
        "inspectionComplete": not issues,
        "contentFingerprint": fingerprint,
        "files": rows,
        "errors": issues,
        "limits": asdict(limits),
        "workBytes": budget.bytes_read,
        "qualification": capabilities(),
        "dependencies": dependency_report(rows),
    }


def emit(report: dict, output: Path | None, source: Path | None = None) -> None:
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if output is None:
        print(text, end="")
        return
    if source is not None and (output.resolve() == source.resolve() or source.resolve() in output.resolve().parents):
        raise AssetError("report must be outside the input tree")
    if output.suffix.lower() != ".json":
        raise AssetError("report output must be a new .json file")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as stream:
        stream.write(text)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    status = commands.add_parser("status", help="show actual qualification gates; never enables loading")
    status.add_argument("--output", type=Path)
    for name in ("inspect", "validate", "convert", "package"):
        command = commands.add_parser(name)
        command.add_argument("source", type=Path)
        command.add_argument("--output", type=Path, help="new JSON report, not an asset output directory")
    args = parser.parse_args(argv)
    try:
        if args.command == "status":
            emit(capabilities(), args.output)
            return 0
        report = inspect(args.source)
        report["requestedOperation"] = args.command
        report["operationSucceeded"] = args.command == "inspect" and report["inspectionComplete"]
        if args.command != "inspect":
            report["errors"].append({"path": "", "message": "PS5 qualification gate closed: " + "; ".join(BLOCKERS)})
        emit(report, args.output, args.source)
        return 0 if report["operationSucceeded"] else 1
    except (AssetError, OSError) as exc:
        print(f"assets: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

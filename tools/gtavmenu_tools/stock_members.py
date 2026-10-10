"""Reviewed stock ownership and key-free RSC7 inputs from the user's own game.

The target manifest contains names, owner paths and verified member byte ranges only.
It is deliberately scoped: an unlisted name is unknown, never evidence of absence.
No game archive table, executable, key or binary metadata is read by this module.
"""

from __future__ import annotations

import hashlib
import json
import re
import stat
from pathlib import Path, PurePosixPath

from gtavmenu_tools import retail_templates as rt
from gtavmenu_tools.host_paths import build_dir

KIND = "gtavmenu-stock-members"
TARGET = rt.DEFAULT_TARGET
MANIFESTS = Path(__file__).resolve().parents[2] / "data/stock_members"
_DEFAULT_MANIFEST_SHA256 = "a68450bd7561e7203abf49aff43b4a7666275532f8ab75f59b2c30e947004a07"
MANIFEST_SHA256 = {TARGET: _DEFAULT_MANIFEST_SHA256}
MAX_MANIFEST_BYTES = 8 * 1024 * 1024
MAX_MEMBER_BYTES = 256 * 1024 * 1024
_NAME = re.compile(r"[a-z0-9_+]{1,63}\.(pft|pdr|pdd|ptd|pmt)\Z")
_HASH = re.compile(r"[0-9a-f]{64}\Z")


class StockError(ValueError):
    """A stock input is unreviewed, invalid, missing or does not match its pin."""


def relative(value: str) -> str:
    if not isinstance(value, str):
        raise StockError("stock member path must be a string")
    path = PurePosixPath(value)
    if (
        not value
        or path.is_absolute()
        or path.as_posix() != value
        or any(p in (".", "..") for p in path.parts)
        or any(ord(c) < 32 or ord(c) > 126 or c in "\\:" for c in value)
    ):
        raise StockError(f"unsafe stock path {value!r}")
    return value


def located(value: str) -> tuple[str, str]:
    if not isinstance(value, str) or value.count(":") != 1:
        raise StockError("stock owner must be archive:member")
    archive, member = value.split(":")
    relative(archive)
    relative(member)
    if not archive.endswith(".rpf") or not _NAME.fullmatch(PurePosixPath(member).name):
        raise StockError(f"invalid stock owner {value!r}")
    return archive, member


def safe_path(root: Path, name: str) -> Path:
    """Keep cache and local-source accesses within their root, refusing symlinks."""
    relative(name)
    path = root
    if root.is_symlink():
        raise StockError(f"symbolic link refused: {root}")
    for part in PurePosixPath(name).parts:
        path = path / part
        if path.is_symlink():
            raise StockError(f"symbolic link refused: {path}")
    if not path.resolve().is_relative_to(root.resolve()):
        raise StockError("stock input escapes its root")
    return path


class Stock:
    """A target-pinned ownership index with verified on-demand resource fetches."""

    def __init__(
        self, game: Path | None = None, cache: Path | None = None, manifest: Path | None = None, target: str = TARGET
    ):
        self.path = manifest or MANIFESTS / f"{target}.json"
        if self.path.is_symlink() or not stat.S_ISREG(self.path.stat().st_mode):
            raise StockError("stock manifest must be a regular file, not a symbolic link")
        if self.path.stat().st_size > MAX_MANIFEST_BYTES:
            raise StockError("stock manifest exceeds size budget")
        raw = self.path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != MANIFEST_SHA256.get(target):
            raise StockError("stock manifest SHA256 differs from the reviewed target metadata")
        try:
            doc = json.loads(raw)
            if doc["kind"] != KIND or doc["schemaVersion"] != 1 or doc["target"] != target:
                raise StockError("stock manifest kind/version/target mismatch")
            self.names = doc["owners"]
            self.absent = doc.get("absent", {})
            self.clothing = doc.get("clothing", {})
            self.archives = doc["archives"]
            if not isinstance(self.names, dict) or not isinstance(self.absent, dict):
                raise StockError("stock owners and absence coverage must be mappings")
            for name, copies in self.names.items():
                if (
                    not _NAME.fullmatch(name)
                    or not isinstance(copies, list)
                    or not copies
                    or len(copies) != len(set(copies))
                ):
                    raise StockError(f"invalid or duplicate stock owners for {name!r}")
                for owner in copies:
                    archive, member = located(owner)
                    if PurePosixPath(member).name != name or archive not in self.archives:
                        raise StockError(f"stock owner/name/archive mismatch: {name}")
            for name, evidence in self.absent.items():
                if not _NAME.fullmatch(name) or name in self.names or not isinstance(evidence, str) or not evidence:
                    raise StockError(f"invalid absence coverage: {name!r}")
            for archive, size in self.archives.items():
                relative(archive)
                if not archive.endswith(".rpf") or type(size) is not int or size <= 0:
                    raise StockError("invalid stock archive pin")
            self.templates = {}
            for row in doc["templates"]:
                owner = row["owner"]
                archive, member = located(owner)
                name = PurePosixPath(member).name
                size, offset = row["bytes"], row["storedOffset"]
                if (
                    owner not in self.names.get(name, [])
                    or owner in self.templates
                    or type(size) is not int
                    or not 16 < size <= MAX_MEMBER_BYTES
                    or type(offset) is not int
                    or offset < 0
                    or offset + size > self.archives[archive]
                    or not isinstance(row["sha256"], str)
                    or not _HASH.fullmatch(row["sha256"])
                    or len(row["flags"]) != 2
                    or any(type(x) is not int or not 0 <= x <= 0xFFFFFFFF for x in row["flags"])
                ):
                    raise StockError(f"invalid stock resource pin: {owner}")
                self.templates[owner] = row
        except (KeyError, TypeError, ValueError) as error:
            if isinstance(error, StockError):
                raise
            raise StockError(f"malformed stock manifest: {error}") from None
        self.target = target
        self.digest = hashlib.sha256(raw).hexdigest()
        self.game = game
        self.cache = cache or build_dir(MANIFESTS.parents[1]) / "stock-members" / target
        self.checked_archives: set[str] = set()

    def scan(self, names: set[str]) -> dict[str, list[str]]:
        unknown = names - self.names.keys() - self.absent.keys()
        if unknown:
            raise StockError(
                f"stock ownership is not reviewed for {', '.join(sorted(unknown))}; expand the target manifest"
            )
        archives = {located(p)[0] for name in names for p in self.names.get(name, [])}
        if self.game is not None:
            for archive in sorted(archives - self.checked_archives):
                path = safe_path(self.game, archive)
                if not stat.S_ISREG(path.stat().st_mode):
                    raise StockError(f"{archive}: source archive must be a regular file")
                if path.stat().st_size != self.archives[archive]:
                    raise StockError(f"{archive}: archive size differs from {self.target}")
                self.checked_archives.add(archive)
        return {name: list(self.names.get(name, [])) for name in names}

    def fetch(self, owner: str) -> bytes:
        row = self.templates.get(owner)
        if row is None:
            raise StockError(f"{owner}: no reviewed resource range; expand the target manifest")
        archive, member = located(owner)
        self.scan({PurePosixPath(member).name})
        key = hashlib.sha256(owner.encode("ascii")).hexdigest()
        path = safe_path(self.cache, f"{key}/{PurePosixPath(member).name}")
        if path.exists():
            if not stat.S_ISREG(path.stat().st_mode):
                raise StockError(f"{member}: cache input must be a regular file")
            if path.stat().st_size == row["bytes"]:
                blob = path.read_bytes()
                if hashlib.sha256(blob).hexdigest() == row["sha256"]:
                    return blob
            raise StockError(f"{member}: cached resource does not match its size/SHA256 pin")
        if self.game is None:
            raise StockError(f"{member}: resource not cached; pass --game with your own matching game copy")
        with safe_path(self.game, archive).open("rb") as handle:
            handle.seek(row["storedOffset"])
            stored = handle.read(row["bytes"])
        if len(stored) != row["bytes"]:
            raise StockError(f"{member}: short archive read")
        blob = rt.rsc7(stored, *row["flags"], owner)
        if hashlib.sha256(blob).hexdigest() != row["sha256"]:
            raise StockError(f"{member}: resource SHA256 differs from {self.target}")
        rt._write_atomic(path, blob)
        return blob


def from_args(args) -> Stock:
    return Stock(args.game, args.stock_cache, args.stock_manifest, args.target)


def arguments(parser) -> None:
    parser.add_argument("--game", type=Path, help="your matching local app0 copy; key-free, read-only range reads")
    parser.add_argument("--stock-cache", type=Path, help="verified stock-resource cache")
    parser.add_argument("--stock-manifest", type=Path, help="reviewed stock owner/member metadata (target default)")
    parser.add_argument("--target", default=TARGET, choices=[TARGET], help="stock metadata target")

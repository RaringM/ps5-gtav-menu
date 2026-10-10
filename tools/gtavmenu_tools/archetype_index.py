"""The archetype index of the user's own game: stock archetype -> its typ, bounding box and lodDist.

The map converters (convert_ymap, convert_pc_mlo, convert_map_mod) classify a mod's entities with it:
an archetype the index knows is stock and gets a `typ <name>.ptyp retail` row for its typ, one it
does not know must come from the mod. The index is built from the retail .ptyp members of the game
image, with no game keys:

  - The typ source list (data/archetype_index/<target>.json, kind "gtavmenu-archetype-sources") pins
    every retail .ptyp member of a game build: its archive (app0-relative), the member path inside
    it, the absolute offset and size of its stored bytes and their sha256, plus the drawable and
    fragment names whose hash is an archetype name. Names, offsets, sizes and hashes only, no retail
    bytes. Archive tables are encrypted, members are not (a resource is a 16-byte prefix plus raw
    deflate), so one range read per typ, as for the retail templates (retail_templates.py), reads it.
  - `IndexBuilder` parses each typ's CBaseArchetypeDef / CTimeArchetypeDef / CMloArchetypeDef rows and
    writes the index document (schema "gtavmenu-archetype-index-v1"):

      {"schema": ..., "source": ..., "typs": {"v_construction": {"paths": [...], "archetypes": 278,
       "residency": "resident"}}, "archetypes": {"0x1a2b3c4d": [[typ, lodDist, bbMin, bbMax, flags,
       kind], ...]}, "names": {"0x1a2b3c4d": "prop_towercrane_01a"}}

Residency is decided by the typ's path (the game's own residency lists are encrypted members),
from hardware evidence: a base-game typ under levels/gta5/props/ or levels/gta5/interiors/int_props/
is resident (all nine such stock rows were resident in a hardware run), a typ of a map area
(levels/gta5/_*/) or of one interior is streamed, everything else (DLC typs) unknown. A `typ` row
for a typ that is already resident costs nothing.
"""

from __future__ import annotations

import hashlib
import json
import re
import struct
import zlib
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from gtavmenu_tools import retail_templates
from gtavmenu_tools.hashes import joaat
from gtavmenu_tools.meta_resource import Meta, joaat_cs

SCHEMA = "gtavmenu-archetype-index-v1"
SOURCES_KIND = "gtavmenu-archetype-sources"
SOURCES_VERSION = 1
SOURCES_DIR = Path(__file__).resolve().parents[2] / "data/archetype_index"
MAX_PAYLOAD = 64 * 1024 * 1024
MAX_STORED = 64 * 1024 * 1024
MAX_SOURCES_BYTES = 32 * 1024 * 1024

KINDS = {"CBaseArchetypeDef": "base", "CTimeArchetypeDef": "time", "CMloArchetypeDef": "mlo"}
KIND_HASHES = {h: kind for name, kind in KINDS.items() for h in (joaat_cs(name), joaat(name))}

_BASE_ARCHIVE = re.compile(r"(prospero[a-z]|common)\.rpf/")
_RESIDENT_DIRS = ("levels/gta5/props/", "levels/gta5/interiors/int_props/")
_NAME = re.compile(r"[!-.0-\[\]-~]{1,96}\Z")  # printable ASCII except space, slash, backslash
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")


class ArchetypeIndexError(ValueError):
    """A typ source list, a typ member or the index is outside what this module reads."""


def residency(paths: list[str]) -> str:
    """Path-based residency class of a typ (see the module docstring for the evidence)."""
    base = [p.lower() for p in paths if _BASE_ARCHIVE.match(p.lower())]
    if any(any(d in p for d in _RESIDENT_DIRS) for p in base):
        return "resident"
    if any("levels/gta5/_" in p.lower() or "levels/gta5/interiors/" in p.lower() for p in paths):
        return "streamed"
    return "unknown"


def payload_of(stored: bytes) -> bytes:
    """The meta payload of a stored resource member (16-byte prefix + raw deflate)."""
    stream = zlib.decompressobj(-15)
    out = stream.decompress(stored[16:], MAX_PAYLOAD)
    if not stream.eof:
        raise ArchetypeIndexError("resource does not inflate")
    return out


def archetypes_of(payload: bytes) -> list[tuple[int, str, float, int, list[float], list[float], int]]:
    """(name hash, kind, lodDist, flags, bbMin, bbMax, assetType) of every archetype of a typ payload."""
    meta = Meta(payload)
    out = []
    for struct_hash, size, at in meta.blocks:
        kind = KIND_HASHES.get(struct_hash)
        if kind is None:
            continue
        stride = meta.structs.get(struct_hash, (0, []))[0]
        if not stride:
            continue
        for base in range(at, at + size - stride + 1, stride):
            lod, flags = struct.unpack_from("<fI", payload, base + 0x08)
            bb_min = list(struct.unpack_from("<3f", payload, base + 0x20))
            bb_max = list(struct.unpack_from("<3f", payload, base + 0x30))
            name = struct.unpack_from("<I", payload, base + 0x58)[0]
            asset_type = struct.unpack_from("<I", payload, base + 0x6C)[0]
            out.append((name, kind, lod, flags, bb_min, bb_max, asset_type))
    return out


class IndexBuilder:
    """Collects typs in walk order (the order decides `paths` and the row order per archetype)."""

    def __init__(self) -> None:
        self.typs: dict[str, dict] = {}
        self.index: dict[str, list] = {}

    def add(self, path: str, stored: bytes) -> None:
        """One retail .ptyp member: `path` is 'archive/member' (app0-relative), `stored` its stored bytes."""
        self.add_rows(path, archetypes_of(payload_of(stored)))

    def add_rows(self, path: str, rows: list[tuple]) -> None:
        """One typ's `archetypes_of` rows."""
        typ = PurePosixPath(path).stem.lower()
        entry = self.typs.setdefault(typ, {"paths": [], "archetypes": 0})
        entry["paths"].append(path)
        entry["archetypes"] = max(entry["archetypes"], len(rows))
        for arch, kind, lod, aflags, bb_min, bb_max, _asset in rows:
            key = f"0x{arch:08x}"
            row = [typ, round(lod, 3), [round(v, 3) for v in bb_min], [round(v, 3) for v in bb_max], aflags, kind]
            if row not in self.index.setdefault(key, []):
                self.index[key].append(row)

    def names(self, stems: Iterable[str]) -> dict[str, str]:
        """hash -> name for the archetypes named by these drawable/fragment stems (the last sorted wins)."""
        names = {f"0x{joaat(stem):08x}": stem for stem in sorted(set(stems))}
        return {key: names[key] for key in self.index if key in names}

    def document(self, source: str, stems: Iterable[str], **extra) -> dict:
        for entry in self.typs.values():
            entry["residency"] = residency(entry["paths"])
        names = self.names(stems)
        return {
            "schema": SCHEMA,
            "source": source,
            "typs": dict(sorted(self.typs.items())),
            "archetypes": dict(sorted(self.index.items())),
            "names": dict(sorted(names.items())),
            **extra,
        }


def dump(document: dict) -> str:
    return json.dumps(document, separators=(",", ":")) + "\n"


# --- the typ source list ------------------------------------------------------------------------


@dataclass(frozen=True)
class TypSource:
    archive: str  # app0-relative archive file
    archive_bytes: int
    member: str  # path inside the archive, nested archives as path components
    stored_offset: int  # absolute offset of the stored bytes in the archive file
    stored_bytes: int
    sha256: str  # of the stored bytes

    @property
    def path(self) -> str:
        return f"{self.archive}/{self.member}"


def sources_path(target: str) -> Path:
    return SOURCES_DIR / f"{retail_templates.pin_target(target)}.json"


def _relative(path: str, label: str) -> str:
    pure = PurePosixPath(path)
    if not path or pure.is_absolute() or ".." in pure.parts or pure.as_posix() != path:
        raise ArchetypeIndexError(f"{label}: unsafe path {path!r}")
    return path


def load_sources(path: Path) -> tuple[dict, list[TypSource], list[str], str]:
    """(document, typ rows in walk order, archetype names, sha256 of the file) of a typ source list."""
    try:
        if path.stat().st_size > MAX_SOURCES_BYTES:
            raise ArchetypeIndexError(f"{path}: typ source list exceeds {MAX_SOURCES_BYTES} bytes")
        raw = path.read_bytes()
        doc = json.loads(raw)
    except (OSError, json.JSONDecodeError) as error:
        raise ArchetypeIndexError(f"cannot read typ source list {path}: {error}") from None
    if not isinstance(doc, dict) or doc.get("kind") != SOURCES_KIND or doc.get("schemaVersion") != SOURCES_VERSION:
        raise ArchetypeIndexError(f"{path}: not a schema {SOURCES_VERSION} {SOURCES_KIND} list")
    rows: list[TypSource] = []
    try:
        for group in doc["archives"]:
            archive, size = _relative(group["archive"], "archive"), int(group["bytes"])
            for member, offset, stored, digest in group["typs"]:
                row = TypSource(archive, size, _relative(member, "member"), int(offset), int(stored), str(digest))
                if not member.lower().endswith(".ptyp") or not 16 < row.stored_bytes <= MAX_STORED:
                    raise ArchetypeIndexError(f"{row.path}: not a .ptyp member within the size budget")
                if row.stored_offset < 0 or row.stored_offset + row.stored_bytes > size:
                    raise ArchetypeIndexError(f"{row.path}: stored bytes run past the archive end")
                if not _SHA256.fullmatch(row.sha256):
                    raise ArchetypeIndexError(f"{row.path}: sha256 must be 64 lowercase hex digits")
                rows.append(row)
        names = [str(n) for n in doc["names"]]
    except (KeyError, TypeError, ValueError) as error:
        if isinstance(error, ArchetypeIndexError):
            raise
        raise ArchetypeIndexError(f"{path}: malformed typ source list: {error}") from None
    bad = [n for n in names if not _NAME.fullmatch(n)]
    if bad:
        raise ArchetypeIndexError(f"{path}: unusable archetype names {bad[:4]}")
    if not rows or len({r.path for r in rows}) != len(rows):
        raise ArchetypeIndexError(f"{path}: needs at least one typ and unique member paths")
    return doc, rows, names, hashlib.sha256(raw).hexdigest()


def sources_document(target: str, title: str, rows: list[TypSource], names: Iterable[str], note: str) -> dict:
    """A typ source list (rows in walk order, grouped by archive in that order)."""
    groups: list[dict] = []
    for row in rows:
        if not groups or groups[-1]["archive"] != row.archive:
            groups.append({"archive": row.archive, "bytes": row.archive_bytes, "typs": []})
        groups[-1]["typs"].append([row.member, row.stored_offset, row.stored_bytes, row.sha256])
    return {
        "kind": SOURCES_KIND,
        "schemaVersion": SOURCES_VERSION,
        "target": target,
        "titleId": title,
        "consoleSource": f"ftp://<PS5_HOST>:2121/mnt/sandbox/{title}_000/app0",
        "note": note,
        "archives": groups,
        "names": sorted(set(names)),
    }


def dump_sources(document: dict) -> str:
    """One typ row and one name per line, so a re-derived list diffs line by line."""
    head = {k: v for k, v in document.items() if k not in ("archives", "names")}
    lines = ["{"] + [f"  {json.dumps(k)}: {json.dumps(v)}," for k, v in head.items()] + ['  "archives": [']
    for g, group in enumerate(document["archives"]):
        lines.append(f'    {{"archive": {json.dumps(group["archive"])}, "bytes": {group["bytes"]}, "typs": [')
        typs = group["typs"]
        lines += [f"      {json.dumps(t)}{',' if i + 1 < len(typs) else ''}" for i, t in enumerate(typs)]
        lines.append(f"    ]}}{',' if g + 1 < len(document['archives']) else ''}")
    lines.append("  ],")
    names = document["names"]
    lines.append('  "names": [')
    lines += [f"    {json.dumps(n)}{',' if i + 1 < len(names) else ''}" for i, n in enumerate(names)]
    lines += ["  ]", "}"]
    return "\n".join(lines) + "\n"


# --- reading the typs ---------------------------------------------------------------------------

# (archive, offset, count) -> bytes, the retail_templates Source.read shape.
RangeRead = Callable[[str, int, int], bytes]


def spans(rows: list[TypSource], gap: int, limit: int) -> list[tuple[str, int, int, list[int]]]:
    """Coalesced reads (archive, offset, count, row indices): neighbours closer than `gap` bytes share one
    read of at most `limit` bytes (a remote source pays per request, not per byte)."""
    order = sorted(range(len(rows)), key=lambda i: (rows[i].archive, rows[i].stored_offset))
    out: list[tuple[str, int, int, list[int]]] = []
    for i in order:
        row = rows[i]
        end = row.stored_offset + row.stored_bytes
        if out:
            archive, start, count, members = out[-1]
            if archive == row.archive and row.stored_offset - (start + count) <= gap and end - start <= limit:
                out[-1] = (archive, start, max(count, end - start), [*members, i])
                continue
        out.append((row.archive, row.stored_offset, row.stored_bytes, [i]))
    return out


def read_typs(
    rows: list[TypSource],
    read: RangeRead,
    gap: int = 64 << 10,
    limit: int = 32 << 20,
    progress: Callable[[int, int], None] | None = None,
) -> list[list[tuple]]:
    """`archetypes_of` rows of every typ (by row index), each typ verified against its pinned sha256."""
    parsed: list[list[tuple]] = [[] for _ in rows]
    plan = spans(rows, gap, limit)
    for n, (archive, start, count, members) in enumerate(plan):
        blob = read(archive, start, count)
        if len(blob) != count:
            raise ArchetypeIndexError(f"{archive}: short read at {start:#x}")
        for i in members:
            row = rows[i]
            stored = blob[row.stored_offset - start : row.stored_offset - start + row.stored_bytes]
            if hashlib.sha256(stored).hexdigest() != row.sha256:
                raise ArchetypeIndexError(
                    f"{row.path}: stored bytes differ from the typ source list (a different game build?)"
                )
            try:
                parsed[i] = archetypes_of(payload_of(stored))
            except (ValueError, struct.error, zlib.error) as error:
                raise ArchetypeIndexError(f"{row.path}: {error}") from None
        if progress:
            progress(n + 1, len(plan))
    return parsed


def build(rows: list[TypSource], names: list[str], parsed: list[list[tuple]], source: str, **extra) -> dict:
    """The index document: typs added in the list's walk order (`parsed` from `read_typs`)."""
    builder = IndexBuilder()
    for row, typ_rows in zip(rows, parsed, strict=True):
        builder.add_rows(row.path, typ_rows)
    return builder.document(source, names, **extra)

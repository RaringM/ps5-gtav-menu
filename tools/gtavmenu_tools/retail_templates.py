"""Retail templates the vehicle converter reads, fetched from the user's own game image.

The template manifest (data/retail_templates/<target>.json) names every retail member the PC
vehicle converter needs: the archive inside the game image (app0-relative), the member path inside
it, where its stored bytes start in that archive file, the stored size and table flags, and the
sha256/size of the file the converter reads. No retail bytes are in the repository.

Retail archive tables are AES-encrypted with keys from the game executable, but the members are
not: a resource is a 16-byte prefix plus raw deflate, an uncompressed binary member is the file
itself. The manifest pins each member's absolute stored offset, so for a pinned game build every
resource and every plain binary member is fetched by one range read with no key at all, rewrapped
(resources: a loose RSC7 header from the pinned flags) and checked against the pinned sha256. Only
encrypted binary members (retail .meta files) need the game's keys; this module never reads keys,
a caller may pass a `MemberCipher` (a developer hook, like build_runtime_pack's table keys).

Encrypted members can instead come from the menu itself: its EXPORT_RETAIL_FILE action (`menu-ctl.sh
export-templates`) reads each one through the running game's file system, which decrypts and
inflates it, and writes the plaintext to <game custom root>/exports/<member basename>. `Exports`
reads that directory (FTP or a local copy) and the file is checked against the same pinned sha256.

Sources: file:///path/to/app0 (a local copy of the game files) or ftp://host:port/mnt/sandbox/
<TITLE>_000/app0 (the console's FTP server while the game runs; the game image is only mounted
then). Range reads over FTP use curl, like tools/upload_runtime_pack.py.

`walk` reads an archive's table through a range reader (OPEN tables key-free, keyed tables through a
caller's table cipher); a developer tool uses it to derive the manifest, the tests with synthetic
archives.
"""

from __future__ import annotations

import hashlib
import json
import os
import struct
import subprocess
import tempfile
import urllib.parse
import zlib
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from gtavmenu_tools import rpf7

KIND = "gtavmenu-retail-templates"
SCHEMA_VERSION = 1
TRANSFORMS = ("rsc7", "copy", "decrypt-inflate")
DEFAULT_TARGET = "ppsa04264-01.010.002"
# Console directory the menu's EXPORT_RETAIL_FILE action writes (src/module/features/
# custom_retail_export.inc); one file per encrypted template, named by the member's basename.
EXPORT_ROOT = "/data/gtavmenu/custom/exports"
MANIFEST_DIR = Path(__file__).resolve().parents[2] / "data/retail_templates"
MAX_MEMBER_BYTES = 1 << 30

# (read offset, byte count) -> bytes, within one archive file of the source.
RangeRead = Callable[[str, int, int], bytes]
# (member basename, unpacked size, stored bytes) -> decrypted stored bytes (developer hook).
MemberCipher = Callable[[str, int, bytes], bytes]
# (table bytes, archive basename, archive size) -> decrypted table (developer hook, rpf7.TableCipher).
TableCipher = rpf7.TableCipher


class TemplateError(RuntimeError):
    """A template could not be fetched, does not verify, or the manifest is malformed."""


@dataclass(frozen=True)
class Template:
    cache: str  # path under the cache directory, the build/assets-relative name the converter reads
    archive: str  # app0-relative archive file
    archive_bytes: int
    member: str  # path inside the archive, nested archives as path components
    transform: str
    stored_offset: int  # absolute offset of the stored bytes in the archive file
    stored_bytes: int
    flags: tuple[int, int]  # resource: system/graphics flags; binary: unpacked size/encryption word
    sha256: str  # of the file written to the cache
    size: int
    required: str  # "always" or the condition under which the converter reads it
    used_by: str

    @property
    def needs_keys(self) -> bool:
        return self.transform == "decrypt-inflate"

    @property
    def export_name(self) -> str:
        """File name of this member in the menu's export directory (encrypted members only): the cache
        file's name, unique per row (update.rpf's and common.rpf's vehiclelayouts.meta share a member name)."""
        return PurePosixPath(self.cache).name


def exportable(templates: list[Template]) -> list[Template]:
    """The templates the menu exports, in manifest order: the EXPORT_RETAIL_FILE row index of each."""
    return [t for t in templates if t.needs_keys]


def pin_target(target: str) -> str:
    """The reviewed resource pins to check; this does not assert regional image identity."""
    return "ppsa04264-01.010.002" if target == "ppsa04263-01.010.002" else target


def manifest_path(target: str = DEFAULT_TARGET) -> Path:
    return MANIFEST_DIR / f"{pin_target(target)}.json"


def _safe_relative(path: str, label: str) -> str:
    pure = PurePosixPath(path)
    if not path or pure.is_absolute() or ".." in pure.parts or pure.as_posix() != path:
        raise TemplateError(f"{label}: unsafe path {path!r}")
    return path


def load_manifest(path: Path | None = None) -> tuple[dict, list[Template]]:
    """The manifest document and its validated template rows."""
    path = path or manifest_path()
    try:
        doc = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise TemplateError(f"cannot read template manifest {path}: {error}") from None
    if doc.get("kind") != KIND or doc.get("schemaVersion") != SCHEMA_VERSION:
        raise TemplateError(f"{path}: not a schema {SCHEMA_VERSION} {KIND} manifest")
    rows = []
    for row in doc.get("templates", []):
        try:
            template = Template(
                cache=_safe_relative(row["cache"], "cache"),
                archive=_safe_relative(row["archive"], "archive"),
                archive_bytes=int(row["archiveBytes"]),
                member=_safe_relative(row["member"], "member"),
                transform=row["transform"],
                stored_offset=int(row["storedOffset"]),
                stored_bytes=int(row["storedBytes"]),
                flags=(int(row["flags"][0], 0), int(row["flags"][1], 0)),
                sha256=row["sha256"],
                size=int(row["bytes"]),
                required=row["required"],
                used_by=row["usedBy"],
            )
        except (KeyError, TypeError, ValueError, IndexError) as error:
            raise TemplateError(f"{path}: malformed template row {row!r}: {error}") from None
        if template.transform not in TRANSFORMS:
            raise TemplateError(f"{template.cache}: unknown transform {template.transform!r}")
        if not (0 < template.stored_bytes <= MAX_MEMBER_BYTES and 0 < template.size <= MAX_MEMBER_BYTES):
            raise TemplateError(f"{template.cache}: size outside the fetch budget")
        if template.stored_offset < 0 or template.stored_offset + template.stored_bytes > template.archive_bytes:
            raise TemplateError(f"{template.cache}: stored bytes run past the archive end")
        if len(template.sha256) != 64 or template.sha256.strip("0123456789abcdef"):
            raise TemplateError(f"{template.cache}: sha256 must be 64 lowercase hex digits")
        rows.append(template)
    caches = [t.cache for t in rows]
    if not rows or len(set(caches)) != len(caches):
        raise TemplateError(f"{path}: needs at least one template and unique cache names")
    return doc, rows


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_path(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


# --- sources ---------------------------------------------------------------------------------


@dataclass
class Source:
    """A game image root: `read(archive, offset, count)` and `size(archive)` per archive file."""

    url: str
    read: RangeRead
    size: Callable[[str], int | None]  # None: the archive is absent (game image not mounted)


def _file_source(root: Path, url: str) -> Source:
    def path(archive: str) -> Path:
        return root / _safe_relative(archive, "archive")

    def read(archive: str, offset: int, count: int) -> bytes:
        with path(archive).open("rb") as stream:
            stream.seek(offset)
            data = stream.read(count)
        if len(data) != count:
            raise TemplateError(f"{archive}: short read at {offset:#x} ({len(data)} of {count} bytes)")
        return data

    def size(archive: str) -> int | None:
        candidate = path(archive)
        return candidate.stat().st_size if candidate.is_file() else None

    return Source(url, read, size)


Curl = Callable[[list[str]], bytes]


def run_curl(args: list[str]) -> bytes:
    """One quiet, failing curl transfer (stdout); raises TemplateError with curl's exit code."""
    result = subprocess.run(["curl", "-s", "--fail", *args], capture_output=True, timeout=900, check=False)
    if result.returncode:
        raise TemplateError(f"curl {' '.join(args[-1:])} failed (exit {result.returncode})")
    return result.stdout


def _ftp_source(url: str, curl: Curl) -> Source:
    base = url.rstrip("/")

    def where(archive: str) -> str:
        return f"{base}/{urllib.parse.quote(_safe_relative(archive, 'archive'))}"

    def read(archive: str, offset: int, count: int) -> bytes:
        data = curl(["--max-time", "900", "-r", f"{offset}-{offset + count - 1}", where(archive)])
        if len(data) != count:
            raise TemplateError(f"{archive}: short FTP read at {offset:#x} ({len(data)} of {count} bytes)")
        return data

    def size(archive: str) -> int | None:
        try:
            head = curl(["--max-time", "30", "-I", where(archive)]).decode("ascii", "replace")
        except TemplateError:
            return None
        for line in head.splitlines():
            key, _, value = line.partition(":")
            if key.strip().lower() == "content-length":
                return int(value.strip())
        return None

    return Source(url, read, size)


@dataclass
class Exports:
    """The menu's export directory: `read(name)` is the whole file, or None when it is absent."""

    url: str
    read: Callable[[str], bytes | None]


def open_exports(url: str, curl: Curl = run_curl) -> Exports:
    """file:///path/to/exports or ftp://host:port/data/gtavmenu/custom/exports (bare path = file)."""
    parsed = urllib.parse.urlsplit(url)

    def checked(name: str) -> str:
        if PurePosixPath(_safe_relative(name, "export")).name != name:
            raise TemplateError(f"export {name!r}: not a plain file name")
        return name

    if parsed.scheme in ("", "file"):
        root = Path(urllib.parse.unquote(parsed.path if parsed.scheme else url))

        def read_file(name: str) -> bytes | None:
            path = root / checked(name)
            return path.read_bytes() if path.is_file() else None

        return Exports(url, read_file)
    if parsed.scheme == "ftp":
        base = url.rstrip("/")

        def read_ftp(name: str) -> bytes | None:
            where = f"{base}/{urllib.parse.quote(checked(name))}"
            try:
                return curl(["--max-time", "300", where])
            except TemplateError:
                return None  # absent (or unreadable): reported as not exported

        return Exports(url, read_ftp)
    raise TemplateError(f"{url}: exports must be file:///path/to/exports or ftp://host:port{EXPORT_ROOT}")


def open_source(url: str, curl: Curl = run_curl) -> Source:
    """file:///path/to/app0 or ftp://host:port/path/to/app0 (a bare path counts as file://)."""
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme in ("", "file"):
        root = Path(urllib.parse.unquote(parsed.path if parsed.scheme else url))
        if not root.is_dir():
            raise TemplateError(f"{url}: not a directory (expected the game's app0 root)")
        return _file_source(root, url)
    if parsed.scheme == "ftp":
        return _ftp_source(url, curl)
    raise TemplateError(f"{url}: source must be file:///path/to/app0 or ftp://host:port/path/to/app0")


# --- members ---------------------------------------------------------------------------------


def rsc7(stored: bytes, sys_flags: int, gfx_flags: int, label: str) -> bytes:
    """Loose RSC7 file of an archive-stored resource: a header from the table flags + the deflate."""
    rpf7.inflate_resource(stored, label)  # the stream must inflate to its end from offset 16
    version = ((sys_flags >> 28) << 4) | (gfx_flags >> 28)
    return struct.pack("<4sIII", b"RSC7", version, sys_flags, gfx_flags) + stored[16:]


def extract(template: Template, stored: bytes, member_cipher: MemberCipher | None = None) -> bytes:
    """The cache file of one template from its stored member bytes."""
    if len(stored) != template.stored_bytes:
        raise TemplateError(f"{template.cache}: stored member is {len(stored)} bytes, not {template.stored_bytes}")
    if template.transform == "rsc7":
        return rsc7(stored, template.flags[0], template.flags[1], template.cache)
    if template.transform == "copy":
        return stored
    if member_cipher is None:
        raise TemplateError(
            f"{template.cache}: {template.member} is an encrypted archive member; decrypting it needs the "
            "game's keys, which this tool does not read"
        )
    unpacked = template.flags[0]
    decrypted = member_cipher(PurePosixPath(template.member).name, unpacked, stored)
    stream = zlib.decompressobj(-15)
    try:
        data = stream.decompress(decrypted, unpacked + 1)
    except zlib.error as error:
        raise TemplateError(f"{template.cache}: decrypted member does not inflate ({error})") from None
    if len(data) != unpacked or not stream.eof:
        raise TemplateError(f"{template.cache}: decrypted member does not inflate to {unpacked} bytes")
    return data


@dataclass
class Outcome:
    template: Template
    status: str  # cached, fetched, skipped, missing, failed
    detail: str = ""


def cached_ok(cache: Path, template: Template) -> bool:
    path = cache / template.cache
    return path.is_file() and path.stat().st_size == template.size and sha256_path(path) == template.sha256


def verified_path(cache: Path, name: str, manifest: Path | None = None) -> Path:
    """Resolve one named converter input only after checking its reviewed size and SHA-256.

    Explicit user-supplied files are a separate converter interface; a cache path must never
    silently fall back to unpinned bytes or an old recipe's private retail copy.
    """
    _, templates = load_manifest(manifest)
    found = [template for template in templates if template.cache == name]
    if len(found) != 1:
        raise TemplateError(f"{name}: no reviewed template row; update the template manifest")
    template = found[0]
    if not cached_ok(cache, template):
        route = "export-templates / fetch-templates" if template.needs_keys else "fetch-templates"
        raise TemplateError(f"{cache / name}: missing or differs from the pinned template; run {route}")
    return cache / name


def _write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
        os.replace(temp, path)
    except BaseException:
        Path(temp).unlink(missing_ok=True)
        raise


def fetch(
    templates: list[Template],
    source: Source | None,
    cache: Path,
    member_cipher: MemberCipher | None = None,
    log: Callable[[str], None] = print,
    exports: Exports | None = None,
    refresh_exports: bool = False,
) -> list[Outcome]:
    """Bring every template into `cache`, verified. Idempotent: verified cache files are kept.

    `source` and `exports` None only check the cache. An encrypted member comes from `exports`
    (the menu's plaintext export) when given, else through `member_cipher`; without either it is
    skipped (reported), not an error: the converter needs it only for mods that ship
    vehiclelayouts.meta. `refresh_exports` verifies the actual exported bytes even with a valid
    cache; a failed export leaves that existing cache untouched.
    """
    outcomes = []
    sizes: dict[str, int | None] = {}
    for template in templates:
        if cached_ok(cache, template) and not (refresh_exports and template.needs_keys and exports is not None):
            outcomes.append(Outcome(template, "cached"))
            continue
        if template.needs_keys and member_cipher is None and exports is not None:
            outcomes.append(_from_export(template, exports, cache, log))
            continue
        if source is None:
            outcomes.append(Outcome(template, "missing", "not in the cache"))
            continue
        if template.needs_keys and member_cipher is None:
            outcomes.append(Outcome(template, "skipped", "encrypted member; needs the game's keys"))
            continue
        if template.archive not in sizes:
            sizes[template.archive] = source.size(template.archive)
        size = sizes[template.archive]
        if size is None:
            outcomes.append(Outcome(template, "failed", f"{template.archive} not found in {source.url}"))
            continue
        if size != template.archive_bytes:
            outcomes.append(
                Outcome(
                    template,
                    "failed",
                    f"{template.archive} is {size} bytes, the manifest pins {template.archive_bytes} "
                    "(a different game build?)",
                )
            )
            continue
        try:
            log(f"fetch {template.cache} <- {template.archive}:{template.member}")
            data = extract(template, source.read(template.archive, template.stored_offset, template.stored_bytes),
                           member_cipher)  # fmt: skip
        except (TemplateError, rpf7.Rpf7Error, OSError) as error:
            outcomes.append(Outcome(template, "failed", str(error)))
            continue
        if len(data) != template.size or sha256_bytes(data) != template.sha256:
            outcomes.append(Outcome(template, "failed", f"sha256 {sha256_bytes(data)} differs from the manifest"))
            continue
        _write_atomic(cache / template.cache, data)
        outcomes.append(Outcome(template, "fetched"))
    return outcomes


def _from_export(template: Template, exports: Exports, cache: Path, log: Callable[[str], None]) -> Outcome:
    """One encrypted template from the menu's export directory, verified like a fetched one."""
    try:
        data = exports.read(template.export_name)
    except (TemplateError, OSError) as error:
        return Outcome(template, "failed", str(error))
    if data is None:
        return Outcome(
            template,
            "missing",
            f"{template.export_name} not exported yet (./menu-ctl.sh export-templates, GTA V running)",
        )
    if len(data) != template.size or sha256_bytes(data) != template.sha256:
        return Outcome(
            template,
            "failed",
            f"exported {template.export_name} ({len(data)} bytes, sha256 {sha256_bytes(data)}) differs from the "
            "manifest",
        )
    log(f"fetch {template.cache} <- {exports.url.rstrip('/')}/{template.export_name}")
    _write_atomic(cache / template.cache, data)
    return Outcome(template, "fetched", "from the menu export")


# --- table walk (manifest derivation, tests) ----------------------------------------------------


def read_entries(read: Callable[[int, int], bytes], base: int, size: int, name: str,
                 decrypt: TableCipher | None) -> list[dict]:  # fmt: skip
    """Table entries of the archive at `base` (rpf7.read_table's rows, offsets archive-relative)."""
    magic, count, names_bytes, tag = struct.unpack("<4I", read(base, 16))
    if magic != rpf7.MAGIC or tag not in (rpf7.TAG_KEYED, rpf7.TAG_OPEN) or count < 1 or count > 1_000_000:
        raise TemplateError(f"{name}: unexpected RPF header")
    if names_bytes >> 28 or 16 + count * 16 + names_bytes > size:
        raise TemplateError(f"{name}: table runs past the end of the archive")
    table = read(base + 16, count * 16 + names_bytes)
    if tag == rpf7.TAG_KEYED:
        if decrypt is None:
            raise TemplateError(f"{name}: keyed table needs the game's table keys (not available here)")
        table = decrypt(table, name, size)
    names = table[count * 16 :]
    entries = []
    for index in range(count):
        q, a, b = struct.unpack_from("<QII", table, index * 16)
        end = names.find(b"\0", q & 0xFFFF)
        if end < 0:
            raise TemplateError(f"{name}: entry {index} name is not terminated")
        entry_name = names[q & 0xFFFF : end].decode("ascii")
        if q >> 32 == rpf7.DIRECTORY:
            entries.append({"kind": "dir", "name": entry_name, "first": a, "count": b})
            continue
        offset, stored = (q >> 40) & 0xFFFFFF, (q >> 16) & 0xFFFFFF
        kind = "res" if offset & 0x800000 else "bin"
        offset = (offset & 0x7FFFFF) * rpf7.BLOCK
        if kind == "res" and stored == rpf7.OVERSIZE:
            stored = rpf7.prefix_size(read(base + offset, 16))
        elif kind == "bin" and stored == 0:
            stored = a  # stored uncompressed: the size field is 0, `a` is the size
        entries.append({"kind": kind, "name": entry_name, "size": stored, "offset": offset, "a": a, "b": b})
    return entries


def walk(
    read: Callable[[int, int], bytes],
    size: int,
    name: str,
    decrypt: TableCipher | None = None,
    base: int = 0,
    prefix: str = "",
) -> dict[str, dict]:
    """Every file of an archive by path (nested archives descended), with absolute stored offsets.

    `read(offset, count)` reads the outer archive file; this archive starts at `base`. Rows: {kind,
    size (stored bytes), offset (absolute), a, b}. A nested archive (an uncompressed .rpf member) is
    listed and descended; its own table key depends on its name and size, as for a top-level one.
    """
    entries = read_entries(read, base, size, name, decrypt)
    out: dict[str, dict] = {}

    def rec(index: int, path: str) -> None:
        entry = entries[index]
        if entry["kind"] == "dir":
            if entry["first"] + entry["count"] > len(entries):
                raise TemplateError(f"{name}: directory {entry['name']!r} children out of range")
            for child in range(entry["first"], entry["first"] + entry["count"]):
                if child <= index:
                    raise TemplateError(f"{name}: directory {entry['name']!r} is not a tree")
                rec(child, f"{path}{entry['name']}/" if index else path)
            return
        full = prefix + path + entry["name"]
        if entry["offset"] + entry["size"] > size:
            raise TemplateError(f"{full}: member runs past the end of its archive")
        out[full] = {k: entry[k] for k in ("kind", "size", "a", "b")} | {"offset": base + entry["offset"]}
        if entry["kind"] == "bin" and entry["name"].endswith(".rpf") and entry["size"] == entry["a"]:
            out.update(walk(read, entry["a"], entry["name"], decrypt, base + entry["offset"], full + "/"))

    rec(0, "")
    return out


def locate(
    read: Callable[[int, int], bytes], size: int, name: str, path: str, decrypt: TableCipher | None = None
) -> dict:
    """One member by path ('dir/nested.rpf/member'), reading only the tables along the path.

    Returns the walk row of the member: {kind, size, a, b, offset (absolute in the outer file)}.
    """
    base, entries, index = 0, read_entries(read, 0, size, name, decrypt), 0
    parts = path.split("/")
    for depth, part in enumerate(parts):
        node = entries[index]
        match = next(
            (i for i in range(node["first"], min(node["first"] + node["count"], len(entries)))
             if entries[i]["name"] == part),
            None,
        )  # fmt: skip
        if match is None:
            raise TemplateError(f"{name}: {'/'.join(parts[: depth + 1])} not found")
        entry = entries[match]
        if entry["kind"] == "dir":
            index = match
            continue
        if depth == len(parts) - 1:
            return {k: entry[k] for k in ("kind", "size", "a", "b")} | {"offset": base + entry["offset"]}
        if entry["kind"] != "bin" or not part.endswith(".rpf"):
            raise TemplateError(f"{name}: {part} is not a nested archive")
        base, name, size = base + entry["offset"], part, entry["a"]
        entries, index = read_entries(read, base, size, name, decrypt), 0
    raise TemplateError(f"{path} names a directory")

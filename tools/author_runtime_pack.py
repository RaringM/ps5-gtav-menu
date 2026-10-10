#!/usr/bin/env python3
"""Author small runtime packs from caller inputs, using the existing builder and validator.

  author_runtime_pack.py labels labels.txt --id my-labels-v1 --carrier filler.ptd
  author_runtime_pack.py timecycle modifiers.xml --like BlackOut --name gm_dark --id dark-v1
  author_runtime_pack.py ptfx fireworks.ppt --name gm_fireworks --effect scr_indep_firework_fountain --id sparks-v1

Labels are KEY=TEXT lines (blank lines and lines starting with # are ignored). Labels/timecycle
need an archive member: --carrier supplies an existing PS5 PTD, otherwise the verified template
cache's corpus/tornado6.ptd is used under a new member name. Omitted timecycle/ptfx inputs use
verified retail cache entries. Explicit inputs remain supported. No retail bytes are distributed.

Timecycle copies one modifier under a new name. Particle authoring copies an existing PS5 .ppt
under a new dictionary name and declares the caller's existing effect name; it does not verify
that effect exists or convert PC .ypt files. All outputs are built and validated in private
staging before a new pack directory is created. Existing outputs are refused, never removed.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import math
import re
import shutil
import stat
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path
from xml.parsers import expat

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import build_runtime_pack as builder  # noqa: E402
import validate_runtime_pack as validator  # noqa: E402
from gtavmenu_tools import retail_templates, runtime_pack  # noqa: E402
from gtavmenu_tools.asset_formats import AssetError, Limits, decode_resource  # noqa: E402
from gtavmenu_tools.asset_metadata import parse_xml  # noqa: E402
from gtavmenu_tools.hashes import joaat  # noqa: E402
from gtavmenu_tools.host_paths import build_dir  # noqa: E402

TC_TEMPLATE = "retail-metas/timecycle_mods_4.xml"
PTFX_TEMPLATE = "ptfx/scr_indep_fireworks.ppt"
CARRIER_TEMPLATE = "corpus/tornado6.ptd"
LABEL_INPUT_MAX = 64 * 1024
XML_LIMITS = Limits(max_metadata_bytes=runtime_pack.DATA_BYTES_MAX, max_xml_nodes=100000, max_xml_depth=16)
_NAME = re.compile(r"[a-z0-9_]{1,63}")
_KEY = re.compile(r"[A-Za-z0-9_]{1,63}")
_TEXT = re.compile(r"[\x20-\x7d]+")
_RESERVED = {"null", "none", "default", "core"}


class AuthorError(ValueError):
    """An unsupported input, collision, or unsafe output was refused."""


def _new_name(name: str) -> None:
    if not _NAME.fullmatch(name) or name in _RESERVED or not joaat(name):
        raise AuthorError("new name must be 1..63 lowercase [a-z0-9_] characters and not null, none, default or core")


def _filename(name: str, extension: str) -> str:
    if (
        not name.endswith(extension)
        or len(name) > runtime_pack.NAME_MAX
        or not _NAME.fullmatch(name[: -len(extension)])
    ):
        raise AuthorError(f"invalid {extension} filename {name!r}")
    return name


def _no_links(path: Path) -> None:
    for part in (path.absolute(), *path.absolute().parents):
        if part.is_symlink():
            raise AuthorError(f"symbolic links are refused: {part}")


def read_input(path: Path, maximum: int) -> bytes:
    _no_links(path)
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= maximum:
        raise AuthorError(f"{path}: input must be a regular file of 1..{maximum} bytes")
    with path.open("rb") as stream:
        data = stream.read(maximum + 1)
    if len(data) != info.st_size or len(data) > maximum:
        raise AuthorError(f"{path}: input changed size during the bounded read")
    return data


def labels_from_text(data: bytes) -> list[tuple[str, str]]:
    if len(data) > LABEL_INPUT_MAX:
        raise AuthorError("label input exceeds 64 KiB")
    try:
        text = data.decode("ascii")
    except UnicodeError:
        raise AuthorError("labels must be printable ASCII") from None
    if any(ord(char) < 32 and char not in "\t\r\n" for char in text):
        raise AuthorError("label input contains unsupported control characters")
    labels, hashes = [], set()
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        key, separator, value = line.partition("=")
        key = key.strip()
        if not separator or not _KEY.fullmatch(key) or not _TEXT.fullmatch(value) or len(value) > runtime_pack.NAME_MAX:
            raise AuthorError(
                f"label line {number}: expected KEY=TEXT, each 1..63 characters, printable text without '~'"
            )
        hashed = joaat(key)
        if not hashed or hashed in hashes:
            raise AuthorError(f"label line {number}: duplicate or zero label hash for {key!r}")
        hashes.add(hashed)
        labels.append((key, value))
        if len(labels) > runtime_pack.LABEL_MAX:
            raise AuthorError(f"at most {runtime_pack.LABEL_MAX} labels fit one pack")
    if not labels:
        raise AuthorError("label input contains no KEY=TEXT rows")
    return labels


def _modifier_spans(data: bytes) -> list[tuple[int, int]]:
    """Byte spans of direct modifier children, from an XML parser rather than a regex over XML."""
    parser = expat.ParserCreate()
    depth, start = 0, 0
    spans = []

    def opened(tag, _attrs):
        nonlocal depth, start
        depth += 1
        if depth == 2 and tag == "modifier":
            start = parser.CurrentByteIndex

    def closed(tag):
        nonlocal depth
        if depth == 2 and tag == "modifier":
            end = parser.CurrentByteIndex
            if data[end : end + 2] == b"</":
                end = data.index(b">", end) + 1
            spans.append((start, end))
        depth -= 1

    parser.StartElementHandler, parser.EndElementHandler = opened, closed
    parser.Parse(data, True)
    return spans


def clone_timecycle(data: bytes, like: str, name: str) -> bytes:
    _new_name(name)
    if len(data) > XML_LIMITS.max_metadata_bytes:
        raise AuthorError("timecycle input exceeds the metadata size limit")
    # The native name gate scans plain ASCII XML. Decode before DTD checks so alternate encodings
    # cannot hide declarations; UTF-16 and escaped/non-ASCII names are deliberately unsupported.
    try:
        text = data.decode("utf-8-sig").replace("\r\n", "\n").replace("\r", "\n")
        data = text.encode("ascii")
    except UnicodeError:
        raise AuthorError("timecycle input must be ASCII-compatible UTF-8 XML") from None
    if any(byte < 32 and byte not in (9, 10, 13) for byte in data):
        raise AuthorError("timecycle XML contains unsupported control characters or encoding")
    root = parse_xml(data, XML_LIMITS)
    if root.tag != "timecycle_modifier_data" or root.attrib != {"version": "1.000000"}:
        raise AuthorError('expected <timecycle_modifier_data version="1.000000">')
    rows, names, selected = list(root), set(), None
    for index, row in enumerate(rows):
        source_name = row.get("name", "")
        if row.tag != "modifier" or not _KEY.fullmatch(source_name) or not joaat(source_name):
            raise AuthorError("timecycle source must contain named modifier children only")
        hashed = joaat(source_name)
        if hashed in names:
            raise AuthorError(f"duplicate timecycle modifier name/hash: {source_name}")
        names.add(hashed)
        if source_name.lower() == like.lower():
            selected = index
    if selected is None:
        raise AuthorError(f"timecycle modifier {like!r} was not found")
    if joaat(name) in names:
        raise AuthorError(f"new timecycle name {name!r} collides with a source modifier")
    row = rows[selected]
    if set(row.attrib) != {"name", "numMods", "userFlags"}:
        raise AuthorError("selected modifier needs exactly name, numMods and userFlags attributes")
    if not re.fullmatch(r"[0-9]{1,6}", row.get("numMods", "")) or int(row.get("numMods")) != len(row) or not len(row):
        raise AuthorError("selected modifier numMods must equal its nonempty variable list")
    if not re.fullmatch(r"[0-9]{1,10}", row.get("userFlags", "")) or int(row.get("userFlags")) > 0xFFFFFFFF:
        raise AuthorError("selected modifier userFlags must be a uint32")
    variables = set()
    if (row.text or "").strip():
        raise AuthorError("selected modifier has unexpected text outside its variables")
    for variable in row:
        if not _KEY.fullmatch(variable.tag) or variable.attrib or len(variable) or variable.tag in variables:
            raise AuthorError("selected modifier variables must be unique plain scalar elements")
        values = (variable.text or "").split()
        try:
            valid = len(values) == 2 and all(
                math.isfinite(float(v)) and abs(float(v)) <= 3.402823466e38 for v in values
            )
        except ValueError:
            valid = False
        if not valid:
            raise AuthorError(f"{variable.tag}: expected two finite float32 values")
        if (variable.tail or "").strip():
            raise AuthorError("selected modifier has unexpected text outside its variables")
        variables.add(variable.tag)
    start, end = _modifier_spans(data)[selected]
    block = data[start:end]
    if any(token in block for token in (b"<!--", b"<![", b"<?")):
        raise AuthorError("selected modifier comments, CDATA and processing instructions are unsupported")
    opening_end = block.index(b">") + 1
    canonical = f'<modifier name="{name}" numMods="{row.get("numMods")}" userFlags="{row.get("userFlags")}">'.encode()
    old = f'<modifier name="{row.get("name")}"'.encode()
    if block.startswith(old):
        # Preserve the accepted retail block byte-for-byte except its first name attribute.
        block = f'<modifier name="{name}"'.encode() + block[len(old) :]
    else:
        block = canonical + block[opening_end:]
    line_start = data.rfind(b"\n", 0, start) + 1
    indent = data[line_start:start]
    if indent.strip():
        indent = b"  "
    return (
        b'<?xml version="1.0" encoding="UTF-8"?>\n\n<timecycle_modifier_data version="1.000000">\n'
        + indent
        + block
        + b"\n</timecycle_modifier_data>\n"
    )


def resource_input(path: Path, extension: str, version: int) -> bytes:
    if path.suffix.lower() != extension:
        raise AuthorError(f"{path}: expected an existing PS5 {extension} resource; no PC conversion is performed")
    data = read_input(path, runtime_pack.ARCHIVE_BYTES_MAX)
    header, _ = decode_resource(data, Limits().max_file_bytes)
    if header["version"] != version:
        raise AuthorError(f"{path}: expected PS5 {extension} resource version {version:#x}")
    return data


def _source(args, explicit: Path | None, template: str) -> Path:
    return explicit if explicit is not None else retail_templates.verified_path(args.templates, template)


def build(args) -> Path:
    if not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?", args.id):
        raise AuthorError("pack id must be 1..64 lowercase letters/digits/hyphens with no edge hyphen")
    target = args.output_root / args.id
    _no_links(target)
    if target.exists():
        raise AuthorError(f"refusing existing output {target}")
    slug = "gm_" + args.id.replace("-", "_")[:40]
    archive = _filename(args.archive or f"{slug}.rpf", ".rpf")
    options, files = [], {}
    if args.command == "labels":
        labels = labels_from_text(read_input(args.input, LABEL_INPUT_MAX))
        for key, value in labels:
            options += ["--label", f"{key}={value}"]
    elif args.command == "timecycle":
        source = _source(args, args.input, TC_TEMPLATE)
        meta = _filename(args.meta or f"{slug}_tc.meta", ".meta")
        files[meta] = clone_timecycle(read_input(source, runtime_pack.DATA_BYTES_MAX), args.like, args.name)
        options += ["--data", f"TIMECYCLEMOD_FILE={meta}", "--spawn", f"timecycle:{args.name}={args.text or args.name}"]
    else:
        _new_name(args.name)
        if not _NAME.fullmatch(args.effect) or len(f"{args.name}:{args.effect}") > runtime_pack.NAME_MAX:
            raise AuthorError("particle dictionary:effect must fit 63 lowercase [a-z0-9_] characters combined")
        source = _source(args, args.input, PTFX_TEMPLATE)
        if joaat(args.name) == joaat(source.stem):
            raise AuthorError("the particle clone needs a new dictionary name, different from its source")
        member = _filename(f"{args.name}.ppt", ".ppt")
        files[member] = resource_input(source, ".ppt", 0x47)
        options += [
            "--member",
            f"{member}={member}",
            "--spawn",
            f"ptfx:{args.name}:{args.effect}={args.text or args.name}",
        ]
    if args.command in ("labels", "timecycle"):
        member = _filename(args.carrier_name or f"{slug}_carrier.ptd", ".ptd")
        carrier = _source(args, args.carrier, CARRIER_TEMPLATE)
        files[member] = resource_input(carrier, ".ptd", 5)
        options += ["--member", f"{member}={member}"]
    args.output_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".author-", dir=args.output_root) as staging:
        stage = Path(staging)
        for name, data in files.items():
            (stage / name).write_bytes(data)
        for i in range(0, len(options), 2):
            if options[i] in ("--member", "--data"):
                key, _, file = options[i + 1].partition("=")
                options[i + 1] = f"{key}={stage / file}"
        command = ["--id", args.id, "--archive", archive, "--output-root", str(stage), *options]
        for key in runtime_pack.ABOUT_KEYS:
            if getattr(args, key):
                command += ["--" + key, getattr(args, key)]
        # Builder output names temporary paths; print only the validated final destination below.
        with contextlib.redirect_stdout(io.StringIO()):
            try:
                builder.main(command)
            except SystemExit as error:
                raise AuthorError(f"pack builder refused the input: {error}") from None
        built = stage / args.id
        pack, errors, warnings = validator.check(built, args.against)
        if errors:
            raise AuthorError("; ".join(errors))
        if pack is None:
            raise AuthorError("built descriptor was not readable")
        labels = {joaat(key) for key, _ in pack.labels}
        for other in args.against:
            prior, _, _ = runtime_pack.check_directory(other)
            if prior and labels & {joaat(key) for key, _ in prior.labels}:
                raise AuthorError(f"label hash collides with --against pack {prior.pack_id}")
        # Reserve a new directory atomically. Exclusive file creates also refuse any concurrent
        # replacement; failures leave our partial output for inspection, never delete user data.
        target.mkdir()
        destination = target / "resources"
        destination.mkdir()
        for source in (built / "resources").iterdir():
            with source.open("rb") as original, (destination / source.name).open("xb") as output:
                shutil.copyfileobj(original, output)
        for warning in warnings:
            print(f"warning: {warning}", file=sys.stderr)
    print(f"wrote {target} (builder and pack validation passed)")
    if args.command == "ptfx":
        print(f"effect {args.effect!r} is caller-declared; its presence and playback are not verified by this tool")
    return target


def parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    for kind in ("labels", "timecycle", "ptfx"):
        sub = commands.add_parser(kind)
        sub.add_argument("input", type=Path, nargs=None if kind == "labels" else "?")
        sub.add_argument("--id", required=True)
        sub.add_argument("--output-root", type=Path, default=build_dir(_HERE.parent) / "custom-assets")
        sub.add_argument("--templates", type=Path, default=build_dir(_HERE.parent) / "retail-templates")
        sub.add_argument("--archive", help="archive filename (default derived from pack id)")
        sub.add_argument(
            "--against", action="append", default=[], type=Path, help="validate alongside this existing pack"
        )
        for key in runtime_pack.ABOUT_KEYS:
            sub.add_argument("--" + key, default="")
        if kind != "ptfx":
            sub.add_argument(
                "--carrier", type=Path, help="PS5 PTD archive filler (default verified corpus/tornado6.ptd)"
            )
            sub.add_argument("--carrier-name", help="new archive member name ending in .ptd")
        if kind != "labels":
            sub.add_argument("--name", required=True, help="new modifier/dictionary name")
            sub.add_argument("--text", help="Custom Packs menu text (at most 39 printable ASCII characters, no '~')")
        if kind == "timecycle":
            sub.add_argument("--like", required=True, help="source modifier name")
            sub.add_argument("--meta", help="data filename ending in .meta (default derived from pack id)")
        if kind == "ptfx":
            sub.add_argument("--effect", required=True, help="existing effect name, declared by the caller")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        build(args)
    except (AuthorError, AssetError, retail_templates.TemplateError, OSError, ET.ParseError, expat.ExpatError) as error:
        print(f"author_runtime_pack: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

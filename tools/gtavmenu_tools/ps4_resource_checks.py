"""PS4 resource checks for runtime-pack archives: skinned vertex declarations and Thin1D textures.

A PS4 pack's archives hold Legacy RSC7 resources: fragments (.oft, version 162) and texture
dictionaries (.otd, version 13). A PC-to-PS4 conversion can produce members that pass every
descriptor check and still draw wrong on the console, so the resource bytes are checked here.

Vertex declarations (.oft): every declaration the fragment's drawables reach (main, cloth, array and
environment-cloth drawables and both drawables of each physics-LOD child; their four LOD model lists
and the all-models list; each model's geometries and vertex buffers). PC Legacy encodes BlendIndices
as Colour (type 9); stock PS4 fragments use UByte4 (8), integer bone indices. A skinned declaration
(flags bit 2, BlendIndices enabled) whose BlendIndices type is not 8 is an error: the console then
renders the vehicle without most of its body. Another enabled component whose type differs from the
stock declaration word STOCK_TYPES is a warning. tools/ps4_fragment_convert.ps1
(Get-Ps4DeclarationTypes) writes the PS4 encoding.

Textures (.otd): a BC1, BC3 or BC5 texture must be stored with GCN Thin1DThin microtiles, as
gtavmenu_tools.ps4_texture_writer writes it: tile word TILE_MODE (0x800001) and tile index 13, an
allocation of exactly the Thin1DThin storage for its size, format and mips, mip views equal to the
computed ones, and the packed allocation size at object +0x40 equal to that storage. A texture of any
other format is not checked (a warning).

The walks are read-only and bounded (asset_formats.Limits); structure offsets follow the pinned
CodeWalker layouts asset_materials uses. A member that cannot be read or walked is an error.
"""

from __future__ import annotations

import struct
from collections import Counter
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING

from . import rpf7
from .asset_formats import AssetError, Limits, decode_resource
from .asset_textures import SYSTEM_BASE, ResourceView
from .ps4_texture_reader import parse_ps4_texture_dictionary
from .ps4_texture_writer import FORMAT_CODES, TILE_MODE, surface_layout, texture_view

if TYPE_CHECKING:
    from .runtime_pack import RuntimePack

FRAGMENT_VERSION = 162
TEXTURE_DICTIONARY_VERSION = 13
CHECKED_SUFFIXES = (".oft", ".otd")

# Vertex declaration (16 bytes): flags u32 (bit i enables semantic i), stride u16, u8, count u8,
# types u64 (nibble i is the component type of semantic i).
SEMANTICS = (
    "Position",
    "BlendWeights",
    "BlendIndices",
    "Normal",
    "Colour0",
    "Colour1",
    *(f"TexCoord{i}" for i in range(8)),
    "Tangent",
    "Binormal",
)
BLEND_INDICES = SEMANTICS.index("BlendIndices")
SKINNED = 1 << BLEND_INDICES
UBYTE4, COLOUR = 8, 9
STOCK_TYPES = 0x7755555555996896  # stock PS4 Adder/Asea: Position Float3 ... BlendIndices UByte4
COMPONENT_TYPES = {
    0: "Nothing",
    1: "Half2",
    2: "Float",
    3: "Half4",
    4: "FloatUnk",
    5: "Float2",
    6: "Float3",
    7: "Float4",
    8: "UByte4",
    9: "Colour",
    10: "RGBA8SNorm",
}

THIN1D_TILE_INDEX = 13
TILE_INDEX_OFFSET = 0x1E  # texture object byte
ALLOCATION_WORD_OFFSET = 0x40  # texture object u32: allocation bytes in bits 8..28
ALLOCATION_SIZE_MASK = 0x1FFFFF00


@dataclass(frozen=True)
class Finding:
    """One problem: the archive member, what in it (a declaration or texture; empty for the member
    itself) and why."""

    member: str
    subject: str
    message: str

    def __str__(self) -> str:
        return ": ".join(part for part in (self.member, self.subject, self.message) if part)


@dataclass
class Report:
    """Errors and warnings, with how much was checked (members, distinct declarations, textures)."""

    errors: list[Finding] = field(default_factory=list)
    warnings: list[Finding] = field(default_factory=list)
    members: int = 0
    declarations: int = 0
    textures: int = 0

    def add(self, other: Report) -> None:
        self.errors += other.errors
        self.warnings += other.warnings
        self.members += other.members
        self.declarations += other.declarations
        self.textures += other.textures


class LegacyWalk:
    """Bounded reads of a Legacy resource's system pages.

    A structure pointer must be non-null, aligned and inside the system pages; array entries and the
    distinct structure bytes visited are charged against `limits`. `label` starts every error.
    """

    def __init__(self, payload: bytes, header: dict, limits: Limits | None = None, label: str = "PS4 fragment"):
        self.limits = Limits() if limits is None else limits
        self.view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
        self.system = self.view.system
        self.label = label
        self.entries = 0
        self.metadata_bytes = 0
        self._seen: set[tuple[int, int]] = set()

    def charge(self, count: int) -> None:
        self.entries += count
        if self.entries > self.limits.max_entries:
            raise AssetError(f"{self.label} drawable walk exceeds entry limit")

    def at(self, pointer: int, size: int, alignment: int = 16) -> int:
        """The system offset of a `size`-byte structure at `pointer`."""
        if not pointer or pointer % alignment:
            raise AssetError(f"{self.label} drawable walk has a missing or unaligned pointer")
        offset = self.view.offset(pointer, size)
        if (pointer, size) not in self._seen:
            self._seen.add((pointer, size))
            self.metadata_bytes += size
            if self.metadata_bytes > self.limits.max_metadata_bytes:
                raise AssetError(f"{self.label} drawable walk exceeds metadata limit")
        return offset

    def u64(self, offset: int) -> int:
        return struct.unpack_from("<Q", self.system, offset)[0]

    def array(self, pointer: int, count: int) -> list[int]:
        """The `count` pointers of a pointer array (no pointer for an empty one)."""
        self.charge(count)
        if bool(pointer) != bool(count):
            raise AssetError(f"{self.label} drawable array presence disagrees with count")
        start = self.at(pointer, count * 8, 8) if count else 0
        return [self.u64(start + index * 8) for index in range(count)]


def fragment_drawables(walk: LegacyWalk) -> list[int]:
    """Every drawable pointer of a Legacy fragment (0 for an empty slot), in walk order.

    FragType (304 bytes at the system base): the main drawable (+0x30, required), the cloth drawable
    slot (+0xF8), the drawable array (+0x38, count +0x48) and each environment cloth's drawable
    (+0x60, count and capacity +0x68, drawable at cloth +24); the physics LOD group (+0xF0) names up to
    three LODs whose children (+0xD0, count +0x11D, repeated at +0x11E) each hold two drawables
    (+0xA0, +0xA8).
    """
    walk.at(SYSTEM_BASE, 304)
    walk.at(walk.u64(0x30), 336)
    drawables = [walk.u64(0x30), walk.u64(0xF8)]
    drawables += walk.array(walk.u64(0x38), struct.unpack_from("<I", walk.system, 0x48)[0])
    cloth_count, cloth_capacity = struct.unpack_from("<HH", walk.system, 0x68)
    if cloth_count > cloth_capacity or cloth_capacity > walk.limits.max_entries:
        raise AssetError(f"{walk.label} cloth counts disagree or exceed entry limit")
    for pointer in walk.array(walk.u64(0x60), cloth_count):
        if pointer:
            drawables.append(walk.u64(walk.at(pointer, 128) + 24))
    physics = walk.u64(0xF0)
    if physics:
        group = walk.at(physics, 48)
        for slot in (16, 24, 32):
            pointer = walk.u64(group + slot)
            if not pointer:
                continue
            lod = walk.at(pointer, 304)
            count, duplicate_count = struct.unpack_from("<BB", walk.system, lod + 0x11D)
            if count != duplicate_count:
                raise AssetError(f"{walk.label} physics child counts disagree")
            for child in walk.array(walk.u64(lod + 0xD0), count):
                if child:
                    child_at = walk.at(child, 256)
                    drawables += [walk.u64(child_at + 0xA0), walk.u64(child_at + 0xA8)]
    walk.charge(len(drawables))
    return drawables


def drawable_declarations(walk: LegacyWalk, drawables: Iterable[int]) -> dict[int, tuple[int, int, int]]:
    """{declaration pointer: (flags, types, geometries using it)} of the `drawables` (0 is skipped).

    Drawable (336 bytes): LOD model lists at +0x50..+0x68 and the all-models list at +0xA0, each a
    pointer list (pointer, count u16, capacity u16) of models (48 bytes: geometries +8, count +16);
    geometry (152 bytes): vertex buffer +0x18; vertex buffer (128 bytes): declaration +0x30.
    """
    declarations: dict[int, list[int]] = {}
    lists: dict[int, None] = {}
    for drawable in dict.fromkeys(drawables):
        if drawable:
            at = walk.at(drawable, 336)
            lists.update(dict.fromkeys(walk.u64(at + offset) for offset in (0x50, 0x58, 0x60, 0x68, 0xA0)))
    models: dict[int, None] = {}
    for pointer in lists:
        if pointer:
            models_at, count, capacity = struct.unpack_from("<QHH", walk.system, walk.at(pointer, 16))
            if count > capacity:
                raise AssetError(f"{walk.label} model list count exceeds its capacity")
            models.update(dict.fromkeys(walk.array(models_at, count)))
    geometries: dict[int, None] = {}
    for pointer in models:
        model = walk.at(pointer, 48)
        count = struct.unpack_from("<H", walk.system, model + 16)[0]
        geometries.update(dict.fromkeys(walk.array(walk.u64(model + 8), count)))
    for pointer in geometries:
        buffer = walk.at(walk.u64(walk.at(pointer, 152) + 0x18), 128)
        declaration = walk.u64(buffer + 0x30)
        if declaration not in declarations:
            flags, _stride, _unknown, _count, types = struct.unpack_from(
                "<IHBBQ", walk.system, walk.at(declaration, 16)
            )
            declarations[declaration] = [flags, types, 0]
        declarations[declaration][2] += 1
    return {pointer: (flags, types, users) for pointer, (flags, types, users) in declarations.items()}


def _type_name(kind: int) -> str:
    return f"{COMPONENT_TYPES[kind]} ({kind})" if kind in COMPONENT_TYPES else f"type {kind}"


def declaration_findings(member: str, flags: int, types: int, geometries: int) -> tuple[list[Finding], list[Finding]]:
    """(errors, warnings) of one vertex declaration (module docstring)."""
    subject = f"vertex declaration flags=0x{flags:x} types=0x{types:016x}"
    used = f"{geometries} geometr{'y' if geometries == 1 else 'ies'}"
    errors, warnings = [], []
    index_type = (types >> (4 * BLEND_INDICES)) & 15
    if flags & SKINNED and index_type != UBYTE4:
        errors.append(
            Finding(
                member,
                subject,
                f"skinned, but BlendIndices is {_type_name(index_type)}"
                f"{', the PC encoding,' if index_type == COLOUR else ','} not {_type_name(UBYTE4)} as in stock PS4 "
                f"fragments: the console renders such parts without most of their geometry ({used})",
            )
        )
    differ = [
        f"{SEMANTICS[i]} is {_type_name((types >> (4 * i)) & 15)}, stock {_type_name((STOCK_TYPES >> (4 * i)) & 15)}"
        for i in range(len(SEMANTICS))
        if i != BLEND_INDICES and flags >> i & 1 and ((types ^ STOCK_TYPES) >> (4 * i)) & 15
    ]
    if differ:
        warnings.append(
            Finding(member, subject, f"differs from the stock PS4 declaration: {'; '.join(differ)} ({used})")
        )
    return errors, warnings


def check_fragment(blob: bytes, member: str, limits: Limits | None = None) -> Report:
    """The vertex declarations of an .oft (a complete RSC7 resource: header and deflated pages)."""
    limits = Limits() if limits is None else limits
    report = Report(members=1)
    try:
        header, payload = decode_resource(blob, limits.max_file_bytes)
        if header["version"] != FRAGMENT_VERSION:
            raise AssetError(f"resource version {header['version']}, not a Legacy version-162 fragment")
        walk = LegacyWalk(payload, header, limits)
        declarations = drawable_declarations(walk, fragment_drawables(walk))
    except (ValueError, struct.error, IndexError) as error:
        report.errors.append(Finding(member, "", f"vertex declarations not readable: {error}"))
        return report
    report.declarations = len(declarations)
    usage: Counter[tuple[int, int]] = Counter()
    for flags, types, geometries in declarations.values():
        usage[(flags, types)] += geometries
    for (flags, types), geometries in sorted(usage.items()):
        errors, warnings = declaration_findings(member, flags, types, geometries)
        report.errors += errors
        report.warnings += warnings
    return report


def check_texture_dictionary(blob: bytes, member: str, limits: Limits | None = None) -> Report:
    """The texture storage of an .otd (a complete RSC7 resource: header and deflated pages)."""
    report = Report(members=1)
    try:
        textures = parse_ps4_texture_dictionary(blob, limits)["textures"]
    except (ValueError, struct.error, IndexError) as error:
        report.errors.append(Finding(member, "", f"textures not readable: {error}"))
        return report
    report.textures = len(textures)
    formats = {code: name for name, code in FORMAT_CODES.items()}
    for texture in textures:
        subject = f"texture {texture['name']}"
        code = texture["formatCode"] & 0xFF
        if code not in formats:
            report.warnings.append(
                Finding(member, subject, f"format code 0x{code:02x} is not BC1, BC3 or BC5; its tiling is not checked")
            )
            continue
        mips = texture["mipLevels"]
        shape = f"{formats[code]} {texture['width']}x{texture['height']} with {mips} mip{'s' if mips != 1 else ''}"
        try:
            layout = surface_layout(texture["width"], texture["height"], texture["mipLevels"], formats[code])
        except AssetError as error:
            report.errors.append(Finding(member, subject, f"{shape}: {error}"))
            continue
        problems = []
        tile_index = texture["rawObject"][TILE_INDEX_OFFSET]
        if texture["tileMode"] != TILE_MODE or tile_index != THIN1D_TILE_INDEX:
            problems.append(
                f"tile word 0x{texture['tileMode']:x} index {tile_index} (Thin1DThin: 0x{TILE_MODE:x} index "
                f"{THIN1D_TILE_INDEX})"
            )
        if len(texture["allocation"]) != layout.storage_bytes:
            problems.append(f"allocation of {len(texture['allocation'])} bytes (Thin1DThin: {layout.storage_bytes})")
        if texture["rawView"] != texture_view(layout):
            problems.append("mip views differ from the Thin1DThin pitch and offsets")
        packed = struct.unpack_from("<I", texture["rawObject"], ALLOCATION_WORD_OFFSET)[0] & ALLOCATION_SIZE_MASK
        if packed != layout.storage_bytes:
            problems.append(f"packed allocation size {packed} at +0x40 (Thin1DThin: {layout.storage_bytes})")
        if problems:
            report.errors.append(
                Finding(member, subject, f"{shape} is not stored as GCN Thin1DThin: {'; '.join(problems)}")
            )
    return report


def check_member(name: str, blob: bytes, limits: Limits | None = None) -> Report:
    """Check a loose .oft or .otd resource named `name`; any other member gives an empty report."""
    suffix = name[name.rfind(".") :].lower()
    if suffix == ".oft":
        return check_fragment(blob, name, limits)
    if suffix == ".otd":
        return check_texture_dictionary(blob, name, limits)
    return Report()


def check_archive(blob: bytes, archive: str, limits: Limits | None = None) -> Report:
    """Check every .oft and .otd member of a plain-table archive, each named `<archive>/<path>`.

    A resource's RSC7 header is rebuilt from its table entry (system and graphics flags, the version in
    their top nibbles), so an oversize member whose prefix carries its stored size reads too. A table
    that cannot be read raises rpf7.Rpf7Error (or ValueError, struct.error, IndexError).
    """
    limits = Limits() if limits is None else limits
    entries, _ = rpf7.read_table(blob, archive)
    report = Report()
    for entry, path in zip(entries, rpf7.entry_paths(entries), strict=True):
        if entry["kind"] == "dir" or not path.lower().endswith(CHECKED_SUFFIXES):
            continue
        member = f"{archive}/{path}"
        if entry["kind"] != "res":
            report.add(Report([Finding(member, "", "stored as a plain file, not a resource")], members=1))
            continue
        stored = blob[entry["offset"] : entry["offset"] + entry["size"]]
        if len(stored) != entry["size"] or len(stored) < 16:
            report.add(Report([Finding(member, "", "runs past the end of the archive")], members=1))
            continue
        system_flags, graphics_flags = entry["a"], entry["b"]
        version = (system_flags >> 28) << 4 | graphics_flags >> 28
        header = struct.pack("<4sIII", b"RSC7", version, system_flags, graphics_flags)
        report.add(check_member(member, header + stored[16:], limits))
    return report


def pack_problems(resources: Path, pack: RuntimePack, limits: Limits | None = None) -> tuple[list[str], list[str]]:
    """(errors, warnings) of the PS4 resources in a pack's archives under `resources`.

    An archive that is missing (the descriptor check names it) or whose table cannot be read is not
    checked; the latter warns only when the pack's override rows name an .oft or .otd in it.
    """
    errors: list[str] = []
    warnings: list[str] = []
    for archive in [pack.archive, *(name for name, _, _ in pack.extra_archives)]:
        try:
            blob = (resources / archive).read_bytes()
        except OSError:
            continue
        try:
            report = check_archive(blob, archive, limits)
        except (rpf7.Rpf7Error, ValueError, struct.error, IndexError):
            if any(row == archive and member.lower().endswith(CHECKED_SUFFIXES) for row, member in pack.overrides):
                warnings.append(f"PS4 resources not checked: {archive} has a keyed or unreadable table")
            continue
        errors += [str(finding) for finding in report.errors]
        warnings += [str(finding) for finding in report.warnings]
    return errors, warnings

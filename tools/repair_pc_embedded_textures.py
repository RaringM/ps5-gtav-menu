#!/usr/bin/env python3
"""Move the textures a PC vehicle fragment embeds out of it, so the fragment is system-only (F1.3).

The PC vehicle route reads system-only Legacy fragments (pc_drawable_materials: "selected Legacy YFT path
supports system-only fixture resources"). Some exporters embed a texture dictionary in the main drawable's
shader group, which puts the pixels in graphics pages: the vans123 aventador_hi.yft embeds one
512x512 BC1 texture (vehicle_generic_tyrewall_spec3). On PC the drawable's shaders bind textures by
name, the embedded dictionary first; on the console the converted fragment references every texture by
name through the car's .ptd. This tool writes a new fixture in which every .yft with graphics pages has:
  - the main drawable's shader-group texture dictionary pointer (FragType +0x30 -> drawable +0x10 ->
    shader group +0x08) set to 0 and each of its texture objects' pixel pointer (+0x70) set to 0, so the
    shader parameters keep pointing at objects that carry the texture's name (TextureBase, all the
    material reader reads) and nothing points into graphics pages any more;
  - each such texture object a shader parameter points at (some exporters point shaders at the embedded
    texture itself rather than at a name-only reference) rewritten as a name-only reference: the 80-byte
    Legacy TextureBase with only its VFT, name pointer, count 1 and tag 2 (what the converter reads);
  - its graphics page flags cleared (the RSC7 version nibble kept), the graphics pages dropped.
Refused unless every graphics pointer of the resource is the pixel pointer of a texture of that
dictionary (anything else in graphics pages is not this quirk); a word of the dictionary's name-hash
array that reads as a graphics pointer (a hash with the padding after it) is not one. The original fragment is kept as
<output>/embedded-textures/<member path> (recorded in the manifest with its sha256): the texture step
(convert_pc_ytd_writer.py --embedded FILE) reads the dictionary from it with embedded_textures() and
adds those textures to the car's .ptd, so every name keeps resolving (a name the .ytd already holds
keeps the .ytd's texture). A fixture without graphics pages is left alone ("no embedded textures").

LSC parts carry their own dictionaries (the Dominator GTX's 19 livery parts, grilles, interior; KoRn a45_livery1..5),
and their names collide: dominatortext_d is a BGRA8 texture in tfdom_grille_01 and a BC1 one in tfdom_grille_02. All
of them end up in ONE dictionary (the car's .ptd, which every part and the car itself search before vehshare), so a
texture whose name (joaat, case-insensitive) is already taken by different pixels -- the car's own .ytd or a shipped
parent .ytd of the fixture, a --shared-dictionary (the retail vehshare the car falls back to), or an embedded texture
of a fragment handled before it (base car first, then the parts in member order) -- is renamed for its fragment: the
same-length name `<name minus its tail>_<n>` (dominatortext_d -> dominatortext_1), written into every name string
of that fragment that spells it (its dictionary's texture objects and every texture reference of the main drawable's
shaders), in both the system-only fragment and the kept original (whose dictionary hash table is re-sorted), so the
part's shaders and the .ptd agree. The same name with the same pixels (format, size, mips) is shared, not renamed.
With --model, a PART (any .yft but <model>.yft / <model>_hi.yft) whose dictionary cannot be moved -- malformed (a
name that repeats or disagrees with its hash: tfdom_stwheel1 repeats `nodirt`) or not this quirk -- is left out of
the output fixture and recorded (convert_vehicle.py prints one `parts:` line per part; prepare_carcols then drops its kit
entries as unshipped); the car's own fragments still refuse.

  repair_pc_embedded_textures.py --fixture build/assets/convert-<id>/fixtures/aventador-import \\
      --output <new fixture> [--model aventador] [--shared-dictionary <templates>/corpus/vehshare-a.ptd]
      (convert_vehicle.py runs it before the first material report)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
from dataclasses import replace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

from gtavmenu_tools.asset_formats import AssetError, Limits, decode_resource, resource_header  # noqa: E402
from gtavmenu_tools.asset_textures import GRAPHICS_BASE, SYSTEM_BASE, ResourceView  # noqa: E402
from gtavmenu_tools.hashes import joaat  # noqa: E402
from gtavmenu_tools.vehicle_repair_inputs import (  # noqa: E402
    FixtureError,
    copy_fixture,
    resource_path,
    validate_fixture,
)

FRAGMENT_DRAWABLE = 0x30  # FragType: main drawable pointer (pc_drawable_materials "drawablePointer")
DRAWABLE_SHADER_GROUP = 0x10  # Drawable: ShaderGroupPointer
GROUP_TEXTURES = 0x08  # ShaderGroup: TextureDictionaryPointer
TEXTURE_HASHES = 32  # texture dictionary: u32 name-hash array
TEXTURE_POINTERS = 48  # texture dictionary: texture object pointer array, u16 count
TEXTURE_DATA = 0x70  # Legacy texture object: pixel data pointer (asset_textures, convert_pc_ytd_writer DATA)
TEXTURE_NAME = 0x28  # Legacy texture object / texture reference: name string pointer
GROUP_SHADERS = 0x10  # ShaderGroup: shader pointer array, u16 count at +0x18 (pc_drawable_materials contract)
SHADER_PARAMETERS = 0x00  # ShaderFX: parameter block pointer, u8 count at +0x10
PARAMETER_BYTES = 16  # ShaderParameter: u8 data type (0 = texture), data pointer at +8
RESOURCE_LIMIT = 512 * 1024 * 1024
SIDECAR = "embedded-textures"
NOTHING = "no embedded textures"


class EmbeddedError(Exception):
    """A fragment whose embedded dictionary cannot be moved (malformed, or not this quirk)."""


def limits() -> Limits:
    return replace(Limits(), max_file_bytes=1 << 30, max_total_bytes=1 << 31, max_entries=4096)


def _u64(data, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def has_graphics(blob: bytes) -> bool:
    """Whether a resource's RSC7 header declares graphics pages (only the 16 header bytes are read)."""
    return resource_header(blob[:16])["graphicsBytes"] != 0


def locate(header: dict, payload: bytes) -> tuple[int, list[int], tuple[int, int]]:
    """(system offset of the dictionary pointer slot, system offsets of its texture objects, the span of its name
    hashes) of a Legacy fragment's embedded dictionary; refuses unless its textures' pixel pointers are every
    graphics pointer."""
    system_bytes, graphics_bytes = header["systemBytes"], header["graphicsBytes"]
    view = ResourceView(payload, system_bytes, graphics_bytes)
    system = view.system
    drawable = view.offset(_u64(system, FRAGMENT_DRAWABLE), DRAWABLE_SHADER_GROUP + 8)
    group = view.offset(_u64(system, drawable + DRAWABLE_SHADER_GROUP), GROUP_TEXTURES + 8)
    slot = group + GROUP_TEXTURES
    dictionary = _u64(system, slot)
    if not dictionary:
        raise EmbeddedError("the fragment has graphics pages but its main drawable embeds no texture dictionary")
    at = view.offset(dictionary, 64)
    pointers, count = struct.unpack_from("<QH", system, at + TEXTURE_POINTERS)
    array = view.offset(pointers, 8 * count)
    objects = [view.offset(_u64(system, array + 8 * i), TEXTURE_DATA + 8) for i in range(count)]
    # The dictionary's u32 name-hash array can read as a graphics pointer (f812_liv5: joaat 0x61d7f720 + padding).
    hashes = view.offset(_u64(system, at + TEXTURE_HASHES), 4 * count)
    graphics = graphics_words(system, graphics_bytes, (hashes, hashes + 4 * count))
    if graphics != {o + TEXTURE_DATA for o in objects}:
        raise EmbeddedError(
            f"graphics pages hold more than the main drawable's embedded textures ({len(graphics)} graphics "
            f"pointers, {len(objects)} embedded textures): not this quirk"
        )
    return slot, objects, (hashes, hashes + 4 * count)


def graphics_words(system, graphics_bytes: int, skip: tuple[int, int]) -> set[int]:
    """System offsets of the 8-byte words that point into the graphics pages, outside SKIP (the name hashes)."""
    end = GRAPHICS_BASE + graphics_bytes
    return {
        o for o in range(0, len(system) - 7, 8) if GRAPHICS_BASE <= _u64(system, o) < end and not skip[0] <= o < skip[1]
    }


def embedded_textures(blob: bytes) -> list[dict]:
    """The embedded textures of a Legacy fragment with graphics pages, in the shape
    convert_pc_ytd_writer.convert reads (convert_pc_drawable.legacy_textures; any bad texture refuses)."""
    import convert_pc_drawable  # the prop route's embedded-dictionary reader

    header, payload = decode_resource(blob, RESOURCE_LIMIT)
    slot, _, _ = locate(header, payload)
    view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
    try:
        return convert_pc_drawable.legacy_textures(view, _u64(view.system, slot), limits())[0]
    except AssetError as error:  # e.g. tfdom_stwheel1: "texture 'nodirt' disagrees with its key or repeats"
        raise EmbeddedError(f"malformed embedded dictionary: {error}") from None


def _offset(system, pointer: int, size: int) -> int:
    """System offset of POINTER with SIZE bytes inside the system pages (EmbeddedError otherwise)."""
    at = pointer - SYSTEM_BASE
    if not 0 <= at <= len(system) - size:
        raise EmbeddedError(f"pointer {pointer:#x} leaves the system pages")
    return at


def _string(system, pointer: int) -> tuple[int, str]:
    """(system offset, text) of the NUL-terminated ASCII name string at POINTER."""
    at = _offset(system, pointer, 1)
    end = bytes(system[at : at + 257]).find(b"\0")
    if end < 0:
        raise EmbeddedError(f"name string at {pointer:#x} is unterminated")
    try:
        return at, bytes(system[at : at + end]).decode("ascii")
    except UnicodeDecodeError:
        raise EmbeddedError(f"name string at {pointer:#x} is not ASCII") from None


def reference_objects(system) -> set[int]:
    """System offsets of the texture objects the main drawable's shaders reference (texture parameters)."""
    drawable = _offset(system, _u64(system, FRAGMENT_DRAWABLE), DRAWABLE_SHADER_GROUP + 8)
    group = _offset(system, _u64(system, drawable + DRAWABLE_SHADER_GROUP), GROUP_SHADERS + 10)
    shaders, count = struct.unpack_from("<QH", system, group + GROUP_SHADERS)
    found = set()
    if not shaders:
        return found
    array = _offset(system, shaders, 8 * count)
    for index in range(count):
        shader = _offset(system, _u64(system, array + 8 * index), 0x11)
        parameters, parameter_count = _u64(system, shader + SHADER_PARAMETERS), system[shader + 0x10]
        if not parameters:
            continue
        block = _offset(system, parameters, PARAMETER_BYTES * parameter_count)
        for slot in range(parameter_count):
            at = block + PARAMETER_BYTES * slot
            if system[at] == 0 and _u64(system, at + 8):  # data type 0: a texture object (or a named reference)
                found.add(_offset(system, _u64(system, at + 8), TEXTURE_NAME + 8))
    return found


def rename_strings(system: bytearray, objects: list[int], renames: dict[str, str]) -> int:
    """Rewrite, in place, every name string of the dictionary's texture OBJECTS and of the main drawable's texture
    references whose text (case-insensitive) is a key of RENAMES (same length); returns the strings rewritten."""
    done: set[int] = set()
    for obj in sorted(set(objects) | reference_objects(system)):
        at, name = _string(system, _u64(system, obj + TEXTURE_NAME))
        new = renames.get(name.lower())
        if new is None or at in done:
            continue
        if len(new) != len(name):
            raise SystemExit(f"internal error: rename {name} -> {new} changes the length")
        system[at : at + len(new)] = new.encode("ascii")
        done.add(at)
    return len(done)


REFERENCE_BYTES = 80  # Legacy TextureBase: what a shader's texture reference is (native_texture_references "legacy")
REFERENCE_KEEP = ((0, 8), (TEXTURE_NAME, TEXTURE_NAME + 8))  # VFT + Unknown_4h, NamePointer
REFERENCE_COUNT_TAG = (0x30, struct.pack("<HH", 1, 2))  # Unknown_30h = 1, Unknown_32h = 2 (CodeWalker TextureBase)


def as_references(system: bytearray, objects: list[int]) -> int:
    """Give the dictionary's texture OBJECTS that the main drawable's shaders point at the zero-metadata Legacy
    reference profile the vehicle converter reads (native_texture_references.source_read: VFT, NamePointer,
    count 1, tag 2, every other byte of the 80-byte TextureBase zero). Some exporters point a shader parameter
    at the embedded texture itself (the Dominator GTX parts) instead of a separate name-only reference; once the
    dictionary is gone it is one. Returns the objects rewritten."""
    count = 0
    for obj in sorted(set(objects) & reference_objects(system)):
        head = bytearray(REFERENCE_BYTES)
        for start, end in REFERENCE_KEEP:
            head[start:end] = system[obj + start : obj + end]
        at, value = REFERENCE_COUNT_TAG
        head[at : at + len(value)] = value
        if bytes(system[obj : obj + REFERENCE_BYTES]) != bytes(head):
            system[obj : obj + REFERENCE_BYTES] = head
            count += 1
    return count


def _resource(blob: bytes, payload: bytes, graphics: bool) -> bytes:
    """BLOB's RSC7 header (graphics page flags dropped unless GRAPHICS) over PAYLOAD as raw deflate."""
    magic, version, system_flags, graphics_flags = struct.unpack_from("<4I", blob)
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    out = struct.pack("<4I", magic, version, system_flags, graphics_flags if graphics else graphics_flags & 0xF0000000)
    return out + packer.compress(payload) + packer.flush()


def system_only(blob: bytes, renames: dict[str, str] | None = None) -> tuple[bytes, int]:
    """The fragment without its embedded dictionary and graphics pages, RENAMES (lowercase old name -> new name of
    the same length) applied to its name strings; returns (resource, textures)."""
    header, payload = decode_resource(blob, RESOURCE_LIMIT)
    slot, objects, hashes = locate(header, payload)
    system = bytearray(payload[: header["systemBytes"]])
    if renames:
        rename_strings(system, objects, renames)
    as_references(system, objects)
    struct.pack_into("<Q", system, slot, 0)
    for at in objects:
        struct.pack_into("<Q", system, at + TEXTURE_DATA, 0)
    out = _resource(blob, bytes(system), graphics=False)
    back_header, back = decode_resource(out, RESOURCE_LIMIT)
    if back_header["graphicsBytes"] or back != bytes(system) or back_header["version"] != header["version"]:
        raise SystemExit("internal error: the system-only fragment does not read back")
    if graphics_words(back, header["graphicsBytes"], hashes):
        raise SystemExit("internal error: a graphics pointer is left in the system pages")
    return out, len(objects)


def renamed_original(blob: bytes, renames: dict[str, str]) -> bytes:
    """The original fragment (graphics pages kept) with RENAMES applied to its name strings and its embedded
    dictionary's hash table rewritten and re-sorted (hash with its texture pointer), so embedded_textures() reads the
    new names: the texture step adds them to the car's .ptd under the names the converted part references."""
    header, payload = decode_resource(blob, RESOURCE_LIMIT)
    slot, objects, _ = locate(header, payload)
    data = bytearray(payload)
    system = memoryview(data)[: header["systemBytes"]]
    work = bytearray(system)
    rename_strings(work, objects, renames)
    dictionary = _offset(work, _u64(work, slot), 64)
    hashes, count = struct.unpack_from("<QH", work, dictionary + TEXTURE_HASHES)
    pointers = _u64(work, dictionary + TEXTURE_POINTERS)
    hashes, pointers = _offset(work, hashes, 4 * count), _offset(work, pointers, 8 * count)
    entries = []
    for index in range(count):
        pointer = _u64(work, pointers + 8 * index)
        _, name = _string(work, _u64(work, _offset(work, pointer, TEXTURE_NAME + 8) + TEXTURE_NAME))
        entries.append((joaat(name.lower()), pointer))
    for index, (key, pointer) in enumerate(sorted(entries)):
        struct.pack_into("<I", work, hashes + 4 * index, key)
        struct.pack_into("<Q", work, pointers + 8 * index, pointer)
    system[:] = work
    out = _resource(blob, bytes(data), graphics=True)
    if decode_resource(out, RESOURCE_LIMIT)[1] != bytes(data):
        raise SystemExit("internal error: the renamed original does not read back")
    return out


def texture_digest(texture: dict) -> str:
    """Identity of a texture's pixels: format, size and every mip (two textures of one name may then share)."""
    digest = hashlib.sha256(f"{texture['format']}|{texture['width']}x{texture['height']}|".encode())
    for mip in texture["mips"]:
        digest.update(mip)
    return digest.hexdigest()


def dictionary_keys(payload: bytes) -> list[int]:
    """The u32 name-hash table of a texture dictionary payload (Legacy .ytd and PS5 .ptd roots alike: +32)."""
    pointer, count = struct.unpack_from("<QH", payload, TEXTURE_HASHES)
    at = _offset(payload, pointer, 4 * count)
    return list(struct.unpack_from(f"<{count}I", payload, at))


def taken_names(fixture: Path, manifest: dict, shared: list[Path]) -> dict[int, str | None]:
    """joaat(name) -> pixel digest (None: unknown) of every texture the car's chain already holds: the fixture's
    .ytd members (the car's own and shipped parents) and the shared dictionaries (retail vehshare)."""
    import convert_pc_drawable

    taken: dict[int, str | None] = {}
    for row in manifest["resources"]:
        if not row["member"].lower().endswith(".ytd"):
            continue
        header, payload = decode_resource(member_path(fixture, row["member"]).read_bytes(), RESOURCE_LIMIT)
        for key in dictionary_keys(payload):
            taken.setdefault(key, None)
        view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
        try:  # unreadable textures stay "taken, pixels unknown" (any embedded copy of the name is renamed)
            textures = convert_pc_drawable.legacy_textures(view, SYSTEM_BASE, limits(), skip_bad=True)[0]
        except AssetError:
            textures = []
        for texture in textures:
            taken[joaat(texture["name"].lower())] = texture_digest(texture)
    for path in shared:
        for key in dictionary_keys(decode_resource(path.read_bytes(), RESOURCE_LIMIT)[1]):
            taken[key] = None
    return taken


def fresh_name(name: str, taken: dict[int, str | None]) -> str:
    """A same-length name `<name minus its tail>_<n>` whose hash nothing has taken (dominatortext_d -> _1)."""
    for number in range(1, 10000):
        tag = f"_{number}"
        if len(tag) >= len(name):
            break
        candidate = name[: len(name) - len(tag)] + tag
        if joaat(candidate.lower()) not in taken:
            return candidate
    raise EmbeddedError(f"texture name {name!r} is too short to rename beside a texture of the same name")


def member_path(fixture: Path, member: str) -> Path:
    return resource_path(fixture, member)


def plan_fragment(blob: bytes, taken: dict[int, str | None]) -> tuple[bytes, bytes, list[str], dict[str, str]]:
    """(system-only fragment, kept original, its texture names after renames, renames) of one fragment; TAKEN gains
    its textures. Raises EmbeddedError (TAKEN unchanged) when its dictionary cannot be moved."""
    textures = embedded_textures(blob)
    mine: dict[int, str | None] = {}
    renames: dict[str, str] = {}
    for texture in textures:
        key, digest = joaat(texture["name"].lower()), texture_digest(texture)
        known = taken.get(key, mine.get(key, False))
        if known is False:
            mine[key] = digest
        elif known is None or known != digest:  # the name is another texture's: this fragment's copy is renamed
            new = fresh_name(texture["name"], taken | mine)
            renames[texture["name"].lower()] = new
            mine[joaat(new.lower())] = digest
    new, count = system_only(blob, renames)
    kept = renamed_original(blob, renames) if renames else blob
    names = [renames.get(t["name"].lower(), t["name"]) for t in textures]
    if len(names) != count:
        raise SystemExit(f"{count} texture objects but {len(names)} textures read")
    taken.update(mine)
    return new, kept, names, renames


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", help="the car's model: other fragments are parts, left out when malformed")
    parser.add_argument(
        "--shared-dictionary", type=Path, action="append", default=[], help="dictionary the car falls back to"
    )
    args = parser.parse_args(argv)
    if args.output.exists():
        raise SystemExit(f"refusing to overwrite {args.output}")
    manifest = validate_fixture(args.fixture)
    own = {f"{args.model}.yft", f"{args.model}_hi.yft"} if args.model else set()
    rows = [
        row
        for row in manifest["resources"]
        if row["member"].lower().endswith(".yft")
        and has_graphics(member_path(args.fixture, row["member"]).read_bytes())
    ]
    if not rows:
        raise SystemExit(NOTHING)
    # the car's own fragments first: their names stay, a part's copy of a taken name is the one renamed
    rows.sort(key=lambda row: (row["member"].rpartition("!/")[2].lower() not in own, row["member"]))
    taken = taken_names(args.fixture, manifest, args.shared_dictionary)
    planned, left = [], []
    for row in rows:
        stem = row["member"].rpartition("!/")[2]
        try:
            new, kept, names, renames = plan_fragment(member_path(args.fixture, row["member"]).read_bytes(), taken)
        except (EmbeddedError, AssetError) as error:
            if not args.model or stem.lower() in own:
                raise SystemExit(f"{row['member']}: {error}") from None
            left.append({"member": row["member"], "reason": str(error)})
            print(f"left out (a part): {stem}: {error}", flush=True)
            continue
        planned.append((row, new, kept, names, renames))
    copy_fixture(args.fixture, args.output, manifest)
    records = []
    for row, new, kept, names, renames in planned:
        relative = Path(*row["member"].replace("!/", "/").split("/"))
        blob = member_path(args.fixture, row["member"]).read_bytes()
        args.output.joinpath("resources", relative).write_bytes(new)
        sidecar = args.output / SIDECAR / relative
        sidecar.parent.mkdir(parents=True, exist_ok=True)
        sidecar.write_bytes(kept)
        row["bytes"] = row["storedBytes"] = len(new)
        row["sha256"] = hashlib.sha256(new).hexdigest()
        records.append(
            {
                "member": row["member"],
                "original": f"{SIDECAR}/{relative.as_posix()}",
                "originalSha256": hashlib.sha256(blob).hexdigest(),
                **({"keptSha256": hashlib.sha256(kept).hexdigest()} if renames else {}),
                "textures": names,
                **({"renames": [{"texture": old, "name": name} for old, name in renames.items()]} if renames else {}),
            }
        )
    for entry in left:
        resource_path(args.output, entry["member"]).unlink()
    gone = {entry["member"] for entry in left}
    manifest["resources"] = [row for row in manifest["resources"] if row["member"] not in gone]
    manifest["embeddedTextureRepairs"] = {
        "tool": "tools/repair_pc_embedded_textures.py",
        "reason": "embedded texture dictionary in graphics pages: textures move to the car's .ptd",
        "sourceFixtureManifestSha256": hashlib.sha256((args.fixture / "manifest.json").read_bytes()).hexdigest(),
        "resources": records,
        **({"leftOut": left} if left else {}),
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n")
    renamed = [
        f"{r['member'].rpartition('!/')[2]}: {x['texture']}->{x['name']}" for r in records for x in r.get("renames", [])
    ]
    for line in renamed:
        print(f"renamed (a name another texture of the car holds) {line}", flush=True)
    print(json.dumps({"output": str(args.output), "resources": [(r["member"], r["textures"]) for r in records]}))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FixtureError as error:
        raise SystemExit(f"fixture refused: {error}") from None

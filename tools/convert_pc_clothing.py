#!/usr/bin/env python3
"""Convert PC clothing into a story-character override or freemode apparel pack.

Story clothing replaces the requested stock drawable and textures. Freemode clothing
adds new drawables after the retail ones. Reviewed stock inputs provide skeletons,
dictionaries and variation layouts through key-free --game range reads or a verified
--stock-cache; the target metadata explicitly limits supported ped/slot combinations.
NumPy and the verified --templates cache are needed for texture conversion.

Example: convert_pc_clothing.py --source MOD --ped player_one --slot legs --drawable 6
         --id my-clothing --game /path/to/app0 --templates build/retail-templates
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from types import SimpleNamespace

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))
ROOT = _HERE.parent

import convert_override as co  # noqa: E402
import convert_vehicle as cv  # noqa: E402
import convert_vehicle_replace as cvr  # noqa: E402
import make_ped_apparel_pack as apparel  # noqa: E402
from gtavmenu_tools import mod_collection, rpf7, stock_members  # noqa: E402
from gtavmenu_tools.asset_formats import AssetError, decode_resource  # noqa: E402
from gtavmenu_tools.hashes import joaat  # noqa: E402
from gtavmenu_tools.host_paths import assets_dir, build_dir  # noqa: E402

STORY = {"player_zero": "Michael", "player_one": "Franklin", "player_two": "Trevor"}
# Freemode peds: (the retail DLC whose variation file is patched, its archive inside dlc.rpf, the pmt stem).
FREEMODE = {
    "mp_m_freemode_01": ("mp2023_01", "mp2023_01_male", "mp_m_freemode_01_mp_m_2023_01"),
    "mp_f_freemode_01": ("mp2023_01", "mp2023_01_female", "mp_f_freemode_01_mp_f_2023_01"),
}
COMPONENTS = apparel.COMPONENTS
# Wardrobe slot names (Self > Appearance > Wardrobe, ped-conversion-feasibility.md 13.4) and component names.
SLOTS = {c: c for c in COMPONENTS} | {
    "face": "head",
    "mask": "berd",
    "torso": "uppr",
    "legs": "lowr",
    "hands": "hand",
    "bags": "hand",
    "shoes": "feet",
    "neck": "teef",
    "undershirt": "accs",
    "armor": "task",
    "decals": "decl",
    "tops": "jbib",
}
WARDROBE = {
    "head": "Face",
    "berd": "Mask",
    "hair": "Hair",
    "uppr": "Torso",
    "lowr": "Legs",
    "hand": "Hands/Bags",
    "feet": "Shoes",
    "teef": "Neck",
    "accs": "Undershirt",
    "task": "Armor",
    "decl": "Decals",
    "jbib": "Tops",
}
RACES = ("uni", "whi", "bla", "chi", "lat", "ara", "bal", "jam", "kor", "ita", "pak")
DRAWABLE = re.compile(r"([a-z]{4})_(\d{3})_([ur])")
DRAWABLE_FILE = re.compile(r"([a-z]{4})_(\d{3})_([ur])\.(ydd|pdd)")
DIFFUSE = re.compile(r"([a-z]{4})_diff_(\d{3})_([a-z])_([a-z]{3})")
DIFFUSE_FILE = re.compile(r"([a-z]{4})_diff_(\d{3})_([a-z])_([a-z]{3})\.(ytd|ptd)")
SUFFIXES = (".ydd", ".ytd", ".pdd", ".ptd")
SYS = 0x50000000
DEFAULT_TEXTURE_MAX = 1024
GLOBAL_TEXTURES = frozenset({"givemechecker", "long_hair_noise", "enveff_gray"})
SHADER_MISSING = "no retail template carries shader"
COP = "peds/s_m_y_cop_01.pdd"  # fetch-templates cache: shader schema carriers for the freemode route
JUGGERNAUT = "peds/u_m_m_juggernaut_03.pdd"


class ClothingError(RuntimeError):
    pass


# --- mod files -----------------------------------------------------------------------------------------------


@dataclass
class ModFile:
    name: str  # lower-case base name (the part after a FiveM '^')
    blob: bytes
    origin: str


@dataclass
class Part:
    """One drawable of the mod and its diffuse textures."""

    file: ModFile
    entry: str | None  # the dictionary key to convert (None: decided from the target, story route)
    number: int | None  # the mod's drawable number (<comp>_<nnn>) when the file is named so
    textures: dict[str, ModFile] = field(default_factory=dict)  # mod letter -> file holding that diffuse
    texture_names: dict[str, str] = field(default_factory=dict)  # mod letter -> texture name inside that file

    @property
    def ps5(self) -> bool:
        return self.file.name.endswith(".pdd")

    @property
    def stem(self) -> str:
        return self.file.name.rsplit(".", 1)[0]


def collect(source: Path) -> tuple[dict[str, ModFile], list[str]]:
    """The mod's drawable dictionaries and texture dictionaries by lower base name."""
    files, origin, notes = mod_collection.collect(source, SUFFIXES)
    out: dict[str, ModFile] = {}
    for name in sorted(files):
        if name == cvr.META_NAME:
            continue
        base = name.rsplit("^", 1)[-1]
        if base in out and out[base].blob != files[name]:
            raise ClothingError(
                f"{base} appears twice with different bytes ({out[base].origin}, {origin[name]}): "
                "give the folder of one ped (e.g. the mod's mp_m_freemode_01 folder)"
            )
        out.setdefault(base, ModFile(base, files[name], origin[name]))
    if not out:
        raise ClothingError(f"{source.name}: no .ydd/.ytd (or PS5 .pdd/.ptd) files")
    return out, notes


def dictionary_keys(blob: bytes) -> list[int]:
    """Key hashes of a drawable dictionary (PC Legacy v165 or PS5 Gen9), in stored order."""
    header, payload = decode_resource(blob, 1 << 28)
    system = payload[: header["systemBytes"]]
    pointer, count = struct.unpack_from("<QH", system, 0x20)
    if not 0 < count <= 4096 or not 0 <= pointer - SYS <= len(system) - 4 * count:
        raise ClothingError("not a drawable dictionary (key array outside its system pages)")
    return list(struct.unpack_from(f"<{count}I", system, pointer - SYS))


def entry_skeleton(blob: bytes, key: int) -> list[dict]:
    """The own skeleton copy of dictionary entry `key` (empty when the entry indexes its ped's skeleton)."""
    import convert_pc_drawable as cpd

    header, payload = decode_resource(blob, 1 << 28)
    system = payload[: header["systemBytes"]]
    keys = dictionary_keys(blob)
    pointer = struct.unpack_from("<Q", system, 0x30)[0] - SYS
    drawable = struct.unpack_from("<Q", system, pointer + 8 * keys.index(key))[0] - SYS
    return cpd.read_skeleton(system, struct.unpack_from("<Q", system, drawable + 0x18)[0], SYS)


def key_names(comp: str | None = None) -> dict[int, str]:
    comps = [comp] if comp else COMPONENTS
    return {joaat(f"{c}_{i:03d}_{r}"): f"{c}_{i:03d}_{r}" for c in comps for i in range(1000) for r in "ur"}


def entry_name(blob: bytes, wanted: str | None) -> str:
    """The dictionary key to convert: `wanted` (it must be there), else the only entry (named by hash)."""
    keys = dictionary_keys(blob)
    if wanted is not None:
        if joaat(wanted) not in keys:
            raise ClothingError(f"the dictionary has no entry {wanted}")
        return wanted
    if len(keys) != 1:
        raise ClothingError(f"the dictionary has {len(keys)} entries: name one as STEM:ENTRY (e.g. x:head_000_r)")
    names = key_names()
    if keys[0] not in names:
        raise ClothingError(f"its entry 0x{keys[0]:08x} is no ped component key (<comp>_<nnn>_<u|r>)")
    return names[keys[0]]


def find_parts(files: dict[str, ModFile], comp: str, specs: list[str]) -> list[Part]:
    """The mod's drawables for `specs` (a number = <comp>_<nnn>, or STEM[:ENTRY]); none = every one of comp."""
    by_number: dict[int, list[ModFile]] = {}
    for f in files.values():
        m = DRAWABLE_FILE.fullmatch(f.name)
        if m and m.group(1) == comp:
            by_number.setdefault(int(m.group(2)), []).append(f)
    parts: list[Part] = []
    if not specs:
        if not by_number:
            listed = sorted({m.group(1) for f in files.values() if (m := DRAWABLE_FILE.fullmatch(f.name))})
            raise ClothingError(
                f"the mod has no {comp} drawable ({comp}_<nnn>_u.ydd); it has {', '.join(listed) or 'none'}: "
                "pick another --slot, or name a whole-ped .ydd as --drawable STEM:ENTRY"
            )
        specs = [str(n) for n in sorted(by_number)]
    for spec in specs:
        if spec.isdigit():
            found = by_number.get(int(spec), [])
            if len(found) != 1:
                have = ", ".join(str(n) for n in sorted(by_number)) or "none"
                raise ClothingError(f"the mod has {len(found)} {comp} drawable {int(spec)} files (it has {have})")
            parts.append(Part(found[0], None, int(spec)))
            continue
        stem, _, entry = spec.lower().partition(":")
        found = [f for f in files.values() if f.name in (f"{stem}.ydd", f"{stem}.pdd")]
        if len(found) != 1:
            raise ClothingError(f"--drawable {cvr.shown(spec)}: the mod has no {stem}.ydd")
        m = DRAWABLE_FILE.fullmatch(found[0].name)
        parts.append(Part(found[0], entry or None, int(m.group(2)) if m and m.group(1) == comp else None))
    return parts


def texture_letters(files: dict[str, ModFile], part: Part, key: str) -> None:
    """Fill part.textures: the diffuse textures of drawable `key` (<c>_<n>_<u|r>) by letter, from
    <c>_diff_<n>_<x>_<race>.ytd files, else from the .ytd of the drawable's own stem."""
    m = DRAWABLE_FILE.fullmatch(part.file.name) or DRAWABLE.fullmatch(key)  # replace mods: the file's slot
    if m is None:
        raise ClothingError(f"{key}: not a ped component key")
    for f in sorted(files.values(), key=lambda f: f.name):
        d = DIFFUSE_FILE.fullmatch(f.name)
        if (
            d
            and d.group(1) == m.group(1)
            and d.group(2) == m.group(2)
            and f.name[-3:] == ("ptd" if part.ps5 else "ytd")
        ):
            if d.group(3) in part.textures:
                raise ClothingError(f"{f.name}: the mod has letter {d.group(3)} of {key} in two races")
            part.textures[d.group(3)] = f
            part.texture_names[d.group(3)] = f.name.rsplit(".", 1)[0]
    own = files.get(f"{part.stem}.ytd")
    if not part.textures and own is not None:
        import convert_pc_drawable as cpd

        names = [t["name"].lower() for t in cpd.ytd_textures(own.blob, None, skip_bad=True)[0]]
        for name in names:
            d = DIFFUSE.fullmatch(name)
            if d and d.group(1) == m.group(1) and d.group(2) == m.group(2) and d.group(3) not in part.textures:
                part.textures[d.group(3)] = own
                part.texture_names[d.group(3)] = name
    if not part.textures:
        raise ClothingError(
            f"{part.file.name} ({key}): the mod ships no diffuse texture {m.group(1)}_diff_{m.group(2)}_a_*"
        )


def letter_map(specs: list[str], have: list[str], *, story: bool) -> list[tuple[str, str]]:
    """(mod letter, shown letter) pairs from --texture-letter X[=Y] (comma lists allowed); default every letter."""
    pairs: list[tuple[str, str]] = []
    for spec in [s for value in specs for s in value.split(",") if s]:
        src, _, dst = spec.partition("=")
        if not re.fullmatch(r"[a-z]", src) or (dst and not re.fullmatch(r"[a-z]", dst)):
            raise ClothingError(f"--texture-letter {cvr.shown(spec)}: a letter, or LETTER=LETTER")
        if dst and not story:
            raise ClothingError("--texture-letter X=Y is for story characters; freemode letters follow the order given")
        if src not in have:
            raise ClothingError(f"--texture-letter {src}: the mod has letters {''.join(have)}")
        pairs.append((src, dst or src))
    pairs = pairs or [(x, x) for x in have]
    if not story:
        pairs = [(src, chr(97 + i)) for i, (src, _) in enumerate(pairs)]
    shown = [dst for _, dst in pairs]
    if len(set(shown)) != len(shown) or len({src for src, _ in pairs}) != len(pairs):
        raise ClothingError("--texture-letter names a letter twice")
    return pairs


# --- owner scan (one scan per run, shared with convert_override) -----------------------------------------------


class Owners:
    """Reviewed stock owner queries, cached for one conversion."""

    def __init__(self, args):
        self.stock = stock_members.from_args(args)
        self.found: dict[str, list[str]] = {}

    def need(self, names: set[str]) -> None:
        missing = names - set(self.found)
        if missing:
            self.found |= self.stock.scan(missing)

    def members(self, folder: str, pattern: re.Pattern) -> dict[str, str]:
        """{member name: the copy the game shows} for names in `folder` matching pattern."""
        out = {}
        for name, copies in self.found.items():
            mine = [c for c in copies if co.inner_path(c) == f"{folder}/{name}"]
            if mine and pattern.fullmatch(name):
                out[name] = max(mine, key=co.rank)
        return out


def fetch(game: co.Game, cache: dict[str, bytes], located: str) -> bytes:
    if located not in cache:
        cache[located] = game.fetch(located)
    return cache[located]


def ped_skeleton(game: co.Game, owners: Owners, cache: dict, folder: str) -> tuple[list[dict], str]:
    import convert_pc_ped as ped

    name = f"{folder}.pft"
    owners.need({name})
    copies = [c for c in owners.found.get(name, []) if co.inner_path(c) == name]
    if not copies:
        raise ClothingError(f"{name} is not in your game copy")
    located = max(copies, key=co.rank)
    return ped.fragment_skeleton(fetch(game, cache, located)), located


def rig_fits(source: list[dict], pft: list[dict], own: list[dict]) -> str | None:
    """None when convert_pc_drawable's ped route maps `source` onto this template (its own skeleton, else the
    ped's), else the reason."""
    import convert_pc_drawable as cpd

    try:
        cpd.compare_ped_skeleton(source, pft, own, [])
    except AssetError as error:
        return str(error).removeprefix("ped route: ")
    return None


# --- story route -----------------------------------------------------------------------------------------------


def story(args, files: dict[str, ModFile], notes: list[str]) -> int:
    import convert_pc_ped as ped

    folder, comp = args.ped, args.comp
    if args.drawable is None or len(args.drawable) != 1 or not args.drawable[0].isdigit():
        raise ClothingError(f"{folder}: give the stock drawable the mod replaces: --drawable N (Wardrobe Style N)")
    number = int(args.drawable[0])
    specs = [args.source_drawable] if args.source_drawable else []
    candidates = find_parts(files, comp, specs)
    if not specs and len(candidates) > 1:
        same = [p for p in candidates if p.number == number]
        if len(same) != 1:
            have = ", ".join(str(p.number) for p in candidates)
            raise ClothingError(f"the mod has {comp} drawables {have}: pick one with --from N")
        candidates = same
    part = candidates[0]
    owners = Owners(args)
    pattern = re.compile(rf"{comp}_(\d{{3}})_([ur])\.pdd")
    scope = owners.stock.clothing.get(folder, {}).get(comp)
    if scope is None or number not in scope["targets"]:
        raise ClothingError(f"{folder} {comp} drawable {number}: not in the reviewed clothing scope")
    wanted = set(scope["members"])
    print(f"clothing: {STORY[folder]} ({folder}) {WARDROBE[comp]} {number}: reviewed {owners.stock.target} inputs")
    owners.need(wanted | {f"{folder}.pft"})
    stock = owners.members(folder, pattern)
    target = next((n for n in (f"{comp}_{number:03d}_u.pdd", f"{comp}_{number:03d}_r.pdd") if n in stock), None)
    if target is None:
        have = sorted(int(n[5:8]) for n in stock)
        raise ClothingError(
            f"{folder} has no {WARDROBE[comp]} drawable {number} (it has 0..{max(have)})"
            if have
            else f"{folder} has no {WARDROBE[comp]} drawables"
        )
    stem = target[:-4]
    letters = {
        m.group(3): m.group(4)
        for name in owners.members(folder, re.compile(rf"{comp}_diff_{number:03d}_[a-z]_[a-z]{{3}}\.ptd"))
        if (m := DIFFUSE_FILE.fullmatch(name))
    }
    game = owners.stock
    cache: dict[str, bytes] = {}
    like = None
    if part.ps5:
        source_key = None
        texture_letters(files, part, entry_name(part.file.blob, part.entry))
    else:
        keys = dictionary_keys(part.file.blob)
        source_key = entry_name(part.file.blob, part.entry or (stem if joaat(stem) in keys else None))
        if len(keys) > 1 and source_key != stem:
            raise ClothingError(
                f"{part.file.name}: {len(keys)} entries; a story override converts a one-entry dictionary "
                f"(or the entry {stem}): export {source_key} alone"
            )
        texture_letters(files, part, source_key)
        bones = entry_skeleton(part.file.blob, joaat(source_key))
        if not bones:
            raise ClothingError(
                f"{part.file.name}: its drawable has no skeleton of its own (a freemode-style or add-on ped part), "
                f"not a {STORY[folder]} component"
            )
        pft, _ = ped_skeleton(game, owners, cache, folder)
        reasons = {}
        order = [target, *sorted(n for n in stock if n != target)]
        fitting = []
        for name in order:
            blob = fetch(game, cache, stock[name])
            own = entry_skeleton(blob, joaat(name[:-4]))
            why = rig_fits(bones, pft, own)
            if why is None:
                fitting.append((name != target, ped.rig_key(own) != ped.rig_key(bones), name))
                if name == target:
                    break
            else:
                reasons[name] = why
        if not fitting:
            raise ClothingError(
                f"skeleton mismatch: the mod's {comp} rig ({len(bones)} bones) fits no reviewed {folder} {comp} template "
                f"({stem}: {reasons[target]})"
            )
        chosen = min(fitting)[2]
        if chosen != target:
            like = f"{target}={chosen}"
            notes.append(
                f"{target}: the mod's rig does not fit its skeleton ({reasons[target]}); converted on {chosen} "
                "(its skeleton fits; --like)"
            )
    pairs = letter_map(args.texture_letter, sorted(part.textures), story=True)
    kept = []
    for src, dst in pairs:
        if dst not in letters:
            notes.append(
                f"letter {src}: stock {WARDROBE[comp]} {number} has textures {''.join(sorted(letters))}; left out"
            )
            continue
        kept.append((src, dst))
    if not kept:
        raise ClothingError(
            f"none of the mod's letters fits stock {WARDROBE[comp]} {number} ({''.join(sorted(letters))})"
        )
    left = sorted(set(letters) - {d for _, d in kept})
    if left:
        notes.append(
            f"stock letters {''.join(left)} not shipped: Texture {', '.join(str(ord(x) - 97) for x in left)} keep the "
            "stock prints (painted for the stock shape)"
        )
    if 1 + len(kept) > co.OVERRIDE_MAX:
        raise ClothingError(f"{1 + len(kept)} override rows; at most {co.OVERRIDE_MAX}: keep fewer --texture-letter")
    # The files convert_override reads, under their stock names (its own work directory stays its own).
    stage = assets_dir(ROOT) / f"convert-{args.id}-clothing"
    if stage.exists():
        raise ClothingError(f"{cv.rel(stage)} exists; choose a new --id or remove it")
    stage.mkdir(parents=True)

    def staged(name: str, blob: bytes) -> Path:
        (stage / name).write_bytes(blob)
        return stage / name

    members = [f"{folder}/{target}={staged(f'{stem}.{part.file.name[-3:]}', part.file.blob)}"]
    renamed = []
    for src, dst in kept:
        name = f"{comp}_diff_{number:03d}_{dst}_{letters[dst]}"
        holder, inner = part.textures[src], part.texture_names[src]
        if holder.name.endswith(".ptd"):
            path = staged(f"{name}.ptd", holder.blob)
        elif (holder.name == f"{name}.ytd" and single_texture(holder.blob) == name) or args.dry_run:
            path = staged(f"{name}.ytd", holder.blob)  # the mod's file as is (convert_override's .ytd route)
        else:
            path = staged(f"{name}.ptd", diffuse_ptd(holder, inner, name, args, stage))
            renamed.append(f"{inner}->{name}")
        members.append(f"{folder}/{name}.ptd={path}")
    if renamed:
        notes.append(f"diffuse textures renamed for {stem} (one texture each, writer route): {', '.join(renamed)}")
    archive = args.archive or f"gmcl_{args.id.removeprefix('gtavmenu-').replace('-', '_')}"[:59] + ".rpf"
    argv = ["--id", args.id, "--folder", folder, "--archive", archive, "--no-spawn", "--target", args.target]
    for flag, value in (
        ("--game", args.game),
        ("--stock-cache", args.stock_cache),
        ("--stock-manifest", args.stock_manifest),
    ):
        if value is not None:
            argv += [flag, str(value)]
    argv += [x for m in members for x in ("--member", m)]
    if like:
        argv += ["--like", like]
    for flag, value in (
        ("--templates", args.templates),
        ("--reference-dir", args.reference_dir),
        ("--output-root", args.output_root),
        ("--max-texture-size", args.max_texture_size),
    ):
        if value is not None:
            argv += [flag, str(value)]
    argv += ["--dry-run"] * args.dry_run
    for note in notes:
        print(f"     note: {cvr.shown(note)}")
    print(
        f"     rows: {1 + len(kept)} ({folder}/{target} <- {part.file.name}"
        + (f" entry {source_key}" if source_key else "")
        + f"; letters {', '.join(s if s == d else f'{s}->{d}' for s, d in kept)})"
    )
    try:
        status = co.main(argv)
    finally:
        if args.dry_run:
            shutil.rmtree(stage, ignore_errors=True)
    if status:
        raise ClothingError("convert_override stopped (its message is above)")
    print(
        f"in game: play {STORY[folder]}, Load while he wears another {WARDROBE[comp]} drawable; then Self > Wardrobe > "
        f"Slot {WARDROBE[comp]} > Style {number} (toast '{WARDROBE[comp]} style {number + 1}/N')"
    )
    return 0


def single_texture(blob: bytes) -> str | None:
    import convert_pc_drawable as cpd

    try:
        textures, _ = cpd.ytd_textures(blob, None)
    except AssetError:
        return None
    return textures[0]["name"].lower() if len(textures) == 1 else None


def diffuse_ptd(holder: ModFile, inner: str, name: str, args, work: Path) -> bytes:
    """A one-texture PS5 dictionary: the mod's texture `inner` of `holder` named `name` (writer route)."""
    import convert_pc_ped as ped
    import convert_pc_ytd_writer

    path = work / "src" / holder.name
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(holder.blob)
    size = DEFAULT_TEXTURE_MAX if args.max_texture_size is None else args.max_texture_size or 16384
    found = ped.prepared_textures(path, size, None, keep=lambda n: n.lower() == inner)
    if len(found) != 1:
        raise ClothingError(f"{holder.name}: texture {inner} is not readable")
    blob, _rows = convert_pc_ytd_writer.convert({"textures": [found[0] | {"name": name}]}, args.templates)
    return blob


# --- freemode route --------------------------------------------------------------------------------------------


def rekey(blob: bytes, key: str) -> bytes:
    """A one-entry PS5 dictionary under key `key` (unchanged bytes when it already is)."""
    import native_drawable_dictionary as ndd

    graph = ndd.parse(blob)
    entries = graph.entries()
    if len(entries) != 1:
        raise ClothingError(f"a streamed component dictionary holds one entry; this one has {len(entries)}")
    if entries[0][0] == joaat(key):
        return blob
    graph.root.refs[0x20][0].data = bytearray(struct.pack("<I", joaat(key)))
    graph.collect()
    return ndd.write(graph, "pack")


def carriers(args, c: dict, template) -> list:
    """Retail ped entries lending shader schemas the template lacks (fetch-templates cache: cop, juggernaut)."""
    import convert_pc_drawable as cpd
    import native_drawable_dictionary as ndd

    out = []
    for name in (COP, JUGGERNAUT):
        path = (args.templates or assets_dir(ROOT)) / name
        if path.is_file():
            graph = ndd.parse(path.read_bytes())
            names = key_names()
            for key, _ in graph.entries():
                if key in names:
                    out.append(cpd.Template(ndd.entry_resource(graph, names[key]), c, f"{name}:{names[key]}"))
    return out


def linked_textures(
    part: Part, wanted: list[str], embedded: list[dict], files: dict[str, ModFile], work: Path, max_size: int
) -> list[dict]:
    """Resolve non-diffuse links from the drawable and its associated texture dictionaries.

    Embedded textures keep their existing precedence. External definitions of a needed name
    must agree on the pixels and dimensions that will be written, even across diffuse variants.
    """
    import convert_pc_ped as ped

    have = {t["name"].lower(): t for t in embedded}
    needed = set(wanted) - have.keys()
    if needed:
        holders = {}
        own = files.get(f"{part.stem}.ytd")
        if own is not None:
            holders[own.name] = own
        for letter in sorted(part.textures):
            holder = part.textures[letter]
            if holder.name.endswith(".ytd"):
                holders.setdefault(holder.name, holder)
        identities = {}
        for holder in holders.values():
            path = work / "src" / holder.name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(holder.blob)
            for texture in ped.prepared_textures(path, max_size, None, keep=lambda n: n.lower() in needed):
                name = texture["name"].lower()
                identity = (
                    texture["width"],
                    texture["height"],
                    texture["format"],
                    texture["mipLevels"],
                    tuple(texture["mips"]),
                )
                previous = identities.get(name)
                if previous is not None and previous[0] != identity:
                    raise ClothingError(
                        f"{part.file.name}: conflicting texture {name} ({previous[1]}, {holder.origin})"
                    )
                identities.setdefault(name, (identity, holder.origin))
                have.setdefault(name, texture)
    missing = [n for n in wanted if n not in have]
    if missing:
        raise ClothingError(
            f"{part.file.name}: the drawable names textures the mod does not ship: {', '.join(missing)}"
        )
    return [have[n] for n in wanted]


def convert_part(args, c: dict, part: Part, key: str, new: str, template_blob: bytes, template_key: str, pft) -> tuple:
    """A PC drawable as the freemode drawable `new`: (pdd bytes, report). The template entry keeps its header;
    palettes are baked onto the ped skeleton (an entry without its own skeleton copy indexes the freemode
    skeleton itself); textures its shaders name other than the diffuse are embedded."""
    import convert_pc_drawable as cpd
    import convert_pc_ped as ped
    import native_drawable_dictionary as ndd

    graph = ndd.parse(template_blob)
    template = cpd.Template(
        ndd.entry_resource(graph, template_key, high_only=True, strip_textures=True), c, template_key
    )
    src, dst = DRAWABLE.fullmatch(key), DRAWABLE.fullmatch(new)
    renames = {
        part.texture_names[x]: f"{dst.group(1)}_diff_{dst.group(2)}_{x}_uni"
        for x in part.textures
        if x in part.texture_names
    }
    renames |= {
        f"{src.group(1)}_diff_{src.group(2)}_{x}_{r}": f"{dst.group(1)}_diff_{dst.group(2)}_{x}_uni"
        for x in "abcdefghijklmnopqrstuvwxyz"
        for r in RACES
    }
    extra: list = []
    for attempt in range(2):
        try:
            blob, report = cpd.convert(
                part.file.blob,
                template,
                extra,
                c,
                entry=key,
                ped_skeleton=pft,
                source_skeleton=pft,
                texture_names=renames,
            )
            break
        except AssetError as error:
            text = str(error)
            if text.startswith("ped route:"):
                raise ClothingError(
                    f"skeleton mismatch: the mod is not rigged for {args.ped} ({text.removeprefix('ped route: ')})"
                ) from None
            if SHADER_MISSING not in text or attempt:
                raise
            extra = carriers(args, c, template)
    embedded = report.pop("_embedded", [])
    diffuse = {f"{dst.group(1)}_diff_{dst.group(2)}_{x}_uni" for x in "abcdefghijklmnopqrstuvwxyz"}
    wanted = sorted(
        {row["name"].lower() for row in report.get("textureLinks", [])} - diffuse - GLOBAL_TEXTURES - {"", "-", "?"}
    )
    size = DEFAULT_TEXTURE_MAX if args.max_texture_size is None else args.max_texture_size or 16384
    textures = linked_textures(part, wanted, embedded, args.files or {}, args.work, size)
    txd = None
    if wanted:
        ns = SimpleNamespace(max_texture_size=args.max_texture_size or DEFAULT_TEXTURE_MAX, templates=args.templates)
        txd, report["embeddedDictionary"] = ped.embedded_dictionary(textures, ns)
    converted = ped.assemble(blob, txd)
    ndd.put_drawable(graph, template_key, converted)
    graph.root.refs[0x20][0].data = bytearray(struct.pack("<I", joaat(new)))
    graph.collect()
    out = ndd.write(graph, "pack")
    after = ndd.parse(out)
    fresh = ped.assemble(blob, txd)
    for obj in fresh.objects:
        if obj.kind == "index-data":
            obj.region = "gfx"
    if [k for k, _ in after.entries()] != [joaat(new)] or ndd.subgraph_compare(fresh.root, after.entries()[0][1]):
        raise ClothingError(f"{new}: the written dictionary does not read back as the converted drawable")
    report["dictionary"] = {"key": new, "bytes": len(out), "sha256": hashlib.sha256(out).hexdigest()}
    report["embedded"] = wanted
    return out, report


def freemode(args, files: dict[str, ModFile], notes: list[str]) -> int:
    import convert_pc_drawable as cpd

    folder, comp = args.ped, args.comp
    dlc_pack, inner_archive, pmt_stem = FREEMODE[folder]
    dlc = args.dlc or f"gm{joaat(args.id):08x}"
    if not re.fullmatch(r"[a-z0-9_]{1,12}", dlc):
        raise ClothingError(f"--dlc {cvr.shown(dlc)}: [a-z0-9_], at most 12 characters (the retail name's length)")
    full = f"{folder}_{dlc}"
    parts = find_parts(files, comp, args.drawable or [])
    keys = []
    for part in parts:
        key = entry_name(part.file.blob, part.entry)
        texture_letters(files, part, key)
        keys.append(key)
    plans = []
    for i, (part, key) in enumerate(zip(parts, keys, strict=True)):
        pairs = letter_map(args.texture_letter, sorted(part.textures), story=False)
        plans.append((part, key, f"{comp}_{i:03d}_u", pairs))
    print(f"clothing: {folder} {WARDROBE[comp]}: {len(plans)} new drawable(s) in {full} (game copy {args.game})")
    for part, key, new, pairs in plans:
        print(f"     {new} <- {part.file.name} entry {key}; textures {', '.join(f'{s}->{d}' for s, d in pairs)}")
    owners = Owners(args)
    scope = owners.stock.clothing.get(folder, {}).get(comp)
    if scope is None:
        raise ClothingError(f"{folder} {comp}: not in the reviewed clothing scope")
    template_names = set(scope["members"])
    pmt_name = f"{pmt_stem}.pmt"
    owners.need(template_names | {pmt_name, f"{folder}.pft"})
    pmt_copy = [
        c
        for c in owners.found.get(pmt_name, [])
        if f"dlcpacks/{dlc_pack}/" in c and c.endswith(f"{inner_archive}.rpf/{pmt_name}")
    ]
    if not pmt_copy:
        raise ClothingError(f"{pmt_name} ({dlc_pack}) is not in your game copy")
    stock = owners.members(folder, re.compile(rf"{comp}_000_[ur]\.pdd"))
    if args.dry_run:
        print(f"planned: pack {args.id}: {full}.pmt + {len(plans)} drawable(s); nothing written (--dry-run)")
        return 0
    game = owners.stock
    cache: dict[str, bytes] = {}
    work = assets_dir(ROOT) / f"convert-{args.id}"
    if work.exists():
        raise ClothingError(f"{cv.rel(work)} exists; choose a new --id or remove it")
    work.mkdir(parents=True)
    args.work, args.files = work, files
    out: dict[str, bytes] = {}
    report: dict = {"tool": "tools/convert_pc_clothing.py", "route": "freemode", "ped": folder, "dlc": dlc}
    rows = []
    pft = c = template_blob = template_key = None
    for part, key, new, pairs in plans:
        if part.ps5:
            pdd = rekey(part.file.blob, new)
            row = {
                "key": new,
                "from": part.file.origin,
                "shipped": "PS5 resource as is" if pdd == part.file.blob else "rekeyed",
            }
        else:
            if pft is None:
                pft, row_pft = ped_skeleton(game, owners, cache, folder)
                c = cpd.contracts(args.reference_dir or assets_dir(ROOT))
                if not stock:
                    raise ClothingError(f"{folder} has no {comp}_000 drawable to use as template")
                template_key = min(stock)[:-4]
                template_blob = fetch(game, cache, stock[min(stock)])
                report["skeleton"] = row_pft
                report["template"] = stock[min(stock)]
            pdd, conv = convert_part(args, c, part, key, new, template_blob, template_key, pft)
            row = {
                "key": new,
                "from": f"{part.file.origin}:{key}",
                "skeleton": conv["skeleton"],
                "embedded": conv["embedded"],
            }
        out[f"{full}/{new}.pdd"] = pdd
        textures = []
        for src, dst in pairs:
            name = f"{comp}_diff_{new[5:8]}_{dst}_uni"
            holder = part.textures[src]
            if holder.name.endswith(".ptd"):
                blob = holder.blob
                if args.tint is not None:
                    blob, _done = apparel.tint_texture(blob, args.tint)
            else:
                blob = diffuse_ptd(holder, part.texture_names[src], name, args, work)
            out[f"{full}/{name}.ptd"] = blob
            textures.append(
                {"letter": dst, "from": f"{holder.origin}:{part.texture_names[src]}", "member": f"{name}.ptd"}
            )
        row["textures"] = textures
        rows.append(row)
    header, payload = apparel.read_rsc7(fetch(game, cache, max(pmt_copy, key=co.rank)), pmt_name)
    pmt = apparel.write_rsc7(header, apparel.patch_pmt_drawables(payload, comp, dlc, [len(p[3]) for p in plans]))
    # Variation round trip: every drawable and diffuse the patched layout names is a member, and nothing else.
    import convert_pc_ped as ped

    names = ped.variation_names(ped.read_variations(pmt))
    listed = {f"{full}/{n}.pdd" for n in names["drawables"]} | {f"{full}/{n}.ptd" for n in names["textures"]}
    if listed != set(out) or names["alternatives"] or names["props"]:
        raise ClothingError(f"variation round trip: layout {sorted(listed)} != members {sorted(out)}")
    report["variationCheck"] = f"drawables {len(names['drawables'])}, textures {len(names['textures'])}: consistent"
    files_out = {f"{full}.pmt": pmt, **out, f"{full}_shop.meta": apparel.shop_meta(folder, dlc).encode("ascii")}
    for name, blob in files_out.items():
        path = work / "out" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(blob)
    archive = args.archive or f"gmcl_{args.id.removeprefix('gtavmenu-').replace('-', '_')}"[:59] + ".rpf"
    resources = apparel.write_pack(files_out, args.id, archive, args.output_root)
    plan = cv.Plan(False)
    plan.run("validate", cv.py("tools/validate_runtime_pack.py", resources.parent))
    report |= {
        "pmtSource": max(pmt_copy, key=co.rank),
        "drawables": rows,
        "members": {n: {"bytes": len(b), "sha256": hashlib.sha256(b).hexdigest()} for n, b in files_out.items()},
        "notes": notes,
    }
    (work / "clothing-report.json").write_text(json.dumps(report, indent=1, sort_keys=True, default=str) + "\n")
    for note in notes:
        print(f"     note: {cvr.shown(note)}")
    print(f"variations: {report['variationCheck']}")
    print(f"done: pack {cv.rel(resources.parent)}; report {cv.rel(work / 'clothing-report.json')}")
    print(
        f"in game: Load before the freemode model is used (fresh process); then Skin Changer > Freemode "
        f"{'MP Male' if folder.startswith('mp_m') else 'MP Female'} > Wardrobe > Slot {WARDROBE[comp]}: the last "
        f"{len(plans)} Style(s) are the new drawables"
    )
    return 0


# --- CLI -------------------------------------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, required=True, help="mod folder, PC dlc.rpf or .oiv package")
    parser.add_argument("--ped", required=True, help=", ".join([*STORY, *FREEMODE]))
    parser.add_argument("--slot", required=True, help="Wardrobe slot (torso, legs, shoes, tops, ...) or component")
    parser.add_argument(
        "--drawable", action="append", metavar="N|STEM[:ENTRY]", help="story: stock drawable; freemode: mod's"
    )
    parser.add_argument("--from", dest="source_drawable", metavar="N|STEM[:ENTRY]", help="story: the mod's drawable")
    parser.add_argument("--texture-letter", action="append", default=[], metavar="X[=Y][,...]")
    parser.add_argument("--id", required=True, help="pack id, e.g. gtavmenu-cli-cloth-story-v1")
    parser.add_argument("--archive", help="archive name (default gmcl_<id>.rpf)")
    parser.add_argument("--dlc", help="freemode: the DLC name of the new drawables (default gm<hash of id>)")
    parser.add_argument("--tint", type=apparel.parse_tint, help="freemode, PS5 .ptd inputs only: recolour (test aid)")
    parser.add_argument("--max-texture-size", type=int, help=f"default {DEFAULT_TEXTURE_MAX}; 0 keeps every mip")
    stock_members.arguments(parser)
    parser.add_argument(
        "--templates",
        type=Path,
        default=build_dir(ROOT) / "retail-templates",
        help="retail template cache (./menu-ctl.sh fetch-templates)",
    )
    parser.add_argument(
        "--reference-dir", type=Path, help="accepted for compatibility; layouts are published format contracts"
    )
    parser.add_argument("--output-root", type=Path, default=build_dir(ROOT) / "custom-assets")
    parser.add_argument("--dry-run", action="store_true", help="check the mod and print the plan")
    args = parser.parse_args(argv)
    for key in ("source", "templates", "game", "stock_cache", "stock_manifest", "output_root", "reference_dir"):
        if getattr(args, key) is not None:
            setattr(args, key, getattr(args, key).absolute())
    if not re.fullmatch(r"[a-z0-9]([a-z0-9-]*[a-z0-9])?", args.id) or len(args.id) > 64:
        parser.error("--id: lowercase letters, digits and '-' (at most 64)")
    if args.max_texture_size is not None and not (args.max_texture_size == 0 or 4 <= args.max_texture_size <= 16384):
        parser.error("--max-texture-size: 0 or 4..16384")
    if args.archive is not None and not (co.ARCHIVE.fullmatch(args.archive)):
        parser.error("--archive: a lowercase NAME.rpf ([a-z0-9_], at most 59 before .rpf)")
    args.files = args.work = None
    try:
        args.comp = SLOTS.get(args.slot.lower())
        if args.comp is None:
            raise ClothingError(f"--slot {cvr.shown(args.slot)}: one of {', '.join(sorted(SLOTS))}")
        if args.ped not in STORY and args.ped not in FREEMODE:
            raise ClothingError(
                f"--ped {cvr.shown(args.ped)}: {', '.join([*STORY, *FREEMODE])} (other peds: convert-override "
                "--folder for stock clothing, convert-ped for a whole add-on ped)"
            )

        if args.ped in STORY and (args.tint or args.dlc):
            raise ClothingError("--tint and --dlc are for the freemode route")
        if args.ped in FREEMODE and args.source_drawable:
            raise ClothingError("--from is for story characters; freemode takes the mod's drawables as --drawable")
        files, notes = collect(args.source)
        return (story if args.ped in STORY else freemode)(args, files, notes)
    except (
        ClothingError,
        co.OverrideError,
        cvr.ReplaceError,
        cv.ConvertError,
        apparel.ApparelError,
        AssetError,
        rpf7.Rpf7Error,
        OSError,
        ValueError,
    ) as error:
        # Retain failed work for inspection; earlier absence never proves ownership of a directory.
        print(f"convert_pc_clothing: {cvr.shown(str(error))}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

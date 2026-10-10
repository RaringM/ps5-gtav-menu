#!/usr/bin/env python3
"""Convert loose or packaged replacement assets into a stock override pack.

Stock folder ownership and overlay copies come from reviewed target metadata. Resource
inputs are hash-verified range reads from your own --game copy, or --stock-cache.
The published scope currently covers the Combat Pistol and selected Franklin clothing;
unsupported names refuse. PC vehicle replacements use convert_vehicle_replace.py.
Shared dictionaries remain refused, and resident assets must be loaded in a fresh
session before they are seen or worn. Conversion does not bypass the console's gate.

Example: convert_override.py --source MOD --id my-override --game /path/to/app0
         --templates build/retail-templates
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))
ROOT = _HERE.parent

import convert_vehicle as cv  # noqa: E402
import convert_vehicle_replace as cvr  # noqa: E402
from gtavmenu_tools import mod_collection, rpf7, stock_members  # noqa: E402
from gtavmenu_tools.asset_formats import AssetError  # noqa: E402
from gtavmenu_tools.host_paths import assets_dir, build_dir  # noqa: E402
from gtavmenu_tools.runtime_pack import NAME_MAX, OVERRIDE_MAX  # noqa: E402

PC_KINDS = {".ytd": "ptd", ".ydr": "pdr", ".yft": "pft", ".ydd": "pdd"}
PS5_KINDS = {".ptd": "ptd", ".pdr": "pdr", ".pft": "pft", ".pdd": "pdd"}
SUFFIXES = (*PC_KINDS, *PS5_KINDS)
ORDER = {"pft": 0, "pdr": 1, "pdd": 2, "ptd": 3}  # pack rows: models first, then texture dictionaries
STEM = re.compile(r"[a-z0-9_+]{1,63}")
FOLDER = re.compile(r"[a-z0-9_]{1,63}")
ARCHIVE = re.compile(r"[a-z0-9_]{1,59}\.rpf")
DRAWABLE_KEY = re.compile(r"([a-z]{4})_(\d{3})_([ur])")  # ped component keys: uppr_014_u
PLAYERS = {"player_zero": "Michael", "player_one": "Franklin", "player_two": "Trevor"}
DEFAULT_TEXTURE_MAX = 1024
SHADER_MISSING = "no retail template carries shader"


class OverrideError(RuntimeError):
    pass


@dataclass
class Source:
    """One input file: a mod file (by its stock base name) or a --member file."""

    name: str  # lower-case base name as shipped (w_pi_combatpistol.ydr) or the --member FILE name
    blob: bytes
    origin: str  # where it came from (mod path or the --member FILE)
    stock: str  # the stock base name it replaces (w_pi_combatpistol.pdr)
    path: Path | None = None  # a --member file read in place


@dataclass
class Row:
    """One stock member the pack replaces."""

    member: str  # path below its archive: w_pi_combatpistol.pdr, player_one/uppr_014_u.pdd
    source: Source
    copies: list[str] = field(default_factory=list)  # where the game has it (archive:path)
    template: str | None = None  # the copy the game shows (archive:path)
    asset: str = "other"
    from_base: bool = False  # a stock _hi drawable converted from the mod's base model

    @property
    def kind(self) -> str:
        return self.member.rsplit(".", 1)[1]

    @property
    def stem(self) -> str:
        return PurePosixPath(self.member).name.rsplit(".", 1)[0]

    @property
    def pc(self) -> bool:
        return self.source.name.endswith(tuple(PC_KINDS))


# --- 1. collect ----------------------------------------------------------------------------------------------


def stock_name(name: str) -> str:
    """The stock base name a file replaces: <stem>.ytd -> <stem>.ptd, .ydr -> .pdr, .yft -> .pft, .ydd -> .pdd."""
    stem, _, ext = name.lower().rpartition(".")
    kind = PC_KINDS.get(f".{ext}") or PS5_KINDS.get(f".{ext}")
    if kind is None or not STEM.fullmatch(stem):
        raise OverrideError(f"{cvr.shown(name)}: not a .ytd/.ydr/.yft/.ydd (or PS5 .ptd/.pdr/.pft/.pdd) stock name")
    return f"{stem}.{kind}"


def collect(source: Path | None, members: list[str]) -> tuple[list[Source], list[str]]:
    """Every input file, by the stock name it replaces, and notes."""
    notes: list[str] = []
    found: list[Source] = []
    if source is not None:
        files, origin, notes = mod_collection.collect(source, SUFFIXES)
        for name in sorted(files):
            if name == cvr.META_NAME:
                notes.append(f"{name} ({origin[name]}): metas are not applied by override packs (ignored)")
                continue
            found.append(Source(name, files[name], origin[name], stock_name(name)))
    for spec in members:
        stock, sep, file = spec.partition("=")
        path = Path(file)
        if not sep or not stock or not file:
            raise OverrideError(f"--member {cvr.shown(spec)}: give STOCK=FILE (e.g. w_pi_stungun.pdr=donor.pdr)")
        if path.is_symlink() or not path.is_file():
            raise OverrideError(f"--member {cvr.shown(spec)}: {cvr.shown(file)} is no regular file (links refused)")
        folder, _, base = stock.lower().rpartition("/")
        kind = stock_name(base).rsplit(".", 1)[1]
        if stock_name(path.name).rsplit(".", 1)[1] != kind:
            raise OverrideError(f"--member {cvr.shown(spec)}: {path.name} is not a source of a .{kind}")
        if folder and not FOLDER.fullmatch(folder):
            raise OverrideError(f"--member {cvr.shown(spec)}: one folder level [a-z0-9_] at most")
        if path.stat().st_size > cvr.MAX_FILE_BYTES:
            raise OverrideError(f"{path}: larger than {cvr.MAX_FILE_BYTES} bytes")
        found.append(Source(path.name.lower(), path.read_bytes(), str(path), stock.lower(), path))
    if not found:
        raise OverrideError("no input: give the mod (folder, dlc.rpf or .oiv) and/or --member STOCK=FILE")
    return found, notes


def select(sources: list[Source], only: list[str]) -> tuple[list[Source], list[str]]:
    """--only: keep the sources whose stock name (with or without extension, folder or not) matches."""
    if not only:
        return sources, []
    kept, notes = [], []
    for s in sources:
        base = PurePosixPath(s.stock).name
        names = {s.stock, base, base.rsplit(".", 1)[0], s.name}
        if any(fnmatch.fnmatchcase(n, p.lower()) for n in names for p in only):
            kept.append(s)
        else:
            notes.append(f"{s.name}: not selected by --only (left out)")
    if not kept:
        raise OverrideError("--only matched none of the mod's files")
    return kept, notes


# --- 2. owners -----------------------------------------------------------------------------------------------


def inner_path(located: str) -> str:
    """The member path below its innermost archive: the name the engine gives its store slot."""
    return located.partition(":")[2].rsplit(".rpf/", 1)[-1]


def asset_class(located: str, member: str) -> str:
    inner = "/" + located.partition(":")[2]
    if cvr.is_vehicle_path(located):
        return "vehicle"
    if re.search(r"/weapons[a-z0-9_]*\.rpf/", inner):
        return "weapon"
    if "/" in member and member.endswith((".pdd", ".ptd")) and "models/cdimages/" in inner:
        return "ped"
    if "/props/" in inner or re.search(r"/[a-z0-9_]*props[a-z0-9_]*\.rpf/", inner):
        return "prop"
    if "levels/gta5/" in inner:
        return "map"
    return "other"


def rank(located: str) -> tuple:
    """Which copy of a name the game shows: base archives first, then DLC packs, patch DLCs by number, the
    20xx patches; the newest one is mounted last. The worker's gate line names the actual owner."""
    if not cvr.is_overlay(located):
        return (0, 0, 0)
    label = cvr.overlay_label(located).lower()
    if m := re.fullmatch(r"patchday(\d+)(b?)ng", label):
        return (2, int(m.group(1)), int(bool(m.group(2))))
    if m := re.fullmatch(r"patch(\d{4})_(\d+)(_g9ec)?", label):
        return (3, int(m.group(1)) * 100 + int(m.group(2)), int(bool(m.group(3))))
    if "dlc_patch/" in located:
        return (4, 0, 0)
    return (1, 0, 0)


def scan(args, wanted: set[str]) -> tuple[dict[str, list[str]], str, bool]:
    stock = stock_members.from_args(args)
    found = stock.scan(wanted)
    return found, f"reviewed {stock.target} owner metadata ({stock.digest[:12]})", True


def locate(row: Row, found: dict[str, list[str]], folder: str | None, hint: str) -> str | None:
    """Pick the stock member path of a row (a refusal text, or None when placed)."""
    base = PurePosixPath(row.member).name
    copies = found.get(base, [])
    if "/" in row.member:  # --member named the folder
        copies = [c for c in copies if inner_path(c) == row.member]
    by_path: dict[str, list[str]] = {}
    for c in copies:
        by_path.setdefault(inner_path(c), []).append(c)
    if not by_path:
        return f"{row.member} is not a stock member of your game"
    if len(by_path) > 1:
        wanted = folder or next((p.split("/", 1)[0] for p in by_path if "/" in p and p.split("/", 1)[0] in hint), None)
        choice = [p for p in by_path if p.split("/", 1)[0] == wanted] if wanted else []
        if len(choice) != 1:
            folders = sorted({p.split("/", 1)[0] if "/" in p else "(none)" for p in by_path})
            return f"{base} is a stock name in {len(by_path)} places ({', '.join(folders)}): pick one with --folder"
        by_path = {choice[0]: by_path[choice[0]]}
    member, copies = next(iter(by_path.items()))
    if member.count("/") > 1:
        return f"{member}: more than one folder level (the override grammar takes one)"
    row.member, row.copies = member, copies
    row.template = max(copies, key=rank)
    row.asset = asset_class(row.template, member)
    return None


def owner_note(row: Row) -> str:
    overlays = sorted({cvr.overlay_label(c) for c in row.copies if cvr.is_overlay(c)})
    shows = f"; the game shows {cvr.overlay_label(row.template)}'s copy" if cvr.is_overlay(row.template) else ""
    if overlays and all(cvr.is_overlay(c) for c in row.copies):
        return f"{row.member}: owned by DLC ({', '.join(overlays)}){shows}"
    if overlays:
        return (
            f"{row.member}: re-shipped by {', '.join(overlays)}{shows} (DLC overlay: the worker removes that overlay "
            "node right before Load)"
        )
    return f"{row.member}: base archive owner ({row.copies[0].split(':', 1)[0]}); plain override"


def residency_advice(row: Row) -> str:
    """Where the stock asset may already be loaded (advice; the worker's gate refuses a loaded target)."""
    folder = row.member.split("/", 1)[0] if "/" in row.member else ""
    if row.asset == "vehicle":
        return cvr.residency_advice(row.stem.removesuffix("+hi").removesuffix("_hi"))
    if row.asset == "weapon":
        model = re.sub(r"(_hi|\+hi|_mag\d+)+$", "", row.stem)
        common = " The story characters and police carry WEAPON_PISTOL, so this model is often loaded."
        return (
            f"{model}: a weapon model is loaded while anyone near draws or holds it: Load with fists out, "
            "before giving or drawing that weapon, away from Ammu-Nation." + (common if model == "w_pi_pistol" else "")
        )
    if row.asset == "ped" and folder in PLAYERS:
        return (
            f"{folder} ({PLAYERS[folder]}): the clothing he wears is loaded. Load while he wears another drawable "
            "of that slot (or while playing another character); do not preview it in a shop or wardrobe first."
        )
    if row.asset == "ped":
        return f"{folder or row.stem}: peds of this model nearby keep it loaded: Load away from them."
    if row.asset in ("prop", "map"):
        return (
            f"{row.stem}: map objects near the player are loaded: Load far from where it stands (or teleport a "
            "few hundred metres away and wait ~2 min), then go there."
        )
    return f"{row.stem}: Load right after entering Story Mode, before the game shows it."


# --- 3. gates ------------------------------------------------------------------------------------------------


def plan_rows(sources: list[Source], found: dict[str, list[str]], args, scanned: bool) -> tuple[list[Row], list[str]]:
    """Rows placed on their stock members with the host gates applied: (rows, notes); refusals raise."""
    notes: list[str] = []
    refusals: list[str] = []
    rows: list[Row] = []
    by_stock: dict[str, Source] = {}
    for s in sources:
        if s.stock in by_stock and by_stock[s.stock].blob != s.blob:
            raise OverrideError(f"{s.stock} comes from two different files ({by_stock[s.stock].origin}, {s.origin})")
        by_stock.setdefault(s.stock, s)
    for s in by_stock.values():
        rows.append(Row(s.stock, s))
    # A stock HD drawable the mod does not ship takes the base model (what replace authors ship anyway).
    for row in list(rows):
        if row.kind == "pdr" and row.pc and not row.stem.endswith("_hi"):
            hd = f"{row.stem}_hi.pdr"
            if hd not in by_stock and found.get(hd):
                rows.append(Row(hd, row.source, from_base=True))
    if not scanned:
        notes.append("stock names not checked on the host (no --game and no retail listings): the console gate decides")
        for row in rows:
            if row.pc and row.kind in ("pdr", "pdd"):
                refusals.append(f"{row.member}: a PC model conversion needs the stock model as template: pass --game")
        if refusals:
            raise OverrideError("refused:\n  " + "\n  ".join(refusals))
        return rows, notes
    kept = []
    for row in rows:
        hint = row.source.origin.replace("\\", "/").lower()
        why = locate(row, found, args.folder, hint)
        if why and row.stem.endswith(("_hi", "+hi")) and why.endswith("not a stock member of your game"):
            notes.append(f"{row.member}: the stock asset has no HD member; {row.source.name} is not shipped")
            continue
        if why:
            refusals.append(why)
            continue
        if cvr.SHARED.fullmatch(row.stem):
            refusals.append(
                f"{row.member} is a shared vehicle texture dictionary: resident and overlaid, never replaceable"
            )
            continue
        if row.from_base:
            notes.append(f"{row.member}: the mod has no {row.stem}.ydr; {row.source.name} is converted on it too")
        notes.append(owner_note(row))
        kept.append(row)
    names = [r.member for r in kept]
    for twin in sorted({m for m in names if names.count(m) > 1}):
        refusals.append(f"{twin}: two files of the mod replace it; keep one (--only)")
    hd_txd = {f"{r.stem}+hi.ptd" for r in kept if r.kind == "ptd" and not r.stem.endswith("+hi")}
    for name in sorted(hd_txd - {PurePosixPath(m).name for m in names}):
        if found.get(name):
            notes.append(f"the stock asset has {name} and the pack ships none: close-ups keep the stock HD textures")
    if refusals:
        raise OverrideError("refused:\n  " + "\n  ".join(refusals))
    kept.sort(key=lambda r: (ORDER[r.kind], r.member.endswith("+hi.ptd"), r.member))
    if len(kept) > OVERRIDE_MAX:
        raise OverrideError(
            f"{len(kept)} override rows; a pack takes at most {OVERRIDE_MAX}: split it with --only (one pack per "
            "part, e.g. --only 'uppr_*' and --only 'lowr_*'), each with its own --id and --archive"
        )
    return kept, notes


# --- 4. convert ----------------------------------------------------------------------------------------------


Game = stock_members.Stock


def write(path: Path, blob: bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)
    return path


def check_rsc7(row: Row) -> None:
    if row.source.blob[:4] != b"RSC7":
        raise OverrideError(f"{row.source.origin}: a PS5 .{row.kind} must be a loose RSC7 resource")


def ydd_entry(blob: bytes, target: str) -> str:
    """The key of a PC ped .ydd to convert: the target's own key if present, else its only entry (a replace
    mod keyed by another drawable index, e.g. the Tech Fleece hoodie uppr_013_u in uppr_014_u.ydd)."""
    import convert_pc_ped as ped

    keys = ped.legacy_dictionary_keys(blob)
    if cv.joaat(target) in keys:
        return target
    m = DRAWABLE_KEY.fullmatch(target)
    if len(keys) != 1 or not m:
        raise OverrideError(f"{target}.ydd: {len(keys)} entries and none named {target}: not a one-component replace")
    names = {cv.joaat(f"{m.group(1)}_{i:03d}_{r}"): f"{m.group(1)}_{i:03d}_{r}" for i in range(1000) for r in "ur"}
    if keys[0] not in names:
        raise OverrideError(f"{target}.ydd: its entry 0x{keys[0]:08x} is no {m.group(1)} drawable key")
    return names[keys[0]]


def diffuse_renames(entry: str, target: str, rows: list[Row]) -> list[str]:
    """--rename rows for a drawable keyed `entry` written as `target`: its diffuse names follow its key
    (<comp>_diff_<nnn>_<x>_<race>); the mod's .ytd files are named for the target slot."""
    if entry == target:
        return []
    src, dst = DRAWABLE_KEY.fullmatch(entry), DRAWABLE_KEY.fullmatch(target)
    out = []
    for row in rows:
        m = re.fullmatch(rf"{dst.group(1)}_diff_{dst.group(2)}_([a-z])_([a-z]+)", row.stem)
        if row.kind == "ptd" and m:
            out += ["--rename", f"{src.group(1)}_diff_{src.group(2)}_{m.group(1)}_{m.group(2)}={row.stem}"]
    return out


def stock_file(game: Game | None, work: Path, located: str, dry_run: bool) -> Path:
    path = work / "stock" / located.replace(":", "/")
    if not dry_run and not path.exists():
        if game is None:
            raise OverrideError(f"{located}: fetching a stock template needs --game or a verified --stock-cache")
        write(path, game.fetch(located))
    return path


def shader_templates(args, game: Game | None, work: Path, found: dict[str, list[str]]) -> list[Path]:
    """--shader-template values: files as given, stock names fetched (the copy the game shows)."""
    out = []
    for value in args.shader_template:
        if Path(value).is_file():
            out.append(Path(value).resolve())
            continue
        name = stock_name(value)
        copies = found.get(name, [])
        if not copies:
            raise OverrideError(f"--shader-template {cvr.shown(value)}: no file and no stock drawable of that name")
        out.append(stock_file(game, work, max(copies, key=rank), args.dry_run))
    return out


def convert_rows(plan: cv.Plan, args, rows: list[Row], found: dict[str, list[str]], work: Path) -> dict[str, Path]:
    """Every row's PS5 member file: {member: path}."""
    game = stock_members.from_args(args) if not args.dry_run else None
    out_dir, mod_dir = work / "out", work / "mod"
    trim = DEFAULT_TEXTURE_MAX if args.max_texture_size is None else args.max_texture_size
    outputs: dict[str, Path] = {}
    done: dict[tuple, Path] = {}  # (kind, source sha, template) -> output: identical inputs convert once
    extra = shader_templates(args, game, work, found)
    pdr_templates = {
        r.member: stock_file(game, work, r.template, args.dry_run) for r in rows if r.kind == "pdr" and r.pc
    }

    def source_file(row: Row) -> Path:
        if row.source.path is not None:
            return row.source.path
        path = mod_dir / row.source.name
        if not args.dry_run and not path.exists():
            write(path, row.source.blob)
        return path

    def key(row: Row) -> tuple:
        return (row.kind, hashlib.sha256(row.source.blob).hexdigest(), row.template if row.kind != "ptd" else "")

    for row in rows:
        target = out_dir / row.member
        if row.member in outputs:  # a .ytd converted with its drawable
            continue
        if not row.pc:
            check_rsc7(row)
            plan.steps.append(f"copy {row.source.origin} -> {row.member}")
            print(f"[{len(plan.steps):02}] {row.member} = {row.source.origin} (PS5 resource, shipped as is)")
            if not args.dry_run:
                write(target, row.source.blob)
            outputs[row.member] = target
        elif key(row) in done:
            plan.steps.append(f"reuse {done[key(row)].name} -> {row.member}")
            print(f"[{len(plan.steps):02}] {row.member} = {cv.rel(done[key(row)])} (same source bytes)")
            if not args.dry_run:
                write(target, done[key(row)].read_bytes())
            outputs[row.member] = target
        elif row.kind == "pdr":
            pair = next((r for r in rows if r.kind == "ptd" and r.stem == row.stem and r.pc), None)
            command = ["--source", source_file(row), "--template", pdr_templates[row.member]]
            if pair is not None and pair.member not in outputs:
                command += ["--ytd", source_file(pair), "--ptd-output", out_dir / pair.member]
                if trim:
                    command += ["--max-texture-size", str(trim)]
                if args.templates is not None:
                    command += ["--templates", args.templates]
            command += ["--output", target]
            tried = [list(extra)]
            if not extra:  # on a missing shader schema, borrow it from the set's other stock drawables
                tried += [[p] for m, p in pdr_templates.items() if m != row.member]
            for attempt, carriers in enumerate(tried):
                shown = [x for p in carriers for x in ("--shader-template", p)]
                try:
                    plan.run(
                        f"drawable {row.member}", cv.py("convert_pc_drawable.py", *command[:4], *shown, *command[4:])
                    )
                    break
                except cv.ConvertError as error:
                    if SHADER_MISSING not in str(error) or attempt + 1 == len(tried):
                        hint = (
                            " (pass --shader-template <a stock drawable that uses it>)"
                            if SHADER_MISSING in str(error)
                            else ""
                        )
                        raise cv.ConvertError(f"{error}{hint}") from None
                    print(f"     {row.member}: retrying with {cv.rel(tried[attempt + 1][0])} as shader template")
            outputs[row.member] = target
            if pair is not None and pair.member not in outputs:
                outputs[pair.member] = out_dir / pair.member
                done[key(pair)] = out_dir / pair.member
            done[key(row)] = target
        elif row.kind == "ptd":
            command = ["--ytd", source_file(row), "--output", target]
            if trim:
                command += ["--max-size", str(trim)]
            if args.templates is not None:
                command += ["--templates", args.templates]
            if not args.dry_run:
                target.parent.mkdir(parents=True, exist_ok=True)
            plan.run(f"textures {row.member}", cv.py("convert_pc_ytd_writer.py", *command))
            outputs[row.member] = done[key(row)] = target
        elif row.kind == "pdd":
            outputs[row.member] = done[key(row)] = convert_component(plan, args, row, rows, game, work, target)
        else:  # .yft of a non-vehicle stock model
            raise OverrideError(
                f"{row.source.name}: a PC fragment of a non-vehicle stock model ({row.asset}); no converter for "
                "non-vehicle fragments yet: give a converted .pft with --member"
            )
    return outputs


def convert_component(plan: cv.Plan, args, row: Row, rows: list[Row], game, work: Path, target: Path) -> Path:
    """A PC ped component .ydd onto the stock dictionary of its slot (convert_pc_ped.py component)."""
    if row.asset != "ped" or "/" not in row.member:
        raise OverrideError(f"{row.member}: a .pdd override needs a streamed ped component (<ped>/<key>.pdd)")
    folder = row.member.split("/", 1)[0]
    archive = row.template.rsplit(".rpf/", 1)[0] + ".rpf"
    pft = stock_file(game, work, f"{archive}/{folder}.pft", args.dry_run)
    pdd = stock_file(game, work, row.template, args.dry_run)
    entry = row.stem if args.dry_run else ydd_entry(row.source.blob, row.stem)
    source = work / "mod" / row.source.name
    if row.source.path is not None:
        source = row.source.path
    elif not args.dry_run:
        write(source, row.source.blob)
    command = ["component", "--pft", pft, "--reference-dir", args.reference_dir]
    command += ["--templates", args.templates or assets_dir(ROOT)]
    command += ["--ydd", source, "--entry", entry, "--pdd", pdd, "--target", row.stem]
    like = dict(spec.lower().split("=", 1) for spec in args.like)
    if row.member in like or PurePosixPath(row.member).name in like:
        other = like.get(row.member) or like[PurePosixPath(row.member).name]
        other = other if "/" in other else f"{folder}/{other}"
        located = f"{archive}/{other}"
        command += ["--template-pdd", stock_file(game, work, located, args.dry_run)]
        command += ["--template-entry", PurePosixPath(other).name.rsplit(".", 1)[0]]
    command += diffuse_renames(entry, row.stem, rows)
    command += ["--output", target]
    if args.max_texture_size is not None and args.max_texture_size != DEFAULT_TEXTURE_MAX:
        command += ["--max-texture-size", str(args.max_texture_size or 16384)]
    if not args.dry_run:
        target.parent.mkdir(parents=True, exist_ok=True)
    plan.run(f"ped component {row.member} (entry {entry})", cv.py("convert_pc_ped.py", *command))
    return target


# --- 5. pack -------------------------------------------------------------------------------------------------


def default_spawn(rows: list[Row], name: str | None) -> str | None:
    """A Custom Packs row for the replaced asset: the weapon (w_<class>_<name> -> weapon_<name>), the
    vehicle, or the prop; none for clothing and textures."""
    models = [r for r in rows if r.kind in ("pdr", "pft") and not r.stem.endswith("_hi")]
    if not models:
        return None
    first = min(models, key=lambda r: len(r.stem))
    if first.asset == "weapon" and (m := re.fullmatch(r"w_[a-z]{2}_([a-z0-9_]+)", first.stem)):
        return f"weapon:weapon_{m.group(1)}={name or m.group(1).replace('_', ' ').title() + ' (replace)'}"
    if first.asset == "vehicle":
        return f"vehicle:{first.stem}={name or first.stem + ' (replace)'}"
    if first.asset in ("prop", "map"):
        return f"object:{first.stem}={name or first.stem[:30] + ' (replace)'}"
    return None


def pack(plan: cv.Plan, args, rows: list[Row], outputs: dict[str, Path], archive: str, spawn: str | None) -> Path:
    members = [r.member for r in rows]
    tool = "tools/build_runtime_pack.py"
    command = cv.py(tool, "--id", args.id, "--archive", archive)
    for member in members:
        command += ["--member", f"{member}={outputs[member]}"]
    command += ["--override-archive", f"{archive}=" + ",".join(members)]
    if spawn:
        command += ["--spawn", spawn]
    command += ["--output-root", args.output_root]
    command += ["--plain-toc"]
    plan.run("pack (overlay archive, override rows)", command)
    out = args.output_root / args.id
    plan.run("validate", cv.py("tools/validate_runtime_pack.py", out))
    return out


# --- pipeline ------------------------------------------------------------------------------------------------


def vehicle_route(args, rows: list[Row], found: dict[str, list[str]]) -> int:
    """A stock vehicle's PC fragments: the whole mod goes to convert_vehicle_replace.py (one command for every
    replace mod; its own gates, repairs and +hi row), with this run's owner scan."""
    models = sorted({r.stem.removesuffix("_hi") for r in rows if r.kind == "pft" and r.pc})
    if len(models) != 1:
        raise OverrideError(f"the mod replaces {len(models)} vehicles ({', '.join(models)}): one per pack (--only)")
    others = [r.member for r in rows if r.stem.removesuffix("_hi").removesuffix("+hi") != models[0]]
    if others:
        raise OverrideError(f"vehicle {models[0]} plus other assets ({', '.join(others)}): one pack each (--only)")
    argv = ["--source", str(args.source), "--id", args.id, "--model", models[0], "--output-root", str(args.output_root)]
    for flag, value in (("--name", args.name), ("--archive", args.archive), ("--templates", args.templates)):
        if value is not None:
            argv += [flag, str(value)]
    if args.max_texture_size:
        argv += ["--max-texture-size", str(args.max_texture_size)]
    for flag, value in (
        ("--game", args.game),
        ("--stock-cache", args.stock_cache),
        ("--stock-manifest", args.stock_manifest),
        ("--target", args.target),
    ):
        if value is not None:
            argv += [flag, str(value)]
    argv += [x for r in args.repair for x in ("--repair", r)]
    argv += ["--no-hd-textures"] * args.no_hd_textures + ["--single-page"] * args.single_page
    argv += ["--dry-run"] * args.dry_run
    print(f"vehicle {models[0]}: converted by convert_vehicle_replace.py (convert-replace)")
    return cvr.main(argv)


def build(args) -> int:
    if args.source is None and not args.member:
        raise OverrideError("give the mod (folder, dlc.rpf or .oiv) and/or --member STOCK=FILE")
    sources, notes = collect(args.source, args.member)
    sources, more = select(sources, args.only)
    notes += more
    archive = args.archive or f"gmovr_{args.id.removeprefix('gtavmenu-').replace('-', '_')}"[:59] + ".rpf"
    if not (ARCHIVE.fullmatch(archive) and len(archive) <= NAME_MAX):
        raise OverrideError(f"--archive {archive!r}: a lowercase NAME.rpf ([a-z0-9_], at most 59 before .rpf)")
    if args.name is not None and not (cv.LABEL_TEXT.fullmatch(args.name) and len(args.name) <= 40):
        raise OverrideError("--name: printable ASCII, at most 40 characters")
    required = {PurePosixPath(s.stock).name for s in sources}
    wanted = set(required)
    wanted |= {f"{n.rsplit('.', 1)[0]}_hi.pdr" for n in wanted if n.endswith(".pdr") and not n.endswith("_hi.pdr")}
    wanted |= {f"{n.rsplit('.', 1)[0]}+hi.ptd" for n in wanted if n.endswith(".ptd") and not n.endswith("+hi.ptd")}
    wanted |= {stock_name(v) for v in args.shader_template if not Path(v).is_file()}
    stems = {n.rsplit(".", 1)[0] for n in wanted if n.endswith(".pft")}
    wanted |= {f"{s.removesuffix('_hi')}{x}" for s in stems for x in (".pft", "_hi.pft", ".ptd", "+hi.ptd")}
    # Optional companions outside the reviewed set are unknown, never declared absent.
    index = stock_members.from_args(args)
    optional_unknown = (wanted - required) - index.names.keys() - index.absent.keys()
    for name in sorted(optional_unknown):
        notes.append(f"{name}: optional companion ownership is not reviewed; not inferred or added")
    wanted -= optional_unknown
    found, scope, scanned = scan(args, wanted)
    print(f"override: stock check against {scope}")
    if any(s.name.endswith(".yft") for s in sources) and scanned:
        for s in sources:
            if s.name.endswith(".yft") and any(cvr.is_vehicle_path(c) for c in found.get(s.stock, [])):
                return vehicle_route(args, [Row(x.stock, x) for x in sources], found)
    rows, gate_notes = plan_rows(sources, found, args, scanned)
    spawn = None if args.no_spawn else (args.spawn or default_spawn(rows, args.name))
    for note in notes + gate_notes:
        print(f"     note: {cvr.shown(note)}")
    advice = sorted({residency_advice(r) for r in rows}) if scanned else []
    for line in advice:
        print(f"     residency: {cvr.shown(line)}")
    print(f"     rows: {len(rows)} ({', '.join(r.member for r in rows)})" + (f"; spawn {spawn}" if spawn else ""))

    plan = cv.Plan(args.dry_run)
    work = assets_dir(ROOT) / f"convert-{args.id}"
    if not args.dry_run and work.exists():
        raise OverrideError(f"{cv.rel(work)} exists; choose a new --id or remove it")
    outputs = convert_rows(plan, args, rows, found, work)
    out = pack(plan, args, rows, outputs, archive, spawn)
    if not args.dry_run:
        report = {
            "tool": "tools/convert_override.py",
            "pack": cv.rel(out),
            "archive": archive,
            "stockScan": scope,
            "rows": [
                {
                    "member": r.member,
                    "source": r.source.origin,
                    "sourceSha256": hashlib.sha256(r.source.blob).hexdigest(),
                    "asset": r.asset,
                    "stock": r.copies,
                    "template": r.template,
                    "outputSha256": hashlib.sha256(outputs[r.member].read_bytes()).hexdigest(),
                }
                for r in rows
            ],
            "spawn": spawn,
            "notes": notes + gate_notes,
            "residency": advice,
            "steps": plan.steps,
        }
        write(work / "override-report.json", (json.dumps(report, indent=1, sort_keys=True) + "\n").encode())
    print(f"{'planned' if args.dry_run else 'done'}: {len(plan.steps)} steps; pack {cv.rel(out)}")
    print("in game: Custom Packs -> Load (see residency above); Manage Packs -> Revert overrides puts the stock back")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, help="mod folder, PC dlc.rpf or .oiv package")
    parser.add_argument(
        "--member", action="append", default=[], metavar="STOCK=FILE", help="a loose file for a stock member"
    )
    parser.add_argument("--id", required=True, help="pack id, e.g. gtavmenu-ovr-glock-v1")
    parser.add_argument("--only", action="append", default=[], metavar="PATTERN", help="stock names to keep (fnmatch)")
    parser.add_argument("--folder", help="the stock folder of ped components found in several (e.g. player_one)")
    parser.add_argument("--archive", help="overlay archive name (default gmovr_<id>.rpf)")
    parser.add_argument("--name", help="spawn row text")
    parser.add_argument("--spawn", metavar="KIND:MODEL=TEXT", help="the spawn row (default: weapon/vehicle/prop)")
    parser.add_argument("--no-spawn", action="store_true", help="no spawn row")
    parser.add_argument(
        "--shader-template", action="append", default=[], metavar="STOCK.pdr|FILE", help="shader schemas for .ydr"
    )
    parser.add_argument(
        "--like", action="append", default=[], metavar="STOCK.pdd=OTHER.pdd", help="ped template entry (rig match)"
    )
    parser.add_argument(
        "--max-texture-size", type=int, help=f"drop larger mips (default {DEFAULT_TEXTURE_MAX}; 0 keeps every mip)"
    )
    stock_members.arguments(parser)
    parser.add_argument(
        "--templates",
        type=Path,
        default=build_dir(ROOT) / "retail-templates",
        help="retail template cache (./menu-ctl.sh fetch-templates)",
    )
    parser.add_argument(
        "--reference-dir",
        type=Path,
        default=assets_dir(ROOT),
        help="accepted for compatibility; layouts are published format contracts",
    )
    parser.add_argument("--output-root", type=Path, default=build_dir(ROOT) / "custom-assets")
    parser.add_argument("--dry-run", action="store_true", help="check the mod and the gates, print the steps")
    vehicle = parser.add_argument_group("vehicle replace mods (passed to convert_vehicle_replace.py)")
    vehicle.add_argument("--repair", action="append", default=[], metavar="NAME[=BONE,...]")
    vehicle.add_argument("--no-hd-textures", action="store_true")
    vehicle.add_argument("--single-page", action="store_true")
    args = parser.parse_args(argv)
    for key in ("source", "templates", "game", "stock_cache", "stock_manifest", "output_root", "reference_dir"):
        if getattr(args, key) is not None:
            setattr(args, key, getattr(args, key).absolute())
    if not re.fullmatch(r"[a-z0-9]([a-z0-9-]*[a-z0-9])?", args.id) or len(args.id) > 64:
        parser.error("--id: lowercase letters, digits and '-' (at most 64)")
    if args.max_texture_size is not None and not (args.max_texture_size == 0 or 4 <= args.max_texture_size <= 16384):
        parser.error("--max-texture-size: 0 or 4..16384")
    if args.spawn and args.no_spawn:
        parser.error("--spawn and --no-spawn exclude each other")
    if any("=" not in spec for spec in args.like):
        parser.error("--like takes STOCK.pdd=OTHER.pdd")
    try:
        return build(args)
    except (
        stock_members.StockError,
        mod_collection.CollectionError,
        OverrideError,
        cvr.ReplaceError,
        cv.ConvertError,
        AssetError,
        rpf7.Rpf7Error,
        OSError,
        ValueError,
    ) as error:
        print(f"convert_override: {cvr.shown(str(error))}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

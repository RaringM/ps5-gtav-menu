#!/usr/bin/env python3
"""Build a runtime pack for the worker's pack lane (archive route).

Output: <output-root>/<id>/resources/{<archive>.rpf, pack.cfg, <data>.meta}. Each archive is a PS5
RPF7 (gtavmenu_tools.rpf7) of loose RSC7 resources stored as-is (the 16-byte RSC7 header is the
opaque prefix, flags from the header). Every member is re-read and inflated before anything is
written. pack.cfg comes from gtavmenu_tools.runtime_pack, the format the worker parses. Refuses to
overwrite an existing pack directory.

Archives are written with tag OPEN and a plaintext table: the worker encrypts the table in place
with the game's own keys before the engine sees it (gtavmenu_tools.rpf_toc), so building a pack
reads nothing from the game executable. Override packs (--override-archive) keep keyed tables
unless --plain-toc is given; keyed tables and --retail-member need the game's table keys, which
this tool does not read itself. Use --plain-toc for override packs with the public tool.

  --member gmprop.ptd=path/to/gmprop.ptd   (a loose RSC7 resource, e.g. from tools/make_pmap.py)
  --card gmprop:gmprop_diffuse
  --data VEHICLE_METADATA_FILE=path/to/vehicles.meta
  --extra-archive gmparts.rpf=a80_spoil_1.pft,a80_spoil_2.pft   (move members into a further archive;
                                                                 up to 3, each registered by the worker)
  --override-archive gmovr.rpf=adder.ptd   (stock member replacement: the archive is rendered with
                                            `overlay` and holds only these members, one `override`
                                            row each; naming --archive makes archive 0 the overlay)
  --member player_one/uppr_014_u.pdd=uppr_014_u.pdd --override-archive gmfr.rpf=player_one/uppr_014_u.pdd
                                           (an override member keeps the one folder its stock archive
                                            files it under: the engine names the slot after that path)
  --description "Malibu mansion" --author Ann --version 1.2   (optional texts Custom Packs >
                                  Manage Packs shows with what the pack adds)
  --spawn weapon:weapon_gmpistol=GM Pistol   (kinds: vehicle, object, ped, weapon, timecycle, ptfx,
                                              component)
  --spawn ptfx:gm_house:scr_indep_firework_fountain=Fountain   (a particle effect of the pack
                                  dictionary member gm_house.ppt; every .ppt member needs such a
                                  row and every ptfx row a .ppt member, so the worker gates the
                                  particle store's headroom before registering the archive)
  --spawn component:weapon_gmpistol3:component_gmpistol_supp_01=GM Pistol: suppressor   (toggles a
                                  weapon component on that weapon, giving the weapon first)
  --wheel-icon WEAPON_GMPISTOL=WEAPON_COMBATPISTOL   (the weapon wheel shows the retail donor's icon
                                  for the pack weapon; every other pack weapon without an icon gets a
                                  row to its WheelSlot's default donor unless --no-default-wheel-icons)
  --tints WEAPON_GMPISTOL=none   (a `tints` row: whether a spawn weapon's model reads a tint palette; without
                                  the option, a WEAPONINFO_FILE's `<!-- gtavmenu tints=palette|none -->` hint
                                  (make_weapon_model_pack.py --tint-palette) gives the row of its weapons)
  --place=-72,-1778,28:Davis gas station   (a Custom Packs teleport row without a map, whole metres)
  --hide prop_streetlight_01=-67,-1787,27,3   (hide the stock map entities of that model within the
                                  radius (metres) of the point for the session, once the maps load)
  --retail-typ int_retail.ptyp   (a stock typ requested keep-resident before the maps: an interior
                                  clone's room props live in its itypDependencies typs)
  --typ-dep gm_int_22.ptyp=v_int_22.ptyp   (bind a --typ row's ITYP dependency on a stock typ, as a
                                  retail _manifest itypDependencies entry does: the engine streams the
                                  stock typ with the pack typ, no --retail-typ row needed)
  --map-dep gmmap_x.pmap=cityhills_03_metadata_004_strm.ptyp:retail   (a map's dependency on a stock
                                  typ that is no --typ row: the engine streams it with the map)
  --listings DIR   (text listings of the user's own retail archives: warns about override members
                    that are not plain stock members)
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import Protocol

from gtavmenu_tools import rpf7, rpf_toc, runtime_pack, weapon_wheel
from gtavmenu_tools.asset_formats import Limits, rpf_members

ROOT = Path(__file__).resolve().parents[1]
# Retail paths whose archives are (or may be) registered as overlays; a stock member found only
# there is refused by the worker's override gate.
_OVERLAY_HINTS = ("dlcpacks/", "dlc_patch/")


class TableKeys(Protocol):
    """Source of the game's archive table keys (not part of this tool; injected by a developer build)."""

    def decrypt_table(self, table: bytes, basename: str, size: int) -> bytes: ...

    def encrypt_table(self, table: bytes, basename: str, size: int) -> bytes: ...

    def table_key_index(self, basename: str, size: int) -> int: ...


def _decrypt(keys: TableKeys | None) -> rpf7.TableCipher | None:
    return keys.decrypt_table if keys is not None else None


def _member_name_ok(name: str, override: bool) -> bool:
    """A lowercase [a-z0-9_] stem and an extension. An override member may also use `+` (`+hi`) and
    one folder level ([a-z0-9_]): the folder its stock archive files it under (player_one/...)."""
    folder, slash, leaf = name.rpartition("/")
    if slash:
        plain_folder = folder.replace("_", "a")
        if not override or not (plain_folder.isalnum() and plain_folder.isascii() and folder == folder.lower()):
            return False
    stem, _, ext = leaf.rpartition(".")
    plain = stem.replace("_", "a").replace("+", "a") if override else stem.replace("_", "a")
    return bool(stem) and plain.isalnum() and plain.isascii() and stem == stem.lower() and bool(ext)


def _stock_path(listed: str) -> str:
    """The member path the engine names a stock slot after: the part of a listing path below its
    innermost archive (levels/gta5/vehicles.rpf/adder.ptd -> adder.ptd;
    models/cdimages/streamedpeds_players.rpf/player_one/uppr_014_u.pdd -> player_one/uppr_014_u.pdd)."""
    return listed.rsplit(".rpf/", 1)[-1]


def _listing_warnings(members: list[str], listings: Path | None = None) -> list[str]:
    """Warnings for override members that the retail listings do not show as plain stock members."""
    if listings is None or not listings.is_dir():
        return []
    found: dict[str, list[str]] = {m: [] for m in members}
    for listing in sorted(listings.glob("*.txt")):
        for line in listing.read_text(encoding="utf-8", errors="replace").splitlines():
            path = line.rsplit(" ", 1)[-1]
            stock = _stock_path(path)
            if stock in found:
                found[stock].append(f"{listing.stem.removeprefix('full-')}:{path}")
    warnings = []
    for member, paths in found.items():
        if not paths:
            warnings.append(f"override {member}: not in {listings} (typo, or not a stock name)")
        elif all(any(h in p for h in _OVERLAY_HINTS) for p in paths):
            warnings.append(f"override {member}: only in DLC/patch archives ({paths[0]}); the gate may refuse it")
    return warnings


def _patch_warnings(keys: TableKeys, members: list[str], retail: Path) -> list[str]:
    """Warnings for override members re-shipped by a local retail patch/DLC archive (an overlay owner)."""
    wanted = set(members)
    warnings = []

    def walk(blob: bytes, name: str, prefix: str) -> None:
        entries, _ = rpf7.read_table(blob, name, keys.decrypt_table)
        for entry, path in zip(entries[1:], rpf7.entry_paths(entries)[1:], strict=True):
            if entry["kind"] == "bin" and entry["name"].endswith(".rpf"):
                walk(blob[entry["offset"] : entry["offset"] + entry["a"]], entry["name"], f"{prefix}{path}/")
            elif entry["kind"] != "dir" and path in wanted:
                warnings.append(f"override {path}: re-shipped by {prefix}; expect the overlay gate to refuse it")

    for path in sorted(retail.glob("*-dlc.rpf")):
        try:
            walk(path.read_bytes(), "dlc.rpf", f"{path.name}:")
        except ValueError as error:
            warnings.append(f"{path.name}: not scanned for overlays ({error})")
    return warnings


def _spawn(spec: str) -> tuple[str, str, str]:
    kind, sep, rest = spec.partition(":")
    model, sep2, text = rest.partition("=")
    if not sep or not sep2 or kind not in runtime_pack.SPAWN_KINDS:
        raise SystemExit(f"--spawn {spec!r}: expected KIND:MODEL=TEXT, KIND one of {runtime_pack.SPAWN_KINDS}")
    return kind, model, text


def _find(keys: TableKeys | None, blob: bytes, archive_name: str, path: str) -> dict:
    """Resolve 'a/b/nested.rpf/member' inside an archive, descending into nested archives."""
    try:
        return rpf7.find_entry(blob, archive_name, path, _decrypt(keys))
    except rpf7.Rpf7Error as error:
        raise SystemExit(str(error)) from None


def _inflate(stored: bytes, label: str) -> bytes:
    try:
        return rpf7.inflate_resource(stored, label)
    except rpf7.Rpf7Error as error:
        raise SystemExit(str(error)) from None


def _retail_member(keys: TableKeys | None, spec: str) -> tuple[str, bytes, int, int, bytes]:
    name, _, source = spec.partition("=")
    archive_path, _, inner = source.partition(":")
    entry = _find(keys, Path(archive_path).read_bytes(), "dlc.rpf", inner)
    if entry["kind"] != "res":
        raise SystemExit(f"{inner} is not a resource")
    stored = entry["blob"][entry["offset"] : entry["offset"] + entry["size"]]
    return name, stored, entry["a"], entry["b"], _inflate(stored, inner)


def _loose_member(spec: str) -> tuple[str, bytes, int, int, bytes]:
    name, _, path = spec.partition("=")
    try:
        stored, sysf, gfxf, raw = rpf7.loose_resource(Path(path).read_bytes(), path)
    except rpf7.Rpf7Error as error:
        raise SystemExit(str(error)) from None
    return name, stored, sysf, gfxf, raw


def _map_places(specs: list[str], maps: list[str]) -> dict[str, tuple[int, int, int, str]]:
    places = {}
    for spec in specs:
        name, _, rest = spec.partition("=")
        coords, _, text = rest.partition(":")
        if name not in maps or name in places:
            raise SystemExit(f"--map-place {spec!r}: not a --map member or repeated")
        x, y, z = (int(v) for v in coords.split(","))
        places[name] = (x, y, z, text)
    return places


def _places(specs: list[str]) -> list[tuple[int, int, int, str]]:
    places = []
    for spec in specs:
        coords, sep, text = spec.partition(":")
        try:
            x, y, z = (int(v) for v in coords.split(","))
        except ValueError:
            raise SystemExit(f"--place {spec!r}: expected X,Y,Z:TEXT (whole metres)") from None
        if not sep:
            raise SystemExit(f"--place {spec!r}: expected X,Y,Z:TEXT (whole metres)")
        places.append((x, y, z, text))
    return places


def _hides(specs: list[str]) -> list[tuple[str, int, int, int, int]]:
    hides = []
    for spec in specs:
        model, _, rest = spec.partition("=")
        try:
            x, y, z, radius = (int(v) for v in rest.split(","))
        except ValueError:
            raise SystemExit(f"--hide {spec!r}: expected MODEL=X,Y,Z,RADIUS (whole metres)") from None
        hides.append((model, x, y, z, radius))
    return hides


def _wheel_icons(specs: list[str], weapons: list[str]) -> list[tuple[str, str]]:
    """`wicon` rows from WEAPON=DONOR specs: lowercase names; WEAPON must be a CWeaponInfo of the pack's
    WEAPONINFO_FILE rows, DONOR a retail weapon (the worker checks it shares the wheel slot)."""
    own = {name.lower() for name in weapons}
    rows = []
    for spec in specs:
        weapon, sep, donor = (part.strip().lower() for part in spec.partition("="))
        if not sep or not weapon or not donor:
            raise SystemExit(f"--wheel-icon {spec!r}: expected WEAPON=DONOR")
        if weapon not in own:
            raise SystemExit(f"--wheel-icon {spec!r}: {weapon} is not a CWeaponInfo of a WEAPONINFO_FILE row")
        if donor in own:
            raise SystemExit(f"--wheel-icon {spec!r}: the donor must be a retail weapon, not a pack weapon")
        rows.append((weapon, donor))
    return rows


# make_weapon_model_pack.py's weapons-meta hint (an XML comment: the engine's parser skips it).
_TINTS_HINT = re.compile(rb"<!-- gtavmenu tints=(palette|none) -->")


def _tints(specs: list[str], spawns: list[tuple[str, str, str]], data: list[tuple[str, Path]]) -> dict[str, str]:
    """`tints` rows: WEAPON=palette|none specs, then for every other spawn weapon the tints hint of the
    WEAPONINFO_FILE that holds its CWeaponInfo (the hint is per file; make_weapon_model_pack.py writes one weapon
    per file). Lowercase weapon names, as the spawn rows."""
    weapons = {model for kind, model, _ in spawns if kind == "weapon"}
    rows: dict[str, str] = {}
    for spec in specs:
        weapon, sep, value = (part.strip().lower() for part in spec.partition("="))
        if not sep or value not in runtime_pack.TINTS_VALUES:
            raise SystemExit(f"--tints {spec!r}: expected WEAPON=palette or WEAPON=none")
        if weapon not in weapons:
            raise SystemExit(f"--tints {spec!r}: {weapon} is not a --spawn weapon row")
        rows[weapon] = value
    for type_name, path in data:
        hint = _TINTS_HINT.search(path.read_bytes()) if type_name == "WEAPONINFO_FILE" else None
        if hint is None:
            continue
        for name, _ in weapon_wheel.weapon_wheel_slots(path.read_bytes()):
            if name.lower() in weapons:
                rows.setdefault(name.lower(), hint.group(1).decode())
    return rows


def _build_archive(keys: TableKeys | None, name: str, members: list, plain: bool = False) -> bytes:
    """One PS5 RPF7 of `members`, re-read and inflated before it is returned.

    plain=True: OPEN tag and plaintext table, also parsed by the production reader and the worker's
    TOC gate (gtavmenu_tools.rpf_toc.toc_span). plain=False: keyed table (needs `keys`).
    """
    if not plain and keys is None:
        raise SystemExit(f"{name}: a keyed table needs the game's table keys")
    encrypt = None if plain else keys.encrypt_table
    try:
        archive = rpf7.build([m[:4] for m in members], name, encrypt)
    except rpf7.Rpf7Error as error:
        raise SystemExit(f"{name}: {error}") from None
    if plain:
        try:
            rpf_toc.toc_span(archive)
            rpf_members(archive, Limits())
        except ValueError as error:
            raise SystemExit(f"{name}: plain table readback failed ({error})") from None
    entries, _ = rpf7.read_table(archive, name, _decrypt(keys))
    expected = {m[0]: (rpf7.oversize_prefix(m[1]) if len(m[1]) >= rpf7.OVERSIZE else m[1], m[4]) for m in members}
    folders = {m[0].rpartition("/")[0] for m in members} - {""}
    try:
        paths = rpf7.entry_paths(entries)
    except rpf7.Rpf7Error as error:
        raise SystemExit(f"{name}: readback table mismatch ({error})") from None
    files = {path: entry for entry, path in zip(entries[1:], paths[1:], strict=True) if entry["kind"] != "dir"}
    dirs = {path for entry, path in zip(entries[1:], paths[1:], strict=True) if entry["kind"] == "dir"}
    if entries[0]["kind"] != "dir" or sorted(files) != sorted(expected) or dirs != folders:
        raise SystemExit(f"{name}: readback table mismatch")
    for path, entry in files.items():
        stored, raw = expected[path]
        back = archive[entry["offset"] : entry["offset"] + entry["size"]]
        if entry["kind"] != "res" or back != stored or _inflate(back, path) != raw:
            raise SystemExit(f"{name}: readback payload mismatch for {path}")
    return archive


def _map_deps(
    specs: list[str], maps: list[str], typs: list[str], retail: Sequence[str] = ()
) -> tuple[dict[str, tuple[str, bool]], set[str]]:
    """--map-dep rows: (map -> (typ, interior), maps whose typ is a stock typ named with ':retail').

    The typ is, in this order: a --typ row; with ':retail', a stock typ that is no --typ row (never an
    interior); else another pack's own typ (bound by the worker when both packs are active). A
    --retail-typ is loaded, never bound, unless ':retail' names it."""
    deps: dict[str, tuple[str, bool]] = {}
    stock: set[str] = set()
    for spec in specs:
        name, _, rest = spec.partition("=")
        typ, _, flag = rest.partition(":")
        if name not in maps or name in deps or flag not in ("", "interior", "retail") or not typ.endswith(".ptyp"):
            raise SystemExit(f"--map-dep {spec!r}: needs a --map row, a typ and at most one dependency")
        if flag == "retail":
            if typ in typs:
                raise SystemExit(f"--map-dep {spec!r}: ':retail' names a stock typ, not a --typ row")
            stock.add(name)
        elif typ in retail:
            raise SystemExit(f"--map-dep {spec!r}: a --retail-typ is loaded, never bound (or use ':retail')")
        elif typ not in typs:
            print(f"note: --map-dep {spec!r}: {typ} is not a --typ row; it must be another active pack's typ")
        deps[name] = (typ, flag == "interior")
    return deps, stock


def _typ_deps(specs: list[str], typs: list[str]) -> dict[str, str]:
    deps: dict[str, str] = {}
    for spec in specs:
        typ, _, dep = spec.partition("=")
        if typ not in typs or typ in deps or not dep.endswith(".ptyp") or dep in typs or dep == typ:
            raise SystemExit(f"--typ-dep {spec!r}: needs a --typ row, a stock typ and at most one dependency")
        deps[typ] = dep
    return deps


def _parser(with_keys: bool, default_listings: Path | None) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--id", required=True)
    parser.add_argument("--archive", default="gtavmenu.rpf")
    parser.add_argument(
        "--description",
        default="",
        metavar="TEXT",
        help=f"what the pack is, shown in Custom Packs > Manage Packs (up to {runtime_pack.DESCRIPTION_MAX} "
        "printable characters, no '~')",
    )
    parser.add_argument(
        "--author", default="", metavar="NAME", help=f"who made it (up to {runtime_pack.AUTHOR_MAX} characters)"
    )
    parser.add_argument(
        "--version",
        default="",
        metavar="VERSION",
        help=f"its version, e.g. 1.2 (up to {runtime_pack.VERSION_MAX} characters of [A-Za-z0-9._+-])",
    )
    if with_keys:
        parser.add_argument(
            "--retail-member",
            action="append",
            default=[],
            metavar="NAME=ARCHIVE:PATH",
            help="copy a member of a keyed retail archive byte-for-byte under a new name",
        )
    parser.add_argument("--member", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--card", action="append", default=[], metavar="DICT:TEXTURE")
    parser.add_argument("--data", action="append", default=[], metavar="TYPE=PATH")
    parser.add_argument("--typ", action="append", default=[], metavar="NAME.ptyp", help="archive member to request")
    parser.add_argument(
        "--retail-typ",
        action="append",
        default=[],
        metavar="NAME.ptyp",
        help="stock typ to request keep-resident before the maps (an interior's itypDependencies)",
    )
    parser.add_argument(
        "--typ-dep",
        action="append",
        default=[],
        metavar="TYP.ptyp=STOCK.ptyp",
        help="bind a --typ row's ITYP dependency on a stock typ (one per typ row); the engine streams it",
    )
    parser.add_argument("--map", action="append", default=[], metavar="NAME.pmap", help="map member to activate")
    parser.add_argument(
        "--map-dep",
        action="append",
        default=[],
        metavar="MAP.pmap=TYP.ptyp[:interior|:retail]",
        help="bind a --map row's ITYP dependency (one per map); ':interior' places an MLO, ':retail' names "
        "a stock typ; a typ that is no --typ row (without ':retail') names another pack's typ, bound when "
        "both packs are active",
    )
    parser.add_argument(
        "--bounds",
        action="append",
        default=[],
        metavar="NAME.pbn",
        help="static collision member to load keep-resident (composite root)",
    )
    parser.add_argument(
        "--map-place",
        action="append",
        default=[],
        metavar="NAME.pmap=X,Y,Z:TEXT",
        help="Custom Packs teleport row for a --map member (whole metres)",
    )
    parser.add_argument("--label", action="append", default=[], metavar="KEY=TEXT", help="text label to add")
    parser.add_argument(
        "--place",
        action="append",
        default=[],
        metavar="X,Y,Z:TEXT",
        help="Custom Packs teleport row not tied to a map (whole metres)",
    )
    parser.add_argument(
        "--hide",
        action="append",
        default=[],
        metavar="MODEL=X,Y,Z,RADIUS",
        help="hide the stock map entities of MODEL within RADIUS metres of the point (whole metres)",
    )
    parser.add_argument(
        "--wheel-icon",
        action="append",
        default=[],
        metavar="WEAPON=DONOR",
        help="weapon wheel shows the retail DONOR weapon's icon for pack weapon WEAPON (same WheelSlot; "
        "`wicon` row, applied by the worker after the weapon loads); every other pack weapon without "
        "an icon gets its WheelSlot's default donor (gtavmenu_tools.weapon_wheel.DEFAULT_DONORS)",
    )
    parser.add_argument(
        "--no-default-wheel-icons",
        action="store_true",
        help="do not add default-donor `wicon` rows (an icon-less weapon then shows the wheel's last icon)",
    )
    parser.add_argument(
        "--tints",
        action="append",
        default=[],
        metavar="WEAPON=palette|none",
        help="`tints` row of a --spawn weapon: whether its model reads a tint palette (with none, Weapon Tint "
        "skips the setter); default: the `<!-- gtavmenu tints=... -->` hint of the weapon's WEAPONINFO_FILE",
    )
    parser.add_argument(
        "--spawn",
        action="append",
        default=[],
        metavar="KIND:MODEL=TEXT",
        help="Custom Packs menu row (vehicle|object|ped|weapon|timecycle|ptfx|component; ptfx MODEL is "
        "ASSET:EFFECT, component MODEL is WEAPON:COMPONENT)",
    )
    parser.add_argument(
        "--extra-archive",
        action="append",
        default=[],
        metavar="NAME.rpf=MEMBER[,MEMBER...]",
        help="put these members into a further archive of the pack",
    )
    parser.add_argument(
        "--override-archive",
        action="append",
        default=[],
        metavar="NAME.rpf=MEMBER[,MEMBER...]",
        help="put these members into an overlay archive that replaces the stock members of the same name"
        " (NAME may be --archive)",
    )
    parser.add_argument(
        "--listings",
        type=Path,
        default=default_listings,
        metavar="DIR",
        help="retail archive listings (*.txt) used to warn about unknown or DLC-only override members",
    )
    parser.add_argument("--output-root", type=Path, default=ROOT / "build/custom-assets")
    toc = parser.add_mutually_exclusive_group()
    toc.add_argument(
        "--plain-toc",
        action="store_true",
        help="force OPEN-tag archives with a plaintext table (the default, except for override packs)",
    )
    toc.add_argument(
        "--keyed-toc",
        action="store_true",
        help="encrypt the archive tables here with the game's table keys (developer builds only)",
    )
    return parser


def main(
    argv: list[str] | None = None,
    *,
    table_keys: Callable[[], TableKeys] | None = None,
    default_listings: Path | None = None,
    retail_dir: Path | None = None,
) -> int:
    """Build one pack. `table_keys` (a developer hook, never set by this tool) supplies the game's
    table keys for --keyed-toc, --retail-member and keyed override packs; `retail_dir` is then
    scanned for local retail DLC archives that re-ship an override member."""
    args = _parser(table_keys is not None, default_listings).parse_args(argv)
    retail_members = getattr(args, "retail_member", [])

    # Plain tables need no game files. Preserve the keyed default for existing developer overlay
    # callers; public override builds explicitly select --plain-toc.
    args.plain_toc = args.plain_toc or not (args.keyed_toc or args.override_archive)
    keys_needed = not args.plain_toc or retail_members
    if keys_needed and table_keys is None:
        raise SystemExit(
            "keyed archive tables (--keyed-toc, or an override pack without --plain-toc) need the game's "
            "table keys, which this tool does not read; build with --plain-toc"
        )
    keys = table_keys() if keys_needed and table_keys is not None else None
    members = [_retail_member(keys, spec) for spec in retail_members]
    members += [_loose_member(spec) for spec in args.member]
    names = [m[0] for m in members]
    if not members or len(set(names)) != len(names):
        raise SystemExit("need at least one member and unique member names")
    overlays: dict[str, list[str]] = {}
    for spec in args.override_archive:
        archive_name, _, listed = spec.partition("=")
        if archive_name in overlays or not listed:
            raise SystemExit(f"invalid or duplicate --override-archive {spec!r}")
        overlays[archive_name] = listed.split(",")
    overridden = {name for listed in overlays.values() for name in listed}
    if len(overridden) != sum(map(len, overlays.values())) or not overridden <= set(names):
        raise SystemExit("--override-archive members must be unique pack members")
    for name in names:
        if not _member_name_ok(name, override=name in overridden):
            raise SystemExit(f"invalid member name {name!r}")
    spawns = [_spawn(spec) for spec in args.spawn]
    # Particle dictionaries: the worker's particle-store headroom gate keys on the ptfx rows.
    ptfx_assets = {model.partition(":")[0] for kind, model, _ in spawns if kind == "ptfx"}
    ppt_members = {name.removesuffix(".ppt") for name in names if name.endswith(".ppt")}
    if ptfx_assets != ppt_members:
        raise SystemExit(
            f".ppt members {sorted(ppt_members)} and ptfx spawn assets {sorted(ptfx_assets)} must match "
            "(each pack particle dictionary needs a `--spawn ptfx:ASSET:EFFECT=TEXT` row and vice versa)"
        )
    cards = []
    for spec in args.card:
        dict_name, _, texture = spec.partition(":")
        if f"{dict_name}.ptd" not in names:
            raise SystemExit(f"card dictionary {dict_name}.ptd is not a member")
        cards.append(runtime_pack.Card(dict_name, texture))
    for typ in [*args.typ, *args.map, *args.bounds]:
        if typ not in names:
            raise SystemExit(f"typ {typ} is not a member")
    for typ in args.retail_typ:
        if typ in names or typ in args.typ:
            raise SystemExit(f"--retail-typ {typ} is also a pack member or --typ row")
    data = []
    for spec in args.data:
        type_name, _, path = spec.partition("=")
        data.append((type_name, Path(path)))

    groups: dict[str, list[str]] = {}
    for spec in args.extra_archive:
        archive_name, _, listed = spec.partition("=")
        if archive_name in groups or archive_name in overlays or archive_name == args.archive or not listed:
            raise SystemExit(f"invalid or duplicate --extra-archive {spec!r}")
        groups[archive_name] = listed.split(",")
    groups.update({name: listed for name, listed in overlays.items() if name != args.archive})
    moved = [name for listed in groups.values() for name in listed]
    if len(set(moved)) != len(moved) or not set(moved) <= set(names):
        raise SystemExit("--extra-archive/--override-archive members must be unique pack members")
    groups = {args.archive: [n for n in names if n not in moved], **groups}
    if not groups[args.archive]:
        raise SystemExit(f"{args.archive} would be empty")
    if args.archive in overlays and sorted(groups[args.archive]) != sorted(overlays[args.archive]):
        # The worker requires an overlay archive to hold exactly its override members.
        raise SystemExit(f"{args.archive} is an overlay archive but would also hold other members")
    overrides = [(archive_name, member) for archive_name, listed in overlays.items() for member in listed]
    overridden_names = [member for _, member in overrides]
    warnings = _listing_warnings(overridden_names, args.listings)
    if overridden_names:
        if keys is None or retail_dir is None:
            warnings.append("retail patch/DLC overlay scan skipped (needs the game's table keys)")
        else:
            warnings += _patch_warnings(keys, overridden_names, retail_dir)
    weapons = [
        name
        for type_name, path in data
        if type_name == "WEAPONINFO_FILE"
        for name, _ in weapon_wheel.weapon_wheel_slots(path.read_bytes())
    ]
    wicons = _wheel_icons(args.wheel_icon, weapons)
    if not args.no_default_wheel_icons:
        for type_name, path in data:
            if type_name != "WEAPONINFO_FILE":
                continue
            for weapon, donor in weapon_wheel.default_wheel_icons(path.read_bytes(), [w for w, _ in wicons]):
                if len(wicons) >= runtime_pack.WICON_MAX:
                    warnings.append(f"{weapon}: no default wheel icon row (a pack holds {runtime_pack.WICON_MAX})")
                    continue
                # Without a frame label the wheel keeps its last drawn icon (WICON-run2).
                print(f"note: wicon {weapon} -> {donor} (default donor of its WheelSlot)", file=sys.stderr)
                wicons.append((weapon, donor))
    for type_name, path in data:
        if type_name == "WEAPONINFO_FILE":
            # The wheel icon is keyed by the weapon name hash (gtavmenu_tools.weapon_wheel).
            warnings += weapon_wheel.icon_warnings(path.read_bytes(), [w for w, _ in wicons])
    for warning in warnings:
        print(f"warning: {warning}", file=sys.stderr)
    by_name = {m[0]: m for m in members}
    archives = {
        name: _build_archive(keys, name, [by_name[n] for n in listed], args.plain_toc)
        for name, listed in groups.items()
    }
    resources = args.output_root / args.id / "resources"
    if resources.exists():
        raise SystemExit(f"refusing to overwrite {resources}")
    archive = archives[args.archive]
    map_deps, retail_map_deps = _map_deps(args.map_dep, args.map, args.typ, args.retail_typ)
    pack = runtime_pack.RuntimePack(
        args.id,
        args.archive,
        len(archive),
        hashlib.sha256(archive).hexdigest(),
        cards,
        [
            runtime_pack.DataFile(t, p.stat().st_size, hashlib.sha256(p.read_bytes()).hexdigest(), p.name)
            for t, p in data
        ],
        [*args.typ, *args.retail_typ],
        list(args.map),
        [tuple(item.split("=", 1)) for item in args.label],
        spawns,
        [
            (name, len(blob), hashlib.sha256(blob).hexdigest())
            for name, blob in archives.items()
            if name != args.archive
        ],
        _map_places(args.map_place, args.map),
        set(overlays),
        overrides,
        list(args.bounds),
        map_deps,
        set(args.retail_typ),
        _places(args.place),
        _hides(args.hide),
        _typ_deps(args.typ_dep, args.typ),
        retail_map_deps,
        wicons,
        _tints(args.tints, spawns, data),
        description=args.description,
        author=args.author,
        version=args.version,
    )
    try:
        descriptor = runtime_pack.render(pack)
    except runtime_pack.RuntimePackError as error:
        raise SystemExit(f"invalid pack: {error}") from None
    resources.mkdir(parents=True)
    for name, blob in archives.items():
        (resources / name).write_bytes(blob)
    for _, path in data:
        (resources / path.name).write_bytes(path.read_bytes())
    (resources / "pack.cfg").write_text(descriptor, encoding="ascii", newline="\n")
    for name, blob in archives.items():
        # A plain table is keyed later by the worker with the same index (no key bytes needed here).
        if args.plain_toc or keys is None:
            key = rpf_toc.toc_index(name, len(blob))
        else:
            key = keys.table_key_index(name, len(blob))
        overlay = " overlay" if name in overlays else ""
        plain = " toc=plain" if args.plain_toc else ""
        print(f"wrote {resources}/{name} bytes={len(blob)} key={key} members={len(groups[name])}{overlay}{plain}")
    print(descriptor, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

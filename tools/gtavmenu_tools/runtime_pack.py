"""Runtime pack descriptor (resources/pack.cfg), mirrored by src/common/custom_pack_runtime.c.

A runtime pack is one engine archive the worker registers through the memory device, the texture
dictionaries an operator may request and show as a card, and loose data files the worker may load.
"""

from __future__ import annotations

import hashlib
import re
import xml.etree.ElementTree as ET
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from gtavmenu_tools import audio_rel, weapon_wheel

MAGIC = "GTAVPACK,1"
DESCRIPTOR_MAX = 8192
ID_MAX = 64
NAME_MAX = 63
CARD_MAX = 8
ARCHIVE_MAX = 4
ACTIVE_MAX = 8  # packs listed in packs/active and merged by the worker
MENU_LABEL_MAX = 47  # characters of a pack id the Custom Packs page shows
# Optional `description` / `author` / `version` rows (Manage Packs shows them): at most this many
# characters, printable ASCII without '~' (a format token to the game's text renderer).
DESCRIPTION_MAX = 80
AUTHOR_MAX = 32
VERSION_MAX = 16
# The menu's in-game uninstall renames resources/pack.cfg to this (custom_pack_select.inc
# uninstall_pack): every scan path and load opens pack.cfg, so the pack disappears from the menu
# while its files stay. Uploading the pack again restores pack.cfg.
UNINSTALLED_DESCRIPTOR = "pack.cfg.uninstalled"
PLACE_COORD_MAX = 16000  # |x|, |y|, |z| of a map row's menu teleport, whole metres
PLACE_MAX = 8  # `place` rows: Custom Packs teleports not tied to a map row
HIDE_MAX = 32  # `hide` rows: stock map entities hidden with CREATE_MODEL_HIDE after the maps load
HIDE_RADIUS_MAX = 200  # metres
TYP_MAX = 16  # pack typ members plus `retail` stock typs
MAP_MAX = 8
BOUNDS_MAX = 8  # static collision .pbn rows
LABEL_MAX = 128
SPAWN_MAX = 24
SPAWN_TEXT_MAX = 39
# ptfx model: <asset>:<effect>; component model: <weapon>:<component> (toggled on that weapon)
SPAWN_KINDS = ("vehicle", "object", "ped", "weapon", "timecycle", "ptfx", "component")
PAIR_SPAWN_KINDS = ("ptfx", "component")
DATA_MAX = 16
OVERRIDE_MAX = 16  # stock members replaced by `overlay` archives
OVERRIDE_EXTENSIONS = ("ptd", "pft", "pdr", "pdd")
# `wicon` rows: a pack weapon shows a retail donor's weapon wheel icon (hud.gfx label alias)
WICON_MAX = 8
# `tints WEAPON palette|none` rows (one per `spawn weapon` row at most): whether the weapon's model reads a tint
# palette (the retail palette shader); with `none` the menu's Weapon Tint skips the setter ("no tint palette").
TINTS_VALUES = ("palette", "none")
# Merged-set capacities when several packs are active together (gtav_custom_pack_merge).
ARCHIVE_CAP = 16
CARD_CAP = 16
DATA_CAP = 64
TYP_CAP = 48
MAP_CAP = 16
BOUNDS_CAP = 32
LABEL_CAP = 192
SPAWN_CAP = 64
OVERRIDE_CAP = 64
PLACE_CAP = 32
HIDE_CAP = 64
WICON_CAP = 32
ARCHIVE_BYTES_MAX = 64 * 1024 * 1024
DATA_BYTES_MAX = 4 * 1024 * 1024
# Bytes the whole merged set may hold in archives and in data files (each is its own never-freed
# direct-memory buffer on the console): what the earlier 4-pack caps allowed at most.
SET_ARCHIVE_BYTES_MAX = 512 * 1024 * 1024
SET_DATA_BYTES_MAX = 128 * 1024 * 1024
DATA_TYPES = {
    "HANDLING_FILE": 6,
    "CARCOLS_FILE": 10,
    "VEHICLE_METADATA_FILE": 73,
    "VEHICLE_VARIATION_FILE": 135,
    "VEHICLE_LAYOUTS_FILE": 165,
    "PED_METADATA_FILE": 71,
    "WEAPON_METADATA_FILE": 72,
    "WEAPONINFO_FILE": 78,
    "WEAPONCOMPONENTSINFO_FILE": 79,
    "WEAPON_ANIMATIONS_FILE": 118,
    "SHOP_PED_APPAREL_META_FILE": 137,
    "TIMECYCLEMOD_FILE": 28,
    # An audio game-data chunk (.rel, dat151).
    "AUDIO_GAMEDATA": 140,
}
AUDIO_GAMEDATA = "AUDIO_GAMEDATA"
AUDIO_CHUNK_MAX = 31  # the mounter keeps at most 31 characters of the chunk name

_ID = re.compile(r"[a-z0-9](?:[a-z0-9-]*[a-z0-9])?")
_NAME = re.compile(r"[a-z0-9_]+")
_PAIR = re.compile(r"[a-z0-9_]+:[a-z0-9_]+")  # `spawn ptfx` / `spawn component` model: name:name
_ARCHIVE = re.compile(r"[a-z0-9_]+\.rpf")
_TYP = re.compile(r"[a-z0-9_]+\.ptyp")
_MAP = re.compile(r"[a-z0-9_]+\.pmap")
_BOUNDS = re.compile(r"[a-z0-9_]+\.pbn")
_LABEL_KEY = re.compile(r"[A-Za-z0-9_]+")
_LABEL_TEXT = re.compile(r"[\x20-\x7d]+")  # printable ASCII without ~
_FILE = re.compile(r"[a-z0-9_]+\.meta")
# AUDIO_GAMEDATA file: <chunk>_game.rel; the worker stages it as audio/<file> and the mounter names
# the chunk after the basename up to its first '_'.
_AUDIO_FILE = re.compile(rf"([a-z0-9]{{1,{AUDIO_CHUNK_MAX}}})_game\.rel")
# override member: [<folder>/]<stem>.<ext>; one folder level, the member's folder inside its stock
# archive (the engine names the store slot after the path, e.g. player_one/uppr_014_u).
_OVERRIDE = re.compile(r"(?:[a-z0-9_]+/)?[a-z0-9_+]+\.(?:ptd|pft|pdr|pdd)")
_SHA = re.compile(r"[0-9a-f]{64}")
_SIZE = re.compile(r"[1-9][0-9]*")
_COORD = re.compile(r"-?(0|[1-9][0-9]{0,4})")
_RADIUS = re.compile(r"[1-9][0-9]{0,2}")
_ABOUT_TEXT = re.compile(r"[\x20-\x7d]+")
_VERSION = re.compile(r"[A-Za-z0-9._+-]+")


class RuntimePackError(ValueError):
    pass


@dataclass(frozen=True)
class Card:
    dict: str
    texture: str


@dataclass(frozen=True)
class DataFile:
    type: str
    size: int
    sha256: str
    file: str


@dataclass
class RuntimePack:
    pack_id: str
    archive: str
    archive_size: int
    archive_sha256: str
    cards: list[Card] = field(default_factory=list)
    data: list[DataFile] = field(default_factory=list)
    typs: list[str] = field(default_factory=list)
    maps: list[str] = field(default_factory=list)
    labels: list[tuple[str, str]] = field(default_factory=list)
    spawns: list[tuple[str, str, str]] = field(default_factory=list)
    extra_archives: list[tuple[str, int, str]] = field(default_factory=list)
    # Optional Custom Packs teleport per map row: map name -> (x, y, z, menu text).
    map_places: dict[str, tuple[int, int, int, str]] = field(default_factory=dict)
    # Archives registered with the engine's override permit, and the stock members they replace
    # as (archive, member) rows.
    overlay_archives: set[str] = field(default_factory=set)
    overrides: list[tuple[str, str]] = field(default_factory=list)
    # Static collision bounds (.pbn members) requested keep-resident after the maps.
    bounds: list[str] = field(default_factory=list)
    # Map row -> (typ it depends on, places an interior/MLO).
    # A typ that is not a row of this pack is another pack's own typ, bound when both are active.
    map_deps: dict[str, tuple[str, bool]] = field(default_factory=dict)
    # Typ rows that name a stock typ (`typ NAME.ptyp retail`): not archive members, requested so
    # an interior's room props (its itypDependencies) are resident before the map loads.
    retail_typs: set[str] = field(default_factory=set)
    # Map-less Custom Packs teleports: (x, y, z, menu text), whole metres.
    places: list[tuple[int, int, int, str]] = field(default_factory=list)
    # Stock map entities hidden for the session: (model, x, y, z, radius), whole metres.
    hides: list[tuple[str, int, int, int, int]] = field(default_factory=list)
    # Pack typ row -> the stock typ it depends on (`typdep TYP.ptyp STOCK.ptyp`): the worker binds
    # it typ->typ before the typ's request, so the engine streams the stock typ with it.
    typ_deps: dict[str, str] = field(default_factory=dict)
    # Map rows whose map_deps typ is a stock typ by name, not a typ row of any pack
    # (`mapdep MAP.pmap STOCK.ptyp retail`).
    retail_map_deps: set[str] = field(default_factory=set)
    # Weapon wheel icon aliases (`wicon WEAPON DONOR`): the pack weapon shows the retail donor's
    # wheel icon (a hud.gfx frame label alias). Lowercase names.
    wicons: list[tuple[str, str]] = field(default_factory=list)
    # `tints` rows: spawn weapon model -> "palette" or "none" (written after the spawn rows, in spawn order).
    tints: dict[str, str] = field(default_factory=dict)
    # Optional texts Manage Packs shows with what the pack adds ("" = no row).
    description: str = ""
    author: str = ""
    version: str = ""


def valid_id(value: str) -> bool:
    """A pack id as gtav_custom_pack_id_valid accepts it."""
    return bool(_ID.fullmatch(value)) and len(value) <= ID_MAX


def _check(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimePackError(message)


def validate(pack: RuntimePack) -> None:
    _check(bool(_ID.fullmatch(pack.pack_id)) and len(pack.pack_id) <= ID_MAX, "invalid pack id")
    for what, text, pattern, limit in (
        ("description", pack.description, _ABOUT_TEXT, DESCRIPTION_MAX),
        ("author", pack.author, _ABOUT_TEXT, AUTHOR_MAX),
        ("version", pack.version, _VERSION, VERSION_MAX),
    ):
        _check(
            not text or (bool(pattern.fullmatch(text)) and len(text) <= limit),
            f"invalid {what} {text!r} (up to {limit} printable characters, no '~'"
            + (", [A-Za-z0-9._+-] only)" if what == "version" else ")"),
        )
    _check(bool(_ARCHIVE.fullmatch(pack.archive)) and len(pack.archive) <= NAME_MAX, "invalid archive name")
    _check(0 < pack.archive_size <= ARCHIVE_BYTES_MAX, "archive size out of bounds")
    _check(bool(_SHA.fullmatch(pack.archive_sha256)), "invalid archive sha256")
    _check(len(pack.extra_archives) <= ARCHIVE_MAX - 1, "too many archives")
    names = [pack.archive] + [a for a, _, _ in pack.extra_archives]
    _check(len(set(names)) == len(names), "duplicate archive")
    for name, size, sha in pack.extra_archives:
        _check(bool(_ARCHIVE.fullmatch(name)) and len(name) <= NAME_MAX, "invalid archive name")
        _check(0 < size <= ARCHIVE_BYTES_MAX, "archive size out of bounds")
        _check(bool(_SHA.fullmatch(sha)), "invalid archive sha256")
    _check(pack.overlay_archives <= set(names), "overlay flag on an unknown archive")
    _check(len(pack.overrides) <= OVERRIDE_MAX, "too many overrides")
    _check(len({m for _, m in pack.overrides}) == len(pack.overrides), "duplicate override member")
    for archive, member in pack.overrides:
        _check(archive in pack.overlay_archives, f"override of {member!r} names a non-overlay archive")
        _check(bool(_OVERRIDE.fullmatch(member)) and len(member) <= NAME_MAX, f"invalid override {member!r}")
    for archive in pack.overlay_archives:
        _check(any(a == archive for a, _ in pack.overrides), f"overlay archive {archive!r} has no override")
    _check(len(pack.cards) <= CARD_MAX, "too many cards")
    _check(len(set(pack.cards)) == len(pack.cards), "duplicate card")
    for card in pack.cards:
        for name in (card.dict, card.texture):
            _check(bool(_NAME.fullmatch(name)) and len(name) <= NAME_MAX, f"invalid card name {name!r}")
    _check(len(pack.typs) <= TYP_MAX and len(set(pack.typs)) == len(pack.typs), "bad typ list")
    for typ in pack.typs:
        _check(bool(_TYP.fullmatch(typ)) and len(typ) <= NAME_MAX, f"invalid typ {typ!r}")
    _check(pack.retail_typs <= set(pack.typs), "retail flag on an unknown typ")
    _check(len(pack.maps) <= MAP_MAX and len(set(pack.maps)) == len(pack.maps), "bad map list")
    for name in pack.maps:
        _check(bool(_MAP.fullmatch(name)) and len(name) <= NAME_MAX, f"invalid map {name!r}")
    for name, (x, y, z, text) in pack.map_places.items():
        _check(name in pack.maps, f"place for unknown map {name!r}")
        _check(all(abs(v) <= PLACE_COORD_MAX for v in (x, y, z)), "map place out of bounds")
        _check(bool(_LABEL_TEXT.fullmatch(text)) and len(text) < SPAWN_TEXT_MAX, f"invalid map text {text!r}")
    own_typs = set(pack.typs) - pack.retail_typs
    for typ, dep in pack.typ_deps.items():
        _check(typ in own_typs, f"typ dependency of {typ!r}: not a pack typ row")
        _check(bool(_TYP.fullmatch(dep)) and len(dep) <= NAME_MAX, f"invalid typ dependency {dep!r}")
        _check(dep not in own_typs, f"typ dependency {typ!r} -> {dep!r} names a pack typ, not a stock one")
    _check(pack.retail_map_deps <= set(pack.map_deps), "retail flag on a map without a dependency")
    # A mapdep typ is, in this order: a typ row of this pack; with `retail`, a stock typ by name;
    # else another pack's own typ (bound when both packs are active).
    for name, (typ, interior) in pack.map_deps.items():
        _check(name in pack.maps, f"map dependency {name!r} -> {typ!r} is not a map row")
        _check(bool(_TYP.fullmatch(typ)) and len(typ) <= NAME_MAX, f"invalid map dependency typ {typ!r}")
        if name in pack.retail_map_deps:
            _check(typ not in own_typs and not interior, f"stock map dependency {name!r} -> {typ!r}")
        else:
            _check(typ not in pack.retail_typs, f"map dependency {name!r} -> {typ!r} names a retail typ")
    _check(len(pack.bounds) <= BOUNDS_MAX and len(set(pack.bounds)) == len(pack.bounds), "bad bounds list")
    for name in pack.bounds:
        _check(bool(_BOUNDS.fullmatch(name)) and len(name) <= NAME_MAX, f"invalid bounds {name!r}")
    _check(len(pack.data) <= DATA_MAX, "too many data files")
    _check(len(pack.labels) <= LABEL_MAX and len({k for k, _ in pack.labels}) == len(pack.labels), "bad labels")
    _check(len(pack.spawns) <= SPAWN_MAX and len({m for _, m, _ in pack.spawns}) == len(pack.spawns), "bad spawns")
    for kind, model, text in pack.spawns:
        _check(kind in SPAWN_KINDS, f"invalid spawn kind {kind!r}")
        pattern = _PAIR if kind in PAIR_SPAWN_KINDS else _NAME
        _check(bool(pattern.fullmatch(model)) and len(model) <= NAME_MAX, f"invalid spawn model {model!r}")
        _check(bool(_LABEL_TEXT.fullmatch(text)) and len(text) <= SPAWN_TEXT_MAX, f"invalid spawn text {text!r}")
    for key, text in pack.labels:
        _check(bool(_LABEL_KEY.fullmatch(key)) and len(key) <= NAME_MAX, f"invalid label key {key!r}")
        _check(bool(_LABEL_TEXT.fullmatch(text)) and len(text) <= NAME_MAX, f"invalid label text {text!r}")
    _check(len({d.file for d in pack.data}) == len(pack.data), "duplicate data file")
    _check(len(pack.places) <= PLACE_MAX, "too many place rows")
    for x, y, z, text in pack.places:
        _check(all(abs(v) <= PLACE_COORD_MAX for v in (x, y, z)), "place out of bounds")
        _check(bool(_LABEL_TEXT.fullmatch(text)) and len(text) <= SPAWN_TEXT_MAX, f"invalid place text {text!r}")
    _check(len(pack.hides) <= HIDE_MAX, "too many hide rows")
    _check(len({h[:4] for h in pack.hides}) == len(pack.hides), "duplicate hide row")
    for model, x, y, z, radius in pack.hides:
        _check(bool(_NAME.fullmatch(model)) and len(model) <= NAME_MAX, f"invalid hide model {model!r}")
        _check(all(abs(v) <= PLACE_COORD_MAX for v in (x, y, z)), "hide point out of bounds")
        _check(1 <= radius <= HIDE_RADIUS_MAX, f"hide radius {radius} out of bounds")
    _check(len(pack.wicons) <= WICON_MAX, "too many wicon rows")
    _check(len({w for w, _ in pack.wicons}) == len(pack.wicons), "duplicate wicon weapon")
    for weapon, donor in pack.wicons:
        for name in (weapon, donor):
            _check(bool(_NAME.fullmatch(name)) and len(name) <= NAME_MAX, f"invalid wicon name {name!r}")
        _check(weapon != donor, f"wicon {weapon!r} names itself as donor")
    weapons = {model for kind, model, _ in pack.spawns if kind == "weapon"}
    for weapon, value in pack.tints.items():
        _check(weapon in weapons, f"tints row {weapon!r} names no spawn weapon row")
        _check(value in TINTS_VALUES, f"tints {weapon!r}: {value!r} is not palette or none")
    for data in pack.data:
        _check(data.type in DATA_TYPES, f"data type {data.type!r} is not allowed")
        pattern = _AUDIO_FILE if data.type == AUDIO_GAMEDATA else _FILE
        _check(bool(pattern.fullmatch(data.file)) and len(data.file) <= NAME_MAX, f"invalid data file {data.file!r}")
        _check(0 < data.size <= DATA_BYTES_MAX, "data size out of bounds")
        _check(bool(_SHA.fullmatch(data.sha256)), "invalid data sha256")


def render(pack: RuntimePack) -> str:
    validate(pack)

    def overlay(name: str) -> str:
        return "\toverlay" if name in pack.overlay_archives else ""

    lines = [MAGIC, f"pack\t{pack.pack_id}"]
    lines += [f"{key}\t{text}" for key, text in about_rows(pack)]
    lines.append(f"archive\t{pack.archive_size}\t{pack.archive_sha256}\t{pack.archive}{overlay(pack.archive)}")
    lines += [f"archive\t{s}\t{h}\t{n}{overlay(n)}" for n, s, h in pack.extra_archives]
    lines += [f"override\t{a}\t{m}" for a, m in pack.overrides]
    lines += [f"card\t{c.dict}\t{c.texture}" for c in pack.cards]
    lines += [f"typ\t{t}" + ("\tretail" if t in pack.retail_typs else "") for t in pack.typs]
    lines += [f"typdep\t{t}\t{pack.typ_deps[t]}" for t in pack.typs if t in pack.typ_deps]
    lines += [
        f"map\t{m}" + ("\t{}\t{}\t{}\t{}".format(*pack.map_places[m]) if m in pack.map_places else "")
        for m in pack.maps
    ]
    lines += [
        f"mapdep\t{m}\t{pack.map_deps[m][0]}"
        + ("\tinterior" if pack.map_deps[m][1] else "\tretail" if m in pack.retail_map_deps else "")
        for m in pack.maps
        if m in pack.map_deps
    ]
    lines += [f"bounds\t{b}" for b in pack.bounds]
    lines += [f"data\t{d.type}\t{d.size}\t{d.sha256}\t{d.file}" for d in pack.data]
    lines += [f"wicon\t{w}\t{d}" for w, d in pack.wicons]
    lines += [f"label\t{k}\t{t}" for k, t in pack.labels]
    lines += [f"spawn\t{k}\t{m}\t{t}" for k, m, t in pack.spawns]
    lines += [f"tints\t{m}\t{pack.tints[m]}" for k, m, _ in pack.spawns if k == "weapon" and m in pack.tints]
    lines += [f"place\t{x}\t{y}\t{z}\t{t}" for x, y, z, t in pack.places]
    lines += [f"hide\t{m}\t{x}\t{y}\t{z}\t{r}" for m, x, y, z, r in pack.hides]
    text = "\n".join(lines) + "\n"
    _check(len(text) <= DESCRIPTOR_MAX, "descriptor too large")
    return text


ABOUT_KEYS = ("description", "author", "version")


def about_rows(pack: RuntimePack) -> list[tuple[str, str]]:
    """The optional `description` / `author` / `version` rows of `pack`, in descriptor order."""
    return [
        (key, text) for key, text in zip(ABOUT_KEYS, (pack.description, pack.author, pack.version), strict=True) if text
    ]


# What a pack adds, in Manage Packs order (gtav_custom_pack_about in src/common/custom_pack_runtime.c).
_ABOUT_WORDS = (
    ("vehicle", "vehicles"),
    ("weapon", "weapons"),
    ("attachment", "attachments"),
    ("ped", "peds"),
    ("prop", "props"),
    ("effect", "effects"),
    ("map", "maps"),
    ("teleport", "teleports"),
    ("collision file", "collision files"),
    ("hidden model", "hidden models"),
    ("override", "overrides"),
    ("label", "labels"),
    ("data file", "data files"),
    ("archive", "archives"),
)
_ABOUT_SPAWN = {"vehicle": 0, "weapon": 1, "component": 2, "ped": 3, "object": 4, "timecycle": 5, "ptfx": 5}


def about_line(pack: RuntimePack) -> str:
    """The line Custom Packs > Manage Packs shows for `pack` (before any clash text), as the worker
    renders it: "<description> (v<version>, by <author>). Adds 2 vehicles, 1 map"."""
    counts = [0] * len(_ABOUT_WORDS)
    for kind, _, _ in pack.spawns:
        counts[_ABOUT_SPAWN[kind]] += 1
    for index, rows in (
        (6, pack.maps),
        (7, [*pack.places, *pack.map_places]),
        (8, pack.bounds),
        (9, pack.hides),
        (10, pack.overrides),
        (11, pack.labels),
        (12, pack.data),
    ):
        counts[index] = len(rows)
    counts[13] = 1 + len(pack.extra_archives)
    # Data files and archives only count when nothing a player sees is listed.
    if any(counts[:12]) or counts[12]:
        counts[13] = 0
    if any(counts[:12]):
        counts[12] = 0
    meta = ", ".join(([f"v{pack.version}"] if pack.version else []) + ([f"by {pack.author}"] if pack.author else []))
    head = pack.description + (f" ({meta})" if pack.description and meta else meta if not pack.description else "")
    added = ", ".join(f"{n} {_ABOUT_WORDS[k][n != 1]}" for k, n in enumerate(counts) if n) or "nothing"
    return f"{head}. Adds {added}" if head else f"Adds {added}"


def parse(text: str) -> RuntimePack:
    _check(0 < len(text) <= DESCRIPTOR_MAX and text.endswith("\n"), "descriptor size/termination")
    _check("\r" not in text and "\0" not in text, "descriptor contains CR or NUL")
    lines = text[:-1].split("\n")
    _check(len(lines) >= 3 and lines[0] == MAGIC, "missing magic")
    head = lines[1].split("\t")
    _check(len(head) == 2 and head[0] == "pack", "missing pack line")
    # Optional texts between the pack line and the first archive, each once, in ABOUT_KEYS order.
    about: dict[str, str] = {}
    keys = list(ABOUT_KEYS)
    while len(lines) > 3 and "\t" in lines[2] and lines[2].split("\t", 1)[0] in keys:
        key, value = lines[2].split("\t", 1)
        _check(value != "", f"empty {key} row")
        keys = keys[keys.index(key) + 1 :]
        about[key] = value
        del lines[2]
    archive = lines[2].split("\t")
    _check(
        len(archive) in (4, 5)
        and archive[0] == "archive"
        and bool(_SIZE.fullmatch(archive[1]))
        and archive[4:] in ([], ["overlay"]),
        "bad archive line",
    )
    pack = RuntimePack(head[1], archive[3], int(archive[1]), archive[2], **about)
    if len(archive) == 5:
        pack.overlay_archives.add(archive[3])
    for line in lines[3:]:
        fields = line.split("\t")
        # `place` rows follow every other row; `hide` rows come last.
        _check(not pack.hides or fields[0] == "hide", f"row after a hide row {line!r}")
        _check(not pack.places or fields[0] in ("place", "hide"), f"row after a place row {line!r}")
        rows_seen = pack.cards or pack.typs or pack.maps or pack.bounds or pack.data or pack.labels or pack.spawns
        if (
            fields[0] == "archive"
            and len(fields) in (4, 5)
            and fields[4:] in ([], ["overlay"])
            and _SIZE.fullmatch(fields[1])
            and not rows_seen
            and not pack.overrides
        ):
            pack.extra_archives.append((fields[3], int(fields[1]), fields[2]))
            if len(fields) == 5:
                pack.overlay_archives.add(fields[3])
            continue
        if fields[0] == "override" and len(fields) == 3 and not rows_seen:
            pack.overrides.append((fields[1], fields[2]))
            continue
        if fields[0] == "card" and len(fields) == 3 and not (pack.data or pack.typs or pack.maps or pack.bounds):
            pack.cards.append(Card(fields[1], fields[2]))
        elif (
            fields[0] == "typ"
            and fields[2:] in ([], ["retail"])
            and len(fields) >= 2
            and not pack.data
            and not pack.maps
            and not pack.bounds
            and not pack.typ_deps
        ):
            pack.typs.append(fields[1])
            if len(fields) == 3:
                pack.retail_typs.add(fields[1])
        elif (
            fields[0] == "typdep"
            and len(fields) == 3
            and not (pack.maps or pack.bounds or pack.data or pack.labels or pack.spawns)
            and fields[1] in pack.typs
            and fields[1] not in pack.retail_typs
            and fields[1] not in pack.typ_deps
            and fields[1] != fields[2]
        ):
            pack.typ_deps[fields[1]] = fields[2]
        elif fields[0] == "map" and len(fields) in (2, 6) and not (pack.data or pack.bounds or pack.map_deps):
            pack.maps.append(fields[1])
            if len(fields) == 6:
                _check(all(_COORD.fullmatch(v) for v in fields[2:5]), f"bad map place {line!r}")
                pack.map_places[fields[1]] = (int(fields[2]), int(fields[3]), int(fields[4]), fields[5])
        elif (
            fields[0] == "mapdep"
            and (len(fields) == 3 or (len(fields) == 4 and fields[3] in ("interior", "retail")))
            and not (pack.bounds or pack.data or pack.labels or pack.spawns)
            and fields[1] in pack.maps
            and _TYP.fullmatch(fields[2])
            and fields[2] not in (set(pack.typs) - pack.retail_typs if fields[3:] == ["retail"] else pack.retail_typs)
            and fields[1] not in pack.map_deps
        ):
            stock = fields[3:] == ["retail"]
            if stock:
                pack.retail_map_deps.add(fields[1])
            pack.map_deps[fields[1]] = (fields[2], fields[3:] == ["interior"])
        elif fields[0] == "bounds" and len(fields) == 2 and not pack.data and not pack.labels and not pack.spawns:
            pack.bounds.append(fields[1])
        elif fields[0] == "wicon" and len(fields) == 3 and not pack.labels and not pack.spawns:
            pack.wicons.append((fields[1], fields[2]))
        elif fields[0] == "spawn" and len(fields) == 4 and not pack.tints:
            pack.spawns.append((fields[1], fields[2], fields[3]))
        elif fields[0] == "tints" and len(fields) == 3 and fields[1] not in pack.tints:
            pack.tints[fields[1]] = fields[2]
        elif fields[0] == "label" and len(fields) == 3 and not pack.spawns:
            pack.labels.append((fields[1], fields[2]))
        elif fields[0] == "place" and len(fields) == 5:
            _check(all(_COORD.fullmatch(v) for v in fields[1:4]), f"bad place {line!r}")
            pack.places.append((int(fields[1]), int(fields[2]), int(fields[3]), fields[4]))
        elif fields[0] == "hide" and len(fields) == 6:
            _check(all(_COORD.fullmatch(v) for v in fields[2:5]), f"bad hide point {line!r}")
            _check(bool(_RADIUS.fullmatch(fields[5])), f"bad hide radius {line!r}")
            pack.hides.append((fields[1], int(fields[2]), int(fields[3]), int(fields[4]), int(fields[5])))
        elif (
            fields[0] == "data"
            and len(fields) == 5
            and _SIZE.fullmatch(fields[2])
            and not pack.labels
            and not pack.spawns
            and not pack.wicons
        ):
            pack.data.append(DataFile(fields[1], int(fields[2]), fields[3], fields[4]))
        else:
            raise RuntimePackError(f"unexpected line {line!r}")
    validate(pack)
    return pack


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def files(pack: RuntimePack) -> list[tuple[str, int, str]]:
    """Every file pack.cfg names, as (name, size, sha256): the archives, then the data files."""
    rows = [(pack.archive, pack.archive_size, pack.archive_sha256), *pack.extra_archives]
    return rows + [(data.file, data.size, data.sha256) for data in pack.data]


def overlay_toc_entries(pack: RuntimePack, archive: str) -> int:
    """TOC entries the worker requires of overlay archive `archive`: the root, one directory per
    folder of its override members, and one entry per override row (it holds nothing else)."""
    members = [member for name, member in pack.overrides if name == archive]
    folders = {member.rpartition("/")[0] for member in members} - {""}
    return 1 + len(folders) + len(members)


def merge_conflicts(packs: Sequence[RuntimePack]) -> list[str]:
    """Why the worker would refuse to load `packs` together (empty when they merge).

    Mirrors gtav_custom_pack_merge: at most ACTIVE_MAX packs, the merged rows within the *_CAP
    capacities, the merged archive and data bytes within SET_ARCHIVE_BYTES_MAX and
    SET_DATA_BYTES_MAX, and no pack id, archive, data file, typ (unless every copy is `retail`), map,
    bounds, label key, spawn model, override member, card or wicon weapon repeated across packs.
    """
    problems: list[str] = []
    if len(packs) > ACTIVE_MAX:
        problems.append(f"{len(packs)} packs active together (max {ACTIVE_MAX})")
    totals: tuple[tuple[str, int, Callable[[RuntimePack], int]], ...] = (
        ("archives", ARCHIVE_CAP, lambda p: 1 + len(p.extra_archives)),
        ("cards", CARD_CAP, lambda p: len(p.cards)),
        ("typ rows", TYP_CAP, lambda p: len(p.typs)),
        ("map rows", MAP_CAP, lambda p: len(p.maps)),
        ("label rows", LABEL_CAP, lambda p: len(p.labels)),
        ("spawn rows", SPAWN_CAP, lambda p: len(p.spawns)),
        ("data rows", DATA_CAP, lambda p: len(p.data)),
        ("override rows", OVERRIDE_CAP, lambda p: len(p.overrides)),
        ("bounds rows", BOUNDS_CAP, lambda p: len(p.bounds)),
        ("place rows", PLACE_CAP, lambda p: len(p.places)),
        ("hide rows", HIDE_CAP, lambda p: len(p.hides)),
        ("wicon rows", WICON_CAP, lambda p: len(p.wicons)),
    )
    for what, cap, count in totals:
        total = sum(count(p) for p in packs)
        if total > cap:
            problems.append(f"{total} {what} together (max {cap})")
    byte_totals: tuple[tuple[str, int, Callable[[RuntimePack], int]], ...] = (
        ("archive bytes", SET_ARCHIVE_BYTES_MAX, lambda p: p.archive_size + sum(s for _, s, _ in p.extra_archives)),
        ("data bytes", SET_DATA_BYTES_MAX, lambda p: sum(d.size for d in p.data)),
    )
    for what, cap, count in byte_totals:
        total = sum(count(p) for p in packs)
        if total > cap:
            problems.append(f"{total} {what} together (max {cap})")
    names: tuple[tuple[str, Callable[[RuntimePack], list[str]]], ...] = (
        ("pack id", lambda p: [p.pack_id]),
        ("archive", lambda p: [p.archive] + [a for a, _, _ in p.extra_archives]),
        ("data file", lambda p: [d.file for d in p.data]),
        ("typ", lambda p: [t for t in p.typs if t not in p.retail_typs]),
        ("map", lambda p: list(p.maps)),
        ("bounds", lambda p: list(p.bounds)),
        ("label", lambda p: [k for k, _ in p.labels]),
        ("spawn model", lambda p: [m for _, m, _ in p.spawns]),
        ("override", lambda p: [m for _, m in p.overrides]),
        ("card", lambda p: [f"{c.dict}/{c.texture}" for c in p.cards]),
        ("wicon weapon", lambda p: [w for w, _ in p.wicons]),
    )
    for what, rows in names:
        owner: dict[str, str] = {}
        for pack in packs:
            for name in rows(pack):
                if name in owner:
                    problems.append(f"{what} {name!r} is in both {owner[name]} and {pack.pack_id}")
                else:
                    owner[name] = pack.pack_id
    # Only `retail` typ rows may repeat: a pack's own typ may not share a name with another's retail row.
    for pack in packs:
        for typ in pack.typs:
            if typ in pack.retail_typs:
                continue
            for other in packs:
                if other is not pack and typ in other.retail_typs:
                    problems.append(f"typ {typ!r} is in both {pack.pack_id} and {other.pack_id}")
    return problems


def external_map_deps(pack: RuntimePack) -> list[tuple[str, str]]:
    """(map, typ) mapdep rows that name another pack's typ (not a typ row of `pack`, not `retail`)."""
    return [
        (name, typ)
        for name, (typ, _) in pack.map_deps.items()
        if typ not in pack.typs and name not in pack.retail_map_deps
    ]


def unbound_map_deps(packs: Sequence[RuntimePack]) -> list[str]:
    """Cross-pack mapdep rows no other pack of `packs` provides as its own (non-`retail`) typ.

    The worker merges such a set (gtav_custom_pack_merge binds what it finds) but refuses the map
    row at load time: "pack map dependency typ is in no active pack".
    """
    problems = []
    for pack in packs:
        for name, typ in external_map_deps(pack):
            owners = [o.pack_id for o in packs if o is not pack and typ in o.typs and typ not in o.retail_typs]
            if not owners:
                problems.append(f"map {name!r} of {pack.pack_id} depends on typ {typ!r}, which no other pack provides")
    return problems


def descriptor_path(resources: Path) -> Path:
    """The descriptor of a resources/ directory: pack.cfg, else a pack.cfg.uninstalled the menu's
    uninstall left (a pack copied back from the console), which an upload restores as pack.cfg."""
    cfg = resources / "pack.cfg"
    hidden = resources / UNINSTALLED_DESCRIPTOR
    return hidden if not cfg.exists() and hidden.is_file() else cfg


def check_directory(pack_dir: Path) -> tuple[RuntimePack | None, list[str], list[str]]:
    """Check a pack directory the way the uploader and the worker will.

    Returns (pack or None, errors, warnings): resources/pack.cfg parses within the row limits and
    every file it names exists with the declared size and sha256. Files in resources/ that pack.cfg
    does not name are warnings (they are not uploaded). A pack.cfg.uninstalled (the menu's uninstall
    renamed pack.cfg) stands in for a missing pack.cfg, with a warning; next to a pack.cfg it is
    ignored.
    """
    resources = pack_dir / "resources"
    cfg = descriptor_path(resources)
    notes: list[str] = []
    if cfg.name == UNINSTALLED_DESCRIPTOR:
        notes.append(f"{UNINSTALLED_DESCRIPTOR}: uninstalled in the menu; checked as pack.cfg, an upload restores it")
    elif (resources / UNINSTALLED_DESCRIPTOR).is_file():
        notes.append(f"{UNINSTALLED_DESCRIPTOR}: left by the menu's uninstall; ignored (pack.cfg is used)")
    try:
        descriptor = cfg.read_text(encoding="ascii")
    except OSError as exc:
        return None, [f"cannot read {resources / 'pack.cfg'}: {exc.strerror or exc}"], []
    except UnicodeDecodeError:
        return None, ["pack.cfg is not ASCII"], []
    try:
        pack = parse(descriptor)
    except RuntimePackError as exc:
        return None, [f"pack.cfg: {exc}"], []
    errors: list[str] = []
    audio_files = {d.file for d in pack.data if d.type == AUDIO_GAMEDATA}
    for name, size, sha in files(pack):
        path = resources / name
        if not path.is_file():
            errors.append(f"{name}: named by pack.cfg but missing")
            continue
        blob = path.read_bytes()
        if len(blob) != size:
            errors.append(f"{name}: {len(blob)} bytes, pack.cfg says {size}")
        elif hashlib.sha256(blob).hexdigest() != sha:
            errors.append(f"{name}: sha256 differs from pack.cfg")
        elif name in audio_files:
            try:
                audio_rel.check(audio_rel.parse(blob))
            except audio_rel.RelError as exc:
                errors.append(f"{name}: audio chunk refused by the worker's gate: {exc}")
        elif name in pack.overlay_archives:
            # The header's entry count is plain even when the table is keyed.
            entries = int.from_bytes(blob[4:8], "little") if len(blob) >= 16 else -1
            if entries != overlay_toc_entries(pack, name):
                errors.append(
                    f"{name}: overlay archive has {entries} table entries, the worker requires "
                    f"{overlay_toc_entries(pack, name)} (root, folders, one per override row)"
                )
    named = {name for name, _, _ in files(pack)} | {"pack.cfg", UNINSTALLED_DESCRIPTOR}
    warnings = notes + [
        f"{path.name}: not named by pack.cfg (not uploaded)"
        for path in sorted(resources.iterdir())
        if path.name not in named
    ]
    if len(pack.pack_id) > MENU_LABEL_MAX:
        warnings.append(f"pack id is {len(pack.pack_id)} characters; the menu shows the first {MENU_LABEL_MAX}")
    if not errors:
        warnings += wheel_icon_warnings(resources, pack)
    return pack, errors, warnings


def wheel_icon_warnings(resources: Path, pack: RuntimePack) -> list[str]:
    """One warning per CWeaponInfo of the pack's WEAPONINFO_FILE rows that has no wheel icon of its own
    and no `wicon` row: the weapon wheel keeps its last drawn icon for it (seen on hardware)."""
    aliased = [weapon for weapon, _ in pack.wicons]
    warnings = []
    for data in pack.data:
        if data.type != "WEAPONINFO_FILE":
            continue
        try:
            missing = weapon_wheel.missing_wheel_icons((resources / data.file).read_bytes(), aliased)
        except (OSError, ET.ParseError):
            warnings.append(f"{data.file}: weapons meta not readable; wheel icons not checked")
            continue
        for name, slot in missing:
            donor = weapon_wheel.DEFAULT_DONORS.get(slot, "a retail weapon of its WheelSlot")
            warnings.append(
                f"{data.file}: {name} has no wicon row; the weapon wheel shows the last drawn icon for it "
                f"(rebuild with the default donor, e.g. --wheel-icon {name}={donor})"
            )
    return warnings


def describe_directory(
    resources: Path, pack_id: str, archive: str, cards: list[Card], data: list[tuple[str, str]]
) -> RuntimePack:
    """Build a descriptor for files already present in `resources` (archive + data files)."""
    archive_path = resources / archive
    pack = RuntimePack(pack_id, archive, archive_path.stat().st_size, sha256_file(archive_path), list(cards))
    for type_name, name in data:
        path = resources / name
        pack.data.append(DataFile(type_name, path.stat().st_size, sha256_file(path), name))
    validate(pack)
    return pack

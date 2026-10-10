"""Which archetypes a runtime pack's typs define and its maps place, read from the pack archives.

Only archives with a plain (OPEN) table are read; packs built by tools/build_runtime_pack.py are
plain since TOC-run1. A keyed table needs the game's keys and is reported as unreadable.

A typ (.ptyp, CMapTypes) defines one archetype per CBaseArchetypeDef / CTimeArchetypeDef /
CMloArchetypeDef struct (name hash at +0x58); a map (.pmap, CMapData) places one per CEntityDef /
CMloInstanceDef struct (archetypeName at +0x08). The worker requests every typ row of every active
pack before it loads any map row, so a map may place archetypes of another active pack's typ.

Streamed model and texture members (.pdr, .pdd, .pft, .ptd) are found by their name alone, so the
same member name in two active packs' archives is a conflict: one registration wins and the other
pack's archetypes draw with its drawable or textures.
"""

from __future__ import annotations

import struct
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

from gtavmenu_tools import rpf7
from gtavmenu_tools.hashes import joaat
from gtavmenu_tools.meta_resource import Meta, joaat_cs
from gtavmenu_tools.runtime_pack import RuntimePack

ARCHETYPE_STRUCTS = tuple(joaat_cs(n) for n in ("CBaseArchetypeDef", "CTimeArchetypeDef", "CMloArchetypeDef"))
ENTITY_STRUCTS = tuple(joaat_cs(n) for n in ("CEntityDef", "CMloInstanceDef"))
ARCHETYPE_NAME, ENTITY_ARCHETYPE = 0x58, 0x08
STREAMED_BY_NAME = (".pdr", ".pdd", ".pft", ".ptd")


def _names(payload: bytes, structs: tuple[int, ...], at: int) -> list[int]:
    meta = Meta(payload)
    out = []
    for struct_hash, size, offset in meta.blocks:
        if struct_hash not in structs or struct_hash not in meta.structs:
            continue
        stride = meta.structs[struct_hash][0]
        if stride < at + 4:
            continue
        out += [struct.unpack_from("<I", payload, offset + i * stride + at)[0] for i in range(size // stride)]
    return out


def typ_archetypes(payload: bytes) -> list[int]:
    """Archetype name hashes a typ payload (inflated .ptyp) defines."""
    return _names(payload, ARCHETYPE_STRUCTS, ARCHETYPE_NAME)


def map_archetypes(payload: bytes) -> list[int]:
    """Archetype name hashes a map payload (inflated .pmap) places, one per entity."""
    return _names(payload, ENTITY_STRUCTS, ENTITY_ARCHETYPE)


def archive_payloads(blob: bytes, name: str, suffixes: tuple[str, ...] = (".ptyp", ".pmap")) -> dict[str, bytes]:
    """Inflated resource members of a plain-table archive whose names end with `suffixes`.

    Raises rpf7.Rpf7Error for a keyed table (or a broken archive).
    """
    entries, _ = rpf7.read_table(blob, name)
    out = {}
    for entry in entries:
        if entry["kind"] == "res" and entry["name"].endswith(suffixes):
            stored = blob[entry["offset"] : entry["offset"] + entry["size"]]
            out[entry["name"]] = rpf7.inflate_resource(stored, f"{name}/{entry['name']}")
    return out


def archive_member_names(blob: bytes, name: str, suffixes: tuple[str, ...] = STREAMED_BY_NAME) -> list[str]:
    """Lower-case names of the resource members of a plain-table archive ending with `suffixes`."""
    entries, _ = rpf7.read_table(blob, name)
    return [e["name"].lower() for e in entries if e["kind"] == "res" and e["name"].lower().endswith(suffixes)]


@dataclass
class PackArchetypes:
    """What one pack's own typ rows define and its map rows place."""

    pack_id: str
    defines: dict[int, str] = field(default_factory=dict)  # archetype hash -> typ member
    places: dict[str, list[int]] = field(default_factory=dict)  # map member -> archetype hashes
    unreadable: list[str] = field(default_factory=list)  # archives whose table is keyed or broken
    streamed: set[str] = field(default_factory=set)  # STREAMED_BY_NAME members of its archives


def read_pack(pack_dir: Path, pack: RuntimePack) -> PackArchetypes:
    """Typ and map contents of `pack` from the archives in pack_dir/resources."""
    found = PackArchetypes(pack.pack_id)
    payloads: dict[str, bytes] = {}
    for archive in [pack.archive, *(a for a, _, _ in pack.extra_archives)]:
        try:
            blob = (pack_dir / "resources" / archive).read_bytes()
            payloads.update(archive_payloads(blob, archive))
            found.streamed.update(archive_member_names(blob, archive))
        except (OSError, rpf7.Rpf7Error, ValueError, struct.error, IndexError):
            found.unreadable.append(archive)
    for typ in pack.typs:
        if typ not in pack.retail_typs and typ in payloads:
            for value in typ_archetypes(payloads[typ]):
                found.defines.setdefault(value, typ)
    for name in pack.maps:
        if name in payloads:
            found.places[name] = map_archetypes(payloads[name])
    return found


def cross_check(
    packs: Sequence[tuple[RuntimePack, PackArchetypes]],
) -> tuple[list[str], list[str]]:
    """(notes, warnings) for packs active together.

    Notes say, per map, how many distinct archetypes come from its own typs, from each other pack
    and from no pack typ (stock archetypes, or a pack whose archive could not be read). An archetype
    no readable typ defines but another pack's spawn row names (a keyed pack) is counted for that
    pack and marked "(spawn row)". Warnings name archetypes defined by typs of two packs (the later
    registration replaces the earlier) and streamed members (STREAMED_BY_NAME) in two packs' archives.
    """
    known = {joaat(m): m for pack, _ in packs for _, m, _ in pack.spawns}
    spawned = {joaat(m): pack.pack_id for pack, _ in packs for _, m, _ in pack.spawns}
    notes: list[str] = []
    warnings: list[str] = []
    owner: dict[int, str] = {}
    holder: dict[str, str] = {}
    for pack, found in packs:
        for value in found.defines:
            if value in owner and owner[value] != pack.pack_id:
                label = known.get(value, f"{value:#010x}")
                warnings.append(f"archetype {label} is defined by typs of both {owner[value]} and {pack.pack_id}")
            owner.setdefault(value, pack.pack_id)
        for member in sorted(found.streamed):
            if member in holder and holder[member] != pack.pack_id:
                warnings.append(
                    f"member {member} is in the archives of both {holder[member]} and {pack.pack_id} "
                    "(streamed by name: one registration wins, the other pack's archetypes use it)"
                )
            holder.setdefault(member, pack.pack_id)
        for archive in found.unreadable:
            notes.append(f"{pack.pack_id}: {archive} has a keyed or unreadable table; its typs/maps were not read")
    for pack, found in packs:
        for name, placed in found.places.items():
            distinct = list(dict.fromkeys(placed))
            own = sum(1 for v in distinct if v in found.defines)
            other: dict[str, list[str]] = {}
            for value in distinct:
                if value in found.defines:
                    continue
                label = known.get(value, f"{value:#010x}")
                if value in owner:
                    other.setdefault(owner[value], []).append(label)
                elif spawned.get(value, pack.pack_id) != pack.pack_id:
                    other.setdefault(spawned[value], []).append(f"{label} (spawn row)")
            stock = len(distinct) - own - sum(len(v) for v in other.values())
            parts = [f"{own} own"] + [f"{len(v)} from {p} ({', '.join(v)})" for p, v in other.items()]
            notes.append(
                f"{pack.pack_id}: map {name} places {len(placed)} entities of {len(distinct)} archetypes: "
                f"{', '.join(parts)}, {stock} stock or from no pack here"
            )
    return notes, warnings

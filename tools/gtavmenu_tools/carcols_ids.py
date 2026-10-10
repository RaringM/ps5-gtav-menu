"""Light, siren, mod-kit and wheel keys of a pack's carcols and carvariations data rows.

The CARCOLS_FILE merge keys Kits by their u16 <id>, Lights and Sirens by their u8 <id> (0xFFFF /
0xFF always append and cannot be referenced) and the wheels of each wheel type by the hash of
<wheelName>. An item whose key is already loaded is skipped without a word ("first wins"), so a pack
id that repeats a retail id or another active pack's id silently loses its item. A carvariations
<lightSettings>/<sirenSettings> id that no loaded item has resolves to index 0xff, and the siren
renderer reads past the siren array when that car's siren is switched on. These checks run on the
host before an upload.
"""

from __future__ import annotations

import re
import xml.etree.ElementTree as ET
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field

from gtavmenu_tools.hashes import joaat

# Highest retail 01.010.002 ids (base carcols.pmt + the 39 update.rpf DLC carcols, census 2026-10-07):
# lights 0-211, sirens 0-20, kits 0-613 plus 999. A pack id must lie above them.
RETAIL_MAX = {"kit": 613, "light": 211, "siren": 20}
RETAIL_EXTRA = {"kit": {999}, "light": set(), "siren": set()}
APPEND = {"kit": 0xFFFF, "light": 0xFF, "siren": 0xFF}
CARCOLS = "CARCOLS_FILE"
VARIATION = "VEHICLE_VARIATION_FILE"


@dataclass
class CarcolsKeys:
    """Keys one data row adds (carcols) or references (carvariations)."""

    kits: list[int] = field(default_factory=list)
    lights: list[int] = field(default_factory=list)
    sirens: list[int] = field(default_factory=list)
    wheels: list[str] = field(default_factory=list)
    light_refs: list[int] = field(default_factory=list)
    siren_refs: list[int] = field(default_factory=list)


def _ids(root: ET.Element, array: str) -> list[int]:
    out = []
    for parent in root.iter(array):
        for item in parent:
            found = item.find("id")
            value = found.get("value") if found is not None else None
            if value is not None and re.fullmatch(r"\d+", value.strip()):
                out.append(int(value))
    return out


def read_keys(type_name: str, blob: bytes) -> CarcolsKeys:
    """The keys of one data file (raises ValueError when it is not XML)."""
    try:
        text = blob.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise ValueError(f"not XML: {exc}") from exc
    if "<!" in text.replace("<!--", ""):  # DOCTYPE/ENTITY/CDATA: the worker refuses them too
        raise ValueError("not plain XML (declarations or CDATA)")
    try:
        root = ET.fromstring(text)
    except ET.ParseError as exc:
        raise ValueError(f"not XML: {exc}") from exc
    keys = CarcolsKeys()
    if type_name == CARCOLS:
        keys.kits, keys.lights, keys.sirens = _ids(root, "Kits"), _ids(root, "Lights"), _ids(root, "Sirens")
        keys.wheels = [(w.text or "").strip() for w in root.iter("wheelName")]
    elif type_name == VARIATION:
        for tag, out in (("lightSettings", keys.light_refs), ("sirenSettings", keys.siren_refs)):
            for element in root.iter(tag):
                value = (element.get("value") or "").strip()
                if re.fullmatch(r"\d+", value):
                    out.append(int(value))
    return keys


def is_retail(kind: str, value: int) -> bool:
    return value <= RETAIL_MAX[kind] or value in RETAIL_EXTRA[kind]


def problems(rows: Sequence[tuple[str, str, CarcolsKeys]]) -> list[str]:
    """Why the (pack id, file, keys) rows of packs loaded together would lose items or dangle.

    Every kit, light and siren id must lie above the retail range and below the append value and
    appear once across all rows; every wheel name once; every carvariations light/siren reference
    must be retail (0 = no siren) or an id one of the rows defines.
    """
    found: list[str] = []
    owners: dict[tuple[str, int | str], str] = {}
    for pack_id, name, keys in rows:
        where = f"{pack_id} {name}"
        for kind, values in (("kit", keys.kits), ("light", keys.lights), ("siren", keys.sirens)):
            for value in values:
                if value >= APPEND[kind]:
                    found.append(f"{where}: {kind} id {value} always appends and cannot be referenced")
                elif is_retail(kind, value):
                    found.append(
                        f"{where}: {kind} id {value} is a retail id (use {RETAIL_MAX[kind] + 1}-{APPEND[kind] - 1}); "
                        "the game keeps its own item and drops this one"
                    )
                elif (kind, value) in owners:
                    found.append(
                        f"{kind} id {value} is in both {owners[kind, value]} and {where} (the first loaded wins)"
                    )
                else:
                    owners[kind, value] = where
        for wheel in keys.wheels:
            key = ("wheel", joaat(wheel))
            if not wheel:
                found.append(f"{where}: a wheel has no wheelName")
            elif key in owners:
                found.append(f"wheel {wheel!r} is in both {owners[key]} and {where} (the first loaded wins)")
            else:
                owners[key] = where
    for pack_id, name, keys in rows:
        for kind, refs in (("light", keys.light_refs), ("siren", keys.siren_refs)):
            for value in refs:
                if not is_retail(kind, value) and (kind, value) not in owners:
                    hazard = "; switching that car's siren on reads past the siren array" if kind == "siren" else ""
                    found.append(
                        f"{pack_id} {name}: {kind}Settings {value} names no retail {kind} and no {kind} item "
                        f"of the packs checked{hazard}"
                    )
    return list(dict.fromkeys(found))


def pack_rows(
    pack_id: str, data: Iterable[tuple[str, str, bytes]]
) -> tuple[list[tuple[str, str, CarcolsKeys]], list[str]]:
    """(rows, errors) for one pack's (type, file, bytes) data files; other types are skipped."""
    rows, errors = [], []
    for type_name, name, blob in data:
        if type_name not in (CARCOLS, VARIATION):
            continue
        try:
            rows.append((pack_id, name, read_keys(type_name, blob)))
        except ValueError as exc:
            errors.append(f"{pack_id} {name}: {exc}")
    return rows, errors

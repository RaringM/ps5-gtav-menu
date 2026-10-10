#!/usr/bin/env python3
"""Prepare a converted car's carcols.meta + carvariations.meta for runtime loading (CARCOLS_FILE 10).

Runtime merge rules: Kits are matched by their
u16 <id> (0xFFFF = always append), Lights and Sirens by their u8 <id> (0xFF = always append); an
item whose id already exists is skipped ("first wins"), arrays grow without a hard cap. A mod kit
or light whose id collides with a retail one is therefore silently dropped. This tool:
  - renames each kit to PREFIX + kitName and gives the kits --kit-id, +1, ... in file order (a
    multi-car pack needs one id per kit: the merge keeps only the first kit of an id); drops <visibleMods> items whose
    <modelName> is not shipped in the pack (their fragments would be missing), keeps statMods;
  - numbers the Lights items --light-id, +1, ... in file order; drops Sirens unless --keep-sirens --siren-id N: then every
    Sirens item is kept and renumbered N, N+1, ... in file order;
  - rewrites carvariations.meta: kit names, <lightSettings value>, <sirenSettings value>. A value naming
    one of the mod's lights follows to --light-id; one naming a mod siren follows its renumbered item
    (kept) or becomes 0, the retail default siren (dropped). A value naming no mod item is a retail
    reference and stays when it is a retail id (in the --census, else at most RETAIL_MAX): emergency
    add-ons often ship no Sirens and use a stock siren set (sirenSettings 1). Any other value names
    nothing the game holds: lights fall back to --light-id (refused when the mod ships no Lights),
    sirens to 0. Each kept or replaced reference is printed as a note;
  - with --shop-label-prefix P: gives every kept visible mod a new <modShopLabel> P_<SLOT>_<n> (SLOT =
    <type> without VMT_, n counts per slot) and writes --out-labels, one `KEY=TEXT` line per mod for
    the pack builder's --label rows. The text is the original label's string from --gxt2 (the PC mod's
    own text table) when present, else a name derived from <modelName> (a80_bumf_2 -> "Front Bumper 2").
    The worker adds a pack label only when its key is new (existing labels win), so retail keys such
    as YOSE_WING1 can never be renamed; new keys can.
  - --siren-preset NAME | --siren-spec FILE.json with --siren-id N: writes ONE Sirens item (id N) from a
    pattern spec instead of the mod's items, and points every non-zero carvariations <sirenSettings> at
    it. A spec names lamp groups (32-step on/off rhythm, colour as RGB, flash or rotate) and assigns each
    of the 20 siren lamps (bones siren1..siren20) and the left/right head- and tail-lights to a group;
    --siren-lamps overrides the lamp assignment for one car (e.g. LLRR------------LRLR: '-' = unused).
    The engine lights a lamp on step `(elapsed_ms * timeMultiplier / (60000 / bpm)) % 32` when that step's
    bit (first step = most significant bit) is set (vehicle-conversion-notes.md "Custom siren settings").
    --list-siren-presets prints the presets (SIREN_PRESETS);
  - <Wheels>: wheel items whose <wheelName> is not shipped (--ship-wheel, the pack's NAME.pdr drawables)
    are dropped, and a carcols left with no wheels loses its <Wheels> element (the worker refuses a
    growing wheel array only while a modded car's wheels are drawn). A kept wheel whose <wheelVariation>
    (the custom-tyre model) is not shipped uses its own wheel model there; with --shop-label-prefix it
    gets the label P_WHEEL_<n>;
  - with --out-labels and --gxt2, every kit <liveryNames> key the mod's text table defines becomes a pack-owned
    key P_LIV_<n> (in the carcols) with a label row of the mod's text: the mod's own keys are often the game's
    (aventador: ZENTORNO_LV1..6), and the worker never replaces a key the game has; keys the mod's text does not
    define stay (they name the game's text);
  --census FILE... lists the kit/light/siren ids used by retail carcols (decrypted XML carcols.meta and
  the binary PSO base ps5/data/carcols.pmt, taken from the user's own game) and refuses colliding ids. Without --census, light and siren ids must lie
  in the reserved range 224-254 (GTAVMENU_IDS): retail 01.010.002 uses lights 0-211 and sirens 0-20
  (base PSO 0-76 / 0-13, DLC XML up to 211 / 20); 255 means "always append" and cannot be referenced.
  Packs loaded together need distinct ids too (the first loaded item with an id wins, silently).

  prepare_carcols.py --carcols a80/carcols.meta --variations v1/carvariations.meta --prefix GTAVMENU_ \\
      --kit-id 4609 --light-id 251 --out-carcols out/carcols.meta --out-variations out/carvariations.meta
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
import xml.dom.minidom
from pathlib import Path

from gtavmenu_tools import runtime_pack
from gtavmenu_tools.asset_formats import Limits, gxt2_entries
from gtavmenu_tools.hashes import joaat

KIT_ITEM = re.compile(r"(<Kits>)(.*?)(</Kits>)", re.S)
ITEM_ID = re.compile(r'<id value="(\d+)"\s*/>')
GTAVMENU_IDS = range(224, 255)  # light/siren ids free of retail without a census (u8; 255 = append)
RETAIL_MAX = {"light": 211, "siren": 20}  # highest retail 01.010.002 ids (base PSO + DLC XML); --census replaces it
# PSO (binary carcols.pmt): PMAP block struct name hash -> census key; field "id" (case-kept joaat).
PSO_ID_ARRAYS = {0x99D7AF68: "lights", 0x84EB9B0D: "sirens", 0x287AB4AA: "kits"}  # vehicleLightSettings,
PSO_ID_FIELD = 0x1B60404D  # sirenSettings, CVehicleKit; "id"
PSO_INTS = {2: ">B", 4: ">H"}  # PSO data types u8 / u16
LABEL_KEY = re.compile(r"[A-Za-z0-9_]+")
LABEL_TEXT = re.compile(r"[\x20-\x7d]+")  # printable ASCII without ~ (a game text format token)
# <modelName> tokens of PC mod parts -> shop words; unknown tokens are title-cased.
PART_WORDS = {
    "badge": "Badge",
    "bonnet": "Hood",
    "bumf": "Front Bumper",
    "bumr": "Rear Bumper",
    "cage": "Roll Cage",
    "bnt": "Hood",
    "exh": "Exhaust",
    "fender": "Fender",
    "grill": "Grille",
    "hood": "Hood",
    "liv": "Livery",
    "livery": "Livery",
    "mirror": "Mirror",
    "roof": "Roof",
    "skirt": "Side Skirt",
    "spoil": "Spoiler",
    "spoiler": "Spoiler",
    "split": "Splitter",
    "wing": "Wing",
}


def items(block: str) -> list[tuple[int, int]]:
    """Top-level <Item>...</Item> spans inside an array body (nesting-aware)."""
    spans, depth, start = [], 0, 0
    for match in re.finditer(r"<Item\b[^>/]*>|</Item>|<Item\b[^>]*/>", block):
        tag = match.group(0)
        if tag.endswith("/>"):
            continue
        if tag.startswith("</"):
            depth -= 1
            if depth == 0:
                spans.append((start, match.end()))
        else:
            if depth == 0:
                start = match.start()
            depth += 1
    return spans


def part_name(model: str) -> str:
    """Readable shop name from a part's model name, dropping the vehicle prefix (a80_spoil_1 -> Spoiler 1);
    a number glued to a word is split off (a45_livery1 -> Livery 1, skyline_bumr2 -> Rear Bumper 2)."""
    tokens = [t for t in model.split("_") if t]
    tokens = tokens[1:] if len(tokens) > 1 else tokens
    words = []
    for token in tokens:
        glued = re.fullmatch(r"([A-Za-z]+?)(\d+)", token)
        for part in glued.groups() if glued else (token,):
            words.append(PART_WORDS.get(part.lower(), part.capitalize()))
    return " ".join(words)


def relabel(item: str, prefix: str, counters: dict[str, int], texts: dict[int, str]) -> tuple[str, str, str]:
    """Give one <visibleMods> item a new pack-owned <modShopLabel>; returns (item, key, text)."""
    model = re.search(r"<modelName>\s*([^<]+?)\s*</modelName>", item).group(1)
    slot = re.search(r"<type>\s*([^<]+?)\s*</type>", item).group(1).removeprefix("VMT_")
    counters[slot] = counters.get(slot, 0) + 1
    key = f"{prefix}_{slot}_{counters[slot]}"
    if not LABEL_KEY.fullmatch(key) or len(key) > runtime_pack.NAME_MAX:
        raise SystemExit(f"{model}: label key {key!r} is not [A-Za-z0-9_] of at most {runtime_pack.NAME_MAX}")
    old = re.search(r"<modShopLabel>\s*([^<]*?)\s*</modShopLabel>|<modShopLabel\s*/>", item)
    if old is None:
        raise SystemExit(f"{model}: visible mod has no <modShopLabel>")
    text = texts.get(joaat(old.group(1) or ""), "")
    if not (LABEL_TEXT.fullmatch(text) and len(text) <= runtime_pack.NAME_MAX):
        text = part_name(model)
    if not (LABEL_TEXT.fullmatch(text) and len(text) <= runtime_pack.NAME_MAX):
        raise SystemExit(f"{model}: no printable shop name of at most {runtime_pack.NAME_MAX} characters")
    return item.replace(old.group(0), f"<modShopLabel>{key}</modShopLabel>", 1), key, text


def pso_census(data: bytes, used: dict[str, set[int]]) -> None:
    """Add the kit/light/siren ids of a binary PSO carcols (PSIN/PMAP/PSCH sections, big-endian)."""
    sections, offset = {}, 0
    while offset + 8 <= len(data):
        ident, length = data[offset : offset + 4], struct.unpack_from(">I", data, offset + 4)[0]
        if length < 8:
            raise SystemExit(f"PSO section {ident!r} at {offset:#x} has length {length}")
        sections[ident], offset = offset, offset + length
    if not {b"PSIN", b"PMAP", b"PSCH"} <= sections.keys():
        raise SystemExit("PSO carcols without PSIN/PMAP/PSCH sections")
    schema, base = {}, sections[b"PSCH"]
    for index in range(struct.unpack_from(">I", data, base + 8)[0]):
        name, where = struct.unpack_from(">Ii", data, base + 12 + index * 8)
        kind, _, count, size = struct.unpack_from(">BBhi", data, base + where)
        fields = (
            struct.unpack_from(">IBBH", data, base + where + 12 + i * 12) for i in range(count if kind == 0 else 0)
        )
        schema[name] = (size, {f[0]: (f[1], f[3]) for f in fields})
    pmap = sections[b"PMAP"]
    for index in range(struct.unpack_from(">h", data, pmap + 12)[0]):
        name, start, _, length = struct.unpack_from(">IiiI", data, pmap + 16 + index * 16)
        if name not in PSO_ID_ARRAYS:
            continue
        size, fields = schema[name]
        kind, field = fields[PSO_ID_FIELD]
        if size <= 0 or length % size or kind not in PSO_INTS:
            raise SystemExit(f"PSO block {name:#x}: {length} bytes of {size}-byte items, id type {kind}")
        fmt = PSO_INTS[kind]
        used[PSO_ID_ARRAYS[name]] |= {
            struct.unpack_from(fmt, data, start + i * size + field)[0] for i in range(length // size)
        }


def census(paths: list[Path]) -> dict[str, set[int]]:
    used = {"kits": set(), "lights": set(), "sirens": set()}
    for path in paths:
        data = path.read_bytes()
        if data[:4] == b"PSIN":
            pso_census(data, used)
            continue
        text = data.decode("utf-8-sig", errors="replace")
        for key, tag in (("kits", "Kits"), ("lights", "Lights"), ("sirens", "Sirens")):
            block = re.search(rf"<{tag}>(.*?)</{tag}>", text, re.S)
            if not block:
                continue
            for start, end in items(block.group(1)):
                found = ITEM_ID.search(block.group(1)[start:end])
                if found:
                    used[key].add(int(found.group(1)))
    return used


def check_ids(kind: str, ids: list[int], used: set[int] | None) -> None:
    """Light/siren ids are u8 keys (255 = append, unreferenceable); free per census or in GTAVMENU_IDS."""
    for value in ids:
        if not 0 <= value < 0xFF:
            raise SystemExit(f"{kind} id {value} is outside 0-254 (u8 runtime key; 255 always appends)")
        if used is None and value not in GTAVMENU_IDS:
            raise SystemExit(
                f"{kind} id {value} is outside the reserved {GTAVMENU_IDS.start}-{GTAVMENU_IDS.stop - 1} "
                "range; pick one there or prove it free with --census (retail carcols)"
            )
        if used is not None and value in used:
            raise SystemExit(f"{kind} id {value} collides with a retail {kind} id")


def is_retail(kind: str, value: int, used: dict[str, set[int]] | None) -> bool:
    """A light/siren id the retail game defines: in the census, or at most RETAIL_MAX without one."""
    return value in used[f"{kind}s"] if used is not None else 0 <= value <= RETAIL_MAX[kind]


def point(
    variations: str,
    kind: str,
    mapping: dict[int, int],
    used: dict[str, set[int]] | None,
    fallback: int | None,
    notes: list[str],
) -> str:
    """Rewrite each carvariations <{kind}Settings value="N" />.

    N naming one of the mod's items follows `mapping`; a retail id stays (a car may use a stock light
    or siren set, e.g. an emergency car on retail siren 1); any other id names nothing the game will
    hold and becomes `fallback` (refused when None).
    """
    tag = f"{kind}Settings"

    def one(match: re.Match) -> str:
        old = int(match.group(1))
        if old in mapping:
            return f'<{tag} value="{mapping[old]}" />'
        if is_retail(kind, old, used):
            notes.append(f"{tag} {old}: retail {kind} id, kept")
            return match.group(0)
        if fallback is None:
            raise SystemExit(f"{tag} {old} names no {kind} of the mod and no retail {kind}")
        notes.append(f"{tag} {old}: names no {kind} of the mod and no retail {kind} -> {fallback}")
        return f'<{tag} value="{fallback}" />'

    return re.sub(rf'<{tag} value="(\d+)"\s*/>', one, variations)


def siren_ids(carcols: str) -> list[int]:
    """The <id value> of every Sirens item, in file order."""
    block = re.search(r"<Sirens>(.*?)</Sirens>", carcols, re.S)
    body = block.group(1) if block else ""
    return [int(found.group(1)) for s, e in items(body) if (found := ITEM_ID.search(body[s:e]))]


def remap_sirens(
    carcols: str,
    variations: str,
    first_id: int,
    used: dict[str, set[int]] | None = None,
    notes: list[str] | None = None,
) -> tuple[str, str, dict[int, int]]:
    """Renumber every Sirens item first_id, first_id+1, ... and point carvariations at the new ids.

    <sirenSettings> values that name none of the mod's sirens stay when they are retail ids (0 = the
    stock default); others become 0 (point). Returns (carcols, variations, {old id: new id}).
    """
    notes = [] if notes is None else notes
    block = re.search(r"<Sirens>(.*?)</Sirens>", carcols, re.S)
    mapping: dict[int, int] = {}
    if block is None:
        return carcols, point(variations, "siren", mapping, used, 0, notes), mapping
    body, out, last = block.group(1), [], 0
    for start, end in items(body):
        item = body[start:end]
        found = ITEM_ID.search(item)
        old = int(found.group(1)) if found else None
        if old is None or old in mapping:
            raise SystemExit(f"Sirens item {len(mapping) + 1}: missing or repeated <id value> ({old})")
        mapping[old] = first_id + len(mapping)
        out.append(body[last:start] + item.replace(found.group(0), f'<id value="{mapping[old]}" />', 1))
        last = end
    carcols = carcols.replace(block.group(0), f"<Sirens>{''.join(out)}{body[last:]}</Sirens>", 1)
    return carcols, point(variations, "siren", mapping, used, 0, notes), mapping


# Siren pattern specs. Groups: {"pattern": 32 steps of 1/0 (first step first; spaces and _ ignored) or an
# int / "0x..." (bit 31 = first step), "rgb": [r, g, b], "mode": "flash" | "rotate", "speed": radians per
# second for rotate (retail 3-12), "start": start angle}. "lamps": 20 group keys (a string of one-character
# keys, or a list; "-" / null = lamp unused), "head_lights" / "tail_lights": [left, right] group keys or
# null (steady off). Optional "corona" {intensity, size, pull}, "light_intensity", "time_multiplier".
SIREN_LAMPS = 20
SIREN_PRESETS: dict[str, dict] = {
    # SIREN-run1's "GM slow wigwag" (gtavmenu-siren-v1, PASS): all white, two interleaved groups that swap
    # every 2 s; reproduces that pack's Sirens item byte for byte.
    "test-white-slow": {
        "name": "GM slow wigwag",
        "bpm": 120,
        "groups": {
            "A": {"pattern": "0xF0F0F0F0", "rgb": [255, 255, 255]},
            "B": {"pattern": "0x0F0F0F0F", "rgb": [255, 255, 255]},
        },
        "lamps": "AB" * 10,
        "head_lights": ["A", "B"],
        "tail_lights": ["A", "B"],
    },
    # Left side lit 400 ms, then the right side 400 ms (75 swaps a minute); left red, right blue.
    "classic-wigwag": {
        "name": "GM classic wigwag",
        "bpm": 300,
        "groups": {
            "L": {"pattern": "11001100" * 4, "rgb": [255, 3, 0]},
            "R": {"pattern": "00110011" * 4, "rgb": [0, 10, 255]},
        },
        "lamps": "LR" * 10,
        "head_lights": ["L", "R"],
        "tail_lights": ["L", "R"],
    },
    # Two 100 ms flashes on the left, then two on the right, every 800 ms; head/tail lights stay normal.
    "fast-strobe": {
        "name": "GM fast strobe",
        "bpm": 600,
        "groups": {
            "L": {"pattern": "10100000" * 4, "rgb": [255, 3, 0]},
            "R": {"pattern": "00001010" * 4, "rgb": [0, 10, 255]},
        },
        "lamps": "LR" * 10,
        "head_lights": [None, None],
        "tail_lights": [None, None],
    },
    # Every lamp a steadily lit amber beam turning about once a second, alternate lamps half a turn apart.
    "rotating-beacon": {
        "name": "GM rotating beacon",
        "bpm": 120,
        "groups": {
            "A": {"pattern": "1" * 32, "rgb": [255, 160, 0], "mode": "rotate", "speed": 6.0},
            "B": {"pattern": "1" * 32, "rgb": [255, 160, 0], "mode": "rotate", "speed": 6.0, "start": 3.14159},
        },
        "lamps": "AB" * 10,
        "head_lights": [None, None],
        "tail_lights": [None, None],
    },
}
SIREN_NAME = re.compile(r"[ -~]{1,63}")


def siren_pattern(value: object) -> int:
    """32-bit sequencer of a group pattern: '1'/'0' steps (first step = bit 31) or an int / '0x..'."""
    if isinstance(value, bool):
        raise SystemExit("siren pattern must be 32 steps of 1/0 or a 32-bit number")
    if isinstance(value, int):
        number = value
    elif isinstance(value, str) and value.lower().startswith("0x"):
        number = int(value, 16)
    elif isinstance(value, str):
        steps = value.replace(" ", "").replace("_", "")
        if len(steps) != 32 or set(steps) - {"0", "1"}:
            raise SystemExit(f"siren pattern {value!r}: want 32 steps of 1/0 (first step first)")
        number = int(steps, 2)
    else:
        raise SystemExit("siren pattern must be 32 steps of 1/0 or a 32-bit number")
    if not 0 <= number <= 0xFFFFFFFF:
        raise SystemExit(f"siren pattern {value!r} does not fit 32 bits")
    return number


def _number(spec: dict, key: str, default: float, low: float, high: float) -> float:
    value = spec.get(key, default)
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not low <= value <= high:
        raise SystemExit(f"siren spec {key}: want a number in {low}..{high}")
    return float(value)


def siren_spec(spec: dict, lamps: str | None = None) -> dict:
    """Checked, normalised spec: groups {key: (sequencer, 0xAARRGGBB, mode, speed, start)}, 20 lamp keys."""
    if not isinstance(spec, dict):
        raise SystemExit("siren spec must be a JSON object")
    known = {"name", "bpm", "groups", "lamps", "head_lights", "tail_lights", "corona", "light_intensity"}
    if set(spec) - known - {"time_multiplier"}:
        raise SystemExit(f"siren spec: unknown keys {sorted(set(spec) - known - {'time_multiplier'})}")
    name = spec.get("name")
    if not isinstance(name, str) or not SIREN_NAME.fullmatch(name) or set(name) & set("<>&"):
        raise SystemExit("siren spec name: 1-63 printable ASCII characters without <, > or &")
    bpm = spec.get("bpm")
    if isinstance(bpm, bool) or not isinstance(bpm, int) or not 1 <= bpm <= 6000:
        raise SystemExit("siren spec bpm: an integer 1..6000 (one pattern step = 60000/bpm ms)")
    groups = {}
    raw_groups = spec.get("groups")
    if not isinstance(raw_groups, dict) or not raw_groups:
        raise SystemExit("siren spec groups: an object of lamp groups")
    for key, group in raw_groups.items():
        if not isinstance(key, str) or len(key) != 1 or key == "-" or not isinstance(group, dict):
            raise SystemExit(f"siren spec group {key!r}: one-character key ('-' means unused) and an object")
        if set(group) - {"pattern", "rgb", "mode", "speed", "start"}:
            raise SystemExit(f"siren spec group {key}: unknown keys")
        rgb = group.get("rgb")
        if not (isinstance(rgb, list) and len(rgb) == 3 and all(type(c) is int and 0 <= c <= 255 for c in rgb)):
            raise SystemExit(f"siren spec group {key} rgb: [r, g, b], each 0..255")
        mode = group.get("mode", "flash")
        if mode not in ("flash", "rotate"):
            raise SystemExit(f"siren spec group {key} mode: flash or rotate")
        speed = _number(group, "speed", 12.0 if mode == "rotate" else 0.0, 0.0, 100.0)
        start = _number(group, "start", 0.0, -7.0, 7.0)
        colour = 0xFF000000 | rgb[0] << 16 | rgb[1] << 8 | rgb[2]
        groups[key] = (siren_pattern(group.get("pattern")), colour, mode, speed, start)
    lamp_keys = list(lamps if lamps is not None else spec.get("lamps", ""))
    lamp_keys = ["-" if k is None else k for k in lamp_keys]
    if len(lamp_keys) != SIREN_LAMPS or any(k != "-" and k not in groups for k in lamp_keys):
        raise SystemExit(f"siren lamps: {SIREN_LAMPS} group keys (siren1..siren20), '-' for an unused lamp")
    sides = {}
    for key in ("head_lights", "tail_lights"):
        pair = spec.get(key, [None, None])
        if not (isinstance(pair, list) and len(pair) == 2 and all(k is None or k in groups for k in pair)):
            raise SystemExit(f"siren spec {key}: [left, right] group keys or null")
        sides[key] = [groups[k][0] if k else 0 for k in pair]
    corona = spec.get("corona", {})
    if not isinstance(corona, dict) or set(corona) - {"intensity", "size", "pull"}:
        raise SystemExit("siren spec corona: {intensity, size, pull}")
    return {
        "name": name,
        "bpm": bpm,
        "time_multiplier": _number(spec, "time_multiplier", 1.0, 0.01, 100.0),
        "groups": groups,
        "lamps": lamp_keys,
        "sides": sides,
        "corona": (
            _number(corona, "intensity", 50.0, 0.0, 1000.0),
            _number(corona, "size", 1.5, 0.0, 100.0),
            _number(corona, "pull", 0.15, 0.0, 10.0),
        ),
        "light_intensity": _number(spec, "light_intensity", 2.0, 0.0, 100.0),
    }


def siren_lamp(n: int, spec: dict) -> str:
    """One <sirens> entry (lamp n = bone siren<n>); an unused lamp never lights."""
    key = spec["lamps"][n - 1]
    sequencer, colour, mode, speed, start = spec["groups"][key] if key != "-" else (0, 0xFFFFFFFF, "flash", 0.0, 0.0)
    rotate = mode == "rotate"
    intensity, size, pull = spec["corona"]
    return f"""        <!-- SIREN{n} -->
        <Item>
          <rotation>
            <delta value="0.000000" />
            <start value="{start:.6f}" />
            <speed value="{speed:.6f}" />
            <sequencer value="{sequencer if rotate else 0}" />
            <multiples value="1" />
            <direction value="false" />
            <syncToBpm value="true" />
          </rotation>
          <flashiness>
            <delta value="0.000000" />
            <start value="0.000000" />
            <speed value="0.000000" />
            <sequencer value="{sequencer}" />
            <multiples value="1" />
            <direction value="true" />
            <syncToBpm value="true" />
          </flashiness>
          <corona>
            <intensity value="{intensity:.6f}" />
            <size value="{size:.6f}" />
            <pull value="{pull:.6f}" />
            <faceCamera value="true" />
          </corona>
          <color value="0x{colour:08X}" />
          <intensity value="{spec['light_intensity']:.6f}" />
          <lightGroup value="1" />
          <rotate value="{str(rotate).lower()}" />
          <scale value="{str(not rotate).lower()}" />
          <scaleFactor value="{1 if rotate else 5}" />
          <flash value="{str(not rotate).lower()}" />
          <light value="{str(key != '-').lower()}" />
          <spotLight value="false" />
          <castShadows value="false" />
        </Item>
"""


def siren_item(spec: dict, siren_id: int) -> str:
    """The Sirens <Item> of a normalised spec (siren_spec) under `siren_id`."""
    (left_head, right_head), (left_tail, right_tail) = spec["sides"]["head_lights"], spec["sides"]["tail_lights"]
    head = f"""    <Item>
      <id value="{siren_id}" />
      <name>{spec['name']}</name>
      <timeMultiplier value="{spec['time_multiplier']:.6f}" />
      <lightFalloffMax value="10.000000" />
      <lightFalloffExponent value="10.000000" />
      <lightInnerConeAngle value="2.290610" />
      <lightOuterConeAngle value="70.000000" />
      <lightOffset value="0.000000" />
      <textureName>VehicleLight_sirenlight</textureName>
      <sequencerBpm value="{spec['bpm']}" />
      <leftHeadLight>
        <sequencer value="{left_head}" />
      </leftHeadLight>
      <rightHeadLight>
        <sequencer value="{right_head}" />
      </rightHeadLight>
      <leftTailLight>
        <sequencer value="{left_tail}" />
      </leftTailLight>
      <rightTailLight>
        <sequencer value="{right_tail}" />
      </rightTailLight>
      <leftHeadLightMultiples value="1" />
      <rightHeadLightMultiples value="1" />
      <leftTailLightMultiples value="1" />
      <rightTailLightMultiples value="1" />
      <useRealLights value="true" />
      <sirens>
"""
    lamps = "".join(siren_lamp(n, spec) for n in range(1, SIREN_LAMPS + 1))
    return head + lamps + "      </sirens>\n    </Item>\n"


def load_siren_spec(preset: str | None, path: Path | None, lamps: str | None) -> dict:
    if preset is not None:
        if preset not in SIREN_PRESETS:
            raise SystemExit(f"--siren-preset {preset}: one of {', '.join(SIREN_PRESETS)}")
        return siren_spec(SIREN_PRESETS[preset], lamps)
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise SystemExit(f"--siren-spec {path}: {exc}") from exc
    return siren_spec(raw, lamps)


def put_sirens(carcols: str, variations: str, item: str, siren_id: int, notes: list[str]) -> tuple[str, str]:
    """Replace the carcols Sirens with `item` (inserted after </Lights> when there were none) and point
    every non-zero carvariations <sirenSettings> at `siren_id` (0, no siren, stays)."""
    block = re.search(r"<Sirens>.*?</Sirens>|<Sirens\s*/>", carcols, re.S)
    replacement = f"<Sirens>\n{item}  </Sirens>"
    if block is not None:
        if siren_ids(carcols):
            notes.append(f"the mod's Sirens items {siren_ids(carcols)} replaced by the spec item {siren_id}")
        carcols = carcols.replace(block.group(0), replacement, 1)
    elif carcols.count("</Lights>") == 1:
        carcols = carcols.replace("</Lights>", f"</Lights>\n  {replacement}", 1)
    else:
        carcols = carcols.replace("</Kits>", f"</Kits>\n  {replacement}", 1)
        if replacement not in carcols:
            raise SystemExit("carcols has no </Lights> or </Kits> to place the Sirens item after")

    def one(match: re.Match) -> str:
        if match.group(1) == "0":
            return match.group(0)
        notes.append(f"sirenSettings {match.group(1)} -> {siren_id} (spec item)")
        return f'<sirenSettings value="{siren_id}" />'

    variations, count = re.subn(r'<sirenSettings value="(\d+)"\s*/>', one, variations)
    if not count:
        notes.append("carvariations names no sirenSettings; the spec item is unused")
    return carcols, variations


WHEEL_TYPES = 13  # VWT_SPORT .. the last Benny's type: the engine's per-type wheel arrays


def wheel_types(block: str) -> list[tuple[int, int, int, int]]:
    """Top-level children of a <Wheels> body, self-closing ones included: (start, end, inner start, inner end)."""
    spans, depth, start, inner = [], 0, 0, 0
    for match in re.finditer(r"<([A-Za-z_][\w.:-]*)\b[^>]*?/>|<([A-Za-z_][\w.:-]*)\b[^>]*>|</[^>]+>", block):
        tag = match.group(0)
        if tag.startswith("</"):
            depth -= 1
            if depth == 0:
                spans.append((start, match.end(), inner, match.start()))
        elif tag.endswith("/>"):
            if depth == 0:
                spans.append((match.start(), match.end(), match.end(), match.end()))
        else:
            if depth == 0:
                start, inner = match.start(), match.end()
            depth += 1
    return spans


def keep_wheels(
    carcols: str, shipped: set[str], label_prefix: str | None, texts: dict[int, str], notes: list[str]
) -> tuple[str, list[tuple[str, str]]]:
    """Drop wheels whose model is not shipped (a <Wheels> left empty is removed); label the kept ones."""
    block = re.search(r"\n?[ \t]*<Wheels>(.*?)</Wheels>", carcols, re.S)
    if block is None:
        return carcols, []
    body, labels, out, last, counter = block.group(1), [], [], 0, 0
    types = wheel_types(body)
    if len(types) > WHEEL_TYPES:
        raise SystemExit(f"<Wheels> has {len(types)} wheel types; the game has {WHEEL_TYPES}")
    kept = 0
    for index, (start, end, inner_start, inner_end) in enumerate(types):
        inner = body[inner_start:inner_end]
        items_out = []
        for s, e in items(inner):
            wheel = inner[s:e]
            name = re.search(r"<wheelName>\s*([^<]*?)\s*</wheelName>", wheel)
            name = name.group(1) if name else ""
            if name.lower() not in shipped:
                notes.append(f"wheel {name or '(unnamed)'} (type {index}) dropped: its model is not shipped")
                continue
            variation = re.search(r"<wheelVariation>\s*([^<]*?)\s*</wheelVariation>|<wheelVariation\s*/>", wheel)
            if variation is not None and (variation.group(1) or "").lower() not in shipped:
                wheel = wheel.replace(variation.group(0), f"<wheelVariation>{name}</wheelVariation>", 1)
            if label_prefix is not None:
                counter += 1
                key = f"{label_prefix}_WHEEL_{counter}"
                if not LABEL_KEY.fullmatch(key) or len(key) > runtime_pack.NAME_MAX:
                    raise SystemExit(f"wheel label key {key!r} is not [A-Za-z0-9_] of at most {runtime_pack.NAME_MAX}")
                old = re.search(r"<modShopLabel>\s*([^<]*?)\s*</modShopLabel>|<modShopLabel\s*/>", wheel)
                text = texts.get(joaat(old.group(1) or "")) if old else None
                if not (text and LABEL_TEXT.fullmatch(text) and len(text) <= runtime_pack.NAME_MAX):
                    text = part_name(name)
                new = f"<modShopLabel>{key}</modShopLabel>"
                wheel = wheel.replace(old.group(0), new, 1) if old else wheel.replace("</Item>", f"  {new}\n</Item>", 1)
                labels.append((key, text))
            items_out.append(wheel)
            kept += 1
        rebuilt = f"<Item>\n      {chr(10).join(items_out)}\n    </Item>" if items_out else "<Item />"
        out.append(body[last:start] + rebuilt)
        last = end
    if not kept:
        return carcols.replace(block.group(0), "", 1), []
    rebuilt = block.group(0).replace(body, "".join(out) + body[last:], 1)
    return carcols.replace(block.group(0), rebuilt, 1), labels


def livery_labels(
    carcols: str, texts: dict[int, str], notes: list[str], prefix: str
) -> tuple[str, list[tuple[str, str]]]:
    """Pack-owned keys PREFIX_LIV_<n> for the kits' <liveryNames> keys the mod's text table defines (rewritten in
    the carcols) and their KEY=TEXT rows. The mod's own keys are often the game's (the vans123 aventador names
    ZENTORNO_LV1..6): a pack label never replaces game text, so a row under such a key would show the game's name."""
    labels: list[tuple[str, str]] = []

    def rename(match: re.Match) -> str:
        def one(item: re.Match) -> str:
            key = item.group(1)
            text = texts.get(joaat(key))
            if not (text and LABEL_TEXT.fullmatch(text) and len(text) <= runtime_pack.NAME_MAX):
                notes.append(f"livery name {key}: not in the mod's text table (a retail key shows its game text)")
                return item.group(0)
            new = f"{prefix}_LIV_{len(labels) + 1}"
            if not LABEL_KEY.fullmatch(new) or len(new) > runtime_pack.NAME_MAX:
                raise SystemExit(f"livery label key {new!r} is not [A-Za-z0-9_] of at most {runtime_pack.NAME_MAX}")
            labels.append((new, text))
            return f"<Item>{new}</Item>"

        return re.sub(r"<Item>\s*([^<]*?)\s*</Item>", one, match.group(0))

    return re.sub(r"<liveryNames>(.*?)</liveryNames>", rename, carcols, flags=re.S), labels


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--carcols", type=Path, required=True)
    parser.add_argument("--variations", type=Path, required=True)
    parser.add_argument("--prefix", default="GTAVMENU_")
    parser.add_argument("--kit-id", type=int, required=True)
    parser.add_argument("--light-id", type=int, required=True)
    parser.add_argument("--ship-model", action="append", default=[], help="mod part fragments shipped")
    parser.add_argument("--keep-sirens", action="store_true", help="keep the mod's Sirens (needs --siren-id)")
    parser.add_argument("--siren-id", type=int, help="first free u8 siren id; the kept Sirens are numbered from it")
    parser.add_argument("--siren-preset", help="write one Sirens item from this preset (needs --siren-id)")
    parser.add_argument("--siren-spec", type=Path, help="write one Sirens item from this JSON spec (needs --siren-id)")
    parser.add_argument("--siren-lamps", help="20 group keys for siren1..siren20 of this car, '-' = unused")
    parser.add_argument("--list-siren-presets", action="store_true", help="print the siren presets and exit")
    parser.add_argument("--ship-wheel", action="append", default=[], help="wheel models (NAME.pdr) shipped")
    parser.add_argument("--census", type=Path, nargs="*", default=[])
    parser.add_argument("--shop-label-prefix", help="new <modShopLabel> keys PREFIX_<SLOT>_<n> ([A-Za-z0-9_])")
    parser.add_argument("--gxt2", type=Path, help="PC mod text table naming the original shop labels")
    parser.add_argument("--out-labels", type=Path, help="KEY=TEXT lines for the pack's label rows")
    parser.add_argument("--out-carcols", type=Path, required=True)
    parser.add_argument("--out-variations", type=Path, required=True)
    if argv is None:
        argv = sys.argv[1:]
    if "--list-siren-presets" in argv:
        for name, preset in SIREN_PRESETS.items():
            spec = siren_spec(preset)
            groups = ", ".join(
                f"{k}: {s:032b} rgb {c & 0xFFFFFF:06X} {m}" for k, (s, c, m, _, _) in spec["groups"].items()
            )
            print(f"{name}: \"{spec['name']}\" {spec['bpm']} bpm ({60000 // spec['bpm']} ms a step); {groups}")
        return 0
    args = parser.parse_args(argv)
    spec_given = args.siren_preset is not None or args.siren_spec is not None
    if args.siren_preset is not None and args.siren_spec is not None:
        raise SystemExit("--siren-preset and --siren-spec are alternatives")
    if spec_given and (args.siren_id is None or args.keep_sirens):
        raise SystemExit("--siren-preset/--siren-spec need --siren-id and replace the mod's Sirens (no --keep-sirens)")
    if args.siren_lamps is not None and not spec_given:
        raise SystemExit("--siren-lamps goes with --siren-preset or --siren-spec")
    if not 0 <= args.kit_id < 0xFFFF:
        raise SystemExit("kit id must be < 0xFFFF (u16 runtime key; 0xFFFF always appends)")
    if not spec_given and args.keep_sirens != (args.siren_id is not None):
        raise SystemExit("--keep-sirens and --siren-id go together")
    if (args.shop_label_prefix is None) != (args.out_labels is None) or (args.gxt2 and not args.out_labels):
        raise SystemExit("--shop-label-prefix and --out-labels go together (--gxt2 needs both)")
    if args.shop_label_prefix is not None and not LABEL_KEY.fullmatch(args.shop_label_prefix):
        raise SystemExit("--shop-label-prefix must be [A-Za-z0-9_]")
    texts = dict(gxt2_entries(args.gxt2.read_bytes(), Limits())) if args.gxt2 else {}
    spec = load_siren_spec(args.siren_preset, args.siren_spec, args.siren_lamps) if spec_given else None
    used = census(args.census) if args.census else None
    if used is not None:
        print("census: " + ", ".join(f"{k} {len(v)} (max {max(v, default=-1)})" for k, v in used.items()))
        if args.kit_id in used["kits"]:
            raise SystemExit(f"kit id {args.kit_id} collides with a retail kit id")
    check_ids("light", [args.light_id], None if used is None else used["lights"])

    carcols = args.carcols.read_text(encoding="utf-8-sig").replace("\r\n", "\n")
    variations = args.variations.read_text(encoding="utf-8-sig").replace("\r\n", "\n")
    renames: dict[str, str] = {}
    labels: list[tuple[str, str]] = []
    counters: dict[str, int] = {}

    kit_ids: list[int] = []

    def fix_kits(match: re.Match) -> str:
        body = match.group(2)
        out, last = [], 0
        for start, end in items(body):
            item = body[start:end]
            name = re.search(r"<kitName>\s*([^<]+?)\s*</kitName>", item).group(1)
            renames[name] = args.prefix + name
            item = item.replace(f"<kitName>{name}</kitName>", f"<kitName>{renames[name]}</kitName>")
            kit_ids.append(args.kit_id + len(kit_ids))  # one id per kit: a repeated id loses the later kit
            item = re.sub(r'<id value="\d+"\s*/>', f'<id value="{kit_ids[-1]}" />', item, count=1)
            visible = re.search(r"<visibleMods>(.*?)</visibleMods>", item, re.S)
            if visible:
                keep = [
                    visible.group(1)[s:e]
                    for s, e in items(visible.group(1))
                    if re.search(r"<modelName>\s*([^<]+?)\s*</modelName>", visible.group(1)[s:e]).group(1)
                    in args.ship_model
                ]
                if args.shop_label_prefix is not None:
                    for index, mod in enumerate(keep):
                        keep[index], key, text = relabel(mod, args.shop_label_prefix, counters, texts)
                        labels.append((key, text))
                inner = "\n        ".join(keep)
                item = item.replace(
                    visible.group(0), f"<visibleMods>{inner}</visibleMods>" if keep else "<visibleMods />"
                )
            out.append(body[last:start] + item)
            last = end
        out.append(body[last:])
        return match.group(1) + "".join(out) + match.group(3)

    carcols = KIT_ITEM.sub(fix_kits, carcols, count=1)
    for kit_id in kit_ids:
        if kit_id >= 0xFFFF or (used is not None and kit_id in used["kits"]):
            raise SystemExit(
                f"kit id {kit_id} (kit {kit_id - args.kit_id + 1}) collides with a retail kit id or 0xFFFF"
            )
    notes: list[str] = []
    lights = re.search(r"<Lights>(.*?)</Lights>", carcols, re.S)
    spans = items(lights.group(1)) if lights else []
    light_map: dict[int, int] = {}
    if spans:  # every Lights item is kept as --light-id, +1, ...; carvariations naming a mod light follow it
        kept_lights = []
        for index, (s, e) in enumerate(spans):
            item = lights.group(1)[s:e]
            found = ITEM_ID.search(item)
            if found is not None:
                light_map.setdefault(int(found.group(1)), args.light_id + index)
            kept_lights.append(
                re.sub(r'<id value="\d+"\s*/>', f'<id value="{args.light_id + index}" />', item, count=1)
            )
        check_ids("light", [args.light_id + i for i in range(len(spans))], None if used is None else used["lights"])
        carcols = carcols.replace(lights.group(0), "<Lights>\n    " + "\n    ".join(kept_lights) + "\n  </Lights>")
    variations = point(variations, "light", light_map, used, args.light_id if spans else None, notes)
    sirens: dict[int, int] = {}
    if spec is not None:
        check_ids("siren", [args.siren_id], None if used is None else used["sirens"])
        carcols, variations = put_sirens(carcols, variations, siren_item(spec, args.siren_id), args.siren_id, notes)
        sirens = {-1: args.siren_id}
    elif args.keep_sirens:
        carcols, variations, sirens = remap_sirens(carcols, variations, args.siren_id, used, notes)
        check_ids("siren", list(sirens.values()), None if used is None else used["sirens"])
        if not sirens:
            notes.append(f"--keep-sirens: the mod has no Sirens items; siren id {args.siren_id} is unused")
    else:  # carvariations naming a dropped mod siren fall back to retail siren 0; retail ids stay
        dropped = dict.fromkeys(siren_ids(carcols), 0)
        carcols = re.sub(r"<Sirens>.*?</Sirens>", "<Sirens />", carcols, flags=re.S)
        variations = point(variations, "siren", dropped, used, 0, notes)
    for old, new in renames.items():
        variations = re.sub(rf"<Item>\s*{re.escape(old)}\s*</Item>", f"<Item>{new}</Item>", variations)
    shipped_wheels = {name.lower() for name in args.ship_wheel}
    carcols, wheel_labels = keep_wheels(carcols, shipped_wheels, args.shop_label_prefix, texts, notes)
    labels += wheel_labels
    if args.out_labels is not None and args.gxt2:
        carcols, names = livery_labels(carcols, texts, notes, args.shop_label_prefix)
        labels += names
    if len({joaat(key) for key, _ in labels}) != len(labels):
        raise SystemExit("shop label keys collide after joaat")
    for path, text in ((args.out_carcols, carcols), (args.out_variations, variations)):
        xml.dom.minidom.parseString(text.encode("utf-8"))
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
    if args.out_labels is not None:
        args.out_labels.parent.mkdir(parents=True, exist_ok=True)
        args.out_labels.write_text("".join(f"{k}={t}\n" for k, t in labels), encoding="ascii", newline="\n")
        print(f"shop labels {len(labels)} -> {args.out_labels}")
    for note in dict.fromkeys(notes):
        print(f"note: {note}")
    kept = sirens or ("none in the mod" if args.keep_sirens else "dropped")
    if spec is not None:
        kept = f"spec item {args.siren_id} ({spec['name']})"
    wheels = len(re.findall(r"<wheelName>", carcols))
    ids = f"kit id {args.kit_id}" if len(kit_ids) < 2 else f"kit ids {kit_ids[0]}-{kit_ids[-1]}"
    lights_kept = sorted(set(light_map.values())) or [args.light_id]
    light_text = (
        f"light id {lights_kept[0]}" if len(lights_kept) < 2 else f"light ids {lights_kept[0]}-{lights_kept[-1]}"
    )
    print(f"kits renamed {renames}; {ids}; {light_text}; sirens {kept}; wheels {wheels}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

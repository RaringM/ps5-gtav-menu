#!/usr/bin/env python3
"""Sources for a weapon with its own model (WEAPON_METADATA_FILE, type 72).

Clones a weapon model (drawable + texture dictionary: a retail one from your own game, or a PC model
converted onto it) under a new name, registers it with a weaponarchetypes-style meta and clones a
retail weapon onto it in a weapons.meta, plus its animations:

- The model name, the drawable member name and the archetype name must be the same: the game's
  weapon model InitData step finds the drawable by the model's own name, so `<model>.pdr` is the
  drawable.
  `txdName` names the texture dictionary member `<model>.ptd`.
- AddWeaponModel has no name check, so the model name must be new (the worker refuses known names).
- Data rows load in descriptor order: WEAPON_METADATA_FILE, then WEAPONINFO_FILE, then
  WEAPON_ANIMATIONS_FILE (`build_args` emits them so).

  make_weapon_model_pack.py --pdr w_pi_pistol.pdr --ptd w_pi_pistol.ptd \\
      --weapons retail/weapons.meta --animations retail/weaponanimations.meta \\
      --archetypes retail/weaponarchetypes.meta --out SRC_DIR [--tint ffe000 | --tint none] \\
      [--build gtavmenu-wmodel-v1 --output-root build/custom-assets]

Inputs: loose RSC7 members and the decrypted retail XML metas, all from your own game.
Omitted meta paths use the verified --templates cache (default build/retail-templates), populated
by export-templates / fetch-templates. Explicit --weapons/--animations/--archetypes/--components
remain available for a donor mod's own XML, including DLC donors outside the base template.
Writes SRC_DIR/{<model>.pdr, <model>.ptd, <prefix>_archetypes.meta, <prefix>_weapons.meta,
<prefix>_anims.meta} (--meta-prefix, default wmodel; packs active together need distinct data file
names, so menu-ctl.sh convert-weapon uses the model name); refuses to overwrite. --tint recolours the copy's diffuse texture (named like
the source model) and its `_dpal` tint palette to one flat colour; every block is rewritten the
same way, so the GPU tiling needs no decoding. --build runs build_runtime_pack.py (next to this tool)
with the rows in load order and a `weapon` spawn row (Weapon Browser -> Custom); the builder adds the
weapon wheel icon row of the weapon's slot.

The donor (--source) decides the weapon class:
its CWeaponInfo (fire type, ammo, stats, flags), its animation rows, its model's archetype row (lodDist) and
its attachment points; the wheel icon row is its WheelSlot's default. Donors that cannot work are refused
with the reason: no weapon model (unarmed, gadgets, damage types), vehicle weapons, FireType NONE, particle
sprayers (petrol can, fire extinguisher). An attachment point whose bone the model lacks is dropped, and a
donor whose default component (a clip, a sniper scope) needs such a bone is refused. `--classify` prints
the donor's class line and exits (menu-ctl.sh convert-weapon runs it first); `--orders auto` puts the new
slot right after the donor's slot in each order list.

Attachments as components: `--attachment F.pdr` (repeated; F.ptd next to it)
takes a converted model named like a retail component model of the donor (its magazine w_ar_carbinerifle_mag1, a
scope w_at_scope_medium, ...; `--classify --components` lists them) and adds a clone of that component on the new
model `<model>_<suffix>` (COMPONENT_<weapon>_<suffix>), listed on the same attach point of the new weapon. A default
component (the magazine) replaces the donor's default there; the others get a Custom Packs toggle row each. Needs
`--components` (the retail weaponcomponents.meta); rows load 72 -> 79 (`<prefix>_components.meta`) -> 78 -> 118.

Tints: `--tint-palette auto` (menu-ctl.sh convert-weapon's default) finishes a model converted
by tools/weapon_tint_drawable.py: the tint palette in the .ptd, the tinted diffuse textures brightened, and a
`<!-- gtavmenu tints=palette|none -->` hint in the weapons meta, which build_runtime_pack.py (--build) turns into the
pack.cfg `tints` row the menu reads (Weapon Tint skips a `none` weapon); a model without a tint material prints one
`tints: none (<reason>)` line.
"""

from __future__ import annotations

import argparse
import copy
import json
import re
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
import zlib
from dataclasses import dataclass, replace
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

from gtavmenu_tools import retail_templates  # noqa: E402
from gtavmenu_tools.asset_formats import Limits  # noqa: E402
from gtavmenu_tools.ps5_texture_reader import parse_ps5_texture_dictionary  # noqa: E402
from gtavmenu_tools.ps5_texture_writer import (  # noqa: E402
    Ps5TextureInput,
    Ps5TextureMetadata,
    Ps5TextureStorage,
    plan_ps5_texture_dictionary,
    write_ps5_texture_dictionary,
)
from gtavmenu_tools.texture_layout import surface_layout, tile_mips, untile_mips  # noqa: E402
from gtavmenu_tools.texture_names import SOURCE_NAME_POLICY  # noqa: E402

META_PREFIX = "wmodel"


def meta_files(prefix: str = META_PREFIX) -> tuple[str, str, str]:
    """The archetypes, weapons and animations data file names (unique per pack among active packs)."""
    return f"{prefix}_archetypes.meta", f"{prefix}_weapons.meta", f"{prefix}_anims.meta"


def components_file(prefix: str = META_PREFIX) -> str:
    """The WEAPONCOMPONENTSINFO_FILE data file name (packs with --attachment)."""
    return f"{prefix}_components.meta"


ARCHETYPES_FILE, WEAPONS_FILE, ANIMATIONS_FILE = meta_files()
# Load order of the data rows (types 72, 78, 118).
DATA_ROWS = (
    ("WEAPON_METADATA_FILE", ARCHETYPES_FILE),
    ("WEAPONINFO_FILE", WEAPONS_FILE),
    ("WEAPON_ANIMATIONS_FILE", ANIMATIONS_FILE),
)
_NAME = re.compile(r"[a-z0-9_]{1,40}")
_LIMITS = replace(Limits(), max_file_bytes=1 << 28, max_total_bytes=1 << 30)
_BC1, _BC3, _BGRA8 = 71, 77, 87  # PS5 format codes: BC1 / BC3 diffuse (suppressor / pistol), palette


class WeaponModelError(ValueError):
    pass


@dataclass(frozen=True)
class Spec:
    model: str = "w_pi_gmpistol"
    source_model: str = "w_pi_pistol"
    weapon: str = "WEAPON_GMPISTOL2"
    source: str = "WEAPON_PISTOL"
    slot: str = "SLOT_GMPISTOL2"
    label: str = "WT_GMPIST2"
    # Navigate orders next to SLOT_PISTOL (90, 110) and best order next to 190; weapon-v1 took
    # 91/111/191, so both packs can be active together.
    orders: tuple[int, int] = (92, 112)
    best: int = 192
    donor_model: str = ""  # the donor's own model (component model names drop it as a prefix)

    def check(self) -> None:
        for name in (self.model, self.source_model):
            if not _NAME.fullmatch(name):
                raise WeaponModelError(f"model name {name!r} must be lower-case [a-z0-9_]")
        if self.model == self.source_model:
            raise WeaponModelError("the clone needs a new model name (AddWeaponModel has no name check)")
        if self.weapon == self.source or not re.fullmatch(r"WEAPON_[A-Z0-9_]+", self.weapon):
            raise WeaponModelError(f"weapon name {self.weapon!r} must be a new WEAPON_* name")
        if not re.fullmatch(r"SLOT_[A-Z0-9_]+", self.slot) or not re.fullmatch(r"[A-Z0-9_]{1,63}", self.label):
            raise WeaponModelError(f"slot {self.slot!r} must be SLOT_* and label {self.label!r} [A-Z0-9_]")


# ---- models -------------------------------------------------------------------------------------


def read_rsc7(blob: bytes, label: str) -> tuple[bytes, bytes]:
    """(16-byte RSC7 header, inflated payload) of a loose resource."""
    if blob[:4] != b"RSC7" or len(blob) < 17:
        raise WeaponModelError(f"{label}: not a loose RSC7 resource")
    stream = zlib.decompressobj(-15)
    payload = stream.decompress(blob[16:])
    if not stream.eof or stream.unused_data:
        raise WeaponModelError(f"{label}: payload does not raw-inflate to its end")
    return blob[:16], payload


def write_rsc7(header: bytes, payload: bytes) -> bytes:
    deflate = zlib.compressobj(9, zlib.DEFLATED, -15)
    return header + deflate.compress(payload) + deflate.flush()


def rgb565(rgb: tuple[int, int, int]) -> int:
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def tint_bc3(storage: bytes, rgb: tuple[int, int, int]) -> bytes:
    """Every BC3 block's colour half set to one flat colour; the alpha half is kept."""
    if len(storage) % 16:
        raise WeaponModelError("BC3 storage is not whole 16-byte blocks")
    colour = struct.pack("<HHI", rgb565(rgb), rgb565(rgb), 0)
    out = bytearray(storage)
    for at in range(0, len(out), 16):
        out[at + 8 : at + 16] = colour
    return bytes(out)


def tint_bc1(storage: bytes, rgb: tuple[int, int, int]) -> bytes:
    """Every BC1 block set to one flat colour (both endpoints, all indices 0: opaque colour 0)."""
    if len(storage) % 8:
        raise WeaponModelError("BC1 storage is not whole 8-byte blocks")
    return struct.pack("<HHI", rgb565(rgb), rgb565(rgb), 0) * (len(storage) // 8)


def tint_bgra8(storage: bytes, rgb: tuple[int, int, int]) -> bytes:
    """Every BGRA8 texel set to one colour; alpha is kept."""
    if len(storage) % 4:
        raise WeaponModelError("BGRA8 storage is not whole texels")
    r, g, b = rgb
    out = bytearray(storage)
    out[0::4], out[1::4], out[2::4] = (bytes([v]) * (len(out) // 4) for v in (b, g, r))
    return bytes(out)


def tint_dictionary(blob: bytes, diffuse: str, rgb: tuple[int, int, int]) -> tuple[bytes, list[str]]:
    """Recolour texture `diffuse` (BC3 or BC1) and `<diffuse>_dpal` (BGRA8, optional) of a PS5 ptd.

    Only those textures' graphics storage changes; the result is re-parsed and every other
    texture and all structure compared. Returns (resource, recoloured texture names).
    """
    before = parse_ps5_texture_dictionary(blob, _LIMITS)
    header, payload = read_rsc7(blob, "ptd")
    system = before["header"]["systemBytes"]
    rows = {t["name"]: t for t in before["textures"]}
    targets = {diffuse: (_BC3, _BC1), f"{diffuse}_dpal": (_BGRA8,)}
    tints = {_BC1: tint_bc1, _BC3: tint_bc3, _BGRA8: tint_bgra8}
    if diffuse not in rows:
        raise WeaponModelError(f"ptd has no diffuse texture {diffuse!r}")
    out = bytearray(payload)
    done = []
    for name, codes in targets.items():
        row = rows.get(name)
        if row is None:
            continue
        if row["formatCode"] not in codes:
            raise WeaponModelError(f"{name}: format {row['format']} is not one of the expected codes {codes}")
        at, size = system + row["graphicsOffset"], row["storageBytes"]
        out[at : at + size] = tints[row["formatCode"]](bytes(out[at : at + size]), rgb)
        done.append(name)
    result = write_rsc7(header, bytes(out))
    after = parse_ps5_texture_dictionary(result, _LIMITS)
    for old, new in zip(before["textures"], after["textures"], strict=True):
        same = {k: v for k, v in old.items() if k not in ("storageSha256", "allocation")}
        if same != {k: v for k, v in new.items() if k not in ("storageSha256", "allocation")}:
            raise WeaponModelError(f"{old['name']}: texture structure changed")
        if (old["storageSha256"] != new["storageSha256"]) != (old["name"] in done):
            raise WeaponModelError(f"{old['name']}: unexpected storage change")
    return result, done


# ---- tint palette -----------------------------------------------------------------------------

# Retail weapon bodies draw with weapon_normal_spec_detail_palette: a bright grey diffuse multiplied by a colour of
# the DiffuseTexPal palette (BGRA8 128x32, <model>_dpal); the diffuse alpha picks the column, the engine's weapon
# shader effect writes the tint index into DiffuseTexPaletteSelector, which picks the row (rows 0..7 = Normal, Green,
# Gold, Pink, Army, LSPD, Orange, Platinum; later rows repeat row 7 in the non-MK2 palettes). tools/
# weapon_tint_drawable.py retargets a converted model's body shaders to it and convert_pc_drawable binds DiffuseTexPal to
# TINT_PALETTE (a white 16x16 stand-in); tint_palette_dictionary makes that the real palette and the diffuse textures
# retail-like.
TINT_PALETTE = "gm_weapon_tint_pal"
PALETTE_SHADERS = frozenset(
    {"weapon_normal_spec_detail_palette", "weapon_normal_spec_cutout_palette", "weapon_normal_spec_palette"}
)
PALETTE_WIDTH, PALETTE_HEIGHT, PALETTE_TILE = 128, 32, 5  # the retail palettes' shape and tile mode (w_pi_pistol_dpal)
_PALETTE_METADATA = 0x800219  # metadata word of the retail BGRA8 palettes (w_pi_pistol_dpal); BC1/BC3 rows differ
# Tint rows 1..7 (R, G, B): own colours named after the eight standard tints, darker than white so a tinted body
# reads as painted; row 0 is 255/gain per column group (the model's own look). Relative brightness (luma / 255):
# Green .29, Gold .50, Pink .48, Army .40, LSPD .26, Orange .44, Platinum .67.
TINT_ROWS = (
    (60, 85, 45),  # 1 Green
    (160, 125, 55),  # 2 Gold
    (175, 95, 135),  # 3 Pink
    (115, 100, 70),  # 4 Army
    (45, 65, 120),  # 5 LSPD
    (180, 90, 30),  # 6 Orange
    (170, 170, 175),  # 7 Platinum (rows 8..31 repeat it, as retail)
)
TINT_GAINS = (1, 1.5, 2, 3, 4, 6, 8, 12)  # diffuse brightening levels: one palette group (alpha value) each
_BC1_GROUP = len(TINT_GAINS)  # BC1 diffuse (alpha reads 1.0 -> palette column 127): one shared group
_TARGET_PEAK = 0.97  # the brightened diffuse's 99th percentile channel value (retail grey diffuses reach ~0.94)


def _channels(colour: int) -> tuple[int, int, int]:
    return colour >> 11, (colour >> 5) & 63, colour & 31


_GREY_CHROMA = 12  # 8-bit channel spread of a grey after RGB565 rounding (5-bit steps 8.2, 6-bit 4.0)


def scale565(colour: int, gain: float) -> int:
    """An RGB565 colour times `gain`, requantized with rounding and clamped. A grey (its channels within
    _GREY_CHROMA) stays grey: the gain would turn its rounding spread into a visible colour cast."""
    r, g, b = _channels(colour)
    rgb = (r * 255 / 31, g * 255 / 63, b * 255 / 31)
    if max(rgb) - min(rgb) <= _GREY_CHROMA:
        rgb = (sum(rgb) / 3,) * 3
    r, g, b = (min(255.0, v * gain) for v in rgb)
    return (round(r * 31 / 255) << 11) | (round(g * 63 / 255) << 5) | round(b * 31 / 255)


def colour_peak(blocks: bytes, block_bytes: int) -> float:
    """99th percentile of the largest normalized channel over every colour endpoint (BC1: 8-byte blocks, BC3: 16)."""
    at = 8 if block_bytes == 16 else 0
    values = []
    for start in range(0, len(blocks), block_bytes):
        for colour in struct.unpack_from("<HH", blocks, start + at):
            r, g, b = _channels(colour)
            values.append(max(r / 31, g / 63, b / 31))
    if not values:
        raise WeaponModelError("diffuse texture has no colour blocks")
    values.sort()
    return values[min(len(values) - 1, int(len(values) * 0.99))]


def tint_gain(peak: float) -> float:
    """The largest TINT_GAINS level that keeps the 99th percentile at or below _TARGET_PEAK."""
    return max((g for g in TINT_GAINS if peak * g <= _TARGET_PEAK), default=1)


def brighten_bc1(storage: bytes, gain: float) -> bytes:
    """Every BC1 block's endpoints times `gain` (scale565); the block's mode is kept: an ordering the
    clamp flips is swapped back with its indices remapped, equal endpoints in 4-colour mode take index 0."""
    out = bytearray(storage)
    swap4 = (1, 0, 3, 2)  # 4-colour: c0, c1, 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1
    swap3 = (1, 0, 2, 3)  # 3-colour: c0, c1, mid, transparent
    for at in range(0, len(out) - len(out) % 8, 8):
        c0, c1, bits = struct.unpack_from("<HHI", out, at)
        four = c0 > c1
        n0, n1 = scale565(c0, gain), scale565(c1, gain)
        if four and n0 == n1:
            bits = 0
        elif four != (n0 > n1) and n0 != n1:
            n0, n1 = n1, n0
            table = swap4 if four else swap3
            bits = sum(table[(bits >> (2 * i)) & 3] << (2 * i) for i in range(16))
        struct.pack_into("<HHI", out, at, n0, n1, bits)
    return bytes(out)


def brighten_bc3(storage: bytes, gain: float, alpha: int) -> bytes:
    """Every BC3 block's colour endpoints times `gain` (scale565; BC3 colour is always 4-colour) and its alpha one
    value."""
    out = bytearray(storage)
    constant = bytes((alpha, alpha)) + bytes(6)
    for at in range(0, len(out) - len(out) % 16, 16):
        c0, c1 = struct.unpack_from("<HH", out, at + 8)
        n0, n1 = scale565(c0, gain), scale565(c1, gain)
        out[at : at + 8] = constant
        struct.pack_into("<HH", out, at + 8, n0, n1)
    return bytes(out)


def group_alpha(group: int) -> int:
    """Diffuse alpha of a BC3 palette group (one per TINT_GAINS level): 40..152 in steps of 16, retail's index
    values (the w_pi_pistol diffuse alpha clusters)."""
    return 40 + 16 * group


def group_column(group: int) -> int:
    """The palette column a group's diffuse alpha reads (BC3: alpha / 255 * 128; BC1: alpha 1.0, the last column)."""
    return PALETTE_WIDTH - 1 if group == _BC1_GROUP else group_alpha(group) * PALETTE_WIDTH // 255


def palette_pixels(groups: dict[int, float]) -> bytes:
    """Linear BGRA8 128x32 palette for {group: gain}: each column takes the group whose column (group_column) is
    nearest, so one group fills the whole palette and an index read a few columns off still lands in its group.
    Row 0 = 255/gain grey (the model's own look), rows 1..7 TINT_ROWS, rows 8..31 row 7; alpha 255."""
    if not groups:
        raise WeaponModelError("a tint palette needs at least one diffuse group")
    owner = [min(groups, key=lambda g: (abs(group_column(g) - column), g)) for column in range(PALETTE_WIDTH)]
    rows = []
    for row in range(PALETTE_HEIGHT):
        line = bytearray()
        for column in range(PALETTE_WIDTH):
            if row == 0:
                grey = round(255 / groups[owner[column]])
                colour = (grey, grey, grey)
            else:
                colour = TINT_ROWS[min(row, len(TINT_ROWS)) - 1]
            line += bytes((colour[2], colour[1], colour[0], 255))
        rows.append(bytes(line))
    return b"".join(rows)


@dataclass(frozen=True)
class TintPlan:
    diffuse: tuple[str, ...]  # texture names the palette shaders read as DiffuseTex
    materials: int  # palette shaders bound to TINT_PALETTE
    shaders: int  # every shader of the drawable


def tint_plan(report: dict) -> TintPlan | None:
    """The tinted diffuse textures from a convert_pc_drawable report (weapon_tint_drawable.py run), or None when no
    palette shader reads TINT_PALETTE. Refuses a tinted diffuse another shader also reads (its alpha changes)."""
    names = {s["index"]: s["name"] for s in report.get("shaders", [])}
    links = report.get("textureLinks", [])
    tinted = {
        link["shaderIndex"]
        for link in links
        if link["parameter"] == "DiffuseTexPal"
        and link["name"].lower() == TINT_PALETTE
        and names.get(link["shaderIndex"]) in PALETTE_SHADERS
    }
    if not tinted:
        return None
    diffuse = sorted(
        {link["name"].lower() for link in links if link["shaderIndex"] in tinted and link["parameter"] == "DiffuseTex"}
    )
    shared = sorted({link["name"].lower() for link in links if link["shaderIndex"] not in tinted} & set(diffuse))
    if shared:
        raise WeaponModelError(
            f"{', '.join(shared)} is also read by an untinted shader (its alpha becomes the palette index); "
            "convert with --no-tints"
        )
    if not diffuse:
        raise WeaponModelError("the tinted shaders read no diffuse texture")
    return TintPlan(tuple(diffuse), len(tinted), len(names))


def _ptd_input(row: dict, allocation: bytes, metadata: int | None = None) -> Ps5TextureInput:
    views = {
        "000000000000000000000000000000004100ffffffffffff0000000000000000": "ordinary2d-serialized-v1",
        "000000000000000000000000000000001400ffffffffffff0000000000000000": "constructor-v1",
    }
    meta = row["metadata"]
    view = views.get(meta["rawViewHex"])
    if view is None:
        raise WeaponModelError(f"{row['name']}: texture view is not a converter's (not a converted .ptd?)")
    layout = surface_layout(row["width"], row["height"], row["mipLevels"], row["format"], row["tileMode"])
    return Ps5TextureInput(
        row["name"],
        row["width"],
        row["height"],
        row["formatCode"],
        row["mipLevels"],
        row["tileMode"],
        allocation,
        layout.alignment_bytes,
        Ps5TextureMetadata(
            meta["flags"],
            meta["metadataCandidate"] if metadata is None else metadata,
            meta["referenceCount"],
            meta["viewType"],
            view,
        ),
    )


def tint_palette_dictionary(blob: bytes, plan: TintPlan) -> tuple[bytes, list[str]]:
    """The converted .ptd with TINT_PALETTE as the 128x32 BGRA8 tint palette and each tinted diffuse texture
    brightened by its gain (and, BC3, its alpha set to its palette group's index). Every other texture keeps its
    bytes (re-read and compared). Returns (resource, one line per diffuse texture)."""
    before = parse_ps5_texture_dictionary(blob, _LIMITS)
    rows = {t["name"].lower(): t for t in before["textures"]}
    stand_in = rows.get(TINT_PALETTE)
    if stand_in is None:
        raise WeaponModelError(f"the .ptd has no {TINT_PALETTE} stand-in (convert with tools/weapon_tint_drawable.py)")
    groups: dict[int, float] = {}
    changed: dict[str, bytes] = {}
    lines = []
    bc1 = []
    for name in plan.diffuse:
        row = rows.get(name)
        if row is None:
            raise WeaponModelError(f"the .ptd has no tinted diffuse texture {name}")
        if row["formatCode"] not in (_BC1, _BC3):
            raise WeaponModelError(f"{row['name']}: a tinted diffuse must be BC1 or BC3, not {row['format']}")
        layout = surface_layout(row["width"], row["height"], row["mipLevels"], row["format"], row["tileMode"])
        top = untile_mips(row["allocation"], layout)[0]
        peak = colour_peak(top, layout.block_bytes)
        gain = tint_gain(peak)
        if row["formatCode"] == _BC1:
            bc1.append((row, gain, peak))
            continue
        group = TINT_GAINS.index(gain)
        groups[group] = gain
        changed[row["name"]] = brighten_bc3(row["allocation"], gain, group_alpha(group))
        lines.append(f"tint diffuse {row['name']} BC3 peak={peak:.3f} gain={gain} alpha={group_alpha(group)}")
    if bc1:
        gain = min(g for _, g, _ in bc1)  # one BC1 group (alpha 1.0): the smallest gain fits every member
        groups[_BC1_GROUP] = gain
        for row, _, peak in bc1:
            changed[row["name"]] = brighten_bc1(row["allocation"], gain)
            lines.append(f"tint diffuse {row['name']} BC1 peak={peak:.3f} gain={gain} alpha=255")
    palette = surface_layout(PALETTE_WIDTH, PALETTE_HEIGHT, 1, "BGRA8", PALETTE_TILE)
    pixels = tile_mips((palette_pixels(groups),), palette)
    inputs = []
    for row in before["textures"]:
        if row is stand_in:
            shaped = dict(row, width=PALETTE_WIDTH, height=PALETTE_HEIGHT, mipLevels=1, format="BGRA8")
            shaped.update(formatCode=_BGRA8, tileMode=PALETTE_TILE)
            inputs.append(_ptd_input(shaped, pixels, _PALETTE_METADATA))
        else:
            inputs.append(_ptd_input(row, changed.get(row["name"], row["allocation"])))
    storage = [Ps5TextureStorage(t.name, len(t.allocation), t.storage_alignment) for t in inputs]
    planned = plan_ps5_texture_dictionary(storage, _LIMITS, name_policy=SOURCE_NAME_POLICY)["graphicsBytes"]
    page = None  # one graphics page up to 16 MiB, else <= 4 MiB pages (convert_pc_ytd_writer.graphics_pages)
    if planned > 16 << 20:
        page = max(4 << 20, 1 << (max(len(t.allocation) for t in inputs) - 1).bit_length())
    out = write_ps5_texture_dictionary(inputs, _LIMITS, name_policy=SOURCE_NAME_POLICY, graphics_page_bytes=page)
    header, payload = read_rsc7(out, "ptd")
    result = write_rsc7(header, payload)  # the writer stores; deflate like the converter's dictionaries
    after = {t["name"]: t for t in parse_ps5_texture_dictionary(result, _LIMITS)["textures"]}
    for row in before["textures"]:
        new = after.get(row["name"])
        expected = pixels if row is stand_in else changed.get(row["name"], row["allocation"])
        if new is None or new["allocation"][: len(expected)] != expected:
            raise WeaponModelError(f"{row['name']}: written texture differs")
    lines.append(f"tint palette {TINT_PALETTE} BGRA8 {PALETTE_WIDTH}x{PALETTE_HEIGHT} groups={len(groups)}")
    return result, lines


# ---- model skeleton -----------------------------------------------------------------------------

_SYS = 0x50000000  # system page base of resource pointers
_DRAWABLE_SKELETON = 0x18  # drawable SkeletonPointer (CodeWalker DrawableBase; PS5 Gen9 the same)
_SKELETON_BONES, _SKELETON_COUNT = 0x20, 0x5E  # CodeWalker Skeleton
_BONE_BYTES, _BONE_NAME = 0x50, 0x38  # CodeWalker Bone


def model_bones(pdr: bytes, label: str = "pdr") -> list[str]:
    """Bone names of a PS5 drawable's skeleton (the converted model keeps its template's skeleton)."""
    _, payload = read_rsc7(pdr, label)

    def pointer(at: int) -> int:
        if at < 0 or at + 8 > len(payload):
            raise WeaponModelError(f"{label}: skeleton pointer outside the resource")
        value = struct.unpack_from("<Q", payload, at)[0]
        if value and not _SYS <= value < _SYS + len(payload):
            raise WeaponModelError(f"{label}: not a drawable (pointer {value:#x} outside its system pages)")
        return value - _SYS if value else -1

    skeleton = pointer(_DRAWABLE_SKELETON)
    if skeleton < 0:
        return []
    if skeleton + _SKELETON_COUNT + 2 > len(payload):
        raise WeaponModelError(f"{label}: skeleton outside the resource")
    count = struct.unpack_from("<H", payload, skeleton + _SKELETON_COUNT)[0]
    bones = pointer(skeleton + _SKELETON_BONES)
    if not 0 < count <= 256 or bones < 0:
        raise WeaponModelError(f"{label}: unusable skeleton ({count} bones)")
    names = []
    for index in range(count):
        at = pointer(bones + index * _BONE_BYTES + _BONE_NAME)
        raw = payload[at : at + 64].split(b"\0", 1)[0] if at >= 0 else b""
        if not re.fullmatch(rb"[A-Za-z0-9_]{1,63}", raw):
            raise WeaponModelError(f"{label}: bone {index} has no readable name")
        names.append(raw.decode("ascii"))
    return names


# ---- donor class --------------------------------------------------------------------------------

# CWeaponInfo <Group> -> weapon class (the default wheel icons of gtavmenu_tools.weapon_wheel follow WheelSlot).
CLASSES = {
    "GROUP_PISTOL": "pistol",
    "GROUP_STUNGUN": "pistol",
    "GROUP_SMG": "smg",
    "GROUP_MG": "mg",
    "GROUP_RIFLE": "rifle",
    "GROUP_SHOTGUN": "shotgun",
    "GROUP_SNIPER": "sniper",
    "GROUP_HEAVY": "heavy",
    "GROUP_MELEE": "melee",
    "GROUP_THROWN": "throwable",
}
_GRIP_LEFT = "Gun_GripL"  # two-handed weapons place the left hand on this bone


@dataclass(frozen=True)
class Donor:
    name: str
    kind: str  # CLASSES value
    model: str  # the donor's own model, lower case
    group: str
    fire: str
    wheel: str
    ammo: str
    ammo_model: str  # projectile model of a PROJECTILE donor ("" otherwise)
    two_handed: bool
    attach: tuple[tuple[str, tuple[tuple[str, bool], ...]], ...]  # (bone, ((component, default), ...))

    def line(self) -> str:
        defaults = [c for _, comps in self.attach for c, d in comps if d]
        return (
            f"class={self.kind} donor={self.name} model={self.model} group={self.group} fire={self.fire} "
            f"wheel={self.wheel} ammo={self.ammo or 'none'} attach={len(self.attach)} "
            f"defaults={','.join(defaults) or 'none'}"
        )


def _weapon_infos(root: ET.Element) -> list[ET.Element]:
    return [i for i in root.iter("Item") if i.get("type") == "CWeaponInfo"]


def classify(retail: bytes, donor: str) -> Donor:
    """The donor's weapon class from its CWeaponInfo, or WeaponModelError naming why it cannot be cloned."""
    root = ET.fromstring(retail)
    found = [i for i in _weapon_infos(root) if i.findtext("Name") == donor]
    if len(found) != 1:
        raise WeaponModelError(
            f"weapons.meta: expected one {donor}, found {len(found)} (DLC weapons live in their DLC's weapons "
            "meta: give that file and its animations/archetypes metas)"
            if not found
            else f"weapons.meta: expected one {donor}, found {len(found)}"
        )
    info = found[0]
    text = {tag: (info.findtext(tag) or "").strip() for tag in ("Model", "Group", "FireType", "WheelSlot")}
    if donor.startswith("VEHICLE_WEAPON_"):
        raise WeaponModelError(f"{donor} is a vehicle weapon, not a hand weapon")
    if not text["Model"]:
        raise WeaponModelError(f"{donor} has no weapon model (unarmed, gadgets and damage types cannot be cloned)")
    if text["FireType"] in ("", "NONE"):
        raise WeaponModelError(f"{donor} fires nothing (FireType NONE: a gadget or carried object, not a weapon)")
    if text["FireType"] == "VOLUMETRIC_PARTICLE":
        raise WeaponModelError(
            f"{donor} sprays a particle stream (petrol can, fire extinguisher); not supported yet (its nozzle "
            "effects belong to the stock model)"
        )
    kind = CLASSES.get(text["Group"])
    if kind is None:
        raise WeaponModelError(
            f"{donor} is in {text['Group'] or 'no group'}, not a weapon class convert-weapon supports"
        )
    ammo_ref = info.find("AmmoInfo")
    ammo = (ammo_ref.get("ref") or "").strip() if ammo_ref is not None else ""
    ammo = "" if ammo.upper() == "NULL" else ammo
    ammo_model = ""
    if ammo and text["FireType"] == "PROJECTILE":
        rows = [
            i for i in root.iter("Item") if (i.get("type") or "").startswith("CAmmo") and i.findtext("Name") == ammo
        ]
        ammo_model = (rows[0].findtext("Model") or "").strip().lower() if rows else ""
    attach = []
    points = info.find("AttachPoints")
    for point in points if points is not None else []:
        bone = (point.findtext("AttachBone") or "").strip()
        components = point.find("Components")
        rows = []
        for item in components if components is not None else []:
            default = item.find("Default")
            rows.append(((item.findtext("Name") or "").strip(), default is not None and default.get("value") == "true"))
        attach.append((bone, tuple(rows)))
    return Donor(
        name=donor,
        kind=kind,
        model=text["Model"].lower(),
        group=text["Group"],
        fire=text["FireType"],
        wheel=text["WheelSlot"],
        ammo=ammo,
        ammo_model=ammo_model,
        two_handed="TwoHanded" in (info.findtext("WeaponFlags") or "").split(),
        attach=tuple(attach),
    )


def fit_attach_points(donor: Donor, bones: list[str] | None) -> tuple[set[str], list[str]]:
    """(attach bones to drop, notes) for a model with these bones; refuses a default component on a missing bone."""
    if bones is None:
        return set(), []
    have = {b.lower() for b in bones}
    drop: set[str] = set()
    notes = []
    for bone, components in donor.attach:
        if bone.lower() in have:
            continue
        defaults = [c for c, d in components if d]
        if defaults:
            raise WeaponModelError(
                f"{donor.name}'s default component {defaults[0]} attaches to {bone}, which the model does not have "
                f"(bones: {', '.join(bones)}); pick a donor whose default components fit the model"
            )
        drop.add(bone)
        notes.append(f"dropped attach point {bone} ({', '.join(c for c, _ in components)}): the model has no such bone")
    if donor.two_handed and donor.kind != "melee" and _GRIP_LEFT.lower() not in have:
        # (two-handed melee weapons such as the bat hold by animation: retail w_me_bat has no Gun_GripL either)
        notes.append(f"two-handed donor on a model without {_GRIP_LEFT}: the left hand is not placed on the weapon")
    if donor.ammo_model:
        notes.append(f"the projectile stays the donor's ammo model {donor.ammo_model} ({donor.ammo})")
    return drop, notes


def auto_orders(retail: bytes, donor: str) -> tuple[int, int, int]:
    """NAV1,NAV2,BEST: in each order list, the first number after the donor's slot that no retail slot uses."""
    root = ET.fromstring(retail)
    infos = [i for i in _weapon_infos(root) if i.findtext("Name") == donor]
    slot = (infos[0].findtext("Slot") or "").strip() if len(infos) == 1 else ""
    lists = []
    nav = root.find("SlotNavigateOrder")
    for item in nav if nav is not None else []:
        lists.append(item.find("WeaponSlots"))
    best = root.find("SlotBestOrder")
    lists.append(best.find("WeaponSlots") if best is not None else None)
    used = {int(o.get("value")) for o in root.iter("OrderNumber")}
    out = []
    for listing in lists:
        rows = {
            (e.findtext("Entry") or "").strip(): int(e.find("OrderNumber").get("value"))
            for e in (listing if listing is not None else [])
        }
        if slot not in rows:
            raise WeaponModelError(f"--orders auto: {donor}'s slot {slot or '?'} is not in every order list")
        free = next((n for n in range(rows[slot] + 1, rows[slot] + 10) if n not in used), None)
        if free is None:
            raise WeaponModelError(f"--orders auto: no free order number after {slot}; give --orders")
        out.append(free)
    if len(out) != 3:
        raise WeaponModelError("--orders auto: expected two navigate lists and one best list")
    return out[0], out[1], out[2]


# ---- attachments as components (F4.2) ------------------------------------------------------------

# Attach bone -> the menu row's word for a component there.
_COMPONENT_KINDS = {"WAPScop": "scope", "WAPScop_2": "scope", "WAPSupp": "suppressor", "WAPGrip": "grip",
                    "WAPClip": "magazine", "WAPFlshLasr": "flashlight"}  # fmt: skip


@dataclass(frozen=True)
class PackComponent:
    name: str  # new component, COMPONENT_<weapon short>_<suffix>
    source: str  # the donor's retail component it clones
    bone: str  # the donor attach point (AttachBone) listing it
    default: bool  # the retail component is the attach point's default (the new one replaces it)
    model: str  # new model, <model>_<suffix>
    source_model: str  # the retail component model the attachment is named like

    @property
    def kind(self) -> str:
        return _COMPONENT_KINDS.get(self.bone, "attachment")


def donor_components(retail: bytes, components: bytes, donor: str) -> list[tuple[str, str, bool, str]]:
    """(bone, component, default, model) for every component listed on the donor's attach points; model is the
    component's retail Model (lower case), "" when the components meta has no such component."""
    infos = [i for i in _weapon_infos(ET.fromstring(retail)) if i.findtext("Name") == donor]
    if len(infos) != 1:
        raise WeaponModelError(f"weapons.meta: expected one {donor}, found {len(infos)}")
    root = ET.fromstring(components)
    models = {
        (i.findtext("Name") or "").strip(): (i.findtext("Model") or "").strip().lower()
        for i in (root.find("Infos") if root.find("Infos") is not None else [])
    }
    rows = []
    for point in infos[0].iterfind("AttachPoints/Item"):
        bone = (point.findtext("AttachBone") or "").strip()
        for item in point.iterfind("Components/Item"):
            name = (item.findtext("Name") or "").strip()
            default = item.find("Default")
            rows.append((bone, name, default is not None and default.get("value") == "true", models.get(name, "")))
    return rows


def plan_components(retail: bytes, components: bytes, spec: Spec, attachments: list[str]) -> list[PackComponent]:
    """One PackComponent per attachment model name (a retail component model of the donor), or WeaponModelError."""
    rows = donor_components(retail, components, spec.source)
    short = spec.weapon.removeprefix("WEAPON_")
    donor_short = spec.source.removeprefix("WEAPON_")
    out = []
    for model in attachments:
        found = [r for r in rows if r[3] and r[3] == model.lower()]
        if not found:
            known = ", ".join(sorted({r[3] for r in rows if r[3]})) or "none"
            raise WeaponModelError(
                f"{model} is not the model of a component on {spec.source}'s attach points (they use: {known})"
            )
        bone, source, default, source_model = found[0]
        rest = source.removeprefix("COMPONENT_").removeprefix(f"{donor_short}_")
        name = f"COMPONENT_{short}_{rest}"
        suffix = source_model.removeprefix(f"{spec.source_model}_").removeprefix(f"{spec.donor_model}_")
        suffix = suffix if suffix != source_model else source_model.removeprefix("w_")
        new_model = f"{spec.model}_{suffix}"
        if not _NAME.fullmatch(new_model) or len(name) > 63:
            raise WeaponModelError(f"component model {new_model!r} / name {name!r} too long; shorten --model/--weapon")
        out.append(PackComponent(name, source, bone, default, new_model, source_model))
    if len({c.name for c in out}) != len(out):
        raise WeaponModelError("two attachments map to one component")
    return out


def components_meta(retail: bytes, components: list[PackComponent], weapon: str) -> str:
    """CWeaponComponentInfoBlob with the clones (new Name and Model) and an empty Data list: their ReloadData ref=
    names resolve against the core file's Data (the DLC layout; WCOMP-run1)."""
    infos = ET.fromstring(retail).find("Infos")
    if infos is None:
        raise WeaponModelError("weaponcomponents: no <Infos>")
    texts = []
    for c in components:
        found = [i for i in infos if i.findtext("Name") == c.source]
        if len(found) != 1:
            raise WeaponModelError(f"weaponcomponents: expected one {c.source}, found {len(found)}")
        info = copy.deepcopy(found[0])
        info.find("Name").text = c.name
        model = info.find("Model")
        if model is None or (model.text or "").strip().lower() != c.source_model:
            raise WeaponModelError(f"{c.source}: model is not {c.source_model}")
        model.text = c.model
        for item in info.iter():
            if item.text is not None and not item.text.strip():
                item.text = None
            item.tail = None
        ET.indent(info, space="  ", level=2)
        texts.append("    " + ET.tostring(info, encoding="unicode").rstrip())
    body = "\n".join(texts)
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<CWeaponComponentInfoBlob>
  <Data>
  </Data>
  <Infos>
{body}
  </Infos>
  <InfoBlobName>GTAVMenu {weapon} components</InfoBlobName>
</CWeaponComponentInfoBlob>
"""


def component_text(text: str, component: PackComponent) -> str:
    """Menu row text for a toggle component, within the 39-character spawn text limit."""
    base = re.sub(r"\s*\([^)]*\)\s*$", "", text) or text
    suffix = f": {component.kind} on/off"
    return base[: 39 - len(suffix)].rstrip() + suffix


# ---- metas --------------------------------------------------------------------------------------


def _archetype_item(root: ET.Element, model: str, wanted: tuple[str, ...]) -> str:
    found = []
    for name in dict.fromkeys(n for n in wanted if n):
        found = [item for item in root.iter("Item") if (item.findtext("modelName") or "").strip().lower() == name]
        if found:
            break
    if len(found) != 1:
        raise WeaponModelError(f"weaponarchetypes: expected one {next(n for n in wanted if n)}, found {len(found)}")
    ptfx = (found[0].findtext("ptfxAssetName") or "null").strip() or "null"
    lod = found[0].find("lodDist")
    lod_value = lod.get("value") if lod is not None and lod.get("value") else "30"
    return f"""    <Item>
      <modelName>{model}</modelName>
      <txdName>{model}</txdName>
      <ptfxAssetName>{ptfx}</ptfxAssetName>
      <lodDist value="{lod_value}"/>
    </Item>
"""


def archetypes_meta(
    retail: bytes, spec: Spec, donor_model: str | None = None, components: tuple[PackComponent, ...] = ()
) -> str:
    """One CWeaponModelInfo__InitData under the new model and txd name: the donor model's entry (its class's
    lodDist), else the source model's; then one per component model (its retail model's entry)."""
    root = ET.fromstring(retail)
    items = _archetype_item(root, spec.model, (donor_model or "", spec.source_model))
    items += "".join(_archetype_item(root, c.model, (c.source_model,)) for c in components)
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<CWeaponModelInfo__InitDataList>
  <InitDatas>
{items}  </InitDatas>
</CWeaponModelInfo__InitDataList>
"""


def _slot_items(slot: str, orders: tuple[int, ...]) -> str:
    return "".join(f"""
    <Item>
      <WeaponSlots>
        <Item>
          <OrderNumber value="{order}" />
          <Entry>{slot}</Entry>
        </Item>
      </WeaponSlots>
    </Item>""" for order in orders)


def weapons_meta(
    retail: bytes,
    spec: Spec,
    drop_attach: set[str] = frozenset(),
    components: tuple[PackComponent, ...] = (),
    tints: str | None = None,
) -> str:
    """CWeaponInfoBlob with the source weapon cloned as `spec.weapon` on `spec.model` (stock stats).

    Same layout as gtavmenu-weapon-v1 (WEAPON-run1 PASS): a new slot in both navigate lists and
    the best order, the CWeaponInfo in the second of the four Infos groups. `drop_attach` names
    attach bones (fit_attach_points) whose attach points are left out. Each pack component is listed
    on its attach point; a default one takes the default from the retail component it clones. `tints` ("palette"
    or "none") adds a `<!-- gtavmenu tints=... -->` hint (an XML comment: the engine's parser skips it), from which
    build_runtime_pack.py writes the pack.cfg `tints` row.
    """
    root = ET.fromstring(retail)
    classify(retail, spec.source)  # refuses donors that cannot work, with the reason
    found = [i for i in _weapon_infos(root) if i.findtext("Name") == spec.source]
    used = {int(o.get("value")) for o in root.iter("OrderNumber")}
    if used & {*spec.orders, spec.best}:
        raise WeaponModelError(f"order numbers {spec.orders}/{spec.best} already used")
    info = copy.deepcopy(found[0])
    for tag, value in (("Name", spec.weapon), ("Model", spec.model), ("Slot", spec.slot)):
        element = info.find(tag)
        if element is None:
            raise WeaponModelError(f"{spec.source} has no <{tag}>")
        element.text = value
    points = info.find("AttachPoints")
    if drop_attach and points is not None:
        wanted = {b.lower() for b in drop_attach}
        for point in list(points):
            if (point.findtext("AttachBone") or "").strip().lower() in wanted:
                points.remove(point)
    for c in components:
        point = next((p for p in info.iterfind("AttachPoints/Item") if p.findtext("AttachBone") == c.bone), None)
        listing = point.find("Components") if point is not None else None
        if listing is None:
            raise WeaponModelError(f"{spec.source} has no {c.bone} attach point")
        if c.default:
            for item in listing:
                flag = item.find("Default")
                if item.findtext("Name") == c.source and flag is not None:
                    flag.set("value", "false")
        item = ET.SubElement(listing, "Item")
        ET.SubElement(item, "Name").text = c.name
        ET.SubElement(item, "Default", value="true" if c.default else "false")
    label = info.find("HumanNameHash")
    if label is not None:
        label.text = spec.label
    ET.indent(info, space="  ", level=4)
    text = ET.tostring(info, encoding="unicode").rstrip()
    if tints not in (None, "palette", "none"):
        raise WeaponModelError(f"tints hint {tints!r} is not palette or none")
    hint = f"\n  <!-- gtavmenu tints={tints} -->" if tints else ""
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<CWeaponInfoBlob>
  <SlotNavigateOrder>{_slot_items(spec.slot, spec.orders)}
  </SlotNavigateOrder>
  <SlotBestOrder>
    <WeaponSlots>
      <Item>
        <OrderNumber value="{spec.best}" />
        <Entry>{spec.slot}</Entry>
      </Item>
    </WeaponSlots>
  </SlotBestOrder>
  <TintSpecValues />
  <FiringPatternAliases />
  <UpperBodyFixupExpressionData />
  <AimingInfos />
  <Infos>
    <Item>
      <Infos />
    </Item>
    <Item>
      <Infos>
        {text}
      </Infos>
    </Item>
    <Item>
      <Infos />
    </Item>
    <Item>
      <Infos />
    </Item>
  </Infos>
  <VehicleWeaponInfos />{hint}
  <Name>GTAVMenu {spec.weapon}</Name>
</CWeaponInfoBlob>
"""


def animations_meta(retail: bytes, spec: Spec) -> str:
    """Every animation set's source-weapon entry, re-keyed to the new weapon."""
    root = ET.fromstring(retail)
    out = ET.Element("CWeaponAnimationsSets")
    sets = ET.SubElement(out, "WeaponAnimationsSets")
    listing = root.find("WeaponAnimationsSets")
    for source_set in listing if listing is not None else []:
        listed = source_set.find("WeaponAnimations")
        matches = [i for i in (listed if listed is not None else []) if i.get("key") == spec.source]
        if not matches:
            continue
        if len(matches) != 1:
            raise WeaponModelError(f"set {source_set.get('key')}: {len(matches)} {spec.source} entries")
        new_set = ET.SubElement(sets, "Item", key=source_set.get("key"))
        fallback = source_set.find("Fallback")
        if fallback is not None:
            new_set.append(copy.deepcopy(fallback))
        clone = copy.deepcopy(matches[0])
        clone.set("key", spec.weapon)
        ET.SubElement(new_set, "WeaponAnimations").append(clone)
    if not len(sets):
        raise WeaponModelError(f"weaponanimations: no {spec.source} entries")
    for item in out.iter():
        if item.text is not None and not item.text.strip():
            item.text = None
        item.tail = None
    ET.indent(out, space="  ")
    return '<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(out, encoding="unicode") + "\n"


# ---- pack ---------------------------------------------------------------------------------------


def build_args(
    src: Path,
    pack_id: str,
    spec: Spec,
    archive: str,
    text: str,
    prefix: str = META_PREFIX,
    components: tuple[PackComponent, ...] = (),
) -> list[str]:
    """build_runtime_pack.py arguments: model members, data rows in load order, label, spawn rows (the weapon,
    then one toggle row per non-default component)."""
    args = ["--id", pack_id, "--archive", archive]
    for model in (spec.model, *(c.model for c in components)):
        for ext in ("pdr", "ptd"):
            args += ["--member", f"{model}.{ext}={src / f'{model}.{ext}'}"]
    rows = [(type_name, name) for (type_name, _), name in zip(DATA_ROWS, meta_files(prefix), strict=True)]
    if components:
        rows.insert(1, ("WEAPONCOMPONENTSINFO_FILE", components_file(prefix)))
    for type_name, name in rows:
        args += ["--data", f"{type_name}={src / name}"]
    weapon = spec.weapon.lower()
    args += ["--label", f"{spec.label}={text}", "--spawn", f"weapon:{weapon}={text}"]
    for c in components:
        if not c.default:
            args += ["--spawn", f"component:{weapon}:{c.name.lower()}={component_text(text, c)}"]
    return args


def parse_tint(text: str) -> tuple[int, int, int] | None:
    if text.lower() == "none":
        return None
    if not re.fullmatch(r"[0-9a-fA-F]{6}", text):
        raise argparse.ArgumentTypeError("tint is RRGGBB hex or none")
    return int(text[0:2], 16), int(text[2:4], 16), int(text[4:6], 16)


def parse_orders(text: str) -> tuple[int, int, int] | str:
    if text == "auto":
        return text
    if not re.fullmatch(r"\d{1,4},\d{1,4},\d{1,4}", text):
        raise argparse.ArgumentTypeError("orders are NAV1,NAV2,BEST or auto")
    nav1, nav2, best = (int(v) for v in text.split(","))
    return nav1, nav2, best


def make_sources(args: argparse.Namespace, spec: Spec) -> tuple[dict[str, bytes], tuple[PackComponent, ...]]:
    pdr = args.pdr.read_bytes()
    read_rsc7(pdr, str(args.pdr))
    ptd = args.ptd.read_bytes()
    read_rsc7(ptd, str(args.ptd))
    retail = args.weapons.read_bytes()
    archetypes_file, weapons_file, animations_file = meta_files(args.meta_prefix)
    donor = classify(retail, spec.source)
    drop, notes = fit_attach_points(donor, model_bones(pdr, str(args.pdr)))
    recoloured: list[str] = []
    if args.tint is not None:
        ptd, recoloured = tint_dictionary(ptd, spec.source_model, args.tint)
    else:
        parse_ps5_texture_dictionary(ptd, _LIMITS)
    tints, tint_lines = None, []
    if args.tint_palette is not None:
        ptd, tints, tint_lines = palette_step(args.tint_palette, args.pdr, ptd)
    components = make_components(args, replace(spec, donor_model=donor.model), retail, drop)
    files = {
        f"{spec.model}.pdr": pdr,
        f"{spec.model}.ptd": ptd,
        archetypes_file: archetypes_meta(args.archetypes.read_bytes(), spec, donor.model, components).encode(),
        weapons_file: weapons_meta(retail, spec, drop, components, tints).encode(),
        animations_file: animations_meta(args.animations.read_bytes(), spec).encode(),
    }
    names = [archetypes_file, weapons_file, animations_file]
    if components:
        for c, path in zip(components, args.attachment, strict=True):
            files[f"{c.model}.pdr"] = path.read_bytes()
            files[f"{c.model}.ptd"] = path.with_suffix(".ptd").read_bytes()
        files[components_file(args.meta_prefix)] = components_meta(
            args.components.read_bytes(), list(components), spec.weapon
        ).encode()
        names.append(components_file(args.meta_prefix))
    for name in names:
        ET.fromstring(files[name])  # well-formed
    print(f"{donor.line()} orders={spec.orders[0]},{spec.orders[1]},{spec.best}")
    for c in components:
        role = "default, replaces " + c.source if c.default else "toggle row"
        print(f"component {c.name} on {c.model} at {c.bone} ({c.kind}; clone of {c.source}; {role})")
    for note in notes:
        print(f"note: {note}")
    if recoloured:
        print(f"recoloured {', '.join(recoloured)} to #{''.join(f'{v:02x}' for v in args.tint)}")
    for line in tint_lines:
        print(line)
    return files, components


def palette_step(mode: str, pdr: Path, ptd: bytes) -> tuple[bytes, str, list[str]]:
    """--tint-palette: (ptd, tints hint, lines). `auto` reads the converter report next to the drawable
    (<pdr>.report.json) and, when its palette shaders read TINT_PALETTE, writes the palette (tint_palette_dictionary);
    otherwise, and for `none`, the weapon has no tints and one `tints: none` line says so."""
    why = "turned off"
    if mode == "auto":
        path = pdr.with_name(pdr.name + ".report.json")
        report = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
        plan = tint_plan(report)
        if plan is not None:
            ptd, lines = tint_palette_dictionary(ptd, plan)
            return ptd, "palette", [f"tints: palette on {plan.materials} of {plan.shaders} materials", *lines]
        line = (report.get("weaponTint") or {}).get("line", "")
        why = line.partition("(")[2].rpartition(")")[0] or (
            "no material reads a tint palette" if report else "no converter report next to the drawable"
        )
    return ptd, "none", [f"tints: none ({why}): Weapon Tint shows no change"]


def make_components(args: argparse.Namespace, spec: Spec, retail: bytes, drop: set[str]) -> tuple[PackComponent, ...]:
    """The pack components of the --attachment drawables (each read and checked; its .ptd next to it)."""
    if not args.attachment:
        return ()
    if args.components is None:
        raise WeaponModelError("--attachment needs --components (the retail weaponcomponents.meta)")
    stems = []
    for path in args.attachment:
        read_rsc7(path.read_bytes(), str(path))
        parse_ps5_texture_dictionary(path.with_suffix(".ptd").read_bytes(), _LIMITS)
        stems.append(path.stem.lower())
    components = tuple(plan_components(retail, args.components.read_bytes(), spec, stems))
    lost = [c.bone for c in components if c.bone.lower() in {b.lower() for b in drop}]
    if lost:
        raise WeaponModelError(f"the model has no {', '.join(lost)} bone for the attachment(s)")
    return components


def resolve_meta_inputs(args) -> None:
    """Fill omitted donor XML paths from the verified cache; explicit input paths keep precedence."""
    names = {"weapons": "weapons", "animations": "weaponanimations", "archetypes": "weaponarchetypes"}
    if args.classify:
        names = {"weapons": "weapons"}
    if args.attachment:
        names["components"] = "weaponcomponents"
    for option, basename in names.items():
        if getattr(args, option) is None:
            try:
                path = retail_templates.verified_path(args.templates, f"retail-metas/{basename}.meta")
            except retail_templates.TemplateError as error:
                raise WeaponModelError(f"--{option}: {error}; or supply explicit XML with --{option}") from None
            setattr(args, option, path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pdr", type=Path, help="retail source drawable (loose RSC7)")
    parser.add_argument("--ptd", type=Path, help="retail source texture dictionary (loose RSC7)")
    parser.add_argument("--weapons", type=Path, help="retail weapons.meta (XML; default verified template cache)")
    parser.add_argument("--animations", type=Path, help="retail weaponanimations.meta (XML)")
    parser.add_argument("--archetypes", type=Path, help="retail weaponarchetypes.meta (XML)")
    parser.add_argument(
        "--templates", type=Path, default=_HERE.parent / "build/retail-templates", help="verified retail template cache"
    )
    parser.add_argument("--out", type=Path)
    parser.add_argument(
        "--classify",
        action="store_true",
        help="print the --source donor's class line (or why it cannot be cloned) and exit; needs only --weapons",
    )
    parser.add_argument("--model", default=Spec.model)
    parser.add_argument("--source-model", default=Spec.source_model)
    parser.add_argument("--weapon", default=Spec.weapon)
    parser.add_argument("--source", default=Spec.source, help="the donor weapon (decides the weapon class)")
    parser.add_argument("--tint", type=parse_tint, default=(255, 224, 0), help="RRGGBB or none (default ffe000)")
    parser.add_argument(
        "--tint-palette",
        choices=("auto", "none"),
        help="auto: a drawable converted by tools/weapon_tint_drawable.py gets its tint palette (from the "
        "converter report next to --pdr) and the weapons meta a `tints=palette` hint, else `tints=none`; none: "
        "the `tints=none` hint only (default: neither, the recipes before tints)",
    )
    parser.add_argument("--build", metavar="PACK_ID", help="also run build_runtime_pack.py")
    parser.add_argument("--output-root", type=Path, help="build_runtime_pack.py --output-root")
    parser.add_argument("--archive", default="gmwmodel.rpf")
    parser.add_argument(
        "--meta-prefix",
        default=META_PREFIX,
        type=lambda v: v if re.fullmatch(r"[a-z0-9_]{1,40}", v) else parser.error(f"bad --meta-prefix {v!r}"),
        help=f"data file names <prefix>_weapons.meta etc. (default {META_PREFIX})",
    )
    parser.add_argument("--text", default="GM Pistol (own model)", help="label and menu text")
    parser.add_argument(
        "--components", type=Path, help="retail weaponcomponents.meta (XML): --attachment, --classify's models"
    )
    parser.add_argument(
        "--attachment",
        type=Path,
        action="append",
        default=[],
        metavar="F.pdr",
        help="a converted component model named like the donor's retail component model (F.ptd next to it)",
    )
    parser.add_argument("--slot", default=Spec.slot, help="new weapon slot (unique per active pack)")
    parser.add_argument("--label", default=Spec.label, help="new HumanNameHash label key")
    parser.add_argument(
        "--orders",
        type=parse_orders,
        default=(*Spec.orders, Spec.best),
        help="NAV1,NAV2,BEST slot order numbers (default 92,112,192; weapon-v1 91.., wcomp-v1 93..), or auto: "
        "the first free numbers after the donor's slot",
    )
    args = parser.parse_args(argv)
    try:
        resolve_meta_inputs(args)
    except (WeaponModelError, OSError) as error:
        raise SystemExit(f"error: {error}") from None
    if args.classify:
        try:
            retail = args.weapons.read_bytes()
            donor = classify(retail, args.source)
            models = ""
            if args.components is not None:
                rows = donor_components(retail, args.components.read_bytes(), args.source)
                models = " component-models=" + (",".join(dict.fromkeys(r[3] for r in rows if r[3])) or "none")
        except (WeaponModelError, ET.ParseError, OSError) as error:
            raise SystemExit(f"error: {error}") from None
        print(donor.line() + models)
        return 0
    missing = [f"--{n}" for n in ("pdr", "ptd", "animations", "archetypes", "out") if getattr(args, n) is None]
    if missing:
        parser.error(f"the following arguments are required: {', '.join(missing)}")
    try:
        orders = auto_orders(args.weapons.read_bytes(), args.source) if args.orders == "auto" else args.orders
        spec = replace(
            Spec(),
            model=args.model,
            source_model=args.source_model,
            weapon=args.weapon,
            source=args.source,
            slot=args.slot,
            label=args.label,
            orders=orders[:2],
            best=orders[2],
        )
        spec.check()
        files, components = make_sources(args, spec)
    except (ValueError, ET.ParseError, OSError) as error:  # WeaponModelError, AssetError, bad JSON
        raise SystemExit(f"error: {error}") from None
    args.out.mkdir(parents=True, exist_ok=True)
    clashes = [name for name in files if (args.out / name).exists()]
    if clashes:
        raise SystemExit(f"refusing to overwrite {', '.join(str(args.out / n) for n in clashes)}")
    for name, blob in files.items():
        (args.out / name).write_bytes(blob)
        print(f"wrote {args.out / name} bytes={len(blob)}")
    if args.build:
        command = [sys.executable, str(_HERE / "build_runtime_pack.py")]
        command += build_args(args.out, args.build, spec, args.archive, args.text, args.meta_prefix, components)
        if args.output_root:
            command += ["--output-root", str(args.output_root)]
        print("+ " + " ".join(command))
        return subprocess.run(command, check=False).returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

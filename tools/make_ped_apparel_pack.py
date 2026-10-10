#!/usr/bin/env python3
"""Clothing for an existing ped: a SHOP_PED_APPAREL_META_FILE (type 137) pack (APPAREL-run1).

The 137 load queues one parse; its post-parse
step resolves pedName through the archetype map, skips the ped while its model is streamed in,
and appends the MetaDataStore slot named fullDlcName to the model's variation-info slots. A
streamed ped (mp_m_freemode_01) then finds the DLC's drawable k of component C at
"<model name>_<string(pmt dlcName hash)>/<C>_<kkk>_<u|r>" and its textures at
"..._diff_<kkk>_<a..>_<race>". So:

- the archive holds <ped>_<dlc>.pmt and a folder <ped>_<dlc>/ with the component files;
- the shop meta's fullDlcName is <ped>_<dlc> and the pmt's dlcName hash is joaat(<dlc>); the
  pmt's own string table (header +0x38) carries "<dlc>", which is how the folder name resolves;
- the archive must be registered before the data row (the post-parse step only looks names up).

The pmt is a retail DLC CPedVariationInfo (PSO-in-resource) patched in place to ONE component
with ONE drawable (the source's drawable 000, first texture only), no props and no selection sets;
its dlcName hash and string are replaced (same length or shorter). Drawable and texture are copied
from the same retail DLC; --tint recolours the texture (BC1/BC3 blocks rewritten to one colour)
so the clone is unmistakable on screen.

  make_ped_apparel_pack.py --pmt mp_m_freemode_01_mp_m_2023_01.pmt \\
      --drawable lowr_000_u.pdd --texture lowr_diff_000_a_uni.ptd \\
      [--ped mp_m_freemode_01 --dlc gmapparel_01] --out SRC_DIR [--tint ffe000 | --tint none] \\
      [--build gtavmenu-apparel-v1 --output-root build/custom-assets]

Inputs are loose RSC7 members of your own game. Writes SRC_DIR/<ped>_<dlc>.pmt,
SRC_DIR/<ped>_<dlc>/<files> and SRC_DIR/<ped>_<dlc>_shop.meta (refuses to overwrite). --build also
writes <output-root>/<id>/resources/{<archive>, pack.cfg, <shop meta>}: one plain-table archive with
the folder (build_runtime_pack.py writes flat archives only) and the data row.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
import sys
import zlib
from dataclasses import replace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

from gtavmenu_tools import rpf7, rpf_toc, runtime_pack  # noqa: E402
from gtavmenu_tools.asset_formats import Limits, rpf_members  # noqa: E402
from gtavmenu_tools.meta_resource import RESOURCE_BASE, Meta  # noqa: E402
from gtavmenu_tools.ps5_texture_reader import parse_ps5_texture_dictionary  # noqa: E402

DATA_TYPE = "SHOP_PED_APPAREL_META_FILE"
COMPONENTS = ("head", "berd", "hair", "uppr", "lowr", "hand", "feet", "teef", "accs", "task", "decl", "jbib")
# PSO struct hashes (meta_resource.joaat_cs of the parser names) and CPedVariationInfo layout.
VARIATION_INFO = 0x16760659  # CPedVariationInfo, 0x70 bytes
COMPONENT_DATA = 0xD2E926F4  # CPVComponentData {u8 numAvailTex, atArray aDrawblData3 @8}, 0x18
DRAWABLE_DATA = 0x5B7EF462  # CPVDrawblData {u8 propMask, u8 numAlternatives, atArray aTexData @8}, 0x30
COMPONENT_INFO = 0x6F419FC9  # CComponentInfo: pedXml_compIdx @0x2c, pedXml_drawblIdx @0x2d, 0x30
VI_AVAIL, VI_COMPONENTS, VI_SELECTION_SETS, VI_COMP_INFOS, VI_PROPS, VI_DLC = 0x04, 0x10, 0x20, 0x30, 0x40, 0x68
PROP_META, PROP_ANCHORS = 0x08, 0x18  # atArrays inside CPedPropInfo (u8 numAvailProps @0)
RACE_MASK = 0x10  # propMask bit of an `_r` (race-textured) drawable; `_u` drawables lack it
_NAME = re.compile(r"[a-z0-9_]{1,40}")
_DRAWABLE = re.compile(r"([a-z]{4})_000_([ur])\.pdd")
_TEXTURE = re.compile(r"([a-z]{4})_diff_000_a_([a-z]{3})\.ptd")
_LIMITS = replace(Limits(), max_file_bytes=1 << 28, max_total_bytes=1 << 30)
_BC1, _BC3 = 71, 77  # PS5 texture format codes


class ApparelError(ValueError):
    pass


def joaat(text: str) -> int:
    """Lower-case Jenkins one-at-a-time hash (atStringHash for plain names)."""
    value = 0
    for byte in text.lower().encode("ascii"):
        value = (value + byte) & 0xFFFFFFFF
        value = (value + (value << 10)) & 0xFFFFFFFF
        value ^= value >> 6
    value = (value + (value << 3)) & 0xFFFFFFFF
    value ^= value >> 11
    return (value + (value << 15)) & 0xFFFFFFFF


def read_rsc7(blob: bytes, label: str) -> tuple[bytes, bytes]:
    """(16-byte RSC7 header, inflated payload) of a loose resource."""
    if blob[:4] != b"RSC7" or len(blob) < 17:
        raise ApparelError(f"{label}: not a loose RSC7 resource")
    stream = zlib.decompressobj(-15)
    payload = stream.decompress(blob[16:])
    if not stream.eof or stream.unused_data:
        raise ApparelError(f"{label}: payload does not raw-inflate to its end")
    return blob[:16], payload


def write_rsc7(header: bytes, payload: bytes) -> bytes:
    deflate = zlib.compressobj(9, zlib.DEFLATED, -15)
    return header + deflate.compress(payload) + deflate.flush()


# ---- pmt ----------------------------------------------------------------------------------------


class _Pmt:
    """Block-addressed view of a CPedVariationInfo payload."""

    def __init__(self, payload: bytes):
        self.b = bytearray(payload)
        self.meta = Meta(payload)
        if self.meta.magic != b"0DRP":
            raise ApparelError("pmt payload is not a parser meta (PRD0)")
        found = [(h, size, at) for h, size, at in self.meta.blocks if h == VARIATION_INFO]
        if len(found) != 1 or found[0][1] < 0x70:
            raise ApparelError("pmt has no single CPedVariationInfo block")
        self.vi = found[0][2]

    def array(self, at: int) -> tuple[int | None, int]:
        pointer, count = struct.unpack_from("<QH", self.b, at)
        return (self.meta.ref(pointer) if pointer else None), count

    def set_count(self, at: int, count: int) -> None:
        struct.pack_into("<H", self.b, at + 8, count)

    def string_table(self) -> tuple[int, int]:
        """(offset, length incl. NUL padding) of the trailing string table at header +0x38."""
        pointer = struct.unpack_from("<Q", self.b, 0x38)[0]
        if pointer & 0xF0000000 != RESOURCE_BASE:
            raise ApparelError("pmt has no string table (not a DLC pmt?)")
        start = pointer - RESOURCE_BASE
        end = start
        while end < len(self.b) and self.b[end]:
            end += 1
        pad = end
        while pad < len(self.b) and not self.b[pad] and pad - start < 64:
            pad += 1
        return start, pad - start


def pmt_summary(payload: bytes) -> dict:
    """Components (name -> drawable count), dlcName hash and string, comp infos, props."""
    pmt = _Pmt(payload)
    avail = pmt.b[pmt.vi + VI_AVAIL : pmt.vi + VI_AVAIL + 12]
    data, count = pmt.array(pmt.vi + VI_COMPONENTS)
    components = {}
    for comp, index in enumerate(avail):
        if index != 0xFF and index < count and data is not None:
            components[COMPONENTS[comp]] = pmt.array(data + index * 0x18 + 8)[1]
    start, _ = pmt.string_table()
    end = pmt.b.index(b"\0", start)
    return {
        "components": components,
        "dlc_hash": struct.unpack_from("<I", pmt.b, pmt.vi + VI_DLC)[0],
        "dlc": pmt.b[start:end].decode("ascii"),
        "comp_infos": pmt.array(pmt.vi + VI_COMP_INFOS)[1],
        "props": pmt.b[pmt.vi + VI_PROPS],
    }


def patch_pmt(payload: bytes, component: str, dlc: str | None, race: bool) -> bytes:
    """One component (`component`, the source's drawable 000 with its first texture), no props,
    no selection sets, dlcName = joaat(dlc) with the string table text replaced in place. With
    `dlc` None (a base or add-on ped's own pmt, which has no string table) dlcName is left alone."""
    if component not in COMPONENTS:
        raise ApparelError(f"unknown component {component!r} (one of {', '.join(COMPONENTS)})")
    pmt = _Pmt(payload)
    vi, b = pmt.vi, pmt.b
    comp = COMPONENTS.index(component)
    index = b[vi + VI_AVAIL + comp]
    data, count = pmt.array(vi + VI_COMPONENTS)
    if index == 0xFF or data is None or index >= count:
        raise ApparelError(f"source pmt has no {component} component")
    entry = data + index * 0x18
    drawables, drawable_count = pmt.array(entry + 8)
    if drawables is None or not drawable_count:
        raise ApparelError(f"source {component} has no drawables")
    textures, texture_count = pmt.array(drawables + 8)
    if textures is None or not texture_count:
        raise ApparelError(f"source {component} drawable 000 has no textures")
    if bool(b[drawables] & RACE_MASK) != race:
        raise ApparelError(f"drawable suffix does not match propMask 0x{b[drawables]:02x} (_r needs 0x10)")
    if b[drawables + 1]:
        raise ApparelError(f"source {component} drawable 000 has alternates (_1.. files); pick another")
    # The kept component data moves to index 0: avail -> 0, one entry, one drawable, one texture.
    b[data : data + 0x18] = bytes(b[entry : entry + 0x18])
    b[vi + VI_AVAIL : vi + VI_AVAIL + 12] = bytes(0 if i == comp else 0xFF for i in range(12))
    pmt.set_count(vi + VI_COMPONENTS, 1)
    b[data] = 1  # numAvailTex: the textures of the one drawable
    pmt.set_count(data + 8, 1)
    pmt.set_count(drawables + 8, 1)
    # One CComponentInfo per drawable (retail: count == total drawables), ours first.
    infos, info_count = pmt.array(vi + VI_COMP_INFOS)
    match = [
        i
        for i in range(info_count)
        if infos is not None and b[infos + i * 0x30 + 0x2C : infos + i * 0x30 + 0x2E] == bytes([comp, 0])
    ]
    if len(match) != 1:
        raise ApparelError(f"source pmt has {len(match)} component infos for {component} drawable 000")
    b[infos : infos + 0x30] = bytes(b[infos + match[0] * 0x30 : infos + match[0] * 0x30 + 0x30])
    pmt.set_count(vi + VI_COMP_INFOS, 1)
    pmt.set_count(vi + VI_SELECTION_SETS, 0)
    b[vi + VI_PROPS] = 0
    pmt.set_count(vi + VI_PROPS + PROP_META, 0)
    pmt.set_count(vi + VI_PROPS + PROP_ANCHORS, 0)
    if dlc is None:
        return bytes(b)
    start, room = pmt.string_table()
    text = dlc.encode("ascii")
    if len(text) + 1 > room:
        raise ApparelError(f"dlc name {dlc!r} is longer than the source's string table ({room - 1} bytes)")
    b[start : start + room] = text + bytes(room - len(text))
    struct.pack_into("<I", b, vi + VI_DLC, joaat(dlc))
    return bytes(b)


def _set_dlc(pmt: _Pmt, dlc: str) -> None:
    start, room = pmt.string_table()
    text = dlc.encode("ascii")
    if len(text) + 1 > room:
        raise ApparelError(f"dlc name {dlc!r} is longer than the source's string table ({room - 1} bytes)")
    pmt.b[start : start + room] = text + bytes(room - len(text))
    struct.pack_into("<I", pmt.b, pmt.vi + VI_DLC, joaat(dlc))


def patch_pmt_drawables(payload: bytes, component: str, dlc: str, textures: list[int], race_id: int = 0) -> bytes:
    """patch_pmt for several NEW drawables (convert_pc_clothing.py freemode route): ONE component with
    len(textures) drawables 000.., drawable i having textures[i] diffuse textures (letters a..), all `_u`
    (race_id 0, diffuse suffix uni) or all `_r` with that texture race id (1 = whi); no props, no selection
    sets, one CComponentInfo per drawable, dlcName = joaat(dlc) and the string table text replaced.

    The retail DLC pmt is patched in place (counts shrink, pointers move, nothing grows): drawable slot i keeps
    the source component's own entry i when it fits (race flag, no alternatives, no cloth, at least textures[i]
    texture rows of race_id), else it takes the propMask of the component's drawable 000 (race bit set to the
    route) and the texture-row array of any retail drawable of the pmt that has enough rows of race_id (the rows
    are only {texId, distribution, 0}; several drawables may share one array). A component with fewer drawable
    slots than needed points at the largest drawable array of the pmt (every component but ours is dropped).
    One drawable with one texture on a fitting drawable 000 gives patch_pmt's bytes (APPAREL-run1)."""
    if component not in COMPONENTS:
        raise ApparelError(f"unknown component {component!r} (one of {', '.join(COMPONENTS)})")
    if not textures or any(not 1 <= t <= 26 for t in textures) or sum(textures) > 255:
        raise ApparelError("each new drawable needs 1..26 textures (letters a..z), 255 in all")
    if len(textures) > 255:
        raise ApparelError("at most 255 new drawables in one component")
    pmt = _Pmt(payload)
    vi, b = pmt.vi, pmt.b
    comp = COMPONENTS.index(component)
    race = race_id != 0
    data, count = pmt.array(vi + VI_COMPONENTS)
    if data is None or not count:
        raise ApparelError("source pmt has no component data")
    # Every retail drawable entry, read before anything is written: (entry offset, bytes, texture ids).
    arrays: list[tuple[int, int, int]] = []  # (component entry offset, drawable array offset, drawable count)
    rows: list[tuple[int, bytes, list[int]]] = []
    for i in range(count):
        drawables, n = pmt.array(data + i * 0x18 + 8)
        if drawables is None:
            continue
        arrays.append((data + i * 0x18, drawables, n))
        for d in range(n):
            at = drawables + d * 0x30
            tex, n_tex = pmt.array(at + 8)
            ids = [b[tex + 3 * t] for t in range(n_tex)] if tex is not None else []
            rows.append((at, bytes(b[at : at + 0x30]), ids))
    index = b[vi + VI_AVAIL + comp]
    own_entry = data + index * 0x18 if index != 0xFF and index < count else None
    own_at, own_n = pmt.array(own_entry + 8) if own_entry is not None else (None, 0)
    own = {(r[0] - own_at) // 0x30: r for r in rows if own_at is not None and own_at <= r[0] < own_at + own_n * 0x30}
    base_mask = (own[0][1][0] if 0 in own else 0x01) & ~RACE_MASK | (RACE_MASK if race else 0)

    def fits(row: tuple[int, bytes, list[int]], wanted: int) -> bool:
        _at, raw, ids = row
        return len(ids) >= wanted and all(x == race_id for x in ids[:wanted]) and not any(raw[0x18:0x30])

    entry_at = own_entry if own_entry is not None else max(arrays, key=lambda a: a[2])[0]
    if own_entry is not None and own_n >= len(textures):
        target = own_at
    else:
        carrier = max(arrays, key=lambda a: a[2])
        if carrier[2] < len(textures):
            raise ApparelError(f"no drawable array of the source pmt has {len(textures)} slots")
        target = carrier[1]
        b[entry_at + 8 : entry_at + 16] = b[carrier[0] + 8 : carrier[0] + 16]
    written = []
    for i, wanted in enumerate(textures):
        mine = own.get(i) if target == own_at else None
        if mine is not None and fits(mine, wanted) and bool(mine[1][0] & RACE_MASK) == race and not mine[1][1]:
            raw = bytearray(mine[1])
        else:
            donor = next((r for r in rows if fits(r, wanted)), None)
            if donor is None:
                raise ApparelError(f"no retail drawable of the source pmt has {wanted} textures of race id {race_id}")
            raw = bytearray(0x30)
            raw[0] = base_mask
            raw[8:16] = donor[1][8:16]
        struct.pack_into("<H", raw, 8 + 8, wanted)  # aTexData count (capacity stays)
        written.append(bytes(raw))
    for i, raw in enumerate(written):
        b[target + i * 0x30 : target + (i + 1) * 0x30] = raw
    # The kept component data moves to index 0 (component entry 0 gets ours).
    entry = bytearray(b[entry_at : entry_at + 0x18])
    entry[0] = sum(textures)  # numAvailTex: every texture of the component's drawables
    struct.pack_into("<H", entry, 8 + 8, len(textures))
    b[data : data + 0x18] = entry
    b[vi + VI_AVAIL : vi + VI_AVAIL + 12] = bytes(0 if i == comp else 0xFF for i in range(12))
    pmt.set_count(vi + VI_COMPONENTS, 1)
    # One CComponentInfo per drawable (retail: count == total drawables): the source's info of this
    # component's drawable 000 (else of any drawable 000) copied with pedXml_drawblIdx = i.
    infos, info_count = pmt.array(vi + VI_COMP_INFOS)
    if infos is None or info_count < len(textures):
        raise ApparelError(f"source pmt has {info_count} component infos; {len(textures)} needed")
    pick = [i for i in range(info_count) if b[infos + i * 0x30 + 0x2C : infos + i * 0x30 + 0x2E] == bytes([comp, 0])]
    pick = pick or [i for i in range(info_count) if b[infos + i * 0x30 + 0x2D] == 0]
    if not pick:
        raise ApparelError("source pmt has no component info for a drawable 000")
    info = bytearray(b[infos + pick[0] * 0x30 : infos + pick[0] * 0x30 + 0x30])
    info[0x2C] = comp
    for i in range(len(textures)):
        info[0x2D] = i
        b[infos + i * 0x30 : infos + (i + 1) * 0x30] = info
    pmt.set_count(vi + VI_COMP_INFOS, len(textures))
    pmt.set_count(vi + VI_SELECTION_SETS, 0)
    b[vi + VI_PROPS] = 0
    pmt.set_count(vi + VI_PROPS + PROP_META, 0)
    pmt.set_count(vi + VI_PROPS + PROP_ANCHORS, 0)
    _set_dlc(pmt, dlc)
    return bytes(b)


# ---- textures -----------------------------------------------------------------------------------


def _rgb565(rgb: tuple[int, int, int]) -> int:
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def tint_blocks(storage: bytes, code: int, rgb: tuple[int, int, int]) -> bytes:
    """Every BC1 block (8 bytes) or BC3 colour half set to one flat colour (indices 0)."""
    colour = struct.pack("<HHI", _rgb565(rgb), _rgb565(rgb), 0)
    step, at = (8, 0) if code == _BC1 else (16, 8)
    if len(storage) % step:
        raise ApparelError("texture storage is not whole blocks")
    out = bytearray(storage)
    for block in range(0, len(out), step):
        out[block + at : block + at + 8] = colour
    return bytes(out)


def tint_texture(blob: bytes, rgb: tuple[int, int, int]) -> tuple[bytes, list[str]]:
    """Recolour every BC1/BC3 texture of a PS5 ptd; the result is re-parsed and compared."""
    before = parse_ps5_texture_dictionary(blob, _LIMITS)
    header, payload = read_rsc7(blob, "ptd")
    system = before["header"]["systemBytes"]
    out = bytearray(payload)
    done = []
    for row in before["textures"]:
        if row["formatCode"] not in (_BC1, _BC3):
            continue
        at, size = system + row["graphicsOffset"], row["storageBytes"]
        out[at : at + size] = tint_blocks(bytes(out[at : at + size]), row["formatCode"], rgb)
        done.append(row["name"])
    if not done:
        raise ApparelError("texture dictionary has no BC1/BC3 texture to tint (use --tint none)")
    result = write_rsc7(header, bytes(out))
    after = parse_ps5_texture_dictionary(result, _LIMITS)
    for old, new in zip(before["textures"], after["textures"], strict=True):
        keep = ("storageSha256", "allocation")
        if {k: v for k, v in old.items() if k not in keep} != {k: v for k, v in new.items() if k not in keep}:
            raise ApparelError(f"{old['name']}: texture structure changed")
    return result, done


# ---- metas and archive --------------------------------------------------------------------------


def shop_meta(ped: str, dlc: str) -> str:
    """The SHOP_PED_APPAREL meta: names only (no shop items; scripts never list the clone)."""
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        "<ShopPedApparel>\n"
        f"\t<pedName>{ped}</pedName>\n"
        f"\t<dlcName>{dlc}</dlcName>\n"
        f"\t<fullDlcName>{ped}_{dlc}</fullDlcName>\n"
        "\t<eCharacter>SCR_CHAR_MULTIPLAYER</eCharacter>\n"
        "\t<pedOutfits />\n"
        "\t<pedComponents />\n"
        "\t<pedProps />\n"
        "</ShopPedApparel>\n"
    )


def build_archive(members: dict[str, bytes]) -> bytes:
    """Plain-table (OPEN) PS5 RPF7 of loose RSC7 `members` keyed by path; one folder level allowed.

    Directory children are contiguous and sorted by name, like retail archives; the result is
    re-read with the production reader (asset_formats.rpf_members) and every path resolved.
    """
    tree: dict[str, dict | tuple] = {}
    for path, blob in members.items():
        parts = path.split("/")
        if len(parts) > 2 or not all(parts):
            raise ApparelError(f"member path {path!r}: at most one folder level")
        stored, sysf, gfxf, _raw = rpf7.loose_resource(blob, path)
        node = tree if len(parts) == 1 else tree.setdefault(parts[0], {})
        if not isinstance(node, dict) or parts[-1] in node:
            raise ApparelError(f"member path {path!r} collides")
        node[parts[-1]] = (stored, sysf, gfxf)
    entries: list[list] = [["", None]]  # [name, payload | (first, count)]
    pending = [(0, tree)]
    while pending:
        index, node = pending.pop(0)
        first = len(entries)
        for name in sorted(node):
            entries.append([name, node[name]])
        entries[index][1] = (first, len(node))
        for i, name in enumerate(sorted(node)):
            if isinstance(node[name], dict):
                pending.append((first + i, node[name]))
    names = b"\0"
    offsets = [0]
    for name, _ in entries[1:]:
        offsets.append(len(names))
        names += name.encode("ascii") + b"\0"
    names += bytes(-len(names) % 16)
    table_end = 16 + len(entries) * 16 + len(names)
    cursor = table_end + (-table_end % rpf7.BLOCK)
    table = b""
    blobs = []
    for (_name, value), name_offset in zip(entries, offsets, strict=True):
        if isinstance(value, tuple) and len(value) == 2:
            table += struct.pack("<IIII", name_offset, rpf7.DIRECTORY, value[0], value[1])
            continue
        stored, sysf, gfxf = value
        stored = rpf7.oversize_prefix(stored) if len(stored) >= rpf7.OVERSIZE else stored
        size_field = min(len(stored), rpf7.OVERSIZE)
        table += struct.pack(
            "<QII", name_offset | (size_field << 16) | (((cursor // rpf7.BLOCK) | 0x800000) << 40), sysf, gfxf
        )
        blobs.append((cursor, stored))
        cursor += len(stored) + (-len(stored) % rpf7.BLOCK)
    out = bytearray(struct.pack("<4I", rpf7.MAGIC, len(entries), len(names), rpf7.TAG_OPEN) + table + names)
    for offset, stored in blobs:
        out += bytes(offset - len(out)) + stored
    out += bytes(cursor - len(out))
    archive = bytes(out)
    rpf_toc.toc_span(archive)
    listed = {m.name for m in rpf_members(archive, _LIMITS)}
    if listed != set(members):
        raise ApparelError(f"archive readback lists {sorted(listed)}")
    for path, blob in members.items():
        entry = rpf7.find_entry(archive, "apparel.rpf", path)
        stored, _, _, raw = rpf7.loose_resource(blob, path)
        back = archive[entry["offset"] : entry["offset"] + entry["size"]]
        if rpf7.inflate_resource(back, path) != raw:
            raise ApparelError(f"archive readback payload mismatch for {path}")
    return archive


def parse_tint(text: str) -> tuple[int, int, int] | None:
    if text.lower() == "none":
        return None
    if not re.fullmatch(r"[0-9a-fA-F]{6}", text):
        raise argparse.ArgumentTypeError("tint is RRGGBB hex or none")
    return int(text[0:2], 16), int(text[2:4], 16), int(text[4:6], 16)


def make_sources(args: argparse.Namespace) -> dict[str, bytes]:
    """SRC_DIR-relative path -> bytes: the patched pmt, the component folder and the shop meta."""
    for name in (args.ped, args.dlc):
        if not _NAME.fullmatch(name):
            raise ApparelError(f"name {name!r} must be lower-case [a-z0-9_] (40 at most)")
    drawable = _DRAWABLE.fullmatch(args.drawable.name)
    texture = _TEXTURE.fullmatch(args.texture.name)
    if not drawable or not texture or drawable.group(1) != texture.group(1):
        raise ApparelError("--drawable/--texture must be <comp>_000_<u|r>.pdd and <comp>_diff_000_a_<race>.ptd")
    if drawable.group(2) == "u" and texture.group(2) != "uni":
        raise ApparelError("a _u drawable uses _uni textures")
    full = f"{args.ped}_{args.dlc}"
    header, payload = read_rsc7(args.pmt.read_bytes(), str(args.pmt))
    pmt = write_rsc7(header, patch_pmt(payload, drawable.group(1), args.dlc, drawable.group(2) == "r"))
    pdd = args.drawable.read_bytes()
    read_rsc7(pdd, str(args.drawable))
    ptd = args.texture.read_bytes()
    if args.tint is not None:
        ptd, done = tint_texture(ptd, args.tint)
        print(f"recoloured {', '.join(done)} to #{''.join(f'{v:02x}' for v in args.tint)}")
    else:
        parse_ps5_texture_dictionary(ptd, _LIMITS)
    return {
        f"{full}.pmt": pmt,
        f"{full}/{args.drawable.name}": pdd,
        f"{full}/{args.texture.name}": ptd,
        f"{full}_shop.meta": shop_meta(args.ped, args.dlc).encode("ascii"),
    }


def write_pack(files: dict[str, bytes], pack_id: str, archive_name: str, root: Path) -> Path:
    """<root>/<pack_id>/resources/{archive, pack.cfg, shop meta}; refuses to overwrite."""
    meta = next(name for name in files if name.endswith("_shop.meta"))
    archive = build_archive({name: blob for name, blob in files.items() if name != meta})
    pack = runtime_pack.RuntimePack(
        pack_id,
        archive_name,
        len(archive),
        hashlib.sha256(archive).hexdigest(),
        data=[runtime_pack.DataFile(DATA_TYPE, len(files[meta]), hashlib.sha256(files[meta]).hexdigest(), meta)],
    )
    descriptor = runtime_pack.render(pack)
    resources = root / pack_id / "resources"
    if resources.exists():
        raise ApparelError(f"refusing to overwrite {resources}")
    resources.mkdir(parents=True)
    (resources / archive_name).write_bytes(archive)
    (resources / meta).write_bytes(files[meta])
    (resources / "pack.cfg").write_text(descriptor, encoding="ascii", newline="\n")
    print(f"wrote {resources}/{archive_name} bytes={len(archive)} key={rpf_toc.toc_index(archive_name, len(archive))}")
    print(descriptor, end="")
    return resources


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pmt", type=Path, required=True, help="retail DLC ped variation pmt (loose RSC7)")
    parser.add_argument("--drawable", type=Path, required=True, help="retail <comp>_000_<u|r>.pdd of that DLC")
    parser.add_argument("--texture", type=Path, required=True, help="retail <comp>_diff_000_a_<race>.ptd")
    parser.add_argument("--ped", default="mp_m_freemode_01")
    parser.add_argument("--dlc", default="gmapparel_01")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--tint", type=parse_tint, default=(255, 224, 0), help="RRGGBB or none (default ffe000)")
    parser.add_argument("--build", metavar="PACK_ID", help="also write the runtime pack")
    parser.add_argument("--output-root", type=Path, default=ROOT / "build/custom-assets")
    parser.add_argument("--archive", default="gmapparel.rpf")
    args = parser.parse_args(argv)
    try:
        files = make_sources(args)
        clashes = [name for name in files if (args.out / name).exists()]
        if clashes:
            raise ApparelError(f"refusing to overwrite {', '.join(str(args.out / n) for n in clashes)}")
        for name, blob in files.items():
            (args.out / name).parent.mkdir(parents=True, exist_ok=True)
            (args.out / name).write_bytes(blob)
            print(f"wrote {args.out / name} bytes={len(blob)}")
        if args.build:
            write_pack(files, args.build, args.archive, args.output_root)
    except (ApparelError, rpf7.Rpf7Error, runtime_pack.RuntimePackError, ValueError) as error:
        raise SystemExit(f"error: {error}") from None
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

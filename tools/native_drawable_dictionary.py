#!/usr/bin/env python3
"""PS5 Gen9 (v159) drawable dictionary (.pdd) reader and writer: ped component dictionaries.

Stage S2 of the ped conversion. The reader turns a .pdd into an object
graph: every allocation the resource holds (dictionary root, page info, hash and drawable arrays,
drawables and their names, shader groups, shaders, shader parameter blocks and infos, texture
references and names, skeletons with bones, bone names, tag map, transforms and index arrays,
LOD collections, models, geometry arrays, bounds, shader maps, geometries, bone palettes, vertex
and index buffers with their views, declarations and data). Each object keeps its bytes with the
pointer slots cleared and an explicit list of pointer slots (target object + offset into it, so the
interior pointers of a parameter block or the bone array after its 16-byte header survive a move).
The walk is typed (field offsets of the pinned CodeWalker 485d56b Gen9 classes, the same ones
inspect_ped_resources.py reads); it refuses what it does not model (embedded texture dictionary,
joints, light attributes, bounds, texture data/views on referenced textures, instance parameters)
and proves its own coverage: every nonzero payload byte lies in an object and no pointer-like qword
lies outside a declared slot.

The writer places the graph again and emits RSC7 v159:
  retain  every object at its source page offset with the source page flags (identity proof:
          the inflated payload is byte-identical to retail when nothing changed)
  pack    retail layout rules from scratch: system pages hold everything except index data,
          graphics pages hold index data; the root goes first; then page by page, best-fit by
          decreasing allocation size; alignment 16 for structures, 8 for vertex data, 4 for index
          data and the key array, 1 for strings; allocation sizes round up to the alignment; page
          size = max(next power of two >= largest remaining object, min(previous page, 64 KiB
          system / 8 KiB graphics)); a tail that fits in less takes the largest power of two below
          it (or one page of the next power of two when that would leave more than half again);
          the last page shrinks to the smallest power of two that holds it; pages concatenated
          largest first; flags with the smallest base that expresses the page list (retail);
          ResourcePagesInfo sized for the result. Equal-size ties: `walk` (the canonical walk
          order) or `source` (the source's address order; new objects after).
RAGE breaks size ties in an order not derivable from the file: with `source` ties the retail
s_m_y_cop_01 payload comes out byte-identical, with `walk` ties it is semantically identical
(`compare`: same objects, bytes and pointer targets; only placement and page-info counts differ).
Retail streams are deflated by an encoder other than zlib, so file bytes differ either way.

Edits (on the graph, before placement): `replace_entry` gives one dictionary key an independent
deep copy of another entry's drawable (the old drawable becomes unreachable and is dropped);
`swap_entries` swaps two keys' drawables. Keys stay sorted, nothing is shared.

  native_drawable_dictionary.py roundtrip IN.pdd [--layout retain|pack] [--ties walk|source] [--out OUT.pdd]
  native_drawable_dictionary.py replace IN.pdd --entry lowr_000_u --with accs_002_u --out OUT.pdd
  native_drawable_dictionary.py compare A.pdd B.pdd
  native_drawable_dictionary.py check-variations IN.pdd IN.pmt
"""

from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import struct
import sys
import zlib
from collections import Counter
from dataclasses import dataclass, field
from itertools import pairwise
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

from gtavmenu_tools.asset_formats import AssetError, decode_resource  # noqa: E402
from gtavmenu_tools.hashes import joaat  # noqa: E402

SYS, GFX = 0x50000000, 0x60000000
VERSION = 159
RSC7 = b"RSC7"
MAX_PAYLOAD = 1 << 28
MAX_ENTRIES = 4096
MAX_TAG_CHAIN = 4096
# Flag count fields (shift, width) by rank; rank r holds pages of (512 << base) << (8 - r).
PC_FIELDS = ((4, 1), (5, 2), (7, 4), (11, 6), (17, 7), (24, 1), (25, 1), (26, 1), (27, 1))
PAGE_FLOOR = {"sys": 64 * 1024, "gfx": 8 * 1024}
MIN_PAGE = 8 * 1024
INFO_HEADER, INFO_RECORD = 16, 8
STRING_KINDS = frozenset({"drawable-name", "bone-name", "texture-name"})
ALIGN = {"vertex-data": 8, "index-data": 4, "hashes": 4, "txd-hashes": 4} | dict.fromkeys(STRING_KINDS, 1)
# Embedded (and standalone .ptd) Gen9 texture dictionary: a 0x40 root like the drawable dictionary's,
# 0x58-byte texture objects with their 0x20-byte view right after (the object's +0x30 points there),
# texture data in the graphics pages (ps5_texture_reader / ps5_texture_writer layout).
TEXTURE_OBJECT, TEXTURE_VIEW = 0x58, 0x20
MAX_TEXTURE_ALIGNMENT = 64 * 1024
RAW_KINDS = frozenset({"texture-data"})  # pixel data may hold any bit pattern


class DictionaryError(AssetError):
    pass


@dataclass(eq=False)
class Obj:
    """One allocation: bytes with pointer slots cleared, plus slot -> (target object, delta)."""

    kind: str
    data: bytearray
    region: str = "sys"
    src: int | None = None
    label: str = ""
    refs: dict[int, tuple[Obj, int]] = field(default_factory=dict)
    alignment: int | None = None  # texture data: its surface layout's alignment

    @property
    def align(self) -> int:
        return self.alignment or ALIGN.get(self.kind, 16)

    @property
    def alloc(self) -> int:
        return (len(self.data) + self.align - 1) & -self.align

    def u(self, fmt: str, at: int):
        return struct.unpack_from(fmt, self.data, at)[0]


@dataclass(eq=False)
class Graph:
    root: Obj
    objects: list[Obj]
    version: int = VERSION
    sys_flags: int = 0
    gfx_flags: int = 0

    def reachable(self) -> list[Obj]:
        """Objects reachable from the root, in depth-first slot order (the canonical walk order)."""
        order, done = [self.root], {id(self.root)}
        stack = [iter(sorted(self.root.refs.items()))]
        while stack:
            for _, (target, _) in stack[-1]:
                if id(target) not in done:
                    done.add(id(target))
                    order.append(target)
                    stack.append(iter(sorted(target.refs.items())))
                    break
            else:
                stack.pop()
        return order

    def collect(self) -> None:
        """Drop objects that are no longer reachable from the root."""
        self.objects = self.reachable()

    def entries(self) -> list[tuple[int, Obj]]:
        hashes, drawables = self.root.refs[0x20][0], self.root.refs[0x30][0]
        count = self.root.u("<H", 0x28)
        return [(hashes.u("<I", 4 * i), drawables.refs[8 * i][0]) for i in range(count)]


# ---- reader -------------------------------------------------------------------------------------


class _Reader:
    def __init__(self, blob: bytes, version: int = VERSION):
        try:
            header, self.payload = decode_resource(blob, MAX_PAYLOAD)
        except AssetError as error:
            raise DictionaryError(str(error)) from error
        self.version = header["version"]
        if self.version != version:
            raise DictionaryError(f"resource version {self.version}, expected {version}")
        _, _, self.sys_flags, self.gfx_flags = struct.unpack_from("<4sIII", blob)
        self.system, self.graphics = header["systemBytes"], header["graphicsBytes"]
        self.spans: dict[int, tuple[int, str, str]] = {}  # address -> (size, kind, label)
        self.slots: dict[int, tuple[int, int]] = {}  # slot address -> (pointer value, owner address)
        self.alignments: dict[int, int] = {}  # texture data address -> alignment

    def off(self, pointer: int, size: int = 1) -> int:
        if pointer >= SYS and pointer + size <= SYS + self.system:
            return pointer - SYS
        if pointer >= GFX and pointer + size <= GFX + self.graphics:
            return self.system + pointer - GFX
        raise DictionaryError(f"pointer {pointer:#x} (+{size:#x}) outside the resource")

    def u(self, pointer: int, fmt: str, delta: int = 0):
        return struct.unpack_from(fmt, self.payload, self.off(pointer + delta, struct.calcsize(fmt)))[0]

    def ptr(self, obj: int, delta: int, owner: int | None = None) -> int:
        """Declare the pointer slot obj+delta; `owner` names the object an interior pointer targets."""
        value = self.u(obj, "<Q", delta)
        if obj + delta in self.slots:
            raise DictionaryError(f"pointer slot {obj + delta:#x} declared twice")
        self.slots[obj + delta] = (value, value if owner is None else owner)
        return value

    def add(self, at: int, size: int, kind: str, label: str) -> bool:
        if not at:
            return False
        if size <= 0:
            raise DictionaryError(f"{label}: {kind} has no bytes")
        self.off(at, size)
        if at in self.spans:
            raise DictionaryError(f"{label}: {kind} at {at:#x} is shared with {self.spans[at][2]}")
        self.spans[at] = (size, kind, label)
        return True

    def string(self, at: int, kind: str, label: str) -> None:
        if not at:
            return
        start = self.off(at)
        end = self.payload.find(b"\0", start, start + 256)
        if end < 0:
            raise DictionaryError(f"{label}: unterminated {kind}")
        self.add(at, end + 1 - start, kind, label)

    def zero(self, obj: int, deltas: tuple[int, ...], what: str) -> None:
        """Fields that are pointers in CodeWalker's layout but unmodelled here must not point anywhere."""
        for delta in deltas:
            value = self.u(obj, "<Q", delta)
            if SYS <= value < SYS + self.system or GFX <= value < GFX + self.graphics:
                raise DictionaryError(f"{what}: unsupported pointer {value:#x} at +{delta:#x}")

    # -- typed walk (Gen9 v159 layouts) --

    def dictionary(self) -> None:
        root = SYS
        self.add(root, 0x40, "dictionary", "root")
        info = self.ptr(root, 0x08)
        hashes, count = self.ptr(root, 0x20), self.u(root, "<H", 0x28)
        drawables, values = self.ptr(root, 0x30), self.u(root, "<H", 0x38)
        if count != values or count != self.u(root, "<H", 0x2A) or values != self.u(root, "<H", 0x3A):
            raise DictionaryError("dictionary key and drawable counts disagree")
        if not 0 < count <= MAX_ENTRIES:
            raise DictionaryError(f"dictionary has {count} entries")
        self.zero(root, (0x10,), "dictionary root")
        if not info:
            raise DictionaryError("dictionary root has no page info")
        pages = self.u(info, "<B", 8) + self.u(info, "<B", 9)
        self.add(info, INFO_HEADER + INFO_RECORD * pages, "page-info", "root")
        self.add(hashes, 4 * count, "hashes", "root")
        self.add(drawables, 8 * count, "drawable-array", "root")
        keys = [self.u(hashes, "<I", 4 * i) for i in range(count)]
        if keys != sorted(keys) or len(set(keys)) != count:
            raise DictionaryError("dictionary keys are not strictly sorted")
        for i, key in enumerate(keys):
            self.drawable(self.ptr(drawables, 8 * i), f"entry {i} ({key:#010x})")

    def drawable(self, at: int, label: str, *, resource_root: bool = False) -> None:
        """A dictionary entry (+0x08 must be null) or, with `resource_root`, a .pdr root drawable
        whose +0x08 is the resource's page info."""
        if not self.add(at, 0xD0, "drawable", label):
            raise DictionaryError(f"{label}: null drawable")
        self.zero(at, (0x90, 0xB0, 0xC0, 0xC8) if resource_root else (0x08, 0x90, 0xB0, 0xC0, 0xC8), label)
        if resource_root:
            info = self.ptr(at, 0x08)
            if not info:
                raise DictionaryError(f"{label}: resource root has no page info")
            pages = self.u(info, "<B", 8) + self.u(info, "<B", 9)
            self.add(info, INFO_HEADER + INFO_RECORD * pages, "page-info", label)
        group, skeleton = self.ptr(at, 0x10), self.ptr(at, 0x18)
        lods = [self.ptr(at, delta) for delta in (0x50, 0x58, 0x60, 0x68)]
        models = self.ptr(at, 0xA0)
        if models and models not in lods:
            raise DictionaryError(f"{label}: models pointer +0xA0 is not one of the LOD lists")
        self.string(self.ptr(at, 0xA8), "drawable-name", label)
        if group:
            self.shader_group(group, label)
        if skeleton:
            self.skeleton(skeleton, label)
        for name, collection in zip(("high", "med", "low", "vlow"), lods, strict=True):
            if collection:
                self.collection(collection, f"{label} {name}")

    def shader_group(self, at: int, label: str) -> None:
        self.add(at, 0x40, "shader-group", label)
        txd = self.ptr(at, 0x08)
        if txd:
            self.texture_dictionary(txd, f"{label} txd")
        shaders, count = self.ptr(at, 0x10), self.u(at, "<H", 0x18)
        if count:
            self.add(shaders, 8 * count, "shader-array", label)
        for i in range(count):
            self.shader(self.ptr(shaders, 8 * i), f"{label} shader {i}")

    def shader(self, at: int, label: str) -> None:
        self.add(at, 0x40, "shader", label)
        params = self.u(at, "<Q", 0x08)
        self.ptr(at, 0x08)
        self.ptr(at, 0x10, params)
        self.ptr(at, 0x18, params)
        infos = self.ptr(at, 0x20)
        self.zero(at, (0x28, 0x30), label)
        if not infos:
            raise DictionaryError(f"{label}: shader without parameter infos")
        buffers, textures, unknowns, samplers, count = (self.u(infos, "<B", i) for i in range(5))
        copies = self.u(infos, "<B", 7)
        self.add(infos, 8 + 8 * count, "param-infos", label)
        if not params:
            if buffers or textures or unknowns or samplers:
                raise DictionaryError(f"{label}: parameter infos without a parameter block")
            return
        if not copies:
            raise DictionaryError(f"{label}: parameter block copy count 0")
        pointers = [self.ptr(params, 8 * i, params) for i in range(buffers * copies)]
        buffer_bytes = (pointers[buffers] - pointers[0]) * copies if buffers else 0
        if buffers and (pointers[0] != params + 8 * buffers * copies or buffer_bytes < 0):
            raise DictionaryError(f"{label}: parameter buffers are not inline after their pointers")
        texture_at = params + 8 * buffers * copies + buffer_bytes
        unknown_at = texture_at + 8 * textures * copies
        size = unknown_at + 8 * unknowns * copies + samplers - params
        self.add(params, size, "params", label)
        unknown_ok = self.u(at, "<Q", 0x18) in ((unknown_at,) if unknowns else (0, unknown_at))
        if self.u(at, "<Q", 0x10) not in ((texture_at,) if textures else (0, texture_at)) or not unknown_ok:
            raise DictionaryError(f"{label}: texture/unknown parameter pointers disagree with the block")
        for i in range(textures * copies):
            texture = self.ptr(texture_at, 8 * i)
            if texture and texture not in self.spans:
                self.texture(texture, label)
        for i in range(unknowns * copies):
            if self.ptr(unknown_at, 8 * i):
                raise DictionaryError(f"{label}: instance (unknown) parameter pointers are not supported")

    def texture_dictionary(self, at: int, label: str, *, resource_root: bool = False) -> None:
        """A Gen9 texture dictionary: embedded in a shader group, or (`resource_root`) a .ptd root
        whose +0x08 is the page info."""
        self.add(at, 0x40, "txd", label)
        if resource_root:
            info = self.ptr(at, 0x08)
            if not info:
                raise DictionaryError(f"{label}: resource root has no page info")
            pages = self.u(info, "<B", 8) + self.u(info, "<B", 9)
            self.add(info, INFO_HEADER + INFO_RECORD * pages, "page-info", label)
        else:
            self.zero(at, (0x08,), label)
        self.zero(at, (0x10,), label)
        keys, count = self.ptr(at, 0x20), self.u(at, "<H", 0x28)
        values, value_count = self.ptr(at, 0x30), self.u(at, "<H", 0x38)
        if count != value_count or not 0 < count <= MAX_ENTRIES:
            raise DictionaryError(f"{label}: texture dictionary key and texture counts disagree")
        self.add(keys, 4 * count, "txd-hashes", label)
        self.add(values, 8 * count, "txd-array", label)
        hashes = [self.u(keys, "<I", 4 * i) for i in range(count)]
        if hashes != sorted(hashes) or len(set(hashes)) != count:
            raise DictionaryError(f"{label}: texture dictionary keys are not strictly sorted")
        for i in range(count):
            self.embedded_texture(self.ptr(values, 8 * i), label)

    def embedded_texture(self, at: int, label: str) -> None:
        from gtavmenu_tools.ps5_texture_reader import PS5_TEXTURE_FORMATS
        from gtavmenu_tools.texture_layout import surface_layout

        self.add(at, TEXTURE_OBJECT + TEXTURE_VIEW, "txd-texture", label)
        self.string(self.ptr(at, 0x28), "texture-name", label)
        if self.u(at, "<Q", 0x30) != at + TEXTURE_OBJECT:
            raise DictionaryError(f"{label}: texture view is not right after its object")
        self.ptr(at, 0x30, at)
        data = self.ptr(at, 0x38)
        units, unit_bytes = self.u(at, "<I", 0x08), self.u(at, "<I", 0x0C)
        if not data or self.where(data) != "gfx" or not units or unit_bytes not in (1, 2, 4, 8, 16):
            raise DictionaryError(f"{label}: texture data is not a graphics allocation")
        self.add(data, units * unit_bytes, "texture-data", label)
        width, height, _, _, code, tile, _, mips = (
            self.u(at, f"<{f}", o) for f, o in zip("HHHHBBBB", (24, 26, 28, 30, 32, 33, 34, 35), strict=True)
        )
        if code in PS5_TEXTURE_FORMATS:
            layout = surface_layout(width, height, mips, PS5_TEXTURE_FORMATS[code][0], tile)
            if layout.storage_bytes != units * unit_bytes:
                raise DictionaryError(f"{label}: texture storage differs from its surface layout")
            self.alignments[data] = layout.alignment_bytes
        else:  # e.g. A8 wrinkle masks: no layout model here; keep the source's alignment (<= 64 KiB)
            offset = data - GFX
            self.alignments[data] = min(MAX_TEXTURE_ALIGNMENT, offset & -offset) if offset else MAX_TEXTURE_ALIGNMENT
        self.zero(at, tuple(d for d in range(0, TEXTURE_OBJECT, 8) if d not in (0x28, 0x30, 0x38)), label)

    def texture(self, at: int, label: str) -> None:
        self.add(at, 0x50, "texture", label)
        self.string(self.ptr(at, 0x28), "texture-name", label)
        if self.ptr(at, 0x30) or self.ptr(at, 0x38):
            raise DictionaryError(f"{label}: referenced texture with a view or data")

    def skeleton(self, at: int, label: str) -> None:
        self.add(at, 0x70, "skeleton", label)
        tags, capacity = self.ptr(at, 0x10), self.u(at, "<H", 0x18)
        bones = self.u(at, "<Q", 0x20)
        self.ptr(at, 0x20, bones - 16 if bones else None)
        inverse, transforms, parents, children = (self.ptr(at, d) for d in (0x28, 0x30, 0x38, 0x40))
        self.zero(at, (0x08, 0x48, 0x68), f"{label} skeleton")
        count, child_count = self.u(at, "<H", 0x5E), self.u(at, "<H", 0x60)
        if tags:
            self.add(tags, 8 * capacity, "tag-buckets", label)
            for i in range(capacity):
                tag, steps = self.ptr(tags, 8 * i), 0
                while tag:
                    self.add(tag, 0x10, "tag", label)
                    tag, steps = self.ptr(tag, 0x08), steps + 1
                    if steps > MAX_TAG_CHAIN:
                        raise DictionaryError(f"{label}: bone tag chain does not end")
        if bones:
            self.add(bones - 16, 16 + 0x50 * count, "bones", label)
            for i in range(count):
                self.string(self.ptr(bones + 0x50 * i, 0x38), "bone-name", label)
        self.add(inverse, 64 * count, "inverse-transforms", label)
        self.add(transforms, 64 * count, "transforms", label)
        self.add(parents, 2 * count, "parent-indices", label)
        self.add(children, 2 * child_count, "child-indices", label)

    def collection(self, at: int, label: str) -> None:
        self.add(at, 0x10, "lod-list", label)
        items, count = self.ptr(at, 0x00), self.u(at, "<H", 0x08)
        if count:
            self.add(items, 8 * count, "model-array", label)
        for i in range(count):
            self.model(self.ptr(items, 8 * i), f"{label} model {i}")

    def model(self, at: int, label: str) -> None:
        self.add(at, 0x30, "model", label)
        geometries, count = self.ptr(at, 0x08), self.u(at, "<H", 0x10)
        bounds, shader_map = self.ptr(at, 0x18), self.ptr(at, 0x20)
        if count:
            self.add(geometries, 8 * count, "geometry-array", label)
        self.add(bounds, 32 * (count + (1 if count > 1 else 0)), "bounds", label)
        self.add(shader_map, 2 * count, "shader-map", label)
        for i in range(count):
            self.geometry(self.ptr(geometries, 8 * i), f"{label} geometry {i}")

    def geometry(self, at: int, label: str) -> None:
        self.add(at, 0xA0, "geometry", label)
        vb, ib, bone_ids, vertex_data = (self.ptr(at, d) for d in (0x18, 0x38, 0x68, 0x78))
        self.zero(at, (0x08, 0x10, 0x20, 0x28, 0x30, 0x40, 0x48, 0x50, 0x80, 0x88, 0x90, 0x98), label)
        self.add(bone_ids, 2 * self.u(at, "<H", 0x72), "bone-ids", label)
        self.add(vb, 0x40, "vertex-buffer", label)
        count, stride = self.u(vb, "<I", 0x08), self.u(vb, "<H", 0x0C)
        data, view, declaration = self.ptr(vb, 0x18), self.ptr(vb, 0x30), self.ptr(vb, 0x38)
        self.zero(vb, (0x10, 0x20, 0x28), f"{label} vertex buffer")
        if data != vertex_data:
            raise DictionaryError(f"{label}: geometry vertex data differs from its buffer's data")
        if self.where(data) != "sys":
            raise DictionaryError(f"{label}: vertex data outside system pages")
        self.add(data, count * stride, "vertex-data", label)
        self.add(view, 0x20, "vertex-view", label)
        self.add(declaration, 0x140, "declaration", label)
        self.add(ib, 0x40, "index-buffer", label)
        indices, index_size = self.u(ib, "<I", 0x08), self.u(ib, "<H", 0x0C)
        index_data, index_view = self.ptr(ib, 0x18), self.ptr(ib, 0x30)
        self.zero(ib, (0x10, 0x20, 0x28, 0x38), f"{label} index buffer")
        self.add(index_data, indices * index_size, "index-data", label)
        self.add(index_view, 0x20, "index-view", label)
        for view_at in (view, index_view):
            if view_at:
                self.zero(view_at, (0x00, 0x08, 0x10, 0x18), f"{label} view")

    def where(self, pointer: int) -> str:
        return "sys" if SYS <= pointer < SYS + self.system else "gfx"

    # -- graph --

    def graph(self, *, coverage: bool = True) -> Graph:
        starts = sorted(self.spans)
        objects: dict[int, Obj] = {}
        for at in starts:
            size, kind, label = self.spans[at]
            start = self.off(at, size)
            objects[at] = Obj(kind, bytearray(self.payload[start : start + size]), self.where(at), at, label)
            objects[at].alignment = self.alignments.get(at)
        for previous, at in pairwise(starts):
            if previous + self.spans[previous][0] > at:
                raise DictionaryError(f"objects overlap at {at:#x}")
        for slot, (value, owner) in self.slots.items():
            index = bisect.bisect_right(starts, slot) - 1
            holder_at = starts[index] if index >= 0 else None
            if holder_at is None or slot + 8 > holder_at + self.spans[holder_at][0]:
                raise DictionaryError(f"pointer slot {slot:#x} lies outside every object")
            holder = objects[holder_at]
            struct.pack_into("<Q", holder.data, slot - holder_at, 0)
            if not value:
                continue
            target = objects.get(owner)
            if target is None or not owner <= value <= owner + len(target.data):
                raise DictionaryError(f"pointer {value:#x} at {slot:#x} is not inside its object {owner:#x}")
            holder.refs[slot - holder_at] = (target, value - owner)
        if coverage:
            self.check_coverage(starts)
        root = objects[SYS]
        graph = Graph(root, [], self.version, self.sys_flags, self.gfx_flags)
        graph.collect()
        if len(graph.objects) != len(objects):
            raise DictionaryError(f"{len(objects) - len(graph.objects)} walked objects are unreachable")
        return graph

    def check_coverage(self, starts: list[int]) -> None:
        covered = bytearray(len(self.payload))
        for at in starts:
            start = self.off(at)
            covered[start : start + self.spans[at][0]] = b"\1" * self.spans[at][0]
        loose = [i for i, byte in enumerate(self.payload) if byte and not covered[i]]
        if loose:
            raise DictionaryError(f"{len(loose)} nonzero payload bytes lie outside every object (first {loose[0]:#x})")
        slots = {self.off(slot) for slot in self.slots}
        raw = bytearray(len(self.payload))
        for at in starts:
            if self.spans[at][1] in RAW_KINDS:
                start = self.off(at)
                raw[start : start + self.spans[at][0]] = b"\1" * self.spans[at][0]
        for at in range(0, len(self.payload) - 7, 8):
            if raw[at]:
                continue
            value = struct.unpack_from("<Q", self.payload, at)[0]
            inside = SYS <= value < SYS + self.system or GFX <= value < GFX + self.graphics
            if inside and at not in slots:
                raise DictionaryError(f"undeclared pointer-like value {value:#x} at payload {at:#x}")


def parse(blob: bytes) -> Graph:
    reader = _Reader(blob)
    reader.dictionary()
    return reader.graph()


def parse_texture_dictionary(blob: bytes) -> Graph:
    """A standalone Gen9 v5 texture dictionary (.ptd) as an object graph (root txd + page info),
    e.g. ps5_texture_writer output to embed in a drawable's shader group."""
    reader = _Reader(blob, version=5)
    reader.texture_dictionary(SYS, "ptd", resource_root=True)
    return reader.graph()


def parse_drawable(blob: bytes, *, coverage: bool = True) -> Graph:
    """A standalone Gen9 v159 drawable (.pdr): the drawable is the root at system offset 0 and its
    +0x08 is the page info. Without `coverage` bytes no walked object owns are ignored: a drawable
    written by convert_pc_drawable.py keeps its template's replaced model lists as unreachable
    bytes, and only the reachable graph is taken."""
    reader = _Reader(blob)
    reader.drawable(SYS, "drawable", resource_root=True)
    return reader.graph(coverage=coverage)


# ---- writer -------------------------------------------------------------------------------------


def pc_pages(flags: int) -> list[int]:
    return [
        (512 << (flags & 15)) << (8 - rank)
        for rank, (shift, width) in enumerate(PC_FIELDS)
        for _ in range((flags >> shift) & ((1 << width) - 1))
    ]


def flags_for_pages(sizes: list[int]) -> int:
    """Low 28 flag bits for a largest-first page list; the smallest base that expresses it (retail)."""
    if not sizes:
        return 0
    if sizes != sorted(sizes, reverse=True) or any(s & (s - 1) for s in sizes):
        raise DictionaryError("page sizes must be a largest-first list of powers of two")
    for nibble in range(16):
        base, flags = 512 << nibble, nibble
        for rank, (shift, width) in enumerate(PC_FIELDS):
            count = sizes.count(base << (8 - rank))
            if count >= 1 << width:
                break
            flags |= count << shift
        else:
            if pc_pages(flags) == sizes:
                return flags
    raise DictionaryError(f"no page flags express {len(sizes)} pages {sorted(set(sizes))}")


def _pow2(value: int) -> int:
    return 1 << max(0, (value - 1).bit_length())


@dataclass
class Placement:
    offsets: dict[int, int]  # id(obj) -> offset inside its region
    pages: dict[str, list[int]]  # region -> largest-first page sizes
    layout: str
    flags: tuple[int, int] | None = None  # retain: the source flags (low 28 bits)


def place_retain(graph: Graph) -> Placement:
    """Every object at its source offset; source page flags. Only for unedited graphs."""
    offsets = {}
    for obj in graph.objects:
        if obj.src is None:
            raise DictionaryError("retain layout needs source placements (the graph was edited); use pack")
        offsets[id(obj)] = obj.src - (SYS if obj.region == "sys" else GFX)
    pages = {"sys": pc_pages(graph.sys_flags & 0x0FFFFFFF), "gfx": pc_pages(graph.gfx_flags & 0x0FFFFFFF)}
    flags = (graph.sys_flags & 0x0FFFFFFF, graph.gfx_flags & 0x0FFFFFFF)
    return Placement(offsets, pages, "retain", flags)


def _tie_key(ties: str):
    if ties == "walk":
        return lambda o: -o.alloc  # sorted() is stable: equal sizes keep the walk order
    if ties == "source":
        # Equal sizes keep their source order (RAGE's tie order is not derivable from the graph);
        # objects without a source (edits) follow, in walk order.
        return lambda o: (-o.alloc, o.src is None, o.src or 0)
    raise DictionaryError(f"unknown tie order {ties}")


def _pack_region(
    objects: list[Obj], region: str, root: Obj | None, ties: str = "walk", min_page: int = MIN_PAGE
) -> tuple[dict[int, int], list[int]]:
    """Page-at-a-time best fit by decreasing allocation size; root first. Returns offsets in page
    opening order and the page sizes (largest first after the final shrink)."""
    order = sorted((o for o in objects if o is not root), key=_tie_key(ties))
    remaining = order
    pages: list[tuple[int, dict[int, int], int]] = []  # (size, {id: offset in page}, used)
    previous = None
    while remaining or (root is not None and not pages):
        pending = [o.alloc for o in remaining] + ([root.alloc] if root is not None and not pages else [])
        largest, total = max(pending), sum(pending)
        floor = PAGE_FLOOR[region] if previous is None else min(previous, PAGE_FLOOR[region])
        size = max(_pow2(largest), floor, min_page)
        if total < size:
            # Tail: the largest power of two below what is left, unless that leaves more than half
            # of it again (then one page of the next power of two), as retail tails do (32K+16K).
            low = max(_pow2(largest), min_page, 1 << (total.bit_length() - 1))
            size = low if total <= low + low // 2 else max(low, _pow2(total))
        cursor, placed, left = 0, {}, []
        if root is not None and not pages:
            placed[id(root)] = 0
            cursor = root.alloc
        for obj in remaining:
            at = (cursor + obj.align - 1) & -obj.align
            if at + obj.alloc <= size:
                placed[id(obj)] = at
                cursor = at + obj.alloc
            else:
                left.append(obj)
        if not placed:
            raise DictionaryError(f"{region} packing made no progress")
        pages.append((size, placed, cursor))
        remaining, previous = left, size
    # Shrink the last page to the smallest power of two that holds it.
    size, placed, used = pages[-1]
    pages[-1] = (max(min_page, min(size, _pow2(used))), placed, used)
    sizes = [p[0] for p in pages]
    if sizes != sorted(sizes, reverse=True):
        raise DictionaryError(f"{region} pages are not largest first: {sizes}")
    offsets, base = {}, 0
    for size, placed, _ in pages:
        for key, at in placed.items():
            offsets[key] = base + at
        base += size
    return offsets, sizes


def _expressible(sizes: list[int]) -> bool:
    try:
        flags_for_pages(sizes)
    except DictionaryError:
        return False
    return True


def place_pack(graph: Graph, ties: str = "walk") -> Placement:
    """The retail rules; when the resulting page list has no flags encoding (each page size has a
    small count field: e.g. 13 x 256 KiB pages leave room for only one 8 KiB tail page), the
    smallest page size is raised (16, 32, 64 KiB, ...) until both regions encode."""
    info = graph.root.refs[0x08][0]
    system = [o for o in graph.objects if o.region == "sys"]
    graphics = [o for o in graph.objects if o.region == "gfx"]
    for _ in range(8):
        for min_page in (MIN_PAGE << shift for shift in range(8)):
            sys_offsets, sys_pages = _pack_region(system, "sys", graph.root, ties, min_page)
            gfx_offsets, gfx_pages = _pack_region(graphics, "gfx", None, ties, min_page) if graphics else ({}, [])
            if _expressible(sys_pages) and _expressible(gfx_pages):
                break
        else:
            raise DictionaryError(f"no page flags express the packed pages {sys_pages} / {gfx_pages}")
        want = INFO_HEADER + INFO_RECORD * (len(sys_pages) + len(gfx_pages))
        if len(info.data) == want:
            return Placement(sys_offsets | gfx_offsets, {"sys": sys_pages, "gfx": gfx_pages}, "pack")
        info.data = bytearray(info.data[:INFO_HEADER].ljust(want, b"\0"))
    raise DictionaryError("page-info size did not converge")


def emit(graph: Graph, placement: Placement) -> tuple[bytes, int, int]:
    """Inflated payload plus the system and graphics flags (version nibbles included)."""
    sys_size, gfx_size = sum(placement.pages["sys"]), sum(placement.pages["gfx"])
    payload = bytearray(sys_size + gfx_size)
    base = {"sys": SYS, "gfx": GFX}
    start = {"sys": 0, "gfx": sys_size}
    limit = {"sys": sys_size, "gfx": gfx_size}
    info = graph.root.refs[0x08][0]
    info.data[8], info.data[9] = len(placement.pages["sys"]), len(placement.pages["gfx"])
    owner = bytearray(len(payload))
    for obj in graph.objects:
        at = placement.offsets[id(obj)]
        misaligned = at % obj.align and placement.layout != "retain"  # retain keeps the source's offsets
        if at < 0 or misaligned or at + len(obj.data) > limit[obj.region]:
            raise DictionaryError(f"{obj.kind} ({obj.label}) placed outside its region or misaligned")
        p = start[obj.region] + at
        if any(owner[p : p + len(obj.data)]):
            raise DictionaryError(f"{obj.kind} ({obj.label}) overlaps another object")
        owner[p : p + len(obj.data)] = b"\1" * len(obj.data)
        payload[p : p + len(obj.data)] = obj.data
    for obj in graph.objects:
        p = start[obj.region] + placement.offsets[id(obj)]
        for slot, (target, delta) in obj.refs.items():
            value = base[target.region] + placement.offsets[id(target)] + delta
            struct.pack_into("<Q", payload, p + slot, value)
    _check_page_bounds(graph, placement)
    low = placement.flags or (flags_for_pages(placement.pages["sys"]), flags_for_pages(placement.pages["gfx"]))
    sys_flags = low[0] | (graph.version >> 4) << 28
    gfx_flags = low[1] | (graph.version & 15) << 28
    return bytes(payload), sys_flags, gfx_flags


def _check_page_bounds(graph: Graph, placement: Placement) -> None:
    bounds = {}
    for region, sizes in placement.pages.items():
        rows, at = [], 0
        for size in sizes:
            rows.append((at, at + size))
            at += size
        bounds[region] = rows
    for obj in graph.objects:
        at = placement.offsets[id(obj)]
        if not any(a <= at and at + len(obj.data) <= b for a, b in bounds[obj.region]):
            raise DictionaryError(f"{obj.kind} ({obj.label}) crosses a page boundary")


def encode(payload: bytes, sys_flags: int, gfx_flags: int, version: int = VERSION) -> bytes:
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
    stream = compressor.compress(payload) + compressor.flush()
    return struct.pack("<4sIII", RSC7, version, sys_flags, gfx_flags) + stream


def write(graph: Graph, layout: str = "pack", ties: str = "walk") -> bytes:
    graph.collect()
    placement = place_retain(graph) if layout == "retain" else place_pack(graph, ties)
    payload, sys_flags, gfx_flags = emit(graph, placement)
    return encode(payload, sys_flags, gfx_flags, graph.version)


# ---- edits --------------------------------------------------------------------------------------


def _entry_index(graph: Graph, name: str) -> int:
    key = joaat(name)
    for i, (value, _) in enumerate(graph.entries()):
        if value == key:
            return i
    raise DictionaryError(f"no dictionary entry {name} ({key:#010x})")


def _subgraph(obj: Obj) -> list[Obj]:
    out, seen, stack = [], set(), [obj]
    while stack:
        current = stack.pop()
        if id(current) in seen:
            continue
        seen.add(id(current))
        out.append(current)
        stack.extend(target for target, _ in current.refs.values())
    return out


def deep_copy(obj: Obj, graph: Graph) -> Obj:
    """Independent copy of everything reachable from `obj`; refuses if any of it is shared."""
    inside = _subgraph(obj)
    ids = {id(o) for o in inside}
    for other in graph.reachable():
        if id(other) in ids:
            continue
        for target, _ in other.refs.values():
            if id(target) in ids and target is not obj:
                raise DictionaryError(f"{target.kind} ({target.label}) is shared outside the copied drawable")
    clones = {id(o): Obj(o.kind, bytearray(o.data), o.region, None, o.label + " (copy)") for o in inside}
    for o in inside:
        clones[id(o)].refs = {slot: (clones[id(t)], delta) for slot, (t, delta) in o.refs.items()}
    return clones[id(obj)]


def replace_entry(graph: Graph, entry: str, source: str) -> None:
    """Key `entry` gets an independent deep copy of key `source`'s drawable."""
    target_index, source_index = _entry_index(graph, entry), _entry_index(graph, source)
    if target_index == source_index:
        raise DictionaryError("entry and source are the same key")
    drawables = graph.root.refs[0x30][0]
    copy = deep_copy(drawables.refs[8 * source_index][0], graph)
    drawables.refs[8 * target_index] = (copy, 0)
    graph.collect()


LOD_SLOTS = (0x50, 0x58, 0x60, 0x68)  # high, med, low, vlow model lists
LOD_DISTANCES, RENDER_MASKS = 0x70, 0x80  # 4 x f32, 4 x u32


def entry_resource(graph: Graph, entry: str, *, high_only: bool = False, strip_textures: bool = False) -> bytes:
    """One dictionary entry as a standalone .pdr (an independent deep copy at the resource root, with
    its own page info; pack layout). `high_only` drops the medium, low and very-low model lists and
    zeroes their render masks (the shape of retail high-only ped components), so the result can be a
    convert_pc_drawable.py template (it replaces only the high list). `strip_textures` drops an
    embedded texture dictionary (streamed components carry one; a template's textures are not used)."""
    index = _entry_index(graph, entry)
    drawable = deep_copy(graph.root.refs[0x30][0].refs[8 * index][0], graph)
    if strip_textures and 0x10 in drawable.refs:
        drawable.refs[0x10][0].refs.pop(0x08, None)
    if high_only:
        for lod, slot in enumerate(LOD_SLOTS[1:], 1):
            drawable.refs.pop(slot, None)
            struct.pack_into("<I", drawable.data, RENDER_MASKS + 4 * lod, 0)
        if 0xA0 in drawable.refs and drawable.refs[0xA0][0] is not drawable.refs[0x50][0]:
            raise DictionaryError(f"{entry}: models pointer +0xA0 is not the high list")
    info = Obj("page-info", bytearray(graph.root.refs[0x08][0].data[:INFO_HEADER]), label="page info")
    drawable.refs[0x08] = (info, 0)
    single = Graph(drawable, [], graph.version)
    single.collect()
    return write(single, "pack")


def put_drawable(graph: Graph, entry: str, drawable: Graph, *, index_data_region: str = "gfx") -> Obj:
    """Key `entry` gets the root drawable of the standalone `drawable` graph (parse_drawable); its
    page info is dropped and its index data moves to `index_data_region` (retail .pdd: graphics).
    The old drawable becomes unreachable and is dropped. Refuses objects shared with the dictionary."""
    root = drawable.root
    root.refs.pop(0x08, None)
    inside = {id(o) for o in _subgraph(root)}
    if inside & {id(o) for o in graph.reachable()}:
        raise DictionaryError("the new drawable shares objects with the dictionary")
    for obj in _subgraph(root):
        if obj.kind == "index-data":
            obj.region = index_data_region
        obj.src = None
    graph.root.refs[0x30][0].refs[8 * _entry_index(graph, entry)] = (root, 0)
    graph.collect()
    return root


def add_entry(graph: Graph, entry: str, source: str) -> None:
    """A new key `entry` (inserted in hash order) holding an independent deep copy of key `source`'s
    drawable; put_drawable or replace_entry then gives it its own drawable. Both counts follow."""
    key = joaat(entry)
    entries = graph.entries()
    if any(value == key for value, _ in entries):
        raise DictionaryError(f"dictionary already has {entry}")
    copy = deep_copy(graph.root.refs[0x30][0].refs[8 * _entry_index(graph, source)][0], graph)
    keys = [value for value, _ in entries]
    at = bisect.bisect(keys, key)
    keys.insert(at, key)
    hashes, drawables = graph.root.refs[0x20][0], graph.root.refs[0x30][0]
    rows = [drawables.refs[8 * i] for i in range(len(entries))]
    rows.insert(at, (copy, 0))
    hashes.data = bytearray(struct.pack(f"<{len(keys)}I", *keys))
    drawables.data = bytearray(8 * len(keys))
    drawables.refs = {8 * i: ref for i, ref in enumerate(rows)}
    for slot in (0x28, 0x2A, 0x38, 0x3A):
        struct.pack_into("<H", graph.root.data, slot, len(keys))
    graph.collect()


def rename_textures(obj: Obj, name: str = "givemechecker") -> int:
    """Point every texture reference of the drawable `obj` at the texture `name` (a placeholder
    drawable that must reference only global textures); returns how many names were rewritten."""
    done = 0
    for item in _subgraph(obj):
        if item.kind == "texture" and 0x28 in item.refs:
            string = item.refs[0x28][0]
            string.data = bytearray(name.encode("ascii") + b"\0")
            done += 1
    return done


def select_entries(graph: Graph, names: list[str]) -> None:
    """Keep only the named keys (a new ped's dictionary built from a retail one); the key array and
    the drawable array shrink, both counts follow, unreachable drawables are dropped."""
    rows = sorted({_entry_index(graph, name) for name in names})
    if not rows:
        raise DictionaryError("no entries selected")
    entries = graph.entries()
    keys = [entries[i][0] for i in rows]
    hashes = graph.root.refs[0x20][0]
    drawables = graph.root.refs[0x30][0]
    hashes.data = bytearray(struct.pack(f"<{len(keys)}I", *keys))
    kept = [drawables.refs[8 * i] for i in rows]
    drawables.data = bytearray(8 * len(keys))
    drawables.refs = {8 * i: ref for i, ref in enumerate(kept)}
    for at in (0x28, 0x2A, 0x38, 0x3A):
        struct.pack_into("<H", graph.root.data, at, len(keys))
    graph.collect()


def embed_textures(drawable: Graph, textures: Graph) -> Obj:
    """Attach a standalone texture dictionary (parse_texture_dictionary) as the drawable's embedded
    one (shader group +0x08; its page info is dropped). Refuses a drawable that already has one."""
    group = drawable.root.refs[0x10][0]
    if 0x08 in group.refs:
        raise DictionaryError("the drawable already embeds a texture dictionary")
    textures.root.refs.pop(0x08, None)
    group.refs[0x08] = (textures.root, 0)
    drawable.collect()
    return textures.root


def subgraph_compare(a: Obj, b: Obj) -> list[str]:
    """compare() for two drawables (or any two objects) instead of two dictionaries; page info
    (a resource root's +0x08) is ignored."""
    x, y = Obj(a.kind, a.data, a.region), Obj(b.kind, b.data, b.region)
    x.refs = {k: v for k, v in a.refs.items() if not (k == 0x08 and v[0].kind == "page-info")}
    y.refs = {k: v for k, v in b.refs.items() if not (k == 0x08 and v[0].kind == "page-info")}
    return compare(Graph(x, []), Graph(y, []))


def swap_entries(graph: Graph, first: str, second: str) -> None:
    a, b = _entry_index(graph, first), _entry_index(graph, second)
    drawables = graph.root.refs[0x30][0]
    drawables.refs[8 * a], drawables.refs[8 * b] = drawables.refs[8 * b], drawables.refs[8 * a]


# ---- comparison ---------------------------------------------------------------------------------


def compare(a: Graph, b: Graph) -> list[str]:
    """Walk both graphs in slot order; every object pairs with exactly one object of the same kind,
    region and bytes (pointer slots cleared), and every pointer targets the paired object at the
    same delta. Page-info differs only in its page counts and record space (reported, not fatal)."""
    problems: list[str] = []
    pairs: dict[int, Obj] = {}
    reverse: dict[int, Obj] = {}
    stack = [(a.root, b.root)]
    while stack:
        x, y = stack.pop()
        if id(x) in pairs:
            if pairs[id(x)] is not y:
                problems.append(f"{x.kind} ({x.label}) pairs with two different objects")
            continue
        if id(y) in reverse:
            problems.append(f"{y.kind} ({y.label}) is reached from two different objects")
            continue
        pairs[id(x)], reverse[id(y)] = y, x
        if (x.kind, x.region) != (y.kind, y.region):
            problems.append(f"{x.label}: {x.kind}/{x.region} vs {y.kind}/{y.region}")
            continue
        if x.kind == "page-info":
            if x.data[:8] != y.data[:8] or x.data[10:16] != y.data[10:16]:
                problems.append("page-info header differs outside the page counts")
        elif x.data != y.data:
            problems.append(f"{x.kind} ({x.label}): bytes differ ({len(x.data)} vs {len(y.data)} bytes)")
        if sorted(x.refs) != sorted(y.refs):
            problems.append(f"{x.kind} ({x.label}): pointer slots differ")
            continue
        for slot in sorted(x.refs, reverse=True):
            (tx, dx), (ty, dy) = x.refs[slot], y.refs[slot]
            if dx != dy:
                problems.append(f"{x.kind} ({x.label}) +{slot:#x}: pointer delta {dx:#x} vs {dy:#x}")
            stack.append((tx, ty))
    if len(pairs) != len(a.reachable()) or len(reverse) != len(b.reachable()):
        problems.append(f"object counts differ ({len(a.reachable())} vs {len(b.reachable())})")
    return problems


def census(graph: Graph) -> dict:
    kinds = Counter(o.kind for o in graph.objects)
    return {
        "objects": len(graph.objects),
        "pointerSlots": sum(len(o.refs) for o in graph.objects),
        "bytes": {r: sum(len(o.data) for o in graph.objects if o.region == r) for r in ("sys", "gfx")},
        "kinds": dict(sorted(kinds.items())),
    }


def describe_entries(blob: bytes) -> dict[str, str]:
    """Entry name -> LOD geometry summary (vertices per LOD list), for edit reports."""
    rows = {}
    for entry in describe(blob)["entries"]:
        lods = {k: [g["vertices"] for m in v for g in m["geometries"]] for k, v in entry["lods"].items() if v}
        rows[entry["entry"]] = " ".join(f"{k}={v}" for k, v in lods.items())
    return rows


def describe(blob: bytes) -> dict:
    """Decoded view through inspect_ped_resources (geometries, shaders, skeletons, LODs)."""
    import inspect_ped_resources as ped

    r = ped.Res(blob)
    return ped.inspect_dictionary(r, ped.Names(), True)


# ---- variations (pmt) ---------------------------------------------------------------------------


def check_variations(graph: Graph, pmt: bytes) -> list[str]:
    """Every pmt component drawable `<comp>_<nnn>_<u|r>` has a dictionary key and every key a pmt
    drawable (the pmt's componentDrawables and the dictionary entry count agree)."""
    import inspect_ped_resources as ped

    info = ped.inspect_variations(ped.Res(pmt))
    wanted = set()
    for comp, row in info["components"].items():
        for index, drawable in enumerate(row["drawables"]):
            wanted.add(f"{comp}_{index:03d}_{'r' if drawable['race'] else 'u'}")
    keys = {key for key, _ in graph.entries()}
    problems = [f"pmt drawable {name} has no dictionary key" for name in sorted(wanted) if joaat(name) not in keys]
    names = {joaat(name): name for name in wanted}
    problems += [f"dictionary key {key:#010x} is not a pmt drawable" for key in sorted(keys) if key not in names]
    if info["componentDrawables"] != len(keys):
        problems.append(f"pmt componentDrawables {info['componentDrawables']} != dictionary entries {len(keys)}")
    return problems


# ---- CLI ----------------------------------------------------------------------------------------


def _payload(blob: bytes) -> bytes:
    return decode_resource(blob, MAX_PAYLOAD)[1]


def moved(source: Graph, rebuilt: Graph) -> int:
    """Objects whose region offset differs from the source (paired by the canonical walk)."""
    count = 0
    for x, y in zip(source.reachable(), rebuilt.reachable(), strict=True):
        count += (x.src - (SYS if x.region == "sys" else GFX)) != (y.src - (SYS if y.region == "sys" else GFX))
    return count


def roundtrip_report(blob: bytes, layout: str, ties: str = "walk") -> tuple[bytes, dict]:
    source = parse(blob)
    out = write(parse(blob), layout, ties)
    rebuilt = parse(out)
    a, b = _payload(blob), _payload(out)
    differing = sum(1 for x, y in zip(a, b, strict=False) if x != y) + abs(len(a) - len(b))
    report = {
        "layout": layout,
        "ties": ties if layout == "pack" else None,
        "source": {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest(), "payload": len(a)},
        "output": {"bytes": len(out), "sha256": hashlib.sha256(out).hexdigest(), "payload": len(b)},
        "flags": {
            "source": [f"{x:#010x}" for x in struct.unpack_from("<II", blob, 8)],
            "output": [f"{x:#010x}" for x in struct.unpack_from("<II", out, 8)],
        },
        "pages": {
            "source": {"sys": pc_pages(source.sys_flags & 0x0FFFFFFF), "gfx": pc_pages(source.gfx_flags & 0x0FFFFFFF)},
            "output": {
                "sys": pc_pages(rebuilt.sys_flags & 0x0FFFFFFF),
                "gfx": pc_pages(rebuilt.gfx_flags & 0x0FFFFFFF),
            },
        },
        "payloadIdentical": a == b,
        "payloadBytesDiffering": differing,
        "objectsMoved": moved(source, rebuilt),
        "census": census(source),
        "semanticProblems": compare(source, rebuilt),
        "decodedIdentical": describe(blob) == describe(out),
    }
    return out, report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    rt = sub.add_parser("roundtrip", help="parse, rebuild, compare")
    rt.add_argument("pdd", type=Path)
    rt.add_argument("--layout", choices=("retain", "pack"), default="pack")
    rt.add_argument("--ties", choices=("walk", "source"), default="walk", help="pack: equal-size order")
    rt.add_argument("--out", type=Path)
    rp = sub.add_parser("replace", help="give one key a deep copy of another key's drawable")
    rp.add_argument("pdd", type=Path)
    rp.add_argument("--entry", required=True)
    rp.add_argument("--with", dest="source", required=True)
    rp.add_argument("--out", type=Path, required=True)
    rp.add_argument("--pmt", type=Path, help="check the variation info against the output")
    cp = sub.add_parser("compare", help="semantic comparison of two dictionaries")
    cp.add_argument("a", type=Path)
    cp.add_argument("b", type=Path)
    cv = sub.add_parser("check-variations", help="pmt component drawables vs dictionary keys")
    cv.add_argument("pdd", type=Path)
    cv.add_argument("pmt", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "roundtrip":
            out, report = roundtrip_report(args.pdd.read_bytes(), args.layout, args.ties)
            print(json.dumps(report, indent=1))
            ok = not report["semanticProblems"] and report["decodedIdentical"]
            if args.layout == "retain":
                ok = ok and report["payloadIdentical"]
            if args.out:
                _write_new(args.out, out)
            return 0 if ok else 1
        if args.command == "replace":
            graph = parse(args.pdd.read_bytes())
            replace_entry(graph, args.entry, args.source)
            out = write(graph, "pack")
            check = parse(out)
            entries = describe_entries(out)
            report = {"entries": entries, "census": census(check)}
            if args.pmt:
                report["variationProblems"] = check_variations(check, args.pmt.read_bytes())
            print(json.dumps(report, indent=1))
            _write_new(args.out, out)
            return 1 if report.get("variationProblems") else 0
        if args.command == "compare":
            problems = compare(parse(args.a.read_bytes()), parse(args.b.read_bytes()))
            print("\n".join(problems) if problems else "semantically identical")
            return 1 if problems else 0
        problems = check_variations(parse(args.pdd.read_bytes()), args.pmt.read_bytes())
        print("\n".join(problems) if problems else "variation info consistent")
        return 1 if problems else 0
    except (AssetError, OSError, struct.error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


def _write_new(path: Path, data: bytes) -> None:
    if path.exists():
        raise DictionaryError(f"refusing to overwrite {path}")
    path.write_bytes(data)
    print(f"wrote {path} ({len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()})", file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())

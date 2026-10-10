#!/usr/bin/env python3
"""Convert the own models of a PC map mod (custom archetypes) into PS5 pack members.

For a map mod whose ymaps place the mod's own models (tools/convert_ymap.py classes them "custom").
From the mod archive (dlc.rpf; read as untrusted data with fixed limits, run with `python3 -I`), per
wanted archetype:

- .ydr -> .pdr: tools/convert_pc_drawable.py --prop route. No retail drawable has the mod's
  name, so a generic retail drawable (--template, a map building piece) gives only the drawable root
  words; the PC source supplies everything else. --shader-template drawables give the shader
  schemas (each shader from the first template that carries it, the main template first). The
  model's light attributes are written as PS5 records (--no-lights / --lights-only / --max-lights
  narrow them); a model with more than convert_pc_drawable.OBJECT_LIGHT_LIMIT lights is noted, since
  it must stay a building (the pack typ clears physicsDictionary, so these archetypes do).
- textures: one .ptd per archetype texture dictionary. It holds the textures the converted drawables
  link, taken from their embedded dictionaries first and then from the mod's .ytd of that name (as on
  PC); a linked texture neither has gets a neutral stand-in (diffuse/normal/specular only). A group
  whose archetypes link more textures than one written dictionary holds (convert_pc_ytd_writer
  MAX_DICTIONARY_TEXTURES) is split into consecutive archetype runs, dictionaries <txd>_<n>, and those
  archetypes' textureDictionary in the pack typ names their part. --rename-txd OLD=NEW writes the
  dictionary of OLD (from the mod's OLD.ytd) as NEW.ptd and points its archetypes at NEW: dictionaries
  are streamed by name, so a pack meant to be active with another pack built from the same mod must
  not ship a dictionary of the same name (tools/validate_runtime_pack.py --against warns).
  --txd-alias HASH=NAME names a textureDictionary hash that no known string resolves (mods whose
  archive names are unreadable, e.g. NG-keyed tables carved key-free): the group is read from the
  mod's NAME.ytd (or the .ytd whose name hashes to HASH: Map Builder's `<model>+hidr`) and written as NAME.ptd.
  When joaat(NAME) != HASH the alias is a rename, and those archetypes' textureDictionary in the pack typ
  becomes NAME (a dictionary streams by its name).
- definitions: the archetypes' CBaseArchetypeDef rows from the mod's .ytyp (tools/make_mlo_ptyp.py
  decode) into ONE pack typ (--typ-name), names kept, so the game finds each drawable by its archetype
  name. physicsDictionary is set to 0: the PS5 build of a prop's physics comes from its drawable's
  embedded bound, and this route writes none (PC bounds are not converted), so these archetypes have
  no collision. The mod's .ybn files (static map collision) are listed, not converted.

Writes into --out (new or empty): <archetype>.pdr (+ .report.json), <txd>.ptd, <typ>.ptyp, the typ spec
(<typ>.json), pack-archetypes.json (tools/convert_ymap.py --pack-archetypes) and models-report.json.
An archetype that cannot be converted is reported and left out (its entities stay custom/dropped).

  convert_pc_map_models.py MOD/dlc.rpf --report convert_ymap-report.json --template ch3_03_ss_cb.pdr \\
      --shader-template A.pdr ... --typ-template v_int_22.ptyp --typ-name gm_malibu --out WORK \\
      [--archetype NAME ...] [--max-texture-size 1024] [--templates TEXTURE_TEMPLATE_DIR] \\
      [--rename-txd OLD=NEW ...] [--txd-alias HASH=NAME ...]
"""

from __future__ import annotations

import argparse
import json
import math
import mmap
import re
import struct
import sys
from dataclasses import replace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

import convert_pc_drawable as drawable  # noqa: E402
import convert_pc_ytd_writer  # noqa: E402
import make_mlo_ptyp  # noqa: E402
from gtavmenu_tools.asset_formats import AssetError, read_rpf_member, rpf_header, rpf_members  # noqa: E402
from gtavmenu_tools.hashes import joaat  # noqa: E402
from gtavmenu_tools.ymap import MOD_LIMITS, label_name  # noqa: E402

PACK_ARCHETYPES_SCHEMA = "gtavmenu-pack-archetypes-v1"
SUFFIXES = (".ydr", ".ytd", ".ytyp", ".ybn")
MAX_MEMBER_BYTES = 64 * 1024 * 1024
MAX_READ_BYTES = 1024 * 1024 * 1024
MAX_DEPTH = 4
_NAME = re.compile(r"[a-z0-9_]{1,63}\Z")


def mod_members(view, suffixes: tuple[str, ...] = SUFFIXES) -> tuple[dict[str, bytes], list[str]]:
    """{lower base name with suffix: member bytes (RSC7 header rebuilt)} of a PC RPF7 (OPEN/NONE tables)
    and its uncompressed nested archives; notes for what was skipped. A name that appears twice is refused."""
    found: dict[str, bytes] = {}
    notes: list[str] = []
    pending = [(0, len(view), "", 0)]
    total = 0
    while pending:
        base, size, prefix, depth = pending.pop()

        def read_at(offset: int, count: int, base: int = base, size: int = size) -> bytes:
            if offset < 0 or count < 0 or offset + count > size:
                raise AssetError("archive read outside its bounds")
            return bytes(view[base + offset : base + offset + count])

        end = rpf_header(read_at(0, 16), size)["tableEnd"]
        for member in rpf_members(read_at(0, end), MOD_LIMITS, file_size=size, read_at=read_at, name_check=label_name):
            lower = member.name.lower()
            path = prefix + member.name
            if lower.endswith(".rpf") and not member.resource:
                if member.compressed or member.encrypted or depth >= MAX_DEPTH:
                    notes.append(f"{path}: nested archive is compressed, encrypted or too deep")
                else:
                    pending.append((base + member.offset, member.size, path + "/", depth + 1))
                continue
            if not lower.endswith(suffixes):
                continue
            key = lower.rsplit("/", 1)[-1]
            if not member.resource or member.size > MAX_MEMBER_BYTES:
                notes.append(f"{path}: not a resource or larger than {MAX_MEMBER_BYTES} bytes")
                continue
            if key in found:
                raise AssetError(f"{key} appears twice in the mod archive")
            total += member.size
            if total > MAX_READ_BYTES:
                raise AssetError("mod models exceed the reader budget")
            found[key] = read_rpf_member(
                read_at(member.offset, member.size), replace(member, offset=0), MAX_MEMBER_BYTES
            )
    return found, notes


def folder_members(folder: Path, suffixes: tuple[str, ...] = SUFFIXES) -> tuple[dict[str, bytes], list[str]]:
    """mod_members for an unpacked mod folder: its loose files (searched recursively, symbolic links
    skipped), each a whole RSC7 resource as PC tools write them."""
    found: dict[str, bytes] = {}
    notes: list[str] = []
    total = 0
    for path in sorted(folder.rglob("*")):
        key = path.name.lower()
        if path.is_symlink() or not path.is_file() or not key.endswith(suffixes):
            continue
        size = path.stat().st_size
        if size > MAX_MEMBER_BYTES:
            notes.append(f"{path.relative_to(folder)}: larger than {MAX_MEMBER_BYTES} bytes")
            continue
        if key in found:
            raise AssetError(f"{key} appears twice in the mod folder")
        total += size
        if total > MAX_READ_BYTES:
            raise AssetError("mod models exceed the reader budget")
        blob = path.read_bytes()
        if blob[:4] != b"RSC7":
            notes.append(f"{path.relative_to(folder)}: not an RSC7 resource")
            continue
        found[key] = blob
    return found, notes


def hash_of(value) -> int:
    """A spec hash value ('0x...', 'hash_...', a name, '' or an int) as an int."""
    if isinstance(value, int):
        return value
    text = str(value or "")
    if not text:
        return 0
    if text.startswith("0x"):
        return int(text, 16)
    if text.startswith("hash_"):
        return int(text[5:], 16)
    return joaat(text)


def typ_definitions(members: dict[str, bytes], names: dict[int, str]) -> dict[int, tuple[str, dict]]:
    """archetype hash -> (typ member name, CBase/CTimeArchetypeDef spec row) of every mod .ytyp."""
    out: dict[int, tuple[str, dict]] = {}
    for key, blob in sorted(members.items()):
        if not key.endswith(".ytyp"):
            continue
        spec = make_mlo_ptyp.decode(blob, names)
        for row in spec.get("archetypes") or []:
            if row.get("type") in ("CBaseArchetypeDef", "CTimeArchetypeDef"):
                out.setdefault(hash_of(row.get("name")), (key, row))
    return out


def bbox(row: dict) -> tuple[list[float], list[float]]:
    """bbMin/bbMax ordered per axis (mods ship boxes with swapped axes; the extents need min <= max)."""
    lo, hi = [float(v) for v in row["bbMin"]], [float(v) for v in row["bbMax"]]
    if len(lo) != 3 or len(hi) != 3 or any(abs(v) > 1e5 for v in lo + hi):
        raise AssetError("archetype bounding box is out of range")
    return [min(a, b) for a, b in zip(lo, hi, strict=True)], [max(a, b) for a, b in zip(lo, hi, strict=True)]


def parse_txd_aliases(items: list[str]) -> dict[int, str]:
    """--txd-alias HASH=NAME (HASH as 0x..., hash_... or decimal) -> {hash: name}."""
    aliases: dict[int, str] = {}
    for item in items:
        key, sep, name = item.partition("=")
        name = name.strip().lower()
        try:
            value = hash_of(key.strip()) if key.strip().startswith(("0x", "hash_")) else int(key.strip(), 10)
        except ValueError:
            value = -1
        if not sep or not 0 < value <= 0xFFFFFFFF or not _NAME.fullmatch(name):
            raise ValueError(f"--txd-alias {item!r}: expected HASH=NAME (HASH 0x..., NAME lowercase)")
        if aliases.get(value, name) != name:
            raise ValueError(f"--txd-alias gives {value:#010x} two names")
        aliases[value] = name
    return aliases


def texture_dictionary_name(row: dict, names: dict[int, str], aliases: dict[int, str]) -> str:
    """The archetype's texture dictionary name: an alias, else the resolved hash, else the raw value."""
    value = hash_of(row.get("textureDictionary"))
    if value in aliases:
        return aliases[value]
    return names.get(value, str(row.get("textureDictionary") or ""))


def typ_row(row: dict, txd: str) -> dict:
    """The archetype row with textureDictionary naming the dictionary written for it (a split part, a
    --rename-txd target or a renaming --txd-alias); unchanged when the stored hash already is that name's."""
    return row if hash_of(row.get("textureDictionary")) == joaat(txd) else row | {"textureDictionary": txd}


def plan_archetypes(wanted: list[str], definitions: dict, members: dict[str, bytes]) -> tuple[list, list]:
    """(convertible [(name, typ key, row)], refused [{archetype, reason}])."""
    plan, refused = [], []
    for name in wanted:
        entry = definitions.get(hash_of(name))
        reason = None
        if entry is None:
            reason = "no CBase/CTimeArchetypeDef in the mod's typs (an MLO, or not the mod's)"
        elif name.startswith("hash_"):
            reason = "archetype name unknown (no model of that name in the mod)"
        else:
            row = entry[1]
            if row.get("assetType") not in ("ASSET_TYPE_DRAWABLE", 2):
                reason = f"assetType {row.get('assetType')} (only drawables convert)"
            elif hash_of(row.get("drawableDictionary")):
                reason = "drawable dictionary archetype (.ydd) not supported"
            elif hash_of(row.get("assetName")) != joaat(name):
                reason = "assetName differs from the archetype name"
            elif f"{name}.ydr" not in members:
                reason = f"the mod ships no {name}.ydr"
        if reason:
            refused.append({"archetype": name, "reason": reason})
        else:
            plan.append((name, entry[0], entry[1]))
    return plan, refused


def split_group(txd: str, archetypes: list[str], links: list[list[dict]], cap: int) -> list[tuple[str, list[int]]]:
    """[(dictionary name, archetype indices)]: the whole group under `txd` when the names its archetypes
    link fit `cap`, else consecutive runs that fit, named <txd>_1, <txd>_2, ... An archetype that alone
    links more than `cap` gets a run of its own (and is refused when its dictionary is written)."""
    names = [{link["name"].lower() for link in rows} for rows in links]
    if len(set().union(*names)) <= cap:
        return [(txd, list(range(len(archetypes))))]
    runs: list[list[int]] = []
    current: set[str] = set()
    for index, linked in enumerate(names):
        if runs and len(current | linked) <= cap:
            runs[-1].append(index)
            current |= linked
        else:
            runs.append([index])
            current = set(linked)
    return [(f"{txd}_{n}", run) for n, run in enumerate(runs, 1)]


def pack_typ_spec(typ_name: str, rows: list[dict]) -> tuple[dict, list[dict], list[str]]:
    """(spec, archetypes that lose their PC physicsDictionary, archetypes whose box was reordered):
    the pack typ with the converted archetypes' definitions, physicsDictionary cleared."""
    archetypes, collision, reordered = [], [], []
    for row in rows:
        row = dict(row)
        lo, hi = bbox(row)
        if [lo, hi] != [list(map(float, row["bbMin"])), list(map(float, row["bbMax"]))]:
            reordered.append(row["name"])
            row["bbMin"], row["bbMax"] = lo, hi
        if hash_of(row.get("physicsDictionary")):
            collision.append({"archetype": row["name"], "pcPhysicsDictionary": row["physicsDictionary"]})
        row["physicsDictionary"] = ""
        row["extensions"] = []
        archetypes.append(row)
    spec = {"format": make_mlo_ptyp.FORMAT, "name": typ_name, "archetypes": archetypes}
    spec |= {"extensions": [], "dependencies": [], "compositeEntityTypes": []}
    return spec, collision, reordered


def fitted_row(row: dict, ydr: bytes, c: dict, fitted: list[dict]) -> dict:
    """--fit-bbox: the row with bbMin/bbMax = the high-LOD drawn geometry's box and the sphere around it
    (Map Builder's builderdef.ytyp gives its 7 m wall blocks a 2.4 m box and its 5.4 m tiles a 330 m one;
    the box sizes the map extents and the archetype's culling). A changed row is listed in `fitted`."""
    import drawable_collision  # convert_pc_bounds' encoder: only this option needs it

    triangles, _stats = drawable_collision.drawable_triangles(ydr, c, set())
    if not triangles:
        return row
    points = [v for tri in triangles for v in tri]
    lo = [min(v[k] for v in points) for k in range(3)]
    hi = [max(v[k] for v in points) for k in range(3)]
    if bbox(row) == (lo, hi):
        return row
    centre = [(a + b) / 2 for a, b in zip(lo, hi, strict=True)]
    radius = math.dist(lo, hi) / 2
    fitted.append({"archetype": row["name"], "typ": list(bbox(row)), "drawn": [lo, hi]})
    return row | {"bbMin": lo, "bbMax": hi, "bsCentre": centre, "bsRadius": radius}


def pack_archetypes(typ_name: str, rows: list[tuple[str, dict]], members: dict[str, str]) -> dict:
    """The tools/convert_ymap.py --pack-archetypes table: index-shaped rows for the pack's own archetypes."""
    archetypes, names = {}, {}
    for name, row in rows:
        lo, hi = bbox(row)
        key = f"0x{joaat(name):08x}"
        archetypes[key] = [[typ_name, float(row.get("lodDist") or 0.0), lo, hi, int(row.get("flags") or 0), "drawable"]]
        names[key] = name
    return {
        "schema": PACK_ARCHETYPES_SCHEMA,
        "typ": typ_name,
        "archetypes": archetypes,
        "names": names,
        "members": members,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mod", type=Path, help="the mod's PC archive (dlc.rpf) or unpacked folder")
    parser.add_argument("--report", type=Path, help="tools/convert_ymap.py report.json: its custom archetypes")
    parser.add_argument("--archetype", action="append", default=[], help="archetype to convert (repeatable)")
    parser.add_argument("--template", type=Path, required=True, help="retail PS5 drawable for the root words")
    parser.add_argument("--shader-template", type=Path, action="append", default=[], help="retail shader carriers")
    parser.add_argument("--typ-template", type=Path, required=True, help="retail .ptyp for the meta schema")
    parser.add_argument("--typ-name", required=True, help="name of the pack typ (member <name>.ptyp)")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--reference-dir", type=Path, help="accepted and not read (the layouts come from data/drawable_contracts)"
    )
    parser.add_argument("--templates", type=Path, help="texture template directory (convert_pc_ytd_writer)")
    parser.add_argument("--max-texture-size", type=int, default=1024)
    parser.add_argument(
        "--rename-txd", action="append", default=[], metavar="OLD=NEW", help="write texture dictionary OLD as NEW"
    )
    lights = parser.add_mutually_exclusive_group()
    lights.add_argument("--no-lights", action="store_true", help="drop every model's light attributes")
    lights.add_argument(
        "--lights-only",
        action="append",
        default=[],
        metavar="ARCHETYPE",
        help="write light attributes only for these archetypes (repeatable); the others drop theirs",
    )
    parser.add_argument("--max-lights", type=int, help="write at most the first N light attributes per model")
    parser.add_argument(
        "--fit-bbox",
        action="store_true",
        help="archetype box and sphere from the model's drawn geometry (mods whose .ytyp boxes are wrong)",
    )
    parser.add_argument(
        "--txd-alias",
        action="append",
        default=[],
        metavar="HASH=NAME",
        help="name an unresolved textureDictionary hash (read NAME.ytd, write NAME.ptd; repeatable)",
    )
    args = parser.parse_args(argv)
    try:
        aliases = parse_txd_aliases(args.txd_alias)
    except ValueError as error:
        raise SystemExit(f"convert_pc_map_models: {error}") from None
    if not _NAME.fullmatch(args.typ_name):
        raise SystemExit("--typ-name must be lowercase letters, digits and '_'")
    if args.out.exists() and any(args.out.iterdir()):
        raise SystemExit(f"refusing to write into the non-empty {args.out}")
    wanted = [a.lower() for a in args.archetype]
    if args.report:
        report_in = json.loads(args.report.read_text(encoding="utf-8"))
        wanted += [row["archetype"] for row in report_in.get("classification", {}).get("custom", [])]
    wanted = list(dict.fromkeys(w for w in wanted if _NAME.fullmatch(w)))
    if not wanted:
        raise SystemExit("no archetype to convert (--report / --archetype)")
    try:
        if args.mod.is_dir():
            members, notes = folder_members(args.mod)
        else:
            with args.mod.open("rb") as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as view:
                members, notes = mod_members(view)
        names = {joaat(k.rsplit(".", 1)[0]): k.rsplit(".", 1)[0] for k in members}
        names |= {joaat(w): w for w in wanted}
        definitions = typ_definitions(members, names)
        plan, refused = plan_archetypes(wanted, definitions, members)
        c = drawable.contracts(args.reference_dir)
        template = drawable.Template(args.template.read_bytes(), c, args.template.name)
        shader_templates = [drawable.Template(p.read_bytes(), c, p.name) for p in args.shader_template]
    except (AssetError, make_mlo_ptyp.SpecError, OSError, ValueError, KeyError, struct.error) as error:
        raise SystemExit(f"convert_pc_map_models: {error}") from None

    def txd_name(row: dict) -> str:
        return texture_dictionary_name(row, names, aliases)

    args.out.mkdir(parents=True, exist_ok=True)
    converted, groups, fitted = [], {}, []
    for name, typ_key, row in plan:
        try:
            keep = not args.no_lights and (not args.lights_only or name in {a.lower() for a in args.lights_only})
            blob, report = drawable.convert(
                members[f"{name}.ydr"],
                template,
                shader_templates,
                c,
                prop=name,
                lights=keep,
                max_lights=args.max_lights,
            )
        except (AssetError, OSError, ValueError, KeyError, struct.error) as error:
            refused.append({"archetype": name, "reason": f"drawable: {error}"})
            continue
        embedded = report.pop("_embedded", [])
        drawable.write_new(args.out / f"{name}.pdr", blob)
        drawable.write_new(args.out / f"{name}.pdr.report.json", drawable.canonical(report))
        txd = txd_name(row)
        group = groups.setdefault(txd, {"links": [], "embedded": [], "archetypes": []})
        group["links"].append(report["textureLinks"])
        group["embedded"].append(embedded)
        group["archetypes"].append(name)
        if args.fit_bbox:
            row = fitted_row(row, members[f"{name}.ydr"], c, fitted)
        converted.append((name, typ_key, row, report))
        if report.get("lights", {}).get("exceedsObjectLimit"):
            print(
                f"note: {name} has {report['lights']['count']} lights (> {drawable.OBJECT_LIGHT_LIMIT}): keep it a building"
            )
        print(
            f"pdr {name}: {len(blob)} B pages={report['pages']} geometries={len(report['geometries'])} "
            f"shaders={','.join(sorted({s['name'] for s in report['shaders']}))} "
            f"lights={report['lights']['count'] if 'lights' in report else 0} dropped={report['dropped'] or '-'}"
        )

    renames = dict(item.lower().split("=", 1) for item in args.rename_txd if "=" in item)
    if len(renames) != len(args.rename_txd) or not all(map(_NAME.fullmatch, [*renames, *renames.values()])):
        raise SystemExit("convert_pc_map_models: --rename-txd takes OLD=NEW texture dictionary names")
    dictionaries, ptd_members, failed, txd_of = {}, {}, set(), {}
    taken = {k.rsplit(".", 1)[0] for k in members} | set(groups)
    cap = convert_pc_ytd_writer.MAX_DICTIONARY_TEXTURES
    # An alias of a dictionary whose own name is not a usable member name (Map Builder: `<model>+hidr`) still
    # reads that mod .ytd: the one whose name hashes to the aliased value.
    alias_ytd = {name: f"{names[value]}.ytd" for value, name in aliases.items() if f"{names.get(value)}.ytd" in members}
    for txd, group in sorted(groups.items()):
        ytd_key = f"{txd}.ytd" if f"{txd}.ytd" in members else alias_ytd.get(txd, f"{txd}.ytd")
        if not _NAME.fullmatch(txd):
            refused += [
                {"archetype": a, "reason": f"texture dictionary {txd!r} has no usable name"}
                for a in group["archetypes"]
            ]
            failed.update(group["archetypes"])
            continue
        ytd, skipped = None, []
        for part, run in split_group(renames.get(txd, txd), group["archetypes"], group["links"], cap):
            archetypes = [group["archetypes"][i] for i in run]
            try:
                if part != txd and (part in taken or not _NAME.fullmatch(part)):
                    raise AssetError(f"split dictionary name {part} is taken or unusable")
                sources = [group["embedded"][i] for i in run]
                if ytd_key in members:
                    if ytd is None:
                        ytd, skipped = drawable.ytd_textures(members[ytd_key], args.max_texture_size, skip_bad=True)
                    sources.append(ytd)
                ptd, info = drawable.texture_dictionary(
                    [link for i in run for link in group["links"][i]],
                    sources,
                    args.templates,
                    args.max_texture_size,
                    only_linked=True,
                )
            except (AssetError, SystemExit, ValueError, KeyError) as error:
                refused += [{"archetype": a, "reason": f"textures ({part}): {error}"} for a in archetypes]
                failed.update(archetypes)
                continue
            drawable.write_new(args.out / f"{part}.ptd", ptd)
            ptd_members[f"{part}.ptd"] = f"{part}.ptd"
            dictionaries[part] = info | {
                "archetypes": archetypes,
                "fromYtd": ytd_key in members,
                "skippedYtdTextures": skipped,
                "sourceTxd": txd if part != txd else None,
            }
            txd_of.update(dict.fromkeys(archetypes, part))
            print(f"ptd {part}: {len(ptd)} B textures={len(info['textures'])} neutral={len(info['neutralTextures'])}")

    kept = [(name, typ_row(row, txd_of[name]), report) for name, _typ, row, report in converted if name not in failed]
    for name, _typ, _row, _report in converted:
        if name in failed:
            (args.out / f"{name}.pdr").unlink()
            (args.out / f"{name}.pdr.report.json").unlink()
    if not kept:
        raise SystemExit("convert_pc_map_models: nothing converted")
    spec, collision, reordered = pack_typ_spec(args.typ_name, [row for _n, row, _r in kept])
    try:
        typ = make_mlo_ptyp.build(spec, args.typ_template.read_bytes())
    except make_mlo_ptyp.SpecError as error:
        raise SystemExit(f"convert_pc_map_models: typ: {error}") from None
    drawable.write_new(args.out / f"{args.typ_name}.json", (json.dumps(spec, indent=1) + "\n").encode())
    drawable.write_new(args.out / f"{args.typ_name}.ptyp", typ)
    files = {f"{name}.pdr": f"{name}.pdr" for name, _row, _r in kept} | ptd_members
    files[f"{args.typ_name}.ptyp"] = f"{args.typ_name}.ptyp"
    table = pack_archetypes(args.typ_name, [(name, row) for name, row, _r in kept], files)
    drawable.write_new(args.out / "pack-archetypes.json", (json.dumps(table, indent=1) + "\n").encode())
    summary = {
        "kind": "gtavmenu-pc-map-models",
        "typ": {"name": args.typ_name, "archetypes": len(kept), "bytes": len(typ)},
        "converted": [
            {
                "archetype": name,
                "txd": txd_of[name],
                "geometries": len(report["geometries"]),
                "vertices": sum(g["vertexCount"] for g in report["geometries"]),
                "shaders": sorted({s["name"] for s in report["shaders"]}),
                "dropped": report["dropped"],
                "lights": report.get("lights", {}).get("count", 0),
                "skeleton": report["skeleton"],
            }
            for name, row, report in kept
        ],
        "refused": refused,
        "textureDictionaries": {
            k: {
                key: v[key]
                for key in (
                    "archetypes",
                    "fromYtd",
                    "skippedYtdTextures",
                    "nameConflicts",
                    "neutralTextures",
                    "bytes",
                    "sourceTxd",
                )
            }
            | {"textures": len(v["textures"])}
            for k, v in dictionaries.items()
        },
        "lights": sum(report.get("lights", {}).get("count", 0) for _n, _row, report in kept),
        "noCollision": collision,
        "bboxReordered": reordered,
        **({"bboxFitted": fitted} if args.fit_bbox else {}),
        "collisionFilesNotConverted": sorted(k for k in members if k.endswith(".ybn")),
        "txdAliases": {
            f"0x{value:08x}": {"name": name, "hashMatches": joaat(name) == value} for value, name in aliases.items()
        },
        "notes": notes,
    }
    drawable.write_new(args.out / "models-report.json", (json.dumps(summary, indent=1) + "\n").encode())
    print(
        f"{len(kept)} archetypes converted into {args.typ_name}.ptyp, {len(dictionaries)} texture dictionaries; "
        f"refused {len(refused)}; {len(collision)} lose their PC collision (physicsDictionary -> 0); "
        f"{len(summary['collisionFilesNotConverted'])} .ybn not converted"
    )
    for row in refused:
        print(f"refused: {row['archetype']}: {row['reason']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

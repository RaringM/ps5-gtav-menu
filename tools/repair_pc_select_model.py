#!/usr/bin/env python3
"""Cut a multi-vehicle add-on's data files down to the one converted model.

Add-on packs often hold several cars in one dlc.rpf with shared data files: vans123 (LaFerrari) has
aventador, laferrari and raptor2017; the Gta5KoRn pack has 48. The importer keeps those files
verbatim, and convert_vehicle.py's metas step ships them as they are, so a pack converting one model
would declare every other car (model infos without a drawable), every handling row, and every kit:
prepare_carcols.py gives each kit the one --kit-id (the first kit in the file wins, the converted
car's own kit may be dropped) and keeps only the first Lights item (which may belong to another car).

This tool writes a new fixture whose metadata/ files (as the mod's content.xml places them) hold only
what the model uses; every other file and every resource stay byte-identical:
  - vehicles.meta: the <InitDatas> item whose modelName is the model (case-insensitive: model names
    are joaat keys) and the <txdRelationships> items on its texture dictionary's parent chain;
  - handling.meta: the <HandlingData> item named by the vehicle's handlingId; removed when the file has
    none (a retail handlingId such as POLICE: the car keeps the game's own handling, no HANDLING row);
  - carvariations.meta: the model's <variationData> item;
  - carcols.meta: the <Kits> items the variation names, the <Lights>/<Sirens> items whose id the
    variation's lightSettings/sirenSettings name, and no <Wheels> (an add-on rim list for the LSC
    wheel menu, not part of any one car; the worker refuses carcols wheels, custom_pack_data.inc);
    when none of the model's kits, lights or sirens is in the file (a retail 0_default_modkit and
    retail light/siren ids), the file is removed: convert_vehicle.py then skips the content.xml row
    with a note and the pack has no CARCOLS row (no --kit-id/--light-id needed);
  - vehiclelayouts.meta: removed when the model's vehicles.meta item names none of its definitions
    (the Gta5KoRn gmt400 uses the retail LAYOUT_RANGER), else unchanged (references chain inside the
    file; prepare_vehicle_layouts.py renames every mod definition per model);
  - other files: unchanged.
Kept items are byte-identical, with the whitespace before each of them. Refused when the model is not
declared exactly once or when there is nothing to remove (a single-car pack needs no selection).

  repair_pc_select_model.py --fixture build/assets/convert-<id>/fixtures/laferrari-import \\
      --material-report <its material report> --output <new fixture>   (convert_vehicle.py --repair select-model)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import xml.dom.minidom
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from gtavmenu_tools.vehicle_repair_inputs import FixtureError, copy_fixture, validate_fixture  # noqa: E402

ITEM_TAG = re.compile(r"<Item\b[^>]*?/>|<Item\b[^>]*>|</Item>")


class SelectError(RuntimeError):
    pass


def element(text: str, tag: str) -> re.Match | None:
    """The first <tag>...</tag> element (or <tag/>), not nested in itself."""
    return re.search(rf"<{tag}\s*/>|<{tag}\b[^>]*>(.*?)</{tag}>", text, re.S)


def top_items(body: str) -> list[tuple[int, int]]:
    """Top-level <Item> spans of an array body (nesting-aware; a self-closing <Item/> counts)."""
    spans, depth, start = [], 0, 0
    for match in ITEM_TAG.finditer(body):
        tag = match.group(0)
        if tag.endswith("/>"):
            if depth == 0:
                spans.append((match.start(), match.end()))
        elif tag.startswith("</"):
            depth -= 1
            if depth < 0:
                raise SelectError("unbalanced </Item>")
            if depth == 0:
                spans.append((start, match.end()))
        else:
            if depth == 0:
                start = match.start()
            depth += 1
    if depth:
        raise SelectError("unbalanced <Item>")
    return spans


def field(item: str, tag: str) -> str | None:
    found = re.search(rf"<{tag}>\s*([^<]*?)\s*</{tag}>", item)
    return found.group(1) if found else None


def value(item: str, tag: str) -> str | None:
    found = re.search(rf'<{tag}\s+value="([^"]*)"', item)
    return found.group(1) if found else None


def keep_items(text: str, tag: str, keep) -> tuple[str, int, int]:
    """Text with only the top-level items of <tag> for which keep(item) is true; (text, kept, dropped)."""
    found = element(text, tag)
    if found is None or found.group(1) is None:
        return text, 0, 0
    body = found.group(1)
    spans = top_items(body)
    kept, out, last = 0, [], 0
    for start, end in spans:
        if keep(body[start:end]):
            out.append(body[last:end])  # the item with the whitespace before it
            kept += 1
        last = end
    if not spans:
        return text, 0, 0
    new_body = "".join(out) + body[spans[-1][1] :] if out else body[: spans[0][0]] + body[spans[-1][1] :].lstrip(" \t")
    start, end = found.span(1)
    return text[:start] + new_body + text[end:], kept, len(spans) - kept


def drop_element(text: str, tag: str) -> tuple[str, bool]:
    """Text without the <tag> element (and its line), or unchanged."""
    found = re.search(rf"[ \t]*(<{tag}\s*/>|<{tag}\b[^>]*>.*?</{tag}>)[ \t]*\n?", text, re.S)
    if found is None:
        return text, False
    return text[: found.start()] + text[found.end() :], True


def fold(name: str | None) -> str:
    return (name or "").strip().casefold()


def select(model: str, metas: dict[str, str]) -> tuple[dict[str, str], dict]:
    """The model's records of each meta text (keys: vehicles, handling, carvariations_kit, carcols, layouts)."""
    out, report = dict(metas), {"model": model}
    vehicles = metas["vehicles"]
    found = element(vehicles, "InitDatas")
    items = [vehicles[found.start(1) + s : found.start(1) + e] for s, e in top_items(found.group(1))] if found else []
    mine = [item for item in items if fold(field(item, "modelName")) == model]
    if len(mine) != 1:
        raise SelectError(f"vehicles.meta declares {model} {len(mine)} times (of {len(items)} vehicles)")
    if len(items) == 1:
        raise SelectError(f"vehicles.meta declares only {model}: nothing to select")
    vehicle = mine[0]
    handling_id, txd = fold(field(vehicle, "handlingId")), fold(field(vehicle, "txdName"))
    text, kept, dropped = keep_items(vehicles, "InitDatas", lambda item: fold(field(item, "modelName")) == model)
    report["vehicles"] = {"kept": kept, "dropped": dropped}
    relations = element(text, "txdRelationships")
    chain = {txd}
    if relations is not None and relations.group(1) is not None:
        pairs = [
            (fold(field(relations.group(1)[s:e], "parent")), fold(field(relations.group(1)[s:e], "child")))
            for s, e in top_items(relations.group(1))
        ]
        grew = True
        while grew:  # parents of the model's dictionary, and their parents (mod-defined intermediates)
            grew = False
            for parent, child in pairs:
                if child in chain and parent not in chain:
                    chain.add(parent)
                    grew = True
        text, kept, dropped = keep_items(text, "txdRelationships", lambda item: fold(field(item, "child")) in chain)
        report["txdRelationships"] = {"kept": kept, "dropped": dropped}
    out["vehicles"] = text

    if "handling" in metas:
        text, kept, dropped = keep_items(
            metas["handling"], "HandlingData", lambda item: fold(field(item, "handlingName")) == handling_id
        )
        if kept > 1:
            raise SelectError(f"handling.meta has {kept} items named {handling_id!r} (the vehicle's handlingId)")
        # None (the Rockport Police rpdcar1 drives the retail POLICE handling): the file goes, like an emptied
        # carcols, and the pack has no HANDLING row.
        out["handling"] = text if kept else None
        report["handling"] = {"kept": kept, "dropped": dropped, "removed": not kept}

    text, kept, dropped = keep_items(
        metas["carvariations_kit"], "variationData", lambda item: fold(field(item, "modelName")) == model
    )
    if kept != 1:
        raise SelectError(f"carvariations.meta has {kept} variation items for {model}")
    out["carvariations_kit"], report["carvariations"] = text, {"kept": kept, "dropped": dropped}
    variation = next(
        text[found.start(1) + s : found.start(1) + e]
        for found in [element(text, "variationData")]
        for s, e in top_items(found.group(1))
    )
    kits_list = element(variation, "kits")
    body = kits_list.group(1) if kits_list is not None else None
    kits = {fold(item_text(body[s:e])) for s, e in top_items(body)} if body else set()
    light, siren = value(variation, "lightSettings"), value(variation, "sirenSettings")

    if "carcols" in metas:
        text, kept, dropped = keep_items(metas["carcols"], "Kits", lambda item: fold(field(item, "kitName")) in kits)
        report["kits"] = {"kept": kept, "dropped": dropped}
        text, kept, dropped = keep_items(text, "Lights", lambda item: value(item, "id") == light)
        report["lights"] = {"kept": kept, "dropped": dropped}
        text, kept, dropped = keep_items(text, "Sirens", lambda item: value(item, "id") == siren)
        report["sirens"] = {"kept": kept, "dropped": dropped}
        text, wheels = drop_element(text, "Wheels")
        report["wheelsDropped"] = wheels
        # Nothing of the model's left (retail kit such as 0_default_modkit, retail light/siren ids): the
        # file goes, so the pack has no CARCOLS row and needs no --kit-id/--light-id.
        empty = not any(report[key]["kept"] for key in ("kits", "lights", "sirens"))
        out["carcols"] = None if empty else text
        report["carcolsRemoved"] = empty
    if "layouts" in metas:
        # Mod layout definitions (<Name>) the model's vehicles.meta item names; none (the Gta5KoRn gmt400
        # drives the retail LAYOUT_RANGER) -> the file goes: its 72 renamed definitions would only be
        # merged into the game's arrays for nothing. Otherwise the file stays whole (refs chain inside it).
        defined = {fold(name) for name in re.findall(r"<Name>\s*([^<\s]+)\s*</Name>", metas["layouts"])}
        texts = re.findall(r'>\s*([^<>\s]+)\s*<|value="([^"]+)"', vehicle)  # element texts, value attributes
        named = {fold(text or attribute) for text, attribute in texts}
        report["layoutsUsed"] = sorted(defined & named)
        if not report["layoutsUsed"]:
            out["layouts"] = None
    for text in out.values():
        if text is not None:
            xml.dom.minidom.parseString(text.encode("utf-8"))  # every meta we write must stay XML
    return out, report


def item_text(item: str) -> str:
    """NAME of a <Item>NAME</Item> list entry ("" for anything else)."""
    found = re.fullmatch(r"<Item\b[^>]*>\s*([^<]*?)\s*</Item>", item.strip(), re.S)
    return found.group(1) if found else ""


def meta_paths(fixture: Path) -> dict[str, Path]:
    """The mod's data files by convert_vehicle DATA_ORDER key, relative to the fixture."""
    import convert_vehicle

    found, _ = convert_vehicle.mod_metas(fixture)
    base = (fixture / "metadata").resolve()
    return {key: Path("metadata") / path.resolve().relative_to(base) for key, path in found.items()}


def main(argv: list[str] | None = None) -> int:
    import convert_vehicle

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--material-report", type=Path, required=True, help="recorded (convert_vehicle repair chain)")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", help="model name (default: the fixture manifest's model)")
    args = parser.parse_args(argv)
    if args.output.exists() or args.output.is_symlink():
        raise SystemExit(f"refusing to overwrite {args.output}")
    manifest = validate_fixture(args.fixture)
    model = fold(args.model or manifest.get("model"))
    if not model:
        raise SystemExit("no --model and no model in the fixture manifest")
    paths = meta_paths(args.fixture)
    missing = {"vehicles", "carvariations_kit"} - paths.keys()
    if missing:
        raise SystemExit(f"fixture has no {', '.join(sorted(missing))} meta")
    metas = {key: convert_vehicle.read_meta(args.fixture / path) for key, path in paths.items()}
    try:
        selected, report = select(model, metas)
    except SelectError as error:
        raise SystemExit(f"select-model: {error}") from None
    copy_fixture(args.fixture, args.output, manifest)
    files = []
    for key, path in paths.items():
        if selected[key] == metas[key]:
            continue
        if selected[key] is None:
            (args.output / path).unlink()
        else:
            (args.output / path).write_text(selected[key], encoding="utf-8", newline="\n")
        files.append(str(path))
    manifest["modelSelection"] = {
        "tool": "tools/repair_pc_select_model.py",
        "reason": "multi-vehicle add-on: data files cut down to the converted model's records",
        "sourceFixtureManifestSha256": hashlib.sha256((args.fixture / "manifest.json").read_bytes()).hexdigest(),
        "materialReportSha256": hashlib.sha256(args.material_report.read_bytes()).hexdigest(),
        "files": files,
        **report,
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"output": str(args.output), **report, "files": files}))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FixtureError as error:
        raise SystemExit(f"fixture refused: {error}") from None

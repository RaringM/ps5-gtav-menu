#!/usr/bin/env python3
"""Write a one-ped PED_METADATA_FILE (peds.meta) for a new pack ped, cloned from a donor ped.

The input is a CPedModelInfo__InitDataList XML you supply (for example the InitData list of your own
game's peds.meta, or a peds.meta a ped mod ships). The donor's InitData (--like NAME, case-insensitive;
default: the only one in the file) is copied under the new ped's model name, so the new ped gets the
donor's ped type, behaviour, voice and clip sets. PropsName becomes null (the pack ships no props
dictionary) or, with --props NAME, the name of the props dictionary the pack ships (<name>_p.pdd and
<name>_p.ptd: the game finds them by that name). The output holds that one InitData and an empty
txdRelationships list. Refuses to overwrite.

The target game's ps5/data/peds.pmt is binary PSO. Exporting that file does not make it XML:
PSO-to-XML donor conversion is not implemented here. Per-ped .pmt/.ymt resources contain variation
data, not InitData, and cannot supply behaviour, voices or clip sets. Supply actual InitData XML.

  make_ped_initdata.py donor_peds.meta --name gm_goose_01 [--like S_M_Y_Cop_01] [--props gm_goose_01_p] \
      --output peds_goose.meta
"""

from __future__ import annotations

import argparse
import copy
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

MAX_INPUT_BYTES = 64 * 1024 * 1024
_NAME = re.compile(r"[a-z0-9_]{1,63}")


class InitDataError(ValueError):
    pass


def clone(text: bytes, name: str, like: str | None, props: str | None = None) -> str:
    if not _NAME.fullmatch(name):
        raise InitDataError(f"ped name {name!r} must be lower-case [a-z0-9_] (at most 63)")
    if props is not None and not _NAME.fullmatch(props):
        raise InitDataError(f"props name {props!r} must be lower-case [a-z0-9_] (at most 63)")
    if text.startswith(b"PSIN"):
        raise InitDataError(
            "binary PSO InitData is not XML; PSO-to-XML donor conversion is not implemented. "
            "Exporting ps5/data/peds.pmt alone is insufficient: supply a CPedModelInfo__InitDataList XML "
            "converted from your game or supplied by your ped mod"
        )
    if text.startswith(b"RSC7"):
        raise InitDataError(
            "RSC7 is a resource, not InitData XML; per-ped .pmt/.ymt files describe variations, not behaviour, "
            "voices or clip sets. Supply a CPedModelInfo__InitDataList XML"
        )
    if b"<!DOCTYPE" in text or b"<!ENTITY" in text:
        raise InitDataError("the InitData file declares a DTD or entities; refusing it")
    try:
        root = ET.fromstring(text)
    except ET.ParseError as error:
        raise InitDataError(f"not well-formed XML: {error}") from None
    datas = root.find("InitDatas")
    if root.tag != "CPedModelInfo__InitDataList" or datas is None:
        raise InitDataError("not a CPedModelInfo__InitDataList with InitDatas")
    items = [item for item in datas if item.tag == "Item"]
    if like is None:
        if len(items) != 1:
            raise InitDataError(f"the file has {len(items)} InitDatas; name the donor with --like")
        found = items
    else:
        found = [item for item in items if (item.findtext("Name") or "").strip().lower() == like.lower()]
        if len(found) != 1:
            raise InitDataError(f"expected one InitData named {like}, found {len(found)}")
    item = copy.deepcopy(found[0])
    for tag, value in (("Name", name), ("PropsName", props or "null")):
        element = item.find(tag)
        if element is None:
            raise InitDataError(f"the donor InitData has no <{tag}>")
        element.text = value
    out = ET.Element("CPedModelInfo__InitDataList")
    ET.SubElement(out, "InitDatas").append(item)
    ET.SubElement(out, "txdRelationships")
    for node in out.iter():
        if node.text is not None and not node.text.strip():
            node.text = None
        node.tail = None
    ET.indent(out, space="  ")
    return '<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(out, encoding="unicode") + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("initdata", type=Path, help="a CPedModelInfo__InitDataList XML holding the donor ped")
    parser.add_argument("--name", required=True, help="the new ped's model name (lower case)")
    parser.add_argument("--like", help="the donor InitData's Name (default: the file's only InitData)")
    parser.add_argument("--props", help="the props dictionary the pack ships (default: none, PropsName null)")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.output.exists():
        print(f"make_ped_initdata: refusing to overwrite {args.output}", file=sys.stderr)
        return 1
    try:
        if args.initdata.stat().st_size > MAX_INPUT_BYTES:
            raise InitDataError(f"{args.initdata.name} is larger than {MAX_INPUT_BYTES} bytes")
        text = clone(args.initdata.read_bytes(), args.name, args.like, args.props)
    except (InitDataError, OSError) as error:
        print(f"make_ped_initdata: {error}", file=sys.stderr)
        return 1
    args.output.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {args.output} ({args.name} like {args.like or 'the only InitData'}, {len(text)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

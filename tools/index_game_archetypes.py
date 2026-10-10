#!/usr/bin/env python3
"""Build the archetype index of your own game (menu-ctl.sh index-archetypes), with no game keys.

convert-map, convert-mapmod and convert-mlo classify a mod's entities with it: which archetypes are stock,
their typ (for the pack's `typ <name>.ptyp retail` rows), bounding box and lodDist. Every retail .ptyp
member of the target's game build is pinned in data/archetype_index/<target>.json (archive, member,
stored offset and size, sha256; plus the archetype names), so each typ is read by one range read, checked
against its sha256 and parsed here. Archive tables stay encrypted and unread.

  index_game_archetypes.py --source ftp://PS5:2121/mnt/sandbox/PPSA04264_000/app0
  index_game_archetypes.py --source file:///path/to/app0 [--output FILE] [--force]

The console's game image is mounted only while GTA V runs. The default output is
build/archetype-index/<target>.json, where the convert-* commands look for it. A rerun with the same typ
source list keeps a finished index (--force rebuilds it). Exit status 0 when the index is written or up
to date, 1 on any failure (nothing is written then).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

from gtavmenu_tools import archetype_index as ai  # noqa: E402
from gtavmenu_tools import retail_templates as rt  # noqa: E402
from gtavmenu_tools.host_paths import build_dir  # noqa: E402

ROOT = _HERE.parent


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument(
        "--source", required=True, help="ftp://HOST:PORT/mnt/sandbox/<TITLE>_000/app0 or file:///path/to/app0"
    )
    p.add_argument("--target", default=rt.DEFAULT_TARGET, help=f"game build (default {rt.DEFAULT_TARGET})")
    p.add_argument(
        "--sources",
        type=Path,
        help="typ source list (default: target list; PPSA04263 01.010.002 checks PPSA04264 01.010.002 pins)",
    )
    p.add_argument("--output", type=Path, help="index file (default build/archetype-index/<target>.json)")
    p.add_argument("--force", action="store_true", help="rebuild even when the index is up to date")
    return p


def up_to_date(path: Path, digest: str) -> bool:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return (
        isinstance(doc, dict) and doc.get("schema") == ai.SCHEMA and doc.get("typSources", {}).get("sha256") == digest
    )


def write_atomic(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(text)
        os.replace(temp, path)
    except BaseException:
        Path(temp).unlink(missing_ok=True)
        raise


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    sources = args.sources or ai.sources_path(args.target)
    output = args.output or build_dir(ROOT) / "archetype-index" / f"{args.target}.json"
    try:
        if not sources.is_file():
            raise ai.ArchetypeIndexError(f"no typ source list for {args.target} ({sources})")
        doc, rows, names, digest = ai.load_sources(sources)
        if not args.sources and rt.pin_target(args.target) != args.target:
            print(f"{args.target}: checking source against {doc.get('target')} pins ({sources})")
        if not args.force and up_to_date(output, digest):
            print(f"{output}: up to date ({len(rows)} typs of {doc.get('target')}); source not reread")
            return 0
        source = rt.open_source(args.source)
        for archive in dict.fromkeys(r.archive for r in rows):
            want = next(r.archive_bytes for r in rows if r.archive == archive)
            have = source.size(archive)
            if have is None:
                raise ai.ArchetypeIndexError(f"{archive} not found in {args.source} (is GTA V running?)")
            if have != want:
                raise ai.ArchetypeIndexError(
                    f"{archive} is {have} bytes, the typ source list pins {want} (a different game build?)"
                )

        def progress(done: int, total: int) -> None:
            if done == total or done % 100 == 0:
                print(f"read {done}/{total}", flush=True)

        parsed = ai.read_typs(rows, source.read, progress=progress)
        index = ai.build(
            rows,
            names,
            parsed,
            f"{doc.get('target')} game image ({args.source})",
            typSources={"file": sources.name, "sha256": digest},
            problems=[],
        )
        if not args.sources and rt.pin_target(args.target) != args.target:
            index["selectedTarget"] = args.target
            index["source"] = f"{doc.get('target')} pins from {args.source} (selected target {args.target})"
        write_atomic(output, ai.dump(index))
    except (ai.ArchetypeIndexError, rt.TemplateError, OSError) as error:
        print(f"index_game_archetypes: {error}", file=sys.stderr)
        return 1
    print(
        f"wrote {output}: {len(index['typs'])} typs, {len(index['archetypes'])} archetypes "
        f"({len(index['names'])} named) from {len(rows)} typ members"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

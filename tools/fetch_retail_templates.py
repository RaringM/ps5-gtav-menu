#!/usr/bin/env python3
"""Fetch the retail templates the PC converters need from your own game image (vehicles; menu-ctl.sh
convert-model/-mlo/-ped; convert-weapon: a drawable of every weapon class with its _hi and magazine models).

Every template in data/retail_templates/<target>.json is read from the game files by one range
read at its pinned offset, rewrapped and checked against its pinned sha256, then written to
<cache>/<name> (the layout the vehicle converter's --templates option reads). No game keys are
read: the pinned offsets skip the encrypted archive tables. Re-running is cheap: verified cache files
are kept, so a second run transfers nothing.

  fetch_retail_templates.py --source ftp://PS5:2121/mnt/sandbox/PPSA04264_000/app0 --cache DIR
  fetch_retail_templates.py --source file:///path/to/app0 --cache DIR
  fetch_retail_templates.py --exports ftp://PS5:2121/data/gtavmenu/custom/exports --cache DIR
  fetch_retail_templates.py --cache DIR --check          (verify the cache only, no source)
  fetch_retail_templates.py --list | --exportable

The console's game image is mounted only while the game runs: start GTA V, then fetch over FTP.
Encrypted members (retail .meta files: data/ai/vehiclelayouts.meta from update.rpf, the copy the
game loads, and common.rpf's copy it overrides) need the game's keys, so they are not read from the
game image: the menu exports them instead (`./menu-ctl.sh export-templates` runs its
EXPORT_RETAIL_FILE action, which reads the file through the running game and writes the plaintext
to <game custom root>/exports/<cache file name>), and --exports reads that directory; without it
they are skipped. The converter needs update.rpf's copy only for a mod that ships its own
vehiclelayouts.meta. update.rpf's ps5/audio/config/game.dat151.rel (the base audio game data) is exported the
same way: convert-vehicle checks --audio against its car sounds and clones one for --audio-engine (optional).
Use --refresh-exports to verify those actual files again even when already cached (menu-ctl does this).
Exit status: 0 when every required template (and every --only one) is in the cache, 1 otherwise.
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Callable
from pathlib import Path

from gtavmenu_tools import retail_templates as rt

DEFAULT_CACHE = Path(__file__).resolve().parents[1] / "build/retail-templates"


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--source", help="ftp://HOST:PORT/mnt/sandbox/<TITLE>_000/app0 or file:///path/to/app0")
    p.add_argument(
        "--exports",
        help=f"the menu's export directory for encrypted members: ftp://HOST:PORT{rt.EXPORT_ROOT} or file:///DIR",
    )
    p.add_argument("--cache", type=Path, default=DEFAULT_CACHE, help=f"template cache (default {DEFAULT_CACHE})")
    p.add_argument("--manifest", type=Path, help=f"template manifest (default {rt.manifest_path()})")
    p.add_argument(
        "--only", action="append", default=[], metavar="NAME", help="fetch only these cache names (each is required)"
    )
    p.add_argument("--check", action="store_true", help="verify the cache only; read no source")
    p.add_argument(
        "--refresh-exports", action="store_true", help="verify exported bytes again even when already cached"
    )
    p.add_argument("--list", action="store_true", help="print the manifest and exit")
    p.add_argument(
        "--exportable",
        action="store_true",
        help="print 'ROW CACHE EXPORT BYTES' per template the menu exports (EXPORT_RETAIL_FILE param = ROW)",
    )
    return p


def main(argv: list[str] | None = None, *, member_cipher: Callable[[], rt.MemberCipher] | None = None) -> int:
    """`member_cipher` is a developer hook (never set by this tool) for encrypted members."""
    args = parser().parse_args(argv)
    try:
        if args.refresh_exports and (not args.exports or args.check or args.list or args.exportable):
            raise rt.TemplateError("--refresh-exports requires --exports and cannot be combined with check/list")
        doc, templates = rt.load_manifest(args.manifest)
        if args.only:
            unknown = sorted(set(args.only) - {t.cache for t in templates})
            if unknown:
                raise rt.TemplateError(f"unknown template(s): {', '.join(unknown)}")
            templates = [t for t in templates if t.cache in args.only]
        if args.list:
            for t in templates:
                keyed = " (encrypted: from the menu export or keys)" if t.needs_keys else ""
                print(f"{t.cache}  {t.size} B  {t.archive}:{t.member}  [{t.required}]{keyed}")
            return 0
        if args.exportable:
            _, every = rt.load_manifest(args.manifest)
            for row, t in enumerate(rt.exportable(every)):
                if not args.only or t.cache in args.only:
                    print(f"{row} {t.cache} {t.export_name} {t.size}")
            return 0
        if not args.check and not args.source and not args.exports:
            raise rt.TemplateError("--source or --exports is required (or --check to verify the cache only)")
        source = None if args.check or not args.source else rt.open_source(args.source)
        exports = None if args.check or not args.exports else rt.open_exports(args.exports)
        cipher = member_cipher() if member_cipher is not None and not args.check else None
        outcomes = rt.fetch(
            templates, source, args.cache, cipher, exports=exports, refresh_exports=args.refresh_exports
        )
    except (rt.TemplateError, OSError) as error:
        print(f"fetch_retail_templates: {error}", file=sys.stderr)
        return 1
    bad = False
    for outcome in outcomes:
        t = outcome.template
        line = f"{outcome.status:8} {t.cache}" + (f": {outcome.detail}" if outcome.detail else "")
        if outcome.status in ("failed", "missing", "skipped") and (t.required == "always" or t.cache in args.only):
            bad = True
        print(line, file=sys.stderr if outcome.status == "failed" else sys.stdout)
    have = sum(o.status in ("cached", "fetched") for o in outcomes)
    print(f"{have}/{len(outcomes)} templates verified in {args.cache} ({doc['target']})")
    if bad:
        print(
            "fetch_retail_templates: required templates are missing; with the console, start GTA V first "
            "(the game image is mounted only while it runs)",
            file=sys.stderr,
        )
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())

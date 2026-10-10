#!/usr/bin/env python3
"""Write an audio game-data chunk (AUDIO_GAMEDATA pack row) or inspect one.

  make_audio_gamedata.py car --source game.dat151.rel [--source dlc_game.dat151.rel ...] \\
      --donor DOMINATOR --name GMAUD_RAZOR [--engine-from SANCHEZ] --output gmaudrazor_game.rel
  make_audio_gamedata.py info FILE.rel [--class 3]
  make_audio_gamedata.py check FILE.rel

`car` clones the CarAudioSettings object `--donor` from the source chunks (searched last to first,
as the game looks objects up newest chunk first) under the new name `--name`. With `--engine-from`
its Engine and GranularEngine references are replaced by another vehicle's, so the car keeps the
donor's horn, doors and tuning but sounds like the other engine. A pack vehicle uses it with
`<audioNameHash>NAME</audioNameHash>` in its vehicles.meta (list the AUDIO_GAMEDATA row after that
file to get the worker's model line). The output name must be <chunk>_game.rel: the chunk name
([a-z0-9], at most 31 characters) must not be a chunk the game already has.

Sources are the user's own decrypted game files (e.g. update.rpf ps5/audio/config/game.dat151.rel);
nothing from them is written except the cloned object. `check` applies the worker gate's rules
(gtavmenu_tools.audio_rel.check); `info` lists objects (class, name hash, length).
"""

from __future__ import annotations

import argparse
import re
import sys
from collections.abc import Sequence
from pathlib import Path

from gtavmenu_tools import audio_rel, runtime_pack

_OUTPUT = re.compile(rf"[a-z0-9]{{1,{runtime_pack.AUDIO_CHUNK_MAX}}}_game\.rel")


def _load(path: Path) -> audio_rel.RelChunk:
    try:
        return audio_rel.parse(path.read_bytes())
    except audio_rel.RelError as exc:
        raise SystemExit(f"{path}: {exc}") from None


def _find(sources: list[audio_rel.RelChunk], name: str) -> tuple[audio_rel.RelChunk, int]:
    h = audio_rel.name_hash(name)
    for chunk in reversed(sources):
        if chunk.find(h) is not None:
            return chunk, h
    raise SystemExit(f"{name} ({h:#010x}) is in none of the source chunks")


def car(args: argparse.Namespace) -> int:
    if not _OUTPUT.fullmatch(args.output.name):
        raise SystemExit(f"output must be named <chunk>_game.rel (chunk [a-z0-9]{{1,31}}), not {args.output.name}")
    sources = [_load(p) for p in args.source]
    source, _ = _find(sources, args.donor)
    engines = None
    if args.engine_from:
        engine_source, engine_car = _find(sources, args.engine_from)
        engines = audio_rel.car_engines(engine_source, engine_car)
    new = audio_rel.name_hash(args.name)
    for chunk in sources:
        if chunk.find(new) is not None:
            print(
                f"warning: {args.name} already exists in a source chunk; the pack chunk overrides it", file=sys.stderr
            )
    try:
        body = audio_rel.clone_car(source, args.donor, engines)
        chunk = audio_rel.new_chunk(source.data[:4], [(new, body)])
    except audio_rel.RelError as exc:
        raise SystemExit(str(exc)) from None
    blob = audio_rel.build(chunk)
    if audio_rel.build(audio_rel.parse(blob)) != blob:
        raise SystemExit("internal error: chunk does not round-trip")
    args.output.write_bytes(blob)
    engine, granular = audio_rel.car_engines(chunk, new)
    print(
        f"{args.output}: {len(blob)} bytes, chunk {args.output.name.split('_', 1)[0]}, CarAudioSettings "
        f"{args.name} ({new:#010x}) from {args.donor}, engine {engine:#010x} granular {granular:#010x}"
    )
    return 0


def info(args: argparse.Namespace) -> int:
    chunk = _load(args.file)
    print(f"type {chunk.type}, data {len(chunk.data)} bytes, {len(chunk.index)} objects, "
          f"{len(chunk.hash_offsets)} object refs, {len(chunk.pack_offsets)} bank refs, {len(chunk.name_offsets)} names")  # fmt: skip
    for h, offset, length in chunk.index:
        cls = chunk.data[offset]
        if args.cls is not None and cls != args.cls:
            continue
        extra = ""
        if cls == audio_rel.CAR and length >= audio_rel.CAR_MIN_LENGTH:
            engine, granular = audio_rel.car_engines(chunk, h)
            extra = f" engine {engine:#010x} granular {granular:#010x}"
        print(f"  {h:#010x} class {cls:3d} offset {offset:#x} length {length}{extra}")
    return 0


def check(args: argparse.Namespace) -> int:
    chunk = _load(args.file)
    try:
        audio_rel.check(chunk)
    except audio_rel.RelError as exc:
        print(f"{args.file}: REFUSED: {exc}", file=sys.stderr)
        return 1
    print(f"{args.file}: OK ({len(chunk.index)} objects)")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("car", help="clone a CarAudioSettings under a new name")
    p.add_argument("--source", type=Path, action="append", required=True, help="game-data chunk (.rel), repeatable")
    p.add_argument("--donor", required=True, help="CarAudioSettings to clone, e.g. DOMINATOR")
    p.add_argument("--name", required=True, help="new object name (the vehicles.meta audioNameHash)")
    p.add_argument("--engine-from", help="take Engine + GranularEngine from this CarAudioSettings")
    p.add_argument("--output", type=Path, required=True, help="<chunk>_game.rel")
    p.set_defaults(func=car)
    p = sub.add_parser("info", help="list a chunk's objects")
    p.add_argument("file", type=Path)
    p.add_argument("--class", dest="cls", type=int)
    p.set_defaults(func=info)
    p = sub.add_parser("check", help="apply the worker gate's rules")
    p.add_argument("file", type=Path)
    p.set_defaults(func=check)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())

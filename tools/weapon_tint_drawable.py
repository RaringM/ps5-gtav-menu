#!/usr/bin/env python3
"""Weapon route of convert_pc_drawable.py with the retail tint mechanism (weapon-conversion.md "Weapon tints").

Retail PS5 weapons draw their body with weapon_normal_spec_detail_palette (the knife: weapon_normal_spec_cutout_palette):
the diffuse texture is a bright grey detail map whose alpha picks a column of the DiffuseTexPal palette texture
(`<model>_dpal`, BGRA8 128x32 in the weapon's .ptd) and the engine's weapon shader effect writes the tint index into
DiffuseTexPaletteSelector, which picks the palette row. A PC weapon mod usually draws its body with normal_spec or
default, which read no palette, so Weapon Tint changes nothing on the converted model (session 3, S1).

This wrapper runs convert_pc_drawable.main with every opaque body shader of the source (TINTABLE) retargeted to the
palette shader (--shader-substitute FROM=weapon_normal_spec_detail_palette) when one of the shader templates carries
it. The converter itself binds the new shader's empty DiffuseTexPal to TINT_PALETTE (convert_pc_drawable
NEUTRAL_TEXTURES["DiffuseTexPal"] and neutral_texture_name: a 16x16 white stand-in in the .ptd, like its other
neutral textures), so this wrapper only chooses the substitutes. make_weapon_model_pack.py --tint-palette then turns
that stand-in into the 128x32 tint palette and brightens the tinted diffuse textures (tools/make_weapon_model_pack.py,
tint_palette_dictionary). Without a carrier, or with no tintable source shader, the conversion runs unchanged and one
`tint: none (...)` line says why.

  weapon_tint_drawable.py [--no-tints] <convert_pc_drawable.py arguments>

The plan is added to the converter's report as "weaponTint" ({"line", "palette", "substitutes"}).
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

import convert_pc_drawable as cpd  # noqa: E402
from gtavmenu_tools.asset_formats import AssetError, Limits  # noqa: E402

PALETTE_SHADER = "weapon_normal_spec_detail_palette"  # every retail gun body (w_sb_smg, w_pi_*, w_ar_*, ...)
TINT_PALETTE = cpd.WEAPON_TINT_PALETTE  # the .ptd member DiffuseTexPal reads (make_weapon_model_pack.py writes it)
# Opaque, lit body shaders a palette fits (diffuse + optional bump/spec/detail). Alpha, cutout, decal, glass,
# emissive and already-tinted shaders keep their own shader (a decal or a lens must not take the gun's tint).
TINTABLE = frozenset({"default", "normal", "spec", "normal_spec", "normal_detail", "normal_spec_detail", "spec_const"})
_VALUE_OPTIONS = {
    "--source",
    "--template",
    "--shader-template",
    "--output",
    "--reference-dir",
    "--ytd",
    "--ptd-output",
    "--max-texture-size",
    "--templates",
    "--prop",
    "--max-lights",
    "--shader-substitute",
}


def options(argv: list[str]) -> dict[str, list[str]]:
    """The converter's value options (each name -> its values in order); refuses an unknown or incomplete one."""
    found: dict[str, list[str]] = {}
    at = 0
    while at < len(argv):
        name = argv[at]
        if name in _VALUE_OPTIONS and at + 1 < len(argv):
            found.setdefault(name, []).append(argv[at + 1])
            at += 2
        elif name in ("--no-lights", "--first-carrier"):
            at += 1
        else:
            raise SystemExit(f"error: weapon_tint_drawable.py does not take {name!r}")
    return found


def plan(argv: list[str]) -> tuple[list[str], str]:
    """(substitutes FROM=TO, one `tint:` line) for these converter arguments."""
    given = options(argv)
    if "--prop" in given:
        return [], "tint: none (prop route)"
    c = cpd.contracts()  # --reference-dir is accepted and not read (data/drawable_contracts)
    names = {int(key): row["name"] for key, row in c["shader"]["mappings"].items()}
    carriers = set()
    for path in given.get("--template", []) + given.get("--shader-template", []):
        template = cpd.Template(Path(path).read_bytes(), c, Path(path).name)
        carriers |= {names.get(row["schema"]["nameHash"], "") for row in template.shaders()}
    if PALETTE_SHADER not in carriers:
        return [], "tint: none (the template family has no tint shader)"
    source = cpd.Source(Path(given["--source"][0]).read_bytes(), c, Limits())
    used = [names.get(int(s["nameHash"], 0), s["nameHash"]) for s in source.materials()["shaders"]]
    taken = {item.partition("=")[0] for item in given.get("--shader-substitute", []) if item != "auto"}
    tinted = sorted({name for name in used if name in TINTABLE and name not in taken})
    if not tinted:
        return [], f"tint: none (no opaque body material; the model uses {', '.join(sorted(set(used)))})"
    count = sum(1 for name in used if name in tinted)
    line = f"tint: {count} of {len(used)} materials ({', '.join(tinted)}) -> {PALETTE_SHADER} + {TINT_PALETTE}"
    return [f"{name}={PALETTE_SHADER}" for name in tinted], line


def record(argv: list[str], substitutes: list[str], line: str) -> None:
    """The plan in the converter's report (<output>.report.json "weaponTint"): make_weapon_model_pack.py
    --tint-palette auto prints its reason when the model has no tints."""
    output = Path(options(argv)["--output"][0])
    path = output.with_name(output.name + ".report.json")
    report = json.loads(path.read_text(encoding="utf-8"))
    report["weaponTint"] = {"line": line, "palette": TINT_PALETTE if substitutes else None, "substitutes": substitutes}
    path.write_bytes(cpd.canonical(report))


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    off = "--no-tints" in argv
    argv = [a for a in argv if a != "--no-tints"]
    try:
        substitutes, line = ([], "tint: none (turned off with --no-tints)") if off else plan(argv)
    except (AssetError, OSError, ValueError, KeyError) as error:
        raise SystemExit(f"error: {error}") from None
    extra = []
    for item in substitutes:
        extra += ["--shader-substitute", item]
    code = cpd.main(argv + extra)
    if code == 0:
        record(argv, substitutes, line)
    print(line)
    return code


if __name__ == "__main__":
    raise SystemExit(main())

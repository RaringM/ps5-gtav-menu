#!/usr/bin/env python3
"""Detect and apply every bone-free exporter-quirk repair a PC vehicle fixture needs.

Picking --repair flags by hand from the refusing gate costs one failed conversion per quirk, and a
flag for a quirk the mod does not have refuses too (the Skyline's parts have no zero triangle counts).
This tool runs eight repairs that need no bone list, in a fixed order; each detects its own quirk and
is applied only when found, so a clean fixture passes through unchanged:
  1. select-model         repair_pc_select_model.py         (several vehicles in the data files)
  2. texture-stride       repair_pc_texture_stride.py       (strides, mip chains, render-target placeholders)
  3. texture-names        repair_pc_texture_names.py        (spaces/hyphens in texture names, .ytd and .yft)
  4. nonfinite-texcoords  repair_pc_nonfinite_texcoords.py  (NaN texture coordinates; before any mesh reader)
  5. duplicate-bones      repair_pc_duplicate_bones.py      (misc_a twice)
  6. triangle-counts      repair_pc_triangle_counts.py      (TrianglesCount 0 on triangle lists)
  7. pose-precision       repair_pc_pose_precision.py       (--rejected: bone matrices ~1e-5 off the pose)
  8. fragment-quirks      repair_pc_fragment_quirks.py      (empty octant items, sentinel words,
                                                             FragMatrices; last: its walk needs a clean
                                                             skeleton and pose to reach every fragment)
The detecting walks stop at the converter's first refusal, so a quirk behind another one is only seen
once that one is repaired (the gmt400's physics-child triangle counts sat behind its pose rejection):
the sequence is repeated until a whole round applies nothing (at most ROUNDS). A repair "finds
nothing" when its tool ends with its own no-op message (NOTHING below); any other
refusal stops the run with that tool's message (a quirk it cannot repair safely). The tools run exactly
as convert_vehicle.py runs them; before each one a fresh material report of the current fixture is
written with the members of the given report (the converter walks pin it). Bone repairs that need a
judgement are not guessed: seat-mirror (a mirrored seat) and inverse-precision (inverse translations
only) go before auto (--repair seat-mirror=... --repair auto); pose-precision takes the gate's own
rejected bones and still refuses anything above its 1e-4 bound. The bytes the tools change do not
overlap (metas, .ytd, texture names, texcoords, bone names, triangle counts, bone matrices, fragment words), so the
result equals the same repairs given one by one (checked on the GT-R: identical fixture bytes).

  repair_pc_auto.py --fixture build/assets/convert-<id>/fixtures/gtr-import \\
      --material-report <its material report> --output <new fixture>   (convert_vehicle.py --repair auto)

Intermediate fixtures and reports go to <output>-steps/ beside the output (reports: --reports-dir, or
build/auto-reports/ when the work dir resolves outside <repo>/build, see default_reports).
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

from gtavmenu_tools.host_paths import build_dir  # noqa: E402
from gtavmenu_tools.vehicle_repair_inputs import FixtureError, copy_fixture, validate_fixture  # noqa: E402

STEPS = (  # name, tool, walks the converter (takes --templates)
    ("select-model", "repair_pc_select_model.py", False),
    ("texture-stride", "repair_pc_texture_stride.py", False),
    ("texture-names", "repair_pc_texture_names.py", False),
    ("nonfinite-texcoords", "repair_pc_nonfinite_texcoords.py", True),
    ("duplicate-bones", "repair_pc_duplicate_bones.py", False),
    ("triangle-counts", "repair_pc_triangle_counts.py", True),
    ("pose-precision", "repair_pc_pose_precision.py", False),  # with --rejected
    ("fragment-quirks", "repair_pc_fragment_quirks.py", True),
)
ROUNDS = 4  # the walks stop at the first refusal, so a later repair can uncover an earlier quirk
NOTHING = {
    "select-model": "nothing to select",
    "texture-stride": "no texture stride to repair",
    "texture-names": "no texture names to repair",
    "nonfinite-texcoords": "no non-finite texture coordinates found",
    "duplicate-bones": "no duplicate bones to repair",
    "triangle-counts": "no triangle-count rejections found",
    "pose-precision": "no pose rejections to repair",
    "fragment-quirks": "no quirks found",
}


def report_members(report: Path) -> tuple[list[str], str]:
    """(fragment members, texture member) a material report was made for."""
    rows = json.loads(report.read_bytes())["sources"]["resources"]
    fragments = [row["member"] for row in rows if row["kind"] == "fragment"]
    textures = [row["member"] for row in rows if row["kind"] == "textureDictionary"]
    if not fragments or len(textures) != 1:
        raise SystemExit(f"{report}: expected fragment members and one texture dictionary")
    return fragments, textures[0]


def material_report(fixture: Path, fragments: list[str], texture: str, templates: Path | None, output: Path) -> None:
    """Public material observation command for the current fixture."""
    command = [sys.executable, str(TOOLS / "inspect_pc_vehicle_materials.py"), "--pc-fixture", str(fixture)]
    for member in fragments:
        command += ["--resource-member", member]
    command += ["--texture-member", texture, "--templates", str(templates or build_dir(ROOT) / "retail-templates")]
    command += ["--stock-dictionary-name", "vehshare", "--output", str(output)]
    run(command, "material report")


def default_reports(steps_dir: Path) -> Path:
    """Keep fresh material reports beside this invocation's intermediate fixtures."""
    return steps_dir


def run(command: list[str], label: str) -> subprocess.CompletedProcess:
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
    if result.returncode:
        sys.stdout.write(result.stdout)
        raise SystemExit(f"auto: {label} failed: {(result.stderr or result.stdout).strip().splitlines()[-1:]}")
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--material-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--templates", type=Path, help="retail templates root (passed to the converter walks)")
    parser.add_argument("--reports-dir", type=Path, help="material reports (default: <output>-steps)")
    parser.add_argument("--skip", action="append", default=[], choices=[name for name, *_ in STEPS])
    args = parser.parse_args(argv)
    if args.output.exists() or args.output.is_symlink():
        raise SystemExit(f"refusing to overwrite {args.output}")
    manifest = validate_fixture(args.fixture)
    steps_dir = args.output.parent / (args.output.name + "-steps")
    if steps_dir.exists() or steps_dir.is_symlink():
        raise SystemExit(f"refusing to overwrite {steps_dir}")
    reports = args.reports_dir or default_reports(steps_dir)
    fragments, texture = report_members(args.material_report)
    fixture, report, applied = args.fixture, args.material_report, []
    reported = fixture  # the fixture the current material report was made for
    for round_ in range(1, ROUNDS + 1):
        before = len(applied)
        for index, (name, tool, walks) in enumerate(STEPS):
            if name in args.skip:
                continue
            tag = f"{round_}-{index}-{name}"
            if fixture != reported:
                reported, report = fixture, reports / f"{tag}-materials.json"
                report.parent.mkdir(parents=True, exist_ok=True)
                material_report(fixture, fragments, texture, args.templates, report)
            output = steps_dir / tag
            output.parent.mkdir(parents=True, exist_ok=True)
            command = [sys.executable, str(TOOLS / tool), "--fixture", str(fixture), "--material-report", str(report)]
            command += ["--output", str(output)]
            if walks and args.templates is not None:
                command += ["--templates", str(args.templates)]
            if name == "pose-precision":
                command.append("--rejected")
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
            if result.returncode:
                message = (result.stderr or result.stdout).strip().splitlines()[-1:] or [""]
                if NOTHING[name] in message[0]:
                    print(f"auto: round {round_} {name}: nothing found")
                    continue
                sys.stdout.write(result.stdout)
                raise SystemExit(f"auto: {name} refused: {message[0]}")
            print(f"auto: round {round_} {name}: applied ({result.stdout.strip().splitlines()[-1][:200]})")
            applied.append({"repair": name, "round": round_, "fixture": str(output)})
            fixture = output
        if len(applied) == before:  # no admitted repairs; normal conversion still validates the full graph
            break
    else:
        raise SystemExit(f"auto: still repairing after {ROUNDS} rounds; stop and read the steps in {steps_dir}")
    copy_fixture(fixture, args.output)
    manifest_path = args.output / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    manifest["autoRepairs"] = {
        "tool": "tools/repair_pc_auto.py",
        "applied": [row["repair"] for row in applied],
        "rounds": max((row["round"] for row in applied), default=0) + 1,
        "checked": [name for name, *_ in STEPS if name not in args.skip],
    }
    manifest_path.write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"output": str(args.output), "applied": manifest["autoRepairs"]["applied"]}))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FixtureError as error:
        raise SystemExit(f"fixture refused: {error}") from None

#!/usr/bin/env python3
"""Stage the PS4 plugin PRX, installation snippet, and validation evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from gtavmenu_tools.boot_pool_data_profile import BootPoolProfileError, data_contract  # noqa: E402
from gtavmenu_tools.git_state import source_state  # noqa: E402
from inspect_ps4_plugin import BOOT_POOLS_MODES, ValidationError, validate  # noqa: E402


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def boot_pools_text(mode: str, growth: str) -> str:
    """What the build does with the boot pools, for the package README."""
    if mode == "full":
        return (
            "Boot pools: when the plugin loads before the game sizes its asset stores, it grows "
            f"them at boot ({growth}). When it loads after that but before the first session "
            "creates the model factories (nexHEN loads it at about 10-14 s), it grows only the ped "
            "and vehicle capacities. Later than that it writes nothing and logs why. Either way the "
            "menu starts. Every write is checked against the stock value first."
        )
    if mode == "caps":
        return (
            "Probe build, boot pools in caps mode: it grows only the ped and vehicle model "
            "capacities, before the first session creates them; the asset stores keep their stock "
            "sizes."
        )
    if mode == "observe":
        return (
            "Probe build, boot pools in observe mode: it reads the boot state, logs the stock "
            "values and the boot timings, and writes nothing. Pools stay at stock sizes."
        )
    return "Probe build, boot pools off: pools stay at stock sizes."


# One plugins.ini entry per host; each is merged into that host's existing file.
HOSTS = (
    (
        "goldhen",
        "GoldHEN 2.3 or newer",
        "/data/GoldHEN/plugins/gtav_menu.prx",
        "/data/GoldHEN/plugins.ini",
        "",
        "Enable plugin loading in GoldHEN's settings, then cold-launch",
    ),
    (
        "nexhen",
        "nexHEN PS4 v1.00",
        "/data/nexHEN/plugins/gtav_menu.prx",
        "/data/nexHEN/plugins.ini",
        "?autoload",
        "Cold-launch",
    ),
)


def instructions(digest: str, title: str, title_id: str, mode: str = "full", growth: str = "") -> str:
    probe = mode != "full"
    heading = f"GTAV Menu plugin boot-pool probe ({mode})" if probe else "GTAV Menu plugin candidate"
    warning = (
        "\n**Probe build for hardware measurements, not for release.** The plugin description and its "
        "load notification say so.\n"
        if probe
        else ""
    )
    hosts = "\n".join(f"""### {name}

1. Copy `gtav_menu.prx` to `{prx}`.
2. Merge the `[{title_id}]` entry from `{key}-plugins.ini.example` into the existing
   `{ini}`: add the line under that section, or add the section if it is missing. Keep the
   file and its other plugins; comment out (`;`) any other GTA V menu plugin in the section.
3. {launch} GTA V Legacy {title}.
""" for key, name, prx, ini, _, launch in HOSTS)
    return f"""# {heading} — {title}
{warning}
`gtav_menu.prx` is a fake-signed SELF PRX plugin for GoldHEN 2.3 or newer and for nexHEN PS4
v1.00. It is a plugin, not a payload: it needs no payload loader or debugger.
SHA-256: `{digest}`.
This build has passed host checks; this build has not run on hardware. Earlier builds of the same
plugin passed load, menu, action, profile/touchpad, unload and relaunch checks under GoldHEN, and
load, menu, unload and reload, relaunch, a custom-pack load and the ped/vehicle capacity growth
under nexHEN PS4 v1.00, on firmware 9.00.

{boot_pools_text(mode, growth)} The `boot-pool:` lines in the log say what happened;
creating `/data/GTAVMenu/boot-pools.off` turns boot pools off.

## Install

Never load a second copy of the menu into the same game (for example through a debugger) while
the plugin is loaded. Install the plugin under one host.

{hosts}
The plugin waits for Story Mode, verifies the exact game fingerprints, then installs
its normal `PLAYER_PED_ID` game-thread hook. Open the menu with **R1 + D-pad Left**.
Startup and cleanup diagnostics go to `/data/GTAVMenu/ps4-plugin.log`.

## Smoke and unload

Require a loaded notification, menu drawing, one benign native action, and normal
profile/touchpad behavior. Unload through the host with the menu closed and require the menu
to close, features to shut down, the original hook and pad import to be restored, and gameplay
to continue normally. Repeat clean load/unload cycles and one game relaunch before
treating the lifecycle as dependable. Stop at the first crash, hang, wrong fingerprint,
failed restoration, duplicate hook, or uncertain module state and retain the log.
"""


def growth_text(target_manifest: Path) -> str:
    """The pinned growth, e.g. "vehicle 640 → 840, ped 440 → 540, ...", from the data-method profile."""
    try:
        profile = data_contract(target_manifest)
    except BootPoolProfileError as exc:
        raise ValidationError(f"boot-pool profile: {exc}") from None
    rows = sorted(profile["overrides"], key=lambda o: o["kind"] != "cfg")
    return ", ".join(f"{o['name']} {o['stock']} → {o['new']}" for o in rows)


def stage(
    elf: Path,
    prx: Path,
    target_manifest: Path,
    output: Path,
    probe: str | None = None,
    toolchain: Path | None = None,
) -> dict[str, object]:
    validation = validate(elf, prx, target_manifest, toolchain)
    mode = str(validation.get("bootPoolsMode", ""))
    if mode not in BOOT_POOLS_MODES:
        raise ValidationError(f"validation names no boot-pool mode: {mode!r}")
    if mode != "full" and probe != mode:
        raise ValidationError(f"this PRX is a {mode} boot-pool probe build; package it with --probe {mode}")
    if mode == "full" and probe is not None:
        raise ValidationError(f"--probe {probe} given for a full (release) build")
    growth = growth_text(target_manifest) if mode == "full" else ""
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    staged_prx = output / "gtav_menu.prx"
    shutil.copy2(prx, staged_prx)
    target = json.loads(target_manifest.read_text(encoding="utf-8"))
    examples = []
    for key, _, prx_path, _, suffix, _ in HOSTS:
        example = output / f"{key}-plugins.ini.example"
        example.write_text(f"[{target['titleId']}]\n{prx_path}{suffix}\n", encoding="utf-8")
        examples.append(example)
    validation_path = output / "validation.json"
    validation_path.write_text(json.dumps(validation, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    readme = output / "README.md"
    readme.write_text(
        instructions(
            sha256(staged_prx), f"{target['titleId']} {target['contentVersion']}", target["titleId"], mode, growth
        ),
        encoding="utf-8",
    )
    commit, dirty = source_state(ROOT)
    files = []
    for path, role in (
        (staged_prx, "ps4-plugin"),
        *((example, "installation-config") for example in examples),
        (validation_path, "validation"),
        (readme, "instructions"),
    ):
        files.append({"path": path.name, "role": role, "size": path.stat().st_size, "sha256": sha256(path)})
    manifest = {
        "schemaVersion": 1,
        "kind": "gtavmenu-ps4-plugin-hardware-candidate",
        "targetId": target["targetId"],
        "titleId": target["titleId"],
        "contentVersion": target["contentVersion"],
        "hardwareValidated": False,
        "bootPoolsMode": mode,
        "probeBuild": mode != "full",
        "sourceCommit": commit,
        "sourceDirty": dirty,
        "targetManifestSha256": sha256(target_manifest),
        "files": files,
    }
    (output / "package-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--prx", required=True, type=Path)
    parser.add_argument("--target-manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--probe",
        choices=[mode for mode in BOOT_POOLS_MODES if mode != "full"],
        help="package a probe build (off, observe or caps); required for, and only for, a non-full PRX",
    )
    parser.add_argument("--toolchain", type=Path, help="the OpenOrbis toolchain root, fingerprinted in validation.json")
    args = parser.parse_args()
    try:
        manifest = stage(args.elf, args.prx, args.target_manifest, args.output, args.probe, args.toolchain)
    except (OSError, ValueError, json.JSONDecodeError, ValidationError) as exc:
        parser.error(str(exc))
    print(f"staged {len(manifest['files'])} files to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

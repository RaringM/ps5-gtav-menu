#!/usr/bin/env python3
"""Stage a hardware-unqualified native nexHEN module (broker lane or stock-loader probe)."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "nexhen"))
sys.path.insert(1, str(ROOT / "tools"))

from gtavmenu_tools.git_state import source_state  # noqa: E402
from validate_plugin import ValidationError, validate  # noqa: E402


class PackageError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def readme(plugin_name: str, digest: str) -> str:
    return f"""# GTAV-Menu native nexHEN test candidate

This hardware-unqualified module targets GTA V Enhanced PPSA04264 version 01.010.002
and requires the matching corrected nexHEN protocol-4 bridge and ShellCore service.
Stock nexHEN v0.90 uses the tracing path that previously broke GTA streaming.

Copy `{plugin_name}` to `/user/data/nexHEN/plugins/{plugin_name}`. Preserve existing
startup and autoload settings. Use nexHEN's manual plugin control for the first test.
The file's SHA-256 is `{digest}`.

The first live run must stop on any loading ambiguity, animation or streaming change,
freeze, failed cleanup, or uncertain module presence. A lost operation reply is never
replayed in the same game process. Hardware qualification requires explicit unload,
healthy gameplay afterward, clean exit/relaunch, and repeated cycles.
"""


def probe_readme(plugin_name: str, digest: str) -> str:
    return f"""# GTAV-Menu nexHEN stock-loader probe

This lifecycle-only module answers one question: does stock nexHEN's autoload attach,
made at title start, leave GTA V's streaming I/O healthy? It installs no hooks, starts no
menu and never writes game memory. It records how long after launch `plugin_load` ran,
then samples the streaming ring once per second and scores every 15-sample window
HEALTHY / JAMMED / INDETERMINATE (same rule as `probe_streaming_state.py --verdict`).

Target: GTA V Enhanced PPSA04264 01.010.002, unmodified nexHEN v1.00. SHA-256 of
`{plugin_name}`: `{digest}`.

## Install

1. Copy `{plugin_name}` to `/data/nexHEN/plugins/{plugin_name}`.
2. Add to `/data/nexHEN/plugins.ini` (or use the toolbox Plugins page):

   ```ini
   [PPSA04264]
   /data/nexHEN/plugins/{plugin_name}?autoload
   ```

## Run (fresh boot, stop at the first failure)

1. Fresh boot; load only nexHEN v1.00. No other game plugins or menu payloads.
2. Launch GTA V. A notification reports how many ms after launch the probe loaded.
3. Enter story mode, then enter a vehicle and drive into new scenery for 5+ minutes.
   Idle gameplay streams almost nothing, so only driving windows are decisive.
4. Watch for `streaming HEALTHY` / `streaming JAMMED` notifications.
5. Close GTA through the PS5 shell. Do not unload the probe mid-game.

Collect `/data/GTAVMenu/nexhen-probe.log` (fallback `/user/data/GTAVMenu/`) and the
nexHEN daemon log. Pass: windows reach HEALTHY while driving, the vehicle-entry
animation is normal and nothing freezes. Any JAMMED window, abnormal animation or
freeze is a fail.

Optional control in a later, separate boot: remove `?autoload`, start GTA, and load the
probe by hand from the toolbox mid-gameplay. Prior evidence predicts JAMMED.
"""


def stage(
    *, prx: Path, plugin: Path, providers: Path, target_manifest: Path, output: Path, mode: str = "broker"
) -> dict[str, object]:
    if mode not in {"broker", "probe"}:
        raise PackageError(f"unknown nexHEN package mode: {mode}")
    for path in (prx, plugin, providers, target_manifest):
        if not path.is_file():
            raise PackageError(f"required nexHEN package input is missing: {path}")
    target = json.loads(target_manifest.read_text(encoding="utf-8"))
    identity = (target.get("targetId"), target.get("titleId"), target.get("contentVersion"))
    if identity != ("PPSA04264_01.010.002_DISC", "PPSA04264", "01.010.002"):
        raise PackageError("the native nexHEN module supports only PPSA04264 01.010.002")

    broker = mode == "broker"
    validation = validate(prx, plugin, providers)
    expected_protocol = {"loaderBridge": 4} if broker else None
    if validation["protocol"] != expected_protocol or validation["hardwareValidated"] is not False:
        raise PackageError(f"native validation does not describe an unqualified {mode} module")

    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    staged = output / "gtav_menu_nexhen.sprx"
    shutil.copy2(plugin, staged)
    validation_path = output / "native-validation.json"
    validation_path.write_text(json.dumps(validation, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    readme_path = output / "README.md"
    render_readme = readme if broker else probe_readme
    readme_path.write_text(render_readme(staged.name, sha256(staged)), encoding="utf-8")

    commit, dirty = source_state(ROOT)
    files = []
    for path, role in (
        (staged, "nexhen-native-module"),
        (validation_path, "native-validation"),
        (readme_path, "test-instructions"),
    ):
        files.append({"path": path.name, "role": role, "size": path.stat().st_size, "sha256": sha256(path)})
    manifest = {
        "schemaVersion": 1,
        "kind": "gtavmenu-nexhen-hardware-candidate" if broker else "gtavmenu-nexhen-stock-probe",
        "mode": mode,
        "targetId": identity[0],
        "titleId": identity[1],
        "contentVersion": identity[2],
        "loaderBridgeProtocol": 4 if broker else None,
        "requiresCorrectedLoader": broker,
        "hardwareValidated": False,
        "sourceCommit": commit,
        "sourceDirty": dirty,
        "targetManifestSha256": sha256(target_manifest),
        "providersSha256": sha256(providers),
        "files": files,
    }
    (output / "package-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("broker", "probe"), default="broker")
    parser.add_argument("--prx", required=True, type=Path)
    parser.add_argument("--plugin", required=True, type=Path)
    parser.add_argument("--providers", required=True, type=Path)
    parser.add_argument("--target-manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        manifest = stage(
            prx=args.prx,
            plugin=args.plugin,
            providers=args.providers,
            target_manifest=args.target_manifest,
            output=args.output,
            mode=args.mode,
        )
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError, ValidationError, PackageError) as exc:
        parser.error(str(exc))
    print(f"staged {len(manifest['files'])} files to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

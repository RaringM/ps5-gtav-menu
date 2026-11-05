#!/usr/bin/env python3
"""Validate one target JSON and emit make-safe versioned build settings."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from gtavmenu_tools.target_profile import TargetProfileError, load_manifest


def make_tokens(profile: dict[str, object]) -> list[str]:
    features = profile["features"]
    injection = profile["injection"]
    native = profile["nativeAddresses"]
    globals_input = profile["scriptGlobals"]
    custom_lane = profile.get("customLane")
    boot_pools = profile.get("bootPools")
    assert isinstance(features, dict)
    assert isinstance(injection, dict)
    assert isinstance(native, dict)
    assert isinstance(globals_input, dict)
    values = {
        "profile_ok": 1,
        "target_id": profile["targetId"],
        "title_id": profile["titleId"],
        "content_id": profile["contentId"],
        "content_version": profile["contentVersion"],
        "channel": profile["channel"],
        "native_json": native["json"],
        "native_header": native["header"],
        "native_sha256": native["sha256"],
        "script_json": globals_input["json"],
        "script_header": globals_input["header"],
        "script_sha256": globals_input["sha256"],
        "native_features": features["nativeFeatures"],
        "worker_text": features["workerTextOverlay"],
        "toasts": features["toasts"],
        "vehicle_preview": features["vehiclePreview"],
        "instructional_scaleform": features["instructionalScaleform"],
        "button_glyphs": features["buttonGlyphs"],
        "script_globals": features["scriptGlobals"],
        "phase_intercept": features["phaseIntercept"],
        "phase_draw_list": features["phaseDrawList"],
        "custom_packs": features["customPacks"],
        "custom_lane_header": custom_lane["header"] if custom_lane else "",
        "boot_pools": features["bootPools"],
        "boot_pools_header": boot_pools["header"] if boot_pools else "",
        "injection_lane": injection["lane"],
        "probe_nostop": injection["probeNoStop"],
        "nostop_io": injection["noStopIo"],
        "nostop_strict": injection["noStopStrict"],
        "verify_writes": injection["verifyWrites"],
        "cave_bootstrap": injection["caveBootstrap"],
        "cave_inject": injection["caveInject"],
        "classic_inject": injection["classicInject"],
        "self_start": injection["selfStartWorker"],
        "install_phase": injection["installRenderPhase"],
    }
    return [f"{key}={value}" for key, value in values.items()]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target-manifest", required=True, type=Path)
    parser.add_argument("--make", action="store_true", help="emit whitespace-delimited key=value tokens")
    args = parser.parse_args()
    try:
        _, profile = load_manifest(args.target_manifest)
    except (OSError, TargetProfileError) as exc:
        print(f"target_build_config: {exc}", file=sys.stderr)
        return 1
    if profile["platform"] != "ps5":
        print(
            f"target_build_config: {profile['stem']} is a {profile['platform']} target; this build makes PS5 workers"
            " only",
            file=sys.stderr,
        )
        return 1
    if args.make:
        print(" ".join(make_tokens(profile)))
    else:
        print(f"{profile['stem']} ({profile['channel']}) is reproducible")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

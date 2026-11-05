"""PS4 worker build settings derived from one validated PS4 target manifest.

The manifest's ``build.features`` is the single source for the worker's feature macros: this module
maps each key to its -D flag (``make/ps4-plugin.mk`` and the host tests both consume the result), and the
generated capability header repeats the same keys as ``GTAV_CAPABILITY_GATE_*`` so a mismatch fails
the compile (``src/module/ps4/bridge.cpp``).
"""

from __future__ import annotations

import os
from pathlib import Path

from gtavmenu_tools.target_profile import PS4_FEATURE_KEYS, REPO_ROOT, load_manifest

DEFAULT_TARGET = "cusa00411-01.57"

# Fixed for every PS4 one-shot worker: the menu, features and drawing all run inside the hooked
# PLAYER_PED_ID call on the game thread; there is no scePad import, render-phase callback, rootdir
# escape or log file.
PLATFORM_DEFINES = (
    "__ORBIS__=1",
    "GTAV_MENU_PHASE_DRAW_LIST=1",
    "GTAV_RENDER_PHASE_INTERCEPT=1",
    "GTAV_MENU_ENABLE_PAD_INPUT=0",
    "GTAV_MENU_RENDER_DIAGNOSTICS=0",
    "GTAV_MENU_ENABLE_FRAME_HOOK=1",
    "GTAV_FEATURES_SKIP_FRAME_HOOK_SETUP=1",
    # gtav_features_init runs from _main on the debugger's ELF thread, and a profile import calls
    # natives, so init never loads the profile. Storage builds load it on the first ready game frame.
    "GTAV_FEATURES_SKIP_PROFILE_LOAD=1",
    "GTAV_MENU_NO_ROOTDIR=1",
    "GTAV_MENU_DISABLE_LOG_IO=1",
    # No kernel log in the freestanding worker: the host reads the control block instead.
    "GTAV_MENU_HAS_WORKER_KLOG=0",
    "GTAV_MENU_WORKER_TEXT_OVERLAY_RENDER_LIST=1",
    # The menu ticks once per game frame; menu_timing.h rescales its 60 Hz durations, and there is
    # no worker loop for the Worker Hz row to tune.
    "GTAV_MENU_TICK_HZ=30u",
    "GTAV_MENU_HAS_WORKER_LOOP=0",
    # Host telemetry and the Legacy native argument lists for the shared draw-list consumer.
    "GTAV_MENU_PHASE_DRAW_COUNTERS=1",
    "GTAV_MENU_NATIVE_LEGACY_TRAILING_ARGS=1",
    # The catalog comes from the Enhanced build: warm vehicle classes 64 models a frame and only for
    # models the Legacy CD image has.
    "GTAV_MENU_VEHICLE_CLASS_WARM_CHUNK=64",
    # /data is granted by the host once the menu is active, when the load-time probe saw it.
    'GTAV_CUSTOM_DATA_UNAVAILABLE="Storage unavailable: /data was not visible when the menu loaded"',
    'GTAV_CUSTOM_DATA_FIX_ROW="Storage needs /data at load"',
    'GTAV_CUSTOM_DATA_RETRY_ROW="Restart GTA"',
)

# build.features key -> (macro, inverted). Inverted macros are on when the feature is off.
FEATURE_MACROS: dict[str, tuple[str, bool] | None] = {
    "nativeFeatures": ("GTAV_MENU_ENABLE_NATIVE_FEATURES", False),
    "mainThreadActions": ("GTAV_MENU_GATE_MAINTHREAD_ACTIONS", False),
    "coldModelStreaming": ("GTAV_MENU_STREAM_RESIDENT_ONLY", True),
    "toasts": ("GTAV_MENU_ENABLE_TOASTS", False),
    "workerTextOverlay": ("GTAV_MENU_ENABLE_WORKER_TEXT_OVERLAY", False),
    "hud": ("GTAV_MENU_ENABLE_HUD", False),
    "effects": ("GTAV_MENU_ENABLE_EFFECTS", False),
    "vehicleClasses": ("GTAV_MENU_ENABLE_VEHICLE_CLASSES", False),
    "scriptGlobals": ("GTAV_MENU_ENABLE_SCRIPT_GLOBALS", False),
    "vehiclePreview": ("GTAV_MENU_ENABLE_VEHICLE_PREVIEW", False),
    "buttonGlyphs": ("GTAV_MENU_ENABLE_BUTTON_GLYPHS", False),
    "instructionalScaleform": ("GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM", False),
    "customPacks": ("GTAV_MENU_ENABLE_CUSTOM_PACKS", False),
    # Source-selection features: make/ps4-plugin.mk adds or drops translation units for these.
    "localization": None,
    # Compiles the shared profile parser and the deferred profile load; the /data layer itself
    # stays closed at runtime until the host sets GTAV_PS4_HOST_STORAGE_READY.
    "storage": ("GTAV_PS4_ENABLE_STORAGE", False),
    # The scePadReadState GOT swap (src/module/ps4/pad_hook.c), installed only while the menu's
    # Touchpad setting is on.
    "touchpad": ("GTAV_PS4_ENABLE_TOUCHPAD", False),
    "bootPools": None,
}
assert set(FEATURE_MACROS) == set(PS4_FEATURE_KEYS), "every PS4 feature key needs a build mapping"

# Extra defines an enabled feature implies. The PS4 native table binds DRAW_SPRITE and the streamed
# texture natives only on request (their 1.57 argument shapes are reviewed: 12 DRAW_SPRITE slots).
FEATURE_IMPLIES: dict[str, tuple[str, ...]] = {
    "vehiclePreview": ("GTAV_MENU_BIND_SPRITE_NATIVES=1",),
}

# build.features key -> the shared translation units it adds, as (directory, stem) pairs relative to
# src/. make/ps4-plugin.mk and the host tests (tests/csources.py) both read this list.
FEATURE_SOURCES: dict[str, tuple[tuple[str, str], ...]] = {
    "storage": (("common", "feature_profile"), ("common", "ini_parse")),
    "scriptGlobals": (("module", "script_globals"),),
    # The descriptor parser, refusal texts, archive hashing and TOC encryption.
    "customPacks": (
        ("common", "custom_pack_runtime"),
        ("common", "custom_pack_checks"),
        ("common", "sha256"),
        ("common", "aes256"),
    ),
}


def feature_sources(features: dict[str, object]) -> list[tuple[str, str]]:
    """The extra shared translation units the enabled features compile in, in a stable order."""
    units: list[tuple[str, str]] = []
    for key in PS4_FEATURE_KEYS:
        if features.get(key):
            units.extend(FEATURE_SOURCES.get(key, ()))
    return units


def default_target() -> str:
    """The PS4 target stem the tools use: ``GTAV_PS4_TARGET`` (as for make), else the default."""
    return os.environ.get("GTAV_PS4_TARGET") or DEFAULT_TARGET


def target_manifest_path(target: str | None = None, root: Path = REPO_ROOT) -> Path:
    return root / "data/targets/ps4" / f"{target or default_target()}.json"


def load_config(manifest_path: Path) -> dict[str, object]:
    manifest, profile = load_manifest(manifest_path)
    if profile["platform"] != "ps4":
        raise ValueError(f"{manifest_path} is not a PS4 target manifest")
    features = profile["features"]
    assert isinstance(features, dict)
    defines = list(PLATFORM_DEFINES)
    for key in PS4_FEATURE_KEYS:
        mapping = FEATURE_MACROS[key]
        if mapping is None:
            continue
        macro, inverted = mapping
        enabled = bool(features[key])
        defines.append(f"{macro}={int(enabled != inverted)}")
        if enabled:
            defines.extend(FEATURE_IMPLIES.get(key, ()))
    native = profile["nativeAddresses"]
    capabilities = profile["capabilities"]
    assert isinstance(native, dict) and isinstance(capabilities, dict)
    defines += [
        f'GTAV_MENU_NATIVE_ADDRESSES_HEADER="{native["header"]}"',
        f'GTAV_MENU_CAPABILITIES_HEADER="{capabilities["header"]}"',
        f'GTAV_PS4_TARGET_HEADER="{profile["targetHeader"]}"',
        f'GTAV_PS4_PLUGIN_HEADER="gtavmenu/ps4_plugin_{profile["stem"]}_generated.h"',
    ]
    # The data-method boot-pool layout the PRX grows the pools with (src/module/ps4/plugin_boot_pools.cpp).
    boot_pools = profile["bootPools"]
    if boot_pools is not None:
        assert isinstance(boot_pools, dict)
        defines.append(f'GTAV_PS4_BOOT_POOL_DATA_HEADER="{boot_pools["header"]}"')
    script_globals = profile["scriptGlobals"]
    if script_globals is not None:
        assert isinstance(script_globals, dict)
        defines.append(f'GTAV_MENU_SCRIPT_GLOBALS_HEADER="{script_globals["header"]}"')
    custom_lane = profile["customLane"]
    if custom_lane is not None:
        assert isinstance(custom_lane, dict)
        defines.append(f'GTAV_MENU_CUSTOM_LANE_HEADER="{custom_lane["header"]}"')
    defines.append(f"GTAV_MENU_CUSTOM_PACK_ENGINE={int(custom_lane is not None)}")
    return {
        "stem": profile["stem"],
        "targetId": profile["targetId"],
        "manifest": manifest,
        "features": dict(features),
        "defines": defines,
        "nativeJson": native["json"],
        "nativeHeader": f"include/{native['header']}",
        "capabilityJson": capabilities["json"],
        "capabilityHeader": f"include/{capabilities['header']}",
        "targetHeader": f"include/{profile['targetHeader']}",
        "scriptGlobalsJson": script_globals["json"] if script_globals else "",
        "scriptGlobalsHeader": f"include/{script_globals['header']}" if script_globals else "",
        "customLaneJson": custom_lane["json"] if custom_lane else "",
        "customLaneHeader": f"include/{custom_lane['header']}" if custom_lane else "",
        "featureSources": feature_sources(features),
        "llvmMajor": int(manifest["build"]["toolchain"]["llvmMajor"]),
    }


def runtime_defines(target: str | None = None, root: Path = REPO_ROOT) -> list[str]:
    """The exact -D list the PS4 worker compiles with (host tests reuse it to avoid drift)."""
    return list(load_config(target_manifest_path(target, root))["defines"])


def make_fragment(config: dict[str, object]) -> str:
    def quote(define: str) -> str:
        name, _, value = define.partition("=")
        return f"-D{name}='{value}'" if value.startswith('"') else f"-D{define}"

    features = config["features"]
    assert isinstance(features, dict)
    defines = config["defines"]
    assert isinstance(defines, list)
    sources = config["featureSources"]
    assert isinstance(sources, list)
    lines = [
        "# AUTO-GENERATED by tools/ps4_build.py from the PS4 target manifest. Do not edit.",
        f"PS4_TARGET_ID := {config['targetId']}",
        f"PS4_NATIVE_JSON := {config['nativeJson']}",
        f"PS4_FEATURE_HEADER := {config['nativeHeader']}",
        f"PS4_CAPABILITY_POLICY := {config['capabilityJson']}",
        f"PS4_CAPABILITY_HEADER := {config['capabilityHeader']}",
        f"PS4_HEADER := {config['targetHeader']}",
        f"PS4_LLVM_MAJOR := {config['llvmMajor']}",
        f"PS4_FEATURE_LOCALIZATION := {int(bool(features['localization']))}",
        f"PS4_FEATURE_STORAGE := {int(bool(features['storage']))}",
        f"PS4_FEATURE_TOUCHPAD := {int(bool(features['touchpad']))}",
        f"PS4_FEATURE_SCRIPT_GLOBALS := {int(bool(features['scriptGlobals']))}",
        f"PS4_SCRIPT_GLOBALS_JSON := {config['scriptGlobalsJson']}",
        f"PS4_SCRIPT_GLOBALS_HEADER := {config['scriptGlobalsHeader']}",
        f"PS4_CUSTOM_LANE_JSON := {config['customLaneJson']}",
        f"PS4_CUSTOM_LANE_HEADER := {config['customLaneHeader']}",
        "PS4_FEATURE_MODULE_C := " + " ".join(stem for kind, stem in sources if kind == "module"),
        "PS4_FEATURE_COMMON_C := " + " ".join(stem for kind, stem in sources if kind == "common"),
        "PS4_TARGET_DEFINES := " + " ".join(quote(define) for define in defines),
        "",
    ]
    return "\n".join(lines)

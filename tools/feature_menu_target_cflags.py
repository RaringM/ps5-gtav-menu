#!/usr/bin/env python3
"""Emit per-target CFLAGS for the feature-menu worker ELF.

Reads a pinned target manifest (e.g. data/targets/ppsa04264-01.010.002.json) and
prints a single line of -D flags that bake the correct frame-hook contract into
the worker:

  - GTAV_MENU_FRAME_HOOK_TARGET
  - GTAV_FRAME_HOOK_CONTINUATION
  - GTAV_FRAME_HOOK_STOLEN_BYTES
  - GTAV_FRAME_HOOK_GATEWAY_BYTES

The gateway bytes are computed from the manifest's brokerExpectedBytesCompact
(stolen prologue) and playerPedAnchor, replacing the RIP-relative mov with an
absolute mov from the anchor address.

Usage from make:
    FEATURE_MENU_TARGET_CFLAGS := $(shell $(PYTHON) tools/feature_menu_target_cflags.py --target $(GTAV_TARGET))
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from gtavmenu_tools.target_profile import load_manifest

REPO_ROOT = Path(__file__).resolve().parents[1]
TARGETS_DIR = REPO_ROOT / "data" / "targets"

NATIVE_CFLAGS = {
    "drawRect": "GTAV_MENU_DEFAULT_NATIVE_DRAW_RECT",
    "beginTextCommandDisplayText": "GTAV_MENU_DEFAULT_NATIVE_BEGIN_TEXT_COMMAND_DISPLAY_TEXT",
    "addTextComponentSubstringPlayerName": "GTAV_MENU_DEFAULT_NATIVE_ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME",
    "endTextCommandDisplayText": "GTAV_MENU_DEFAULT_NATIVE_END_TEXT_COMMAND_DISPLAY_TEXT",
    "setTextScale": "GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_SCALE",
    "setTextColour": "GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_COLOUR",
    "setTextFont": "GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_FONT",
    "setTextCentre": "GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_CENTRE",
    "setTextWrap": "GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_WRAP",
    "setTextJustification": "GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_JUSTIFICATION",
    "setTextDropShadow": "GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_DROP_SHADOW",
    "setTextOutline": "GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_OUTLINE",
    "getFrameCount": "GTAV_MENU_DEFAULT_NATIVE_GET_FRAME_COUNT",
    "getGameTimer": "GTAV_MENU_DEFAULT_NATIVE_GET_GAME_TIMER",
    "disableControlAction": "GTAV_MENU_DEFAULT_NATIVE_DISABLE_CONTROL_ACTION",
}
PREVIEW_NATIVE_CFLAGS = {"drawSprite": "GTAV_MENU_DEFAULT_NATIVE_DRAW_SPRITE"}
GLYPH_NATIVE_CFLAGS = {
    **PREVIEW_NATIVE_CFLAGS,
    "hasStreamedTextureDictLoaded": "GTAV_MENU_DEFAULT_NATIVE_HAS_STREAMED_TEXTURE_DICT_LOADED",
    "requestStreamedTextureDict": "GTAV_MENU_DEFAULT_NATIVE_REQUEST_STREAMED_TEXTURE_DICT",
}


def _int(s: str | int) -> int:
    return int(s, 0) if isinstance(s, str) else int(s)


def _hex_list(b: bytes) -> str:
    return ",".join(f"0x{b:02x}" for b in b)


def _make_cflag(name: str, value: str) -> str:
    # Wrap each flag so make/shell are happy even if the value contains commas.
    return f"-D{name}={value}"


def compute_gateway_bytes(stolen: bytes, anchor: int) -> bytes:
    """Replace the RIP-relative mov rax,[rip+disp] in the stolen prologue with
    an absolute mov rax,[anchor32].

    The expected layout (verified on PPSA04264 PLAYER_PED_ID) is:
        0..5   push rbp; mov rbp,rsp; push rbx; push rax
        6..12  48 8b 05 <disp32>   mov rax,[rip+disp]
        13..15 48 89 fb            mov rbx,rdi
    """
    if len(stolen) != 16:
        raise ValueError(f"stolen prologue must be 16 bytes, got {len(stolen)}")
    if stolen[6:9] != b"\x48\x8b\x05":
        raise ValueError(f"expected RIP-relative mov at offset 6, got {stolen[6:9].hex()}")
    if stolen[13:16] != b"\x48\x89\xfb":
        raise ValueError(f"expected mov rbx,rdi at offset 13, got {stolen[13:16].hex()}")
    if anchor > 0xFFFFFFFF:
        raise ValueError(f"anchor {anchor:#x} does not fit in 32-bit absolute address")
    prefix = stolen[:6]
    abs_mov = b"\x48\x8b\x04\x25" + anchor.to_bytes(4, "little")
    suffix = stolen[13:16]
    return prefix + abs_mov + suffix


def _native_addresses(source: Path) -> dict[str, int]:
    import json

    payload = json.loads(source.read_text(encoding="utf-8"))
    result: dict[str, int] = {}
    addresses = payload.get("addresses")
    if isinstance(addresses, list):
        for item in addresses:
            if isinstance(item, dict) and isinstance(item.get("name"), str):
                result[item["name"]] = _int(item["address"])
    targets = payload.get("targets")
    if isinstance(targets, list):
        for item in targets:
            if isinstance(item, dict) and isinstance(item.get("name"), str) and item.get("registrationFound") is True:
                result[item["name"]] = _int(item["handler"])
    if not result:
        raise ValueError(f"native address input has no usable rows: {source}")
    return result


def target_cflags(stem: str, *, include_preview: bool = False, include_glyphs: bool = False) -> list[str]:
    path = TARGETS_DIR / f"{stem}.json"
    if not path.is_file():
        raise FileNotFoundError(f"target manifest not found: {path}")
    manifest, profile = load_manifest(path)
    loader = manifest.get("loader", {})
    frame_hook_target = _int(loader["frameHookTarget"])
    continuation = _int(loader["brokerContinuation"])
    stolen_compact = loader["brokerExpectedBytesCompact"]
    stolen = bytes.fromhex(stolen_compact)
    anchor = _int(loader["playerPedAnchor"])
    gateway = compute_gateway_bytes(stolen, anchor)

    flags = [
        f"-DGTAV_MENU_FRAME_HOOK_TARGET=0x{frame_hook_target:x}ull",
        f"-DGTAV_FRAME_HOOK_CONTINUATION=0x{continuation:x}ull",
        f"-DGTAV_FRAME_HOOK_STOLEN_BYTES={_hex_list(stolen)}",
        f"-DGTAV_FRAME_HOOK_GATEWAY_BYTES={_hex_list(gateway)}",
    ]
    # Build-specific game-thread TLS layout (see include/gtavmenu/tls_layout.h). Both
    # offsets move together between builds (01.005.000: -0x130/+0x188, 01.010.002:
    # -0x140/+0x198) and are derived offline from the decrypted eboot, then pinned in the target
    # manifest. When the manifest pins a row, bake it in; otherwise the header default applies.
    tls_offset = loader.get("tlsGameCtxOffset")
    if tls_offset is not None:
        flags.append(f"-DGTAV_TLS_GAME_CTX_OFFSET={_int(tls_offset)}u")
    ctx_thread_offset = loader.get("tlsCtxNativeThreadOffset")
    if ctx_thread_offset is not None:
        flags.append(f"-DGTAV_TLS_CTX_NATIVE_THREAD_OFFSET={_int(ctx_thread_offset)}u")
    native_input = profile["nativeAddresses"]
    if not isinstance(native_input, dict):
        raise ValueError("validated target profile lacks native address input")
    addresses = _native_addresses(REPO_ROOT / str(native_input["json"]))
    selected = dict(NATIVE_CFLAGS)
    if include_preview:
        selected.update(PREVIEW_NATIVE_CFLAGS)
    if include_glyphs:
        selected.update(GLYPH_NATIVE_CFLAGS)
    for macro in selected.values():
        name = macro.removeprefix("GTAV_MENU_DEFAULT_NATIVE_")
        if name not in addresses:
            raise ValueError(f"native address input is missing {name}")
        flags.append(f"-D{macro}=0x{addresses[name]:x}ull")

    phase = manifest.get("renderPhase")
    if isinstance(phase, dict):
        cycle = phase.get("cycle")
        if not isinstance(cycle, dict):
            raise ValueError("renderPhase.cycle must be an object")
        flags.extend(
            [
                "-DGTAV_RENDER_PHASE_TARGET_VALID=1",
                f"-DGTAV_RENDER_PHASE_ORIGINAL=0x{_int(phase['original']):x}ull",
                f"-DGTAV_RENDER_PHASE_LEAF_VTABLE=0x{_int(phase['leafVtable']):x}ull",
                f"-DGTAV_RENDER_PHASE_TASK_ID=0x{_int(phase['taskId']):x}u",
            ]
        )
        for key, macro in (
            ("epoch", "GTAV_CYCLE_EPOCH_ADDR"),
            ("reset", "GTAV_CYCLE_RESET_ADDR"),
            ("producer", "GTAV_CYCLE_PRODUCER_ADDR"),
            ("consumer", "GTAV_CYCLE_CONSUMER_ADDR"),
            ("count0", "GTAV_CYCLE_COUNT0_ADDR"),
            ("count1", "GTAV_CYCLE_COUNT1_ADDR"),
            ("text", "GTAV_CYCLE_TEXT_ADDR"),
            ("counter", "GTAV_CYCLE_COUNTER_ADDR"),
        ):
            flags.append(f"-D{macro}=0x{_int(cycle[key]):x}ull")
    return flags


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--target", required=True, help="target manifest stem (e.g. ppsa04264-01.010.002)")
    ap.add_argument("--include-preview", action="store_true", help="emit the vehicle-preview sprite native")
    ap.add_argument("--include-glyphs", action="store_true", help="emit sprite and texture-dictionary natives")
    args = ap.parse_args(argv)
    try:
        flags = target_cflags(
            args.target,
            include_preview=args.include_preview,
            include_glyphs=args.include_glyphs,
        )
    except Exception as exc:
        print(f"feature_menu_target_cflags: {exc}", file=sys.stderr)
        return 1
    print(" ".join(flags))
    return 0


if __name__ == "__main__":
    sys.exit(main())

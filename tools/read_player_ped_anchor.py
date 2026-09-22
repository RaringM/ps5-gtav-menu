#!/usr/bin/env python3
"""Read-only validation of the player-ped readiness anchor for the watch lane.

PLAYER_PED_ID's handler (live 0x18f7170) resolves the local player ped as:

    base = *(uint64_t*)0x51cf258      # player-manager singleton
    ped  = *(uint64_t*)(base + 8)     # the local player CPed*; 0 => no player
    return (ped == 0) ? 0 : handle(ped)

So `ped != 0` means "the player exists in a loaded world" -- null at the menus /
during a load, valid once the player is in control. It is the load-readiness anchor used by
`./menu-ctl.sh watch`; game-mode selection is deliberately outside this probe's scope.

This tool ONLY reads memory -- it never writes or injects. Run it at each game state
(landing/menu, mid-load, and in-control) to confirm the chain is 0 until player
control before trusting it as the gate:

    python3 tools/read_player_ped_anchor.py --host <PS5_IP>

It prints base, ped, and READY/NOT-READY after requiring the exact foreground target process. The
address + offset default to the RE'd values but can be overridden for read-only experimentation.
"""

from __future__ import annotations

import argparse
from pathlib import PurePosixPath

from gtavmenu_tools.ps5debug import load_menu_client, read_u64
from gtavmenu_tools.target import APP_VERSION, PED_OFFSET, PLAYER_PED_ANCHOR, TITLE_ID, VERSION_SIGNATURE_ADDR

# Single-sourced from the target manifest (data/targets/*.json) via gtavmenu_tools.target,
# so a game update is re-pinned in one place instead of copied across every tool.
EXPECTED_TITLE_ID = TITLE_ID
EXPECTED_APP_VERSION = APP_VERSION

# RE'd from PLAYER_PED_ID's handler (see module docstring / docs/script-globals-design.md).
DEFAULT_ANCHOR_ADDR = PLAYER_PED_ANCHOR  # static holding the player-manager base pointer
DEFAULT_PED_OFFSET = PED_OFFSET  # base + 8 = local player CPed*


def resolve_player_ped(client, pid: int, anchor_addr: int, ped_offset: int) -> dict:
    """Walk the anchor chain read-only. Returns base, ped, and readiness."""
    base = read_u64(client, pid, anchor_addr)
    ped = read_u64(client, pid, base + ped_offset) if base else 0
    return {
        "anchorAddr": anchor_addr,
        "base": base,
        "pedAddr": (base + ped_offset) if base else 0,
        "ped": ped,
        "ready": bool(ped),
    }


def validate_foreground_process(client, foreground: dict, pid: int) -> list[str]:
    """Return fail-closed identity errors for the selected foreground GTA eboot."""
    errors: list[str] = []
    foreground_pid = int(foreground.get("pid") or 0)
    if foreground.get("titleId") != EXPECTED_TITLE_ID or foreground.get("appVersion") != EXPECTED_APP_VERSION:
        errors.append(f"foreground mismatch: {foreground.get('titleId')} {foreground.get('appVersion')}")
    if pid != foreground_pid:
        errors.append(f"selected pid {pid} is not foreground pid {foreground_pid}")
        return errors

    try:
        info = client.proc_info(pid)
    except Exception as exc:
        errors.append(f"proc_info failed: {exc}")
    else:
        if int(info.get("pid") or 0) != pid or info.get("titleId") != EXPECTED_TITLE_ID:
            errors.append("proc_info does not match the selected GTA process")
        executable_names = [str(info.get(key) or "") for key in ("name", "path")]
        if not any(PurePosixPath(value).name == "eboot.bin" for value in executable_names if value):
            errors.append("proc_info does not identify eboot.bin")

    try:
        maps = client.maps(pid)
    except Exception as exc:
        errors.append(f"process map query failed: {exc}")
    else:
        if not any(
            int(row.get("prot") or 0) & 0x4
            and int(row.get("start") or 0) <= VERSION_SIGNATURE_ADDR < int(row.get("end") or 0)
            for row in maps
        ):
            errors.append(f"no executable mapping contains pinned address {VERSION_SIGNATURE_ADDR:#x}")
    return errors


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host")
    ap.add_argument("--port", type=int, default=744)
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--pid", type=int, default=None)
    ap.add_argument("--addr", type=lambda s: int(s, 0), default=DEFAULT_ANCHOR_ADDR)
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=DEFAULT_PED_OFFSET)
    ap.add_argument(
        "--require-ready",
        action="store_true",
        help="exit non-zero if the player-world/load readiness predicate is false",
    )
    args = ap.parse_args(argv)
    if not args.host:
        ap.error("--host is required")

    mod = load_menu_client()
    cli = mod.Ps5DebugNg(args.host, args.port, timeout=args.timeout)
    fg = cli.foreground()
    pid = args.pid if args.pid is not None else int(fg.get("pid") or 0)
    identity_errors = validate_foreground_process(cli, fg, pid)
    if identity_errors:
        print("target identity validation failed:")
        for error in identity_errors:
            print(f"  - {error}")
        return 3

    r = resolve_player_ped(cli, pid, args.addr, args.offset)
    fg_after = cli.foreground()
    if (
        int(fg_after.get("pid") or 0) != pid
        or fg_after.get("titleId") != EXPECTED_TITLE_ID
        or fg_after.get("appVersion") != EXPECTED_APP_VERSION
    ):
        print("target identity changed while the readiness anchor was sampled")
        return 5
    print(f"pid={pid}")
    print(f"  anchor [{args.addr:#x}]            = base {r['base']:#x}")
    print(f"  ped    [base + {args.offset:#x}] @ {r['pedAddr']:#x} = {r['ped']:#x}")
    print(
        f"  => {'READY (player-world/load predicate only)' if r['ready'] else 'NOT READY (no player ped -- menu/loading)'}"
    )
    print()
    print("Run at the landing menu, mid-load, and in-control: expect NOT READY until")
    print("you have player control, then READY. If the transition is stable, this is the load anchor:")
    print(f"  SP_READY_ADDR={args.addr:#x} SP_READY_DEREF=1 SP_READY_DEREF_OFFSET={args.offset:#x} SP_READY_MODE=1")
    if args.require_ready and not r["ready"]:
        return 4  # pre-flight gate: caller asked us to fail unless the player ped exists
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

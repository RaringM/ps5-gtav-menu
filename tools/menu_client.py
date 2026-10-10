#!/usr/bin/env python3
"""Production ps5debug-NG client for menu status, control, and logs."""

from __future__ import annotations

import argparse
import json
import select
import socket
import struct
import sys
import time
from pathlib import Path

from gtavmenu_tools import parse_int, read_json
from gtavmenu_tools.target import APP_VERSION as _TARGET_APP_VERSION
from gtavmenu_tools.target import TITLE_ID as _TARGET_TITLE_ID

PACKET_MAGIC = 4289379276
CMD_SUCCESS_WIRE = 2147483648
CMD_VERSION = 3170893825
CMD_PROC_LIST = 3182034945
CMD_PROC_READ = 3182034946
CMD_PROC_WRITE = 3182034947
CMD_PROC_MAPS = 3182034948
CMD_PROC_INFO = 3182034954
CMD_PROC_SCAN_AOB = 3182036225
CMD_FOREGROUND_APP = 3185377286
EXPECTED_TITLE_ID = _TARGET_TITLE_ID
EXPECTED_APP_VERSION = _TARGET_APP_VERSION
GTAV_EBOOT_NAME = "eboot.bin"
GTAV_STATUS_MAGIC = 6071226489578280007
GTAV_STATUS_ABI_VERSION = 1
GTAV_STATUS_EVENT_COUNT = 16
GTAV_STATUS_HEADER_FORMAT = "<QIIIIQIIIIIIIIQQII32s32s64s128sII"
GTAV_STATUS_EVENT_FORMAT = "<QII80s"
GTAV_STATUS_HEADER_SIZE = struct.calcsize(GTAV_STATUS_HEADER_FORMAT)
GTAV_STATUS_EVENT_SIZE = struct.calcsize(GTAV_STATUS_EVENT_FORMAT)
GTAV_STATUS_SIZE = GTAV_STATUS_HEADER_SIZE + GTAV_STATUS_EVENT_SIZE * GTAV_STATUS_EVENT_COUNT
GTAV_STATUS_MAGIC_BYTES = struct.pack("<Q", GTAV_STATUS_MAGIC)
GTAV_LOG_RING_MAGIC = 5928794623739778119
GTAV_LOG_RING_ABI_VERSION = 1
GTAV_LOG_RING_ENTRY_COUNT = 64
GTAV_LOG_RING_HEADER_FORMAT = "<QIIII"
GTAV_LOG_RING_ENTRY_FORMAT = "<QII128s"
GTAV_LOG_RING_HEADER_SIZE = struct.calcsize(GTAV_LOG_RING_HEADER_FORMAT)
GTAV_LOG_RING_ENTRY_SIZE = struct.calcsize(GTAV_LOG_RING_ENTRY_FORMAT)
GTAV_LOG_RING_SIZE = GTAV_LOG_RING_HEADER_SIZE + GTAV_LOG_RING_ENTRY_SIZE * GTAV_LOG_RING_ENTRY_COUNT
GTAV_LOG_RING_MAGIC_BYTES = struct.pack("<Q", GTAV_LOG_RING_MAGIC)
GTAV_COMMAND_MAILBOX_MAGIC = 6363377698424837191
GTAV_COMMAND_MAILBOX_ABI_VERSION = 1
GTAV_COMMAND_MAILBOX_FORMAT = "<QIIQQIIQ32s80s"
GTAV_COMMAND_MAILBOX_SIZE = struct.calcsize(GTAV_COMMAND_MAILBOX_FORMAT)
GTAV_COMMAND_MAILBOX_MAGIC_BYTES = struct.pack("<Q", GTAV_COMMAND_MAILBOX_MAGIC)
GTAV_COMMAND_REQUEST_SEQUENCE_OFFSET = struct.calcsize("<QII")
GTAV_COMMAND_FIELDS_OFFSET = struct.calcsize("<QIIQQ")
GTAV_COMMAND_FIELDS_FORMAT = "<IIQ"
STATE_NAMES = {0: "zero", 1: "starting", 2: "running", 3: "stopping", 4: "stopped", 5: "error"}
HOOK_STATUS_NAMES = {
    0: "unknown",
    1: "disabled",
    2: "validating",
    3: "dry_run_passed",
    4: "installed",
    5: "validation_failed",
    6: "install_failed",
    7: "restored",
}
ERROR_NAMES = {
    0: "none",
    1: "invalid_init",
    2: "hook_missing_expected",
    3: "hook_validation_failed",
    4: "hook_install_failed",
    5: "thread_create_failed",
    6: "live_hook_disabled",
}
EVENT_NAMES = {
    0: "none",
    1: "start",
    2: "init",
    3: "log_open",
    4: "notify",
    5: "hook_disabled",
    6: "hook_dry_run_passed",
    7: "hook_installed",
    8: "hook_validation_failed",
    9: "hook_install_failed",
    10: "tick",
    11: "toggle",
    12: "command",
    13: "shutdown",
    14: "thread_failed",
    15: "config_loaded",
    16: "config_default",
    17: "native_bridge",
    18: "native_shell",
}
LEVEL_NAMES = {0: "error", 1: "warn", 2: "info", 3: "debug", 4: "trace"}
LOG_CATEGORY_NAMES = {0: "general", 1: "inject", 2: "hook", 3: "pad", 4: "feat", 5: "native", 6: "menu", 7: "config"}
COMMAND_NAMES = {
    0: "none",
    1: "toggle",
    2: "next",
    3: "prev",
    4: "select",
    5: "back",
    6: "stop",
    7: "telemetry",
    8: "god",
    9: "heal",
    10: "wanted",
    11: "show",
    12: "hide",
    13: "render_interval",
    14: "worker_hz",
    15: "spawn",
    16: "left",
    17: "right",
    18: "load_module",
    19: "activate_action",
    20: "toast_ticks",
    21: "hotkey",
    22: "page_prev",
    23: "page_next",
    24: "home",
    25: "end",
    26: "letter_prev",
    27: "letter_next",
    28: "back_root",
    29: "pin",
    30: "render_diag_mode",
    31: "render_diag_capture",
    32: "render_cycle_probe",
    33: "render_path_low",
    34: "render_path_high",
    35: "render_path_control",
    36: "render_bank_control",
    37: "render_phase_slot",
    38: "render_phase_control",
    39: "activate_action_param",
}
COMMAND_IDS = {
    "toggle": 1,
    "next": 2,
    "down": 2,
    "prev": 3,
    "previous": 3,
    "up": 3,
    "select": 4,
    "activate": 4,
    "back": 5,
    "stop": 6,
    "telemetry": 7,
    "god": 8,
    "heal": 9,
    "armor": 9,
    "heal-armor": 9,
    "heal_armor": 9,
    "wanted": 10,
    "clear-wanted": 10,
    "clear_wanted": 10,
    "show": 11,
    "refresh": 11,
    "redraw": 11,
    "hide": 12,
    "render_interval": 13,
    "set_render_interval": 13,
    "render-interval": 13,
    "worker_hz": 14,
    "worker-hz": 14,
    "render-diag-mode": 30,
    "render-diag-capture": 31,
    "render_diag_mode": 30,
    "render_diag_capture": 31,
    "render-cycle-probe": 32,
    "render-path-low": 33,
    "render-path-high": 34,
    "render-path-control": 35,
    "render-phase-slot": 37,
    "render-phase-control": 38,
    "render-bank-control": 36,
    "render_cycle_probe": 32,
    "set_worker_hz": 14,
    "spawn": 15,
    "spawn_vehicle": 15,
    "left": 16,
    "right": 17,
    "load_module": 18,
    "load-module": 18,
    "activate_action": 19,
    "activate-action": 19,
    "activate_action_param": 39,
    "activate-action-param": 39,
    "toast_ticks": 20,
    "set_toast_ticks": 20,
    "toast-ticks": 20,
    "hotkey": 21,
    "page_prev": 22,
    "page-prev": 22,
    "page_next": 23,
    "page-next": 23,
    "home": 24,
    "end": 25,
    "letter_prev": 26,
    "letter-prev": 26,
    "letter_next": 27,
    "letter-next": 27,
    "back_root": 28,
    "back-root": 28,
    "pin": 29,
}
COMMAND_STATUS_NAMES = {0: "idle", 1: "pending", 2: "acked", 3: "unknown"}


def _cstr(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("utf-8", "replace")


def _hex(data: bytes) -> str:
    return data.hex(" ")


def _hex_addr(value: int) -> str:
    return f"0x{value:x}"


def _map_size(row: dict[str, object]) -> int:
    return int(row["end"]) - int(row["start"])


def _find_map(rows: list[dict[str, object]], address: int, length: int = 1) -> dict[str, object] | None:
    end = address + max(length, 1)
    for row in rows:
        if int(row["start"]) <= address and end <= int(row["end"]):
            return row
    return None


class Ps5DebugNg:

    def __init__(self, host: str, port: int = 744, timeout: float = 10.0) -> None:
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)

    def close(self) -> None:
        self.sock.close()

    def _read_exact(self, size: int) -> bytes:
        chunks: list[bytes] = []
        remaining = size
        while remaining:
            chunk = self.sock.recv(remaining)
            if not chunk:
                raise ConnectionError("ps5debug-NG connection closed")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def _send_header(self, cmd: int, body: bytes = b"") -> None:
        self.sock.sendall(struct.pack("<III", PACKET_MAGIC, cmd, len(body)) + body)

    def _status(self) -> int:
        return struct.unpack("<I", self._read_exact(4))[0]

    def _expect_success(self) -> None:
        status = self._status()
        if status != CMD_SUCCESS_WIRE:
            raise RuntimeError(f"ps5debug-NG command failed: status=0x{status:08x}")

    def _consume_optional_success(self) -> None:
        old_timeout = self.sock.gettimeout()
        self.sock.settimeout(0.05)
        try:
            status = self._status()
        except TimeoutError:
            return
        finally:
            self.sock.settimeout(old_timeout)
        if status != CMD_SUCCESS_WIRE:
            raise RuntimeError(f"ps5debug-NG trailing status failed: status=0x{status:08x}")

    def version(self) -> str:
        self._send_header(CMD_VERSION)
        length = struct.unpack("<I", self._read_exact(4))[0]
        return self._read_exact(length).decode("utf-8", "replace")

    def foreground(self) -> dict[str, object]:
        self._send_header(CMD_FOREGROUND_APP)
        self._expect_success()
        raw = self._read_exact(140)
        pid, title_id, content_id, name, app_ver = struct.unpack("<I16s64s40s16s", raw)
        return {
            "pid": pid,
            "titleId": _cstr(title_id),
            "contentId": _cstr(content_id),
            "name": _cstr(name),
            "appVersion": _cstr(app_ver),
        }

    def processes(self) -> list[dict[str, object]]:
        self._send_header(CMD_PROC_LIST)
        self._expect_success()
        count = struct.unpack("<I", self._read_exact(4))[0]
        rows = []
        for _ in range(count):
            raw = self._read_exact(36)
            name, pid = struct.unpack("<32si", raw)
            rows.append({"name": _cstr(name), "pid": pid})
        return rows

    def proc_info(self, pid: int) -> dict[str, object]:
        self._send_header(CMD_PROC_INFO, struct.pack("<I", pid))
        self._expect_success()
        raw = self._read_exact(188)
        parsed = struct.unpack("<I40s64s16s64s", raw)
        return {
            "pid": parsed[0],
            "name": _cstr(parsed[1]),
            "path": _cstr(parsed[2]),
            "titleId": _cstr(parsed[3]),
            "contentId": _cstr(parsed[4]),
        }

    def maps(self, pid: int) -> list[dict[str, object]]:
        self._send_header(CMD_PROC_MAPS, struct.pack("<I", pid))
        self._expect_success()
        count = struct.unpack("<I", self._read_exact(4))[0]
        maps = []
        for _ in range(count):
            raw = self._read_exact(58)
            name, start, end, offset, prot = struct.unpack("<32sQQQH", raw)
            maps.append({"name": _cstr(name), "start": start, "end": end, "offset": offset, "prot": prot})
        return maps

    def read(self, pid: int, address: int, length: int) -> bytes:
        self._send_header(CMD_PROC_READ, struct.pack("<IQI", pid, address, length))
        self._expect_success()
        return self._read_exact(length)

    def write(self, pid: int, address: int, data: bytes) -> None:
        self._send_header(CMD_PROC_WRITE, struct.pack("<IQI", pid, address, len(data)))
        self._expect_success()
        self.sock.sendall(data)
        self._expect_success()

    def scan_aob_nth(
        self,
        pid: int,
        address: int,
        length: int,
        pattern: bytes,
        mask: bytes | None = None,
        match_index: int = 1,
        stop_unique: bool = False,
    ) -> int:
        if not pattern:
            raise ValueError("empty AOB pattern")
        if mask is None:
            mask = b"\x01" * len(pattern)
        if len(mask) != len(pattern):
            raise ValueError("AOB mask length must match pattern length")
        if match_index < 1 or match_index > 255:
            raise ValueError("match_index must fit in uint8_t")
        body = struct.pack("<IQIBBI", pid, address, length, match_index, 1 if stop_unique else 0, len(pattern))
        self._send_header(CMD_PROC_SCAN_AOB, body)
        self._expect_success()
        self.sock.sendall(pattern + mask)
        self._expect_success()
        match = struct.unpack("<Q", self._read_exact(8))[0]
        self._expect_success()
        return match

    def scan_aob(
        self, pid: int, address: int, length: int, pattern: bytes, mask: bytes | None = None, max_matches: int = 32
    ) -> list[int]:
        matches: list[int] = []
        for index in range(1, max_matches + 1):
            match = self.scan_aob_nth(pid, address, length, pattern, mask, index)
            if not match:
                break
            matches.append(match)
        return matches


def supported_foreground_target(fg: dict[str, object]) -> str:
    """Identify a supported title/version for controls; injection verifies memory pins separately."""
    from gtavmenu_tools.universal_package import TARGETS

    title = str(fg.get("titleId", ""))
    version = str(fg.get("appVersion", ""))
    matches = [
        target
        for target in TARGETS
        if target.split("-", 1) == [title.lower(), version] and target.split("-", 1)[0].upper() == title
    ]
    if len(matches) != 1 or int(fg.get("pid") or 0) <= 0:
        raise RuntimeError(f"foreground is not one supported GTA title/version: {fg}")
    return matches[0]


def require_expected_foreground(client: Ps5DebugNg, args: argparse.Namespace) -> dict[str, object]:
    fg = client.foreground()
    if getattr(args, "auto_target", False):
        if getattr(args, "allow_non_gtav", False):
            raise RuntimeError("--auto-target cannot be combined with --allow-non-gtav")
        supported_foreground_target(fg)
        return fg
    if getattr(args, "allow_non_gtav", False):
        return fg
    expected_title = getattr(args, "expect_title_id", EXPECTED_TITLE_ID)
    expected_version = getattr(args, "expect_app_version", EXPECTED_APP_VERSION)
    title = str(fg.get("titleId", ""))
    version = str(fg.get("appVersion", ""))
    if expected_title and title != expected_title:
        raise RuntimeError(f"foreground title mismatch: expected {expected_title}, got {fg}")
    if expected_version and version != expected_version:
        raise RuntimeError(f"foreground app version mismatch: expected {expected_version}, got {fg}")
    return fg


def resolve_pid(client: Ps5DebugNg, args: argparse.Namespace) -> tuple[int, dict[str, object]]:
    fg = require_expected_foreground(client, args)
    return (int(args.pid) if getattr(args, "pid", None) is not None else int(fg["pid"]), fg)


def gtav_scan_pids(client: Ps5DebugNg, args: argparse.Namespace) -> tuple[list[int], dict[str, object]]:
    """Pids to scan for an injected worker block, best-first, plus the foreground app dict.

    An explicit --pid wins outright. Otherwise scan the foreground eboot first, then every
    sibling eboot.bin: GTA V runs two same-title eboots (the decoy) and the loader may have
    injected the one that is NOT foreground -- which is exactly why a bare `status`/`worker-log`
    (no --pid) comes up empty even though the menu is live, and why menu-ctl's start flow pins
    --pid from the inject log (see report_inject_result). Scanning a sibling only happens when
    the foreground pid yields nothing, so the correctly-targeted case pays no extra cost."""
    fg = require_expected_foreground(client, args)
    if getattr(args, "pid", None) is not None:
        return ([int(args.pid)], fg)
    fg_pid = int(fg["pid"])
    pids = [fg_pid]
    try:
        for proc in client.processes():
            pid = int(proc["pid"])
            if pid != fg_pid and str(proc.get("name", "")) == GTAV_EBOOT_NAME:
                pids.append(pid)
    except Exception:
        pass
    return (pids, fg)


def decode_status(raw: bytes, address: int | None = None) -> dict[str, object]:
    if len(raw) < GTAV_STATUS_SIZE:
        raise ValueError(f"status blob too small: {len(raw)} < {GTAV_STATUS_SIZE}")
    values = struct.unpack_from(GTAV_STATUS_HEADER_FORMAT, raw, 0)
    (
        magic,
        abi_version,
        struct_size,
        state,
        flags,
        ticks,
        initialized,
        visible,
        stop_requested,
        init_result,
        last_error,
        hook_status,
        log_open_result,
        hook_side_ticks,
        game_base,
        hook_addr,
        hook_length,
        expected_len,
        expected,
        original,
        target_id,
        log_path,
        event_write_index,
        event_count,
    ) = values
    if magic != GTAV_STATUS_MAGIC:
        raise ValueError(f"bad status magic: 0x{magic:016x}")
    if abi_version != GTAV_STATUS_ABI_VERSION:
        raise ValueError(f"unsupported status ABI: {abi_version}")
    if struct_size < GTAV_STATUS_HEADER_SIZE or struct_size > len(raw):
        raise ValueError(f"invalid status struct size: {struct_size}")
    count = min(event_count, GTAV_STATUS_EVENT_COUNT)
    start = event_write_index - count if event_write_index >= count else 0
    events = []
    for logical in range(count):
        index = (start + logical) % GTAV_STATUS_EVENT_COUNT
        offset = GTAV_STATUS_HEADER_SIZE + index * GTAV_STATUS_EVENT_SIZE
        event_tick, code, level, message = struct.unpack_from(GTAV_STATUS_EVENT_FORMAT, raw, offset)
        events.append(
            {
                "slot": index,
                "tick": event_tick,
                "code": code,
                "name": EVENT_NAMES.get(code, f"unknown_{code}"),
                "level": level,
                "levelName": LEVEL_NAMES.get(level, f"unknown_{level}"),
                "message": _cstr(message),
            }
        )
    return {
        "address": address,
        "addressHex": _hex_addr(address) if address is not None else None,
        "magic": f"0x{magic:016x}",
        "abiVersion": abi_version,
        "structSize": struct_size,
        "state": state,
        "stateName": STATE_NAMES.get(state, f"unknown_{state}"),
        "flags": flags,
        "flagsHex": f"0x{flags:08x}",
        "ticks": ticks,
        "initialized": bool(initialized),
        "visible": bool(visible),
        "stopRequested": bool(stop_requested),
        "initResult": init_result,
        "lastError": last_error,
        "lastErrorName": ERROR_NAMES.get(last_error, f"unknown_{last_error}"),
        "hookStatus": hook_status,
        "hookStatusName": HOOK_STATUS_NAMES.get(hook_status, f"unknown_{hook_status}"),
        "logOpenResult": log_open_result,
        "reserved0": hook_side_ticks,
        "hookSideTicks": hook_side_ticks,
        "gameBase": game_base,
        "gameBaseHex": _hex_addr(game_base),
        "hookAddr": hook_addr,
        "hookAddrHex": _hex_addr(hook_addr),
        "hookLength": hook_length,
        "expectedLen": expected_len,
        "expected": _hex(expected[: min(expected_len, len(expected))]),
        "original": _hex(original[: min(hook_length, len(original))]),
        "targetId": _cstr(target_id),
        "logPath": _cstr(log_path),
        "eventWriteIndex": event_write_index,
        "eventCount": event_count,
        "events": events,
    }


def status_events_since(
    previous_write_index: int, status: dict[str, object], *, allow_overflow: bool = False
) -> tuple[int, list[dict[str, object]]]:
    """Return newly published ring entries or fail if the bounded ring overran.

    With allow_overflow the retained (newest) entries are returned instead; a feature-action wait
    only needs the terminal result, which is always the newest entry of its action.
    """

    current_write_index = int(status.get("eventWriteIndex") or 0)
    if current_write_index < previous_write_index:
        raise RuntimeError(f"status event index moved backwards: {previous_write_index}->{current_write_index}")
    delta = current_write_index - previous_write_index
    events = list(status.get("events") or [])
    if delta > len(events) and not allow_overflow:
        raise RuntimeError(f"status event ring overflow: need {delta} entries but snapshot retains {len(events)}")
    return current_write_index, events[max(0, len(events) - delta) :] if delta else []


def feature_action_terminal_event(events: list[dict[str, object]], action_name: str) -> dict[str, object] | None:
    queued = f"feature ok action={action_name} message=queued for main thread"
    prefixes = tuple(f"feature {result} action={action_name} " for result in ("ok", "unavailable", "failed"))
    for event in events:
        message = str(event.get("message") or "")
        if message != queued and message.startswith(prefixes):
            return event
    return None


def feature_action_event_succeeded(event: dict[str, object] | None, action_name: str) -> bool:
    message = str((event or {}).get("message") or "")
    return message.startswith(f"feature ok action={action_name} ") and not message.endswith(
        "message=queued for main thread"
    )


def decode_command_mailbox(raw: bytes, address: int | None = None) -> dict[str, object]:
    if len(raw) < GTAV_COMMAND_MAILBOX_SIZE:
        raise ValueError(f"command mailbox blob too small: {len(raw)} < {GTAV_COMMAND_MAILBOX_SIZE}")
    (
        magic,
        abi_version,
        struct_size,
        request_sequence,
        ack_sequence,
        command,
        status,
        argument,
        last_command,
        last_result,
    ) = struct.unpack_from(GTAV_COMMAND_MAILBOX_FORMAT, raw, 0)
    if magic != GTAV_COMMAND_MAILBOX_MAGIC:
        raise ValueError(f"bad command mailbox magic: 0x{magic:016x}")
    if abi_version != GTAV_COMMAND_MAILBOX_ABI_VERSION:
        raise ValueError(f"unsupported command mailbox ABI: {abi_version}")
    if struct_size < GTAV_COMMAND_MAILBOX_SIZE or struct_size > len(raw):
        raise ValueError(f"invalid command mailbox struct size: {struct_size}")
    return {
        "address": address,
        "addressHex": _hex_addr(address) if address is not None else None,
        "magic": f"0x{magic:016x}",
        "abiVersion": abi_version,
        "structSize": struct_size,
        "requestSequence": request_sequence,
        "ackSequence": ack_sequence,
        "command": command,
        "commandName": COMMAND_NAMES.get(command, f"unknown_{command}"),
        "status": status,
        "statusName": COMMAND_STATUS_NAMES.get(status, f"unknown_{status}"),
        "argument": argument,
        "argumentHex": _hex_addr(argument),
        "lastCommand": _cstr(last_command),
        "lastResult": _cstr(last_result),
    }


def find_command_mailbox_candidates(
    client: Ps5DebugNg,
    pid: int,
    maps: list[dict[str, object]],
    max_matches_per_map: int,
    max_map_size: int,
    scan_all_maps: bool,
    injected_range_size: int,
) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    candidates = []
    skipped = []
    if scan_all_maps:
        scan_ranges = [
            {"name": row["name"], "start": row["start"], "end": row["end"], "prot": row["prot"], "source": "full_map"}
            for row in maps
        ]
    else:
        scan_ranges = likely_injected_ranges(maps, injected_range_size)
    for row in scan_ranges:
        start = int(row["start"])
        size = int(row["end"]) - start
        prot = int(row["prot"])
        if size < len(GTAV_COMMAND_MAILBOX_MAGIC_BYTES):
            continue
        if not scan_all_maps and (not prot & 2):
            continue
        if not scan_all_maps and size > max_map_size:
            skipped.append({"startHex": _hex_addr(start), "size": size, "prot": prot, "reason": "map too large"})
            continue
        try:
            matches = client.scan_aob(
                pid,
                start,
                size,
                GTAV_COMMAND_MAILBOX_MAGIC_BYTES,
                b"\x01" * len(GTAV_COMMAND_MAILBOX_MAGIC_BYTES),
                max_matches_per_map,
            )
        except Exception as exc:
            skipped.append({"startHex": _hex_addr(start), "size": size, "prot": prot, "reason": str(exc)})
            continue
        for address in matches:
            try:
                candidates.append(decode_command_mailbox(client.read(pid, address, GTAV_COMMAND_MAILBOX_SIZE), address))
            except Exception as exc:
                skipped.append(
                    {
                        "startHex": _hex_addr(address),
                        "size": GTAV_COMMAND_MAILBOX_SIZE,
                        "prot": prot,
                        "reason": str(exc),
                    }
                )
    return (candidates, skipped)


def find_status_candidates(
    client: Ps5DebugNg,
    pid: int,
    maps: list[dict[str, object]],
    max_matches_per_map: int,
    max_map_size: int,
    scan_all_maps: bool,
    injected_range_size: int,
) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    candidates = []
    skipped = []
    if scan_all_maps:
        scan_ranges = [
            {"name": row["name"], "start": row["start"], "end": row["end"], "prot": row["prot"], "source": "full_map"}
            for row in maps
        ]
    else:
        scan_ranges = likely_injected_ranges(maps, injected_range_size)
    for row in scan_ranges:
        start = int(row["start"])
        end = int(row["end"])
        size = end - start
        prot = int(row["prot"])
        if size < len(GTAV_STATUS_MAGIC_BYTES):
            continue
        if not scan_all_maps and (not prot & 2):
            continue
        if not scan_all_maps and size > max_map_size:
            skipped.append({"startHex": _hex_addr(start), "size": size, "prot": prot, "reason": "map too large"})
            continue
        try:
            matches = client.scan_aob(
                pid, start, size, GTAV_STATUS_MAGIC_BYTES, b"\x01" * len(GTAV_STATUS_MAGIC_BYTES), max_matches_per_map
            )
        except Exception as exc:
            skipped.append(
                {
                    "name": row.get("name"),
                    "source": row.get("source"),
                    "startHex": _hex_addr(start),
                    "endHex": _hex_addr(end),
                    "size": size,
                    "prot": prot,
                    "reason": str(exc),
                }
            )
            continue
        if not matches:
            skipped.append(
                {
                    "name": row.get("name"),
                    "source": row.get("source"),
                    "startHex": _hex_addr(start),
                    "endHex": _hex_addr(end),
                    "size": size,
                    "prot": prot,
                    "reason": "no status magic matches",
                }
            )
            continue
        for address in matches:
            try:
                candidates.append(decode_status(client.read(pid, address, GTAV_STATUS_SIZE), address))
            except Exception as exc:
                skipped.append(
                    {"startHex": _hex_addr(address), "size": GTAV_STATUS_SIZE, "prot": prot, "reason": str(exc)}
                )
    return (candidates, skipped)


def likely_injected_ranges(rows: list[dict[str, object]], max_range_size: int) -> list[dict[str, object]]:
    ordered = sorted(rows, key=lambda item: int(item["start"]))
    ranges = []
    seen: set[int] = set()

    def append_range(row: dict[str, object], start: int, end: int, source: str) -> None:
        if start in seen:
            return
        seen.add(start)
        ranges.append({"name": row["name"], "start": start, "end": end, "prot": row["prot"], "source": source})

    for index, row in enumerate(ordered):
        name = str(row["name"])
        prot = int(row["prot"])
        size = _map_size(row)
        if not name.startswith("anon:") or not prot & 2 or size > max_range_size:
            continue
        start = int(row["start"])
        end = int(row["end"])
        if prot & 6 != 6:
            append_range(row, start, end, "small_writable_anon")
            continue
        cursor = index + 1
        while cursor < len(ordered) and int(ordered[cursor]["start"]) == end:
            next_row = ordered[cursor]
            if str(next_row["name"]) != name:
                break
            if int(next_row["prot"]) & 4:
                break
            next_end = int(next_row["end"])
            if next_end - start > max_range_size:
                break
            end = next_end
            cursor += 1
        append_range(row, start, end, "likely_injected_anon")
    return sorted(ranges, key=lambda item: int(item["start"]), reverse=True)


def _parse_hex_bytes(value: str | bytes | None) -> bytes:
    if value is None:
        return b""
    if isinstance(value, bytes):
        return value
    tokens = str(value).replace(",", " ").split()
    if not tokens:
        return b""
    cleaned = []
    for token in tokens:
        if token.lower().startswith("0x"):
            token = token[2:]
        cleaned.append(token)
    return bytes.fromhex("".join(cleaned))


def _status_hex_field(row: dict[str, object], field: str) -> bytes:
    return _parse_hex_bytes(str(row.get(field, "")))


def _status_is_running(row: dict[str, object]) -> bool:
    return (
        int(row.get("state", 0)) == 2
        and bool(row.get("initialized", False))
        and (not bool(row.get("stopRequested", False)))
    )


def _status_rank(row: dict[str, object]) -> tuple[int, int, int, int, int]:
    return (
        1 if _status_is_running(row) else 0,
        1 if int(row.get("lastError", 0)) == 0 else 0,
        int(row.get("ticks", 0)),
        int(row.get("eventWriteIndex", 0)),
        int(row.get("address") or 0),
    )


def _hook_status_matches(row: dict[str, object], expected: str) -> bool:
    actual_name = str(row.get("hookStatusName", "")).lower()
    actual_value = int(row.get("hookStatus", -1))
    for token in expected.split(","):
        wanted = token.strip().lower().replace("-", "_")
        if not wanted:
            continue
        if wanted == actual_name:
            return True
        try:
            if int(wanted, 0) == actual_value:
                return True
        except ValueError:
            pass
    return False


def _status_filter_reasons(row: dict[str, object], args: argparse.Namespace) -> list[str]:
    reasons = []
    target_id = getattr(args, "target_id", None)
    if target_id and str(row.get("targetId", "")) != target_id:
        reasons.append(f"targetId {row.get('targetId', '')!r} != {target_id!r}")
    hook_addr = getattr(args, "hook_addr", None)
    if hook_addr is not None and int(row.get("hookAddr", 0)) != parse_int(hook_addr):
        reasons.append(f"hookAddr {row.get('hookAddrHex')} != {_hex_addr(parse_int(hook_addr))}")
    hook_length = getattr(args, "hook_length", None)
    if hook_length is not None and int(row.get("hookLength", 0)) != parse_int(hook_length):
        reasons.append(f"hookLength {row.get('hookLength')} != {parse_int(hook_length)}")
    hook_status_name = getattr(args, "hook_status_name", None)
    if hook_status_name and (not _hook_status_matches(row, hook_status_name)):
        reasons.append(f"hookStatusName {row.get('hookStatusName')} != {hook_status_name}")
    expected_original = getattr(args, "expected_original", None)
    if expected_original:
        expected = _parse_hex_bytes(expected_original)
        actual = _status_hex_field(row, "original")
        if actual != expected:
            reasons.append(f"original {actual.hex(' ')} != {expected.hex(' ')}")
    expected_bytes = getattr(args, "expected_bytes", None)
    if expected_bytes:
        expected = _parse_hex_bytes(expected_bytes)
        actual = _status_hex_field(row, "expected")
        if actual != expected:
            reasons.append(f"expected {actual.hex(' ')} != {expected.hex(' ')}")
    if getattr(args, "require_running", False) and (not _status_is_running(row)):
        reasons.append(f"state {row.get('stateName')} is not running")
    if getattr(args, "require_stopped", False) and int(row.get("state", 0)) != 4:
        reasons.append(f"state {row.get('stateName')} is not stopped")
    if getattr(args, "require_no_error", False) and int(row.get("lastError", 0)) != 0:
        reasons.append(f"lastError {row.get('lastErrorName')} is not none")
    return reasons


def _select_status_candidate(
    candidates: list[dict[str, object]], args: argparse.Namespace
) -> tuple[dict[str, object], list[dict[str, object]], list[dict[str, object]]]:
    matched = []
    rejected = []
    for row in candidates:
        reasons = _status_filter_reasons(row, args)
        if reasons:
            rejected.append(
                {
                    "address": row.get("address"),
                    "addressHex": row.get("addressHex"),
                    "targetId": row.get("targetId"),
                    "stateName": row.get("stateName"),
                    "hookStatusName": row.get("hookStatusName"),
                    "ticks": row.get("ticks"),
                    "reasons": reasons,
                }
            )
        else:
            matched.append(row)
    if not matched:
        raise RuntimeError(f"no GTAVMenu status block matched filters; rejected={rejected[:8]}")
    return (max(matched, key=_status_rank), matched, rejected)


def _write_json_output(path: str | Path, payload: dict[str, object]) -> None:
    output = Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def _command_id_from_name(value: str) -> int:
    text = value.strip().lower().replace(" ", "-")
    if not text:
        raise ValueError("empty command")
    if text in COMMAND_IDS:
        return COMMAND_IDS[text]
    try:
        command = int(text, 0)
    except ValueError as exc:
        raise ValueError(f"unknown menu command: {value}") from exc
    if command not in COMMAND_NAMES:
        raise ValueError(f"unknown menu command id: {command}")
    return command


def _mailbox_rank(row: dict[str, object]) -> tuple[int, int, int]:
    return (int(row.get("ackSequence", 0)), int(row.get("requestSequence", 0)), int(row.get("address") or 0))


def _select_command_mailbox_candidate(
    candidates: list[dict[str, object]], maps: list[dict[str, object]], selected_status: dict[str, object] | None
) -> tuple[dict[str, object], list[dict[str, object]], list[dict[str, object]]]:
    if not candidates:
        raise RuntimeError("no GTAVMenu command mailbox found")
    rejected = []
    status_address = int(selected_status.get("address") or 0) if selected_status else 0
    status_map = _find_map(maps, status_address, GTAV_STATUS_SIZE) if status_address else None
    if status_map:
        start = int(status_map["start"])
        end = int(status_map["end"])
        matched = [row for row in candidates if start <= int(row.get("address") or 0) < end]
        for row in candidates:
            if row not in matched:
                rejected.append(
                    {
                        "address": row.get("address"),
                        "addressHex": row.get("addressHex"),
                        "reason": f"not in selected status map {_hex_addr(start)}-{_hex_addr(end)}",
                    }
                )
        if matched:
            return (max(matched, key=_mailbox_rank), matched, rejected)
    return (max(candidates, key=_mailbox_rank), candidates, rejected)


def _apply_candidate_status_filters(args: argparse.Namespace) -> None:
    manifest_path = getattr(args, "manifest", None)
    candidate_id = getattr(args, "id", None)
    if not manifest_path and (not candidate_id):
        return
    if not manifest_path or not candidate_id:
        raise RuntimeError("--manifest and --id must be supplied together")
    manifest = read_json(Path(manifest_path))
    hooks = manifest.get("hooks", [])
    if not isinstance(hooks, list):
        raise RuntimeError("manifest hooks field is not a list")
    candidate = next((hook for hook in hooks if isinstance(hook, dict) and hook.get("id") == candidate_id), None)
    if not candidate:
        raise RuntimeError(f"candidate not found in manifest: {candidate_id}")
    if getattr(args, "target_id", None) is None:
        args.target_id = str(candidate_id)
    if getattr(args, "hook_addr", None) is None:
        args.hook_addr = str(candidate.get("liveAddress", "0x0"))
    if getattr(args, "hook_length", None) is None:
        args.hook_length = int(candidate.get("hookLength", 0))
    if getattr(args, "expected_original", None) is None:
        expected = candidate.get("expectedCompact") or candidate.get("expected")
        if expected:
            args.expected_original = str(expected)


def _collect_status_candidates(
    client: Ps5DebugNg, pid: int, args: argparse.Namespace
) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    if getattr(args, "address", None):
        address = parse_int(args.address)
        return ([decode_status(client.read(pid, address, GTAV_STATUS_SIZE), address)], [])
    if getattr(args, "base", None):
        address = parse_int(args.base) + parse_int(getattr(args, "offset", "0x0"))
        return ([decode_status(client.read(pid, address, GTAV_STATUS_SIZE), address)], [])
    maps = client.maps(pid)
    return find_status_candidates(
        client,
        pid,
        maps,
        args.max_matches_per_map,
        parse_int(args.max_map_size),
        args.scan_all_maps,
        parse_int(args.injected_range_size),
    )


def command_foreground(client: Ps5DebugNg, args: argparse.Namespace) -> None:
    fg = client.foreground()
    expected_title = getattr(args, "expect_title_id", EXPECTED_TITLE_ID)
    expected_version = getattr(args, "expect_app_version", EXPECTED_APP_VERSION)
    fg["matchesExpected"] = (not expected_title or fg.get("titleId") == expected_title) and (
        not expected_version or fg.get("appVersion") == expected_version
    )
    if getattr(args, "auto_target", False):
        try:
            fg["detectedTarget"] = supported_foreground_target(fg)
            fg["matchesExpected"] = True
        except RuntimeError:
            fg["matchesExpected"] = False
    if getattr(args, "output", None):
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(fg, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(fg, indent=2))


def command_processes(client: Ps5DebugNg, args: argparse.Namespace) -> None:
    """List process identities and optionally prove that one title is absent."""

    rows = []
    failures = []
    for process in client.processes():
        pid = int(process.get("pid") or 0)
        if pid <= 0:
            rows.append(process)
            continue
        try:
            row = client.proc_info(pid)
        except (OSError, RuntimeError, ValueError) as exc:
            row = {**process, "infoError": str(exc)}
            failures.append(pid)
        rows.append(row)
    required_absent = getattr(args, "require_title_absent", None)
    if required_absent:
        if failures:
            raise RuntimeError(f"cannot prove {required_absent} is closed; process identity failed for pids {failures}")
        matches = [int(row.get("pid") or 0) for row in rows if row.get("titleId") == required_absent]
        if matches:
            raise RuntimeError(f"title {required_absent} is still running in pids {matches}")
    if getattr(args, "output", None):
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(rows, indent=2))


def command_status(client: Ps5DebugNg, args: argparse.Namespace) -> None:
    _apply_candidate_status_filters(args)
    if getattr(args, "address", None) or getattr(args, "base", None):
        pid, fg = resolve_pid(client, args)
        candidates, skipped = _collect_status_candidates(client, pid, args)
    else:
        pids, fg = gtav_scan_pids(client, args)
        candidates, skipped, pid = ([], [], pids[0])
        for candidate_pid in pids:
            found, missed = _collect_status_candidates(client, candidate_pid, args)
            if found:
                candidates, pid = (found, candidate_pid)
                break
            skipped.extend(missed)
    if not candidates:
        raise RuntimeError(f"no GTAVMenu status block found; skipped={skipped[:8]}")
    selected, matched, rejected = _select_status_candidate(candidates, args)
    result = {
        "foreground": fg,
        "pid": pid,
        "selected": selected,
        "matched": matched,
        "candidates": candidates,
        "rejected": rejected,
        "skipped": skipped,
    }
    if args.output:
        Path(args.output).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result if args.all else {"foreground": fg, "pid": pid, "selected": result["selected"]}, indent=2))


def decode_log_ring(raw: bytes, address: int | None = None) -> dict[str, object]:
    """Decode the worker log ring (GTAVLOGR) into its newest-last list of entries. Pure."""
    if len(raw) < GTAV_LOG_RING_SIZE:
        raise ValueError(f"log ring blob too small: {len(raw)} < {GTAV_LOG_RING_SIZE}")
    magic, abi_version, struct_size, write_index, count = struct.unpack_from(GTAV_LOG_RING_HEADER_FORMAT, raw, 0)
    if magic != GTAV_LOG_RING_MAGIC:
        raise ValueError(f"bad log ring magic: 0x{magic:016x}")
    if abi_version != GTAV_LOG_RING_ABI_VERSION:
        raise ValueError(f"unsupported log ring ABI: {abi_version}")
    if struct_size < GTAV_LOG_RING_HEADER_SIZE or struct_size > len(raw):
        raise ValueError(f"invalid log ring struct size: {struct_size}")
    count = min(count, GTAV_LOG_RING_ENTRY_COUNT)
    start = write_index - count if write_index >= count else 0
    entries = []
    for logical in range(count):
        index = (start + logical) % GTAV_LOG_RING_ENTRY_COUNT
        offset = GTAV_LOG_RING_HEADER_SIZE + index * GTAV_LOG_RING_ENTRY_SIZE
        seq, level, category, message = struct.unpack_from(GTAV_LOG_RING_ENTRY_FORMAT, raw, offset)
        entries.append(
            {
                "slot": index,
                "seq": seq,
                "level": level,
                "levelName": LEVEL_NAMES.get(level, f"unknown_{level}"),
                "category": category,
                "categoryName": LOG_CATEGORY_NAMES.get(category, f"unknown_{category}"),
                "message": _cstr(message),
            }
        )
    return {
        "address": address,
        "addressHex": _hex_addr(address) if address is not None else None,
        "magic": f"0x{magic:016x}",
        "abiVersion": abi_version,
        "structSize": struct_size,
        "writeIndex": write_index,
        "count": count,
        "entries": entries,
    }


def find_log_ring_candidates(
    client: Ps5DebugNg,
    pid: int,
    maps: list[dict[str, object]],
    max_matches_per_map: int,
    max_map_size: int,
    scan_all_maps: bool,
    injected_range_size: int,
) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    """Locate the log ring by AOB-scanning likely-injected ranges for its magic (mirrors
    find_status_candidates)."""
    candidates: list[dict[str, object]] = []
    skipped: list[dict[str, object]] = []
    if scan_all_maps:
        scan_ranges = [
            {"name": row["name"], "start": row["start"], "end": row["end"], "prot": row["prot"]} for row in maps
        ]
    else:
        scan_ranges = likely_injected_ranges(maps, injected_range_size)
    for row in scan_ranges:
        start = int(row["start"])
        end = int(row["end"])
        size = end - start
        prot = int(row["prot"])
        if size < len(GTAV_LOG_RING_MAGIC_BYTES):
            continue
        if not scan_all_maps and (not prot & 2):
            continue
        if not scan_all_maps and size > max_map_size:
            continue
        try:
            matches = client.scan_aob(
                pid,
                start,
                size,
                GTAV_LOG_RING_MAGIC_BYTES,
                b"\x01" * len(GTAV_LOG_RING_MAGIC_BYTES),
                max_matches_per_map,
            )
        except Exception as exc:
            skipped.append({"startHex": _hex_addr(start), "size": size, "reason": str(exc)})
            continue
        for address in matches:
            try:
                candidates.append(decode_log_ring(client.read(pid, address, GTAV_LOG_RING_SIZE), address))
            except Exception as exc:
                skipped.append({"startHex": _hex_addr(address), "reason": str(exc)})
    return (candidates, skipped)


def command_worker_log(client: Ps5DebugNg, args: argparse.Namespace) -> None:
    skipped: list[dict[str, object]] = []
    if getattr(args, "address", None):
        pid, fg = resolve_pid(client, args)
        address = parse_int(args.address)
        candidates = [decode_log_ring(client.read(pid, address, GTAV_LOG_RING_SIZE), address)]
    else:
        pids, fg = gtav_scan_pids(client, args)
        candidates, pid = ([], pids[0])
        for candidate_pid in pids:
            found, missed = find_log_ring_candidates(
                client,
                candidate_pid,
                client.maps(candidate_pid),
                args.max_matches_per_map,
                parse_int(args.max_map_size),
                args.scan_all_maps,
                parse_int(args.injected_range_size),
            )
            if found:
                candidates, pid = (found, candidate_pid)
                break
            skipped.extend(missed)
    if not candidates:
        raise RuntimeError(f"no GTAVMenu log ring found; skipped={skipped[:8]}")
    selected = max(candidates, key=lambda c: int(c["count"]))
    entries = list(selected["entries"])
    if getattr(args, "tail", None):
        entries = entries[-int(args.tail) :]
    if args.json:
        print(json.dumps({"foreground": fg, "pid": pid, "ring": selected}, indent=2))
        return
    lines = [f"[{e['seq']}] [{e['levelName']}] [{e['categoryName']}] {e['message']}" for e in entries]
    text = "\n".join(lines)
    print(text)
    if getattr(args, "output", None):
        Path(args.output).write_text(text + "\n", encoding="utf-8")


def wait_for_status_result(
    client: Ps5DebugNg, pid: int, fg: dict[str, object], args: argparse.Namespace
) -> dict[str, object]:
    deadline = time.monotonic() + args.wait_timeout
    previous_by_address: dict[int, dict[str, object]] = {}
    poll_count = 0
    last_candidates: list[dict[str, object]] = []
    last_matched: list[dict[str, object]] = []
    last_rejected: list[dict[str, object]] = []
    last_skipped: list[dict[str, object]] = []
    last_error = ""
    while poll_count == 0 or time.monotonic() <= deadline:
        poll_count += 1
        try:
            candidates, skipped = _collect_status_candidates(client, pid, args)
            last_candidates = candidates
            last_skipped = skipped
            if not candidates:
                last_error = f"no GTAVMenu status block found; skipped={skipped[:8]}"
            else:
                selected, matched, rejected = _select_status_candidate(candidates, args)
                last_matched = matched
                last_rejected = rejected
                address = int(selected.get("address") or 0)
                previous = previous_by_address.get(address)
                previous_by_address[address] = selected
                tick_increased = previous is not None and int(selected.get("ticks", 0)) > int(previous.get("ticks", 0))
                hook_tick_increased = previous is not None and int(selected.get("hookSideTicks", 0)) > int(
                    previous.get("hookSideTicks", 0)
                )
                if getattr(args, "require_tick_increase", False) and (not tick_increased):
                    last_error = f"waiting for ticks to increase at {selected.get('addressHex')}"
                elif getattr(args, "require_hook_side_tick_increase", False) and (not hook_tick_increased):
                    last_error = f"waiting for hookSideTicks to increase at {selected.get('addressHex')}"
                else:
                    return {
                        "ok": True,
                        "foreground": fg,
                        "pid": pid,
                        "selected": selected,
                        "previous": previous,
                        "pollCount": poll_count,
                        "tickIncreased": tick_increased,
                        "hookSideTickIncreased": hook_tick_increased,
                        "matched": matched,
                        "candidates": candidates,
                        "rejected": rejected,
                        "skipped": skipped,
                    }
        except Exception as exc:
            last_error = str(exc)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(args.interval, remaining))
    message = f"status-wait timed out after {args.wait_timeout:.1f}s; last_error={last_error}; matched={len(last_matched)} candidates={len(last_candidates)} rejected={last_rejected[:8]} skipped={last_skipped[:8]}"
    return {
        "foreground": fg,
        "pid": pid,
        "ok": False,
        "error": message,
        "pollCount": poll_count,
        "lastError": last_error,
        "matched": last_matched,
        "candidates": last_candidates,
        "rejected": last_rejected,
        "skipped": last_skipped,
    }


def command_status_wait(client: Ps5DebugNg, args: argparse.Namespace) -> None:
    _apply_candidate_status_filters(args)
    pid, fg = resolve_pid(client, args)
    result = wait_for_status_result(client, pid, fg, args)
    if args.output:
        _write_json_output(args.output, result)
    if result.get("ok"):
        selected = result["selected"]
        print(
            json.dumps(
                (
                    result
                    if args.all
                    else {"foreground": fg, "pid": pid, "selected": selected, "pollCount": result["pollCount"]}
                ),
                indent=2,
            )
        )
        return
    raise RuntimeError(str(result["error"]))


def run_menu_command_result(
    client: Ps5DebugNg, pid: int, fg: dict[str, object], args: argparse.Namespace
) -> dict[str, object]:
    command_id = _command_id_from_name(args.command_name)
    command_name = COMMAND_NAMES[command_id]
    maps = client.maps(pid)
    selected_status = None
    matched_status: list[dict[str, object]] = []
    rejected_status: list[dict[str, object]] = []
    skipped_status: list[dict[str, object]] = []
    matched_mailboxes: list[dict[str, object]] = []
    rejected_mailboxes: list[dict[str, object]] = []
    skipped_mailboxes: list[dict[str, object]] = []
    if args.status_address:
        status_address = parse_int(args.status_address)
        status_candidates = [decode_status(client.read(pid, status_address, GTAV_STATUS_SIZE), status_address)]
    elif args.status_base:
        status_address = parse_int(args.status_base) + parse_int(args.status_offset)
        status_candidates = [decode_status(client.read(pid, status_address, GTAV_STATUS_SIZE), status_address)]
    else:
        status_candidates, skipped_status = find_status_candidates(
            client,
            pid,
            maps,
            args.max_matches_per_map,
            parse_int(args.max_map_size),
            args.scan_all_maps,
            parse_int(args.injected_range_size),
        )
    if status_candidates:
        selected_status, matched_status, rejected_status = _select_status_candidate(status_candidates, args)
    if args.mailbox_address:
        mailbox_address = parse_int(args.mailbox_address)
        selected_mailbox = decode_command_mailbox(
            client.read(pid, mailbox_address, GTAV_COMMAND_MAILBOX_SIZE), mailbox_address
        )
        matched_mailboxes = [selected_mailbox]
    else:
        mailbox_candidates, skipped_mailboxes = find_command_mailbox_candidates(
            client,
            pid,
            maps,
            args.max_matches_per_map,
            parse_int(args.max_map_size),
            args.scan_all_maps,
            parse_int(args.injected_range_size),
        )
        selected_mailbox, matched_mailboxes, rejected_mailboxes = _select_command_mailbox_candidate(
            mailbox_candidates, maps, selected_status
        )
    mailbox_address = int(selected_mailbox["address"] or 0)
    sequence = max(int(selected_mailbox["requestSequence"]), int(selected_mailbox["ackSequence"])) + 1
    field_data = struct.pack(GTAV_COMMAND_FIELDS_FORMAT, command_id, 1, parse_int(args.argument))
    client.write(pid, mailbox_address + GTAV_COMMAND_FIELDS_OFFSET, field_data)
    client.write(pid, mailbox_address + GTAV_COMMAND_REQUEST_SEQUENCE_OFFSET, struct.pack("<Q", sequence))
    after = decode_command_mailbox(client.read(pid, mailbox_address, GTAV_COMMAND_MAILBOX_SIZE), mailbox_address)
    wait_poll_count = 0
    if args.wait:
        deadline = time.monotonic() + args.wait_timeout
        while time.monotonic() <= deadline:
            wait_poll_count += 1
            after = decode_command_mailbox(
                client.read(pid, mailbox_address, GTAV_COMMAND_MAILBOX_SIZE), mailbox_address
            )
            if int(after["ackSequence"]) >= sequence:
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(args.interval, remaining))
        if int(after["ackSequence"]) < sequence:
            raise RuntimeError(
                f"menu-command timed out after {args.wait_timeout:.1f}s waiting for ack sequence {sequence}; lastMailbox={after}"
            )
    selected_status_after = selected_status
    if selected_status is not None:
        status_address = int(selected_status.get("address") or 0)
        if status_address:
            selected_status_after = decode_status(client.read(pid, status_address, GTAV_STATUS_SIZE), status_address)
    followed_events: list[dict[str, object]] = []
    event_poll_count = 0
    feature_action_result = None
    feature_action_succeeded = None
    events_dropped = 0
    wait_feature_action = getattr(args, "wait_feature_action", None)
    if wait_feature_action:
        if args.feature_wait_timeout <= 0 or args.event_interval <= 0:
            raise RuntimeError("feature-action wait timeout and interval must be positive")
        if selected_status is None or selected_status_after is None:
            raise RuntimeError("feature-action result wait requires a selected status block")
        status_address = int(selected_status.get("address") or 0)
        if not status_address:
            raise RuntimeError("feature-action result wait requires an addressed status block")
        event_cursor = int(selected_status.get("eventWriteIndex") or 0)
        deadline = time.monotonic() + float(args.feature_wait_timeout)
        current_status = selected_status_after
        while True:
            event_poll_count += 1
            previous_cursor = event_cursor
            event_cursor, new_events = status_events_since(event_cursor, current_status, allow_overflow=True)
            events_dropped += event_cursor - previous_cursor - len(new_events)
            followed_events.extend(new_events)
            feature_action_result = feature_action_terminal_event(new_events, wait_feature_action)
            if feature_action_result is not None:
                feature_action_succeeded = feature_action_event_succeeded(feature_action_result, wait_feature_action)
                selected_status_after = current_status
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                messages = [str(event.get("message") or "") for event in followed_events[-8:]]
                raise RuntimeError(f"feature-action result timed out for {wait_feature_action}; recent={messages}")
            time.sleep(min(float(args.event_interval), remaining))
            current_status = decode_status(client.read(pid, status_address, GTAV_STATUS_SIZE), status_address)
    return {
        "foreground": fg,
        "pid": pid,
        "command": command_id,
        "commandName": command_name,
        "sequence": sequence,
        "selectedStatus": selected_status,
        "selectedStatusAfter": selected_status_after,
        "selectedMailboxBefore": selected_mailbox,
        "selectedMailboxAfter": after,
        "acknowledged": int(after["ackSequence"]) >= sequence,
        "waitPollCount": wait_poll_count,
        "eventPollCount": event_poll_count,
        "followedEvents": followed_events,
        "eventsDropped": events_dropped,
        "featureActionResult": feature_action_result,
        "featureActionSucceeded": feature_action_succeeded,
        "matchedStatus": matched_status,
        "rejectedStatus": rejected_status,
        "skippedStatus": skipped_status,
        "matchedMailboxes": matched_mailboxes,
        "rejectedMailboxes": rejected_mailboxes,
        "skippedMailboxes": skipped_mailboxes,
    }


def command_menu_command(client: Ps5DebugNg, args: argparse.Namespace) -> None:
    if not args.yes_command:
        raise SystemExit("menu-command requires --yes-command")
    _apply_candidate_status_filters(args)
    pid, fg = resolve_pid(client, args)
    result = run_menu_command_result(client, pid, fg, args)
    if args.output:
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    if args.wait_feature_action and result["featureActionSucceeded"] is not True:
        raise RuntimeError(f"feature action {args.wait_feature_action} refused: {result['featureActionResult']}")


def command_klog(_client: Ps5DebugNg | None, args: argparse.Namespace) -> None:
    if not args.host:
        raise SystemExit("--host is required for klog")
    deadline = time.monotonic() + args.seconds
    sock = socket.create_connection((args.host, args.klog_port), timeout=args.timeout)
    sock.setblocking(False)
    filters = args.filter or []
    pending = ""
    output_fp = None
    if getattr(args, "output", None):
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output_fp = output.open("w", encoding="utf-8")

    def write_text(text: str) -> None:
        sys.stdout.write(text)
        sys.stdout.flush()
        if output_fp:
            output_fp.write(text)
            output_fp.flush()

    try:
        while time.monotonic() < deadline:
            remaining = max(0.0, deadline - time.monotonic())
            readable, _, _ = select.select([sock], [], [], min(0.5, remaining))
            if not readable:
                continue
            chunk = sock.recv(4096)
            if not chunk:
                break
            text = chunk.decode("utf-8", "replace")
            if not filters:
                write_text(text)
                continue
            pending += text
            lines = pending.splitlines(keepends=True)
            pending = lines.pop() if lines and (not lines[-1].endswith(("\n", "\r"))) else ""
            for line in lines:
                if any(item in line for item in filters):
                    write_text(line)
        if filters and pending and any(item in pending for item in filters):
            write_text(pending)
    finally:
        if output_fp:
            output_fp.close()
        sock.close()


def add_status_filter_args(parser: argparse.ArgumentParser, *, include_wait: bool = False) -> None:
    parser.add_argument("--manifest", help="target manifest path for filter lookups")
    parser.add_argument("--id", help="manifest target id to match")
    parser.add_argument("--target-id", help="require this status block target id")
    parser.add_argument("--hook-addr", help="require this hook address in the status block")
    parser.add_argument("--hook-length", type=int, help="require this hook length")
    parser.add_argument("--hook-status-name", help="require this hook status name")
    parser.add_argument(
        "--expected-original", "--original", dest="expected_original", help="require these original (pre-hook) bytes"
    )
    parser.add_argument(
        "--expected-bytes", "--expected", dest="expected_bytes", help="require these expected hook bytes"
    )
    parser.add_argument("--require-running", action="store_true", help="fail unless the worker reports running")
    parser.add_argument("--require-stopped", action="store_true", help="fail unless the worker reports stopped")
    parser.add_argument("--require-no-error", action="store_true", help="fail if the status block reports an error")
    if include_wait:
        parser.add_argument("--wait-timeout", type=float, default=15.0, help="seconds to wait for conditions")
        parser.add_argument("--interval", type=float, default=0.5, help="poll interval in seconds")
        parser.add_argument(
            "--require-tick-increase", action="store_true", help="fail unless the worker tick counter increases"
        )
        parser.add_argument(
            "--require-hook-side-tick-increase",
            action="store_true",
            help="fail unless the hook-side tick counter increases",
        )


def _add_status_location_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--pid", type=int)
    parser.add_argument("--address")
    parser.add_argument("--base")
    parser.add_argument("--offset", default="0x0")
    parser.add_argument("--max-matches-per-map", type=int, default=4)
    parser.add_argument("--max-map-size", default="0x4000000")
    parser.add_argument("--injected-range-size", default="0x40000")
    parser.add_argument("--scan-all-maps", action="store_true")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--output")
    parser.add_argument("--allow-non-gtav", action="store_true")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", help="PS5 host/IP")
    parser.add_argument("--port", type=int, default=744, help="ps5debug-NG port")
    parser.add_argument("--timeout", type=float, default=10.0, help="socket timeout")
    parser.add_argument("--expect-title-id", default=EXPECTED_TITLE_ID)
    parser.add_argument("--expect-app-version", default=EXPECTED_APP_VERSION)
    parser.add_argument(
        "--auto-target", action="store_true", help="accept any reviewed title/version for live worker controls"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    foreground = sub.add_parser("foreground", help="show the foreground process identity")
    foreground.add_argument("--output")
    foreground.set_defaults(func=command_foreground)

    processes = sub.add_parser("processes", help="show process identities")
    processes.add_argument("--require-title-absent")
    processes.add_argument("--output")
    processes.set_defaults(func=command_processes)

    status = sub.add_parser("status", help="read the menu worker status block")
    _add_status_location_args(status)
    add_status_filter_args(status)
    status.set_defaults(func=command_status)

    worker_log = sub.add_parser("worker-log", help="read the menu worker log ring")
    worker_log.add_argument("--pid", type=int)
    worker_log.add_argument("--address")
    worker_log.add_argument("--max-matches-per-map", type=int, default=4)
    worker_log.add_argument("--max-map-size", default="0x4000000")
    worker_log.add_argument("--injected-range-size", default="0x40000")
    worker_log.add_argument("--scan-all-maps", action="store_true")
    worker_log.add_argument("--tail", type=int)
    worker_log.add_argument("--json", action="store_true")
    worker_log.add_argument("--output")
    worker_log.set_defaults(func=command_worker_log)

    status_wait = sub.add_parser("status-wait", help="wait for menu worker status conditions")
    _add_status_location_args(status_wait)
    add_status_filter_args(status_wait, include_wait=True)
    status_wait.set_defaults(func=command_status_wait)

    menu_command = sub.add_parser("menu-command", help="send a command to the menu worker")
    menu_command.add_argument("--pid", type=int)
    menu_command.add_argument("--command", dest="command_name", required=True)
    menu_command.add_argument("--argument", default="0")
    menu_command.add_argument("--mailbox-address")
    menu_command.add_argument("--status-address")
    menu_command.add_argument("--status-base")
    menu_command.add_argument("--status-offset", default="0x0")
    menu_command.add_argument("--max-matches-per-map", type=int, default=4)
    menu_command.add_argument("--max-map-size", default="0x4000000")
    menu_command.add_argument("--injected-range-size", default="0x40000")
    menu_command.add_argument("--scan-all-maps", action="store_true")
    menu_command.add_argument("--wait", dest="wait", action="store_true", default=True)
    menu_command.add_argument("--no-wait", dest="wait", action="store_false")
    menu_command.add_argument("--wait-timeout", type=float, default=5.0)
    menu_command.add_argument("--interval", type=float, default=0.1)
    menu_command.add_argument("--wait-feature-action")
    menu_command.add_argument("--feature-wait-timeout", type=float, default=3.0)
    menu_command.add_argument("--event-interval", type=float, default=0.005)
    menu_command.add_argument("--yes-command", action="store_true")
    menu_command.add_argument("--output")
    menu_command.add_argument("--allow-non-gtav", action="store_true")
    add_status_filter_args(menu_command)
    menu_command.set_defaults(func=command_menu_command)

    klog = sub.add_parser("klog", help="stream the PS5 kernel log")
    klog.add_argument("--klog-port", type=int, default=3232)
    klog.add_argument("--seconds", type=float, default=10.0)
    klog.add_argument("--timeout", type=float, default=5.0)
    klog.add_argument("--filter", action="append")
    klog.add_argument("--output")
    klog.set_defaults(func=command_klog, no_connect=True)
    return parser


def main(argv: list[str]) -> int:
    args = build_parser().parse_args(argv)
    if getattr(args, "no_connect", False):
        args.func(None, args)
        return 0
    if not args.host:
        raise SystemExit("--host is required for ps5debug-NG commands")
    client = Ps5DebugNg(args.host, args.port, args.timeout)
    try:
        args.func(client, args)
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

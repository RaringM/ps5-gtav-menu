#!/usr/bin/env python3
"""Crash capture for live sessions: a background kernel-log follower and a crash-dump puller.

A GTA crash leaves its evidence in two places that are easy to lose: the console's kernel log (only
what a collector was already receiving survives) and the crash reporter's dumps under
/devlog/system/sce_coredumps.0 (the reporter deletes them once it has sent them). `menu-ctl.sh watch`
and `cave-inject` therefore start one unfiltered, reconnecting kernel-log follower in the background
(opt out with KLOG_AUTO=0), and `crash-pull` copies any new dump files to the host.

  klog-start --host H [--port 3232] [--target T]   start the follower (one per checkout; reused if running)
  klog-stop                                         stop it (only through its pid + start-time record)
  klog-status                                       show it
  crash-pull --host H [--ftp-port 2121] [--out DIR] copy new files under the dump folder (read-only;
                                                    never deletes anything on the console)

The follower is `menu_client.py klog --reconnect`: it keeps reconnecting across GTA crashes and console
reboots until stopped (at most 7 days), appending to build/klog/<date>-<target>.log; each connection
gap is marked with a `# klog` line. Its record lives in build/klog/active.json.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from collections.abc import Callable, Sequence
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from gtavmenu_tools.host_paths import build_dir  # noqa: E402
from gtavmenu_tools.proc_records import (  # noqa: E402
    any_alive,
    process_start,
    read_records,
    stop_record,
    write_records,
)

MENU_CLIENT = ROOT / "tools/menu_client.py"
KLOG_DIR = build_dir(ROOT) / "klog"
KLOG_RECORD = KLOG_DIR / "active.json"
KLOG_SECONDS = 7 * 24 * 3600
CRASH_DIR = build_dir(ROOT) / "crash"
DUMP_ROOT = "/devlog/system/sce_coredumps.0"
DUMP_DEPTH = 3
Curl = Callable[[Sequence[str]], bytes]


def curl(args: Sequence[str]) -> bytes:
    return subprocess.run(["curl", "-s", "--max-time", "120", *args], check=True, capture_output=True).stdout


def normalize_klog(path: Path) -> None:
    """The kernel's lines arrive CR-terminated: make the kept log LF lines."""
    try:
        raw = path.read_bytes()
    except OSError:
        return
    if b"\r" in raw:
        path.write_bytes(raw.replace(b"\r\n", b"\n").replace(b"\r", b"\n"))


def klog_log_path(target: str, *, now: float | None = None) -> Path:
    stamp = time.strftime("%Y%m%d", time.localtime(now))
    return KLOG_DIR / f"{stamp}-{target or 'console'}.log"


def klog_command(host: str, port: str, log: Path) -> list[str]:
    return [
        sys.executable,
        str(MENU_CLIENT),
        "--host",
        host,
        "klog",
        "--klog-port",
        port,
        "--reconnect",
        "--append",
        "--quiet",
        "--seconds",
        str(KLOG_SECONDS),
        "--output",
        str(log),
    ]


def klog_start(host: str, port: str, target: str, *, record_path: Path | None = None, spawn=subprocess.Popen) -> int:
    record_path = record_path or KLOG_RECORD
    current = read_records(record_path)
    if any_alive(current):
        print(f"[klog] already following the kernel log -> {current.get('log')} (stop: ./menu-ctl.sh klog-stop)")
        return 0
    log = klog_log_path(target)
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("a", encoding="utf-8") as stream:
        stream.write(f"\n# klog follower start {time.strftime('%Y-%m-%d %H:%M:%S')} target={target} host={host}\n")
    err = log.with_suffix(".err")
    with err.open("a", encoding="utf-8") as err_stream:
        child = spawn(
            klog_command(host, port, log),
            cwd=ROOT,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=err_stream,
            start_new_session=True,
        )
    record = {
        "role": "klog",
        "pid": child.pid,
        "start": process_start(child.pid),
        "log": str(log),
    }
    write_records(
        {
            "target": target,
            "host": host,
            "log": str(log),
            "started": time.strftime("%Y-%m-%d %H:%M:%S"),
            "procs": [record],
        },
        record_path,
    )
    print(f"[klog] following the unfiltered kernel log -> {log} (stop: ./menu-ctl.sh klog-stop)")
    return 0


def klog_stop(*, record_path: Path | None = None) -> int:
    record_path = record_path or KLOG_RECORD
    data = read_records(record_path)
    if data is None:
        record_path.unlink(missing_ok=True)
        print("[klog] no kernel-log follower recorded")
        return 0
    for record in data["procs"]:
        print(f"[klog] {stop_record(record)}")
        if record.get("log"):
            normalize_klog(Path(record["log"]))
    record_path.unlink(missing_ok=True)
    if data.get("log"):
        print(f"[klog] log kept: {data['log']}")
    return 0


def klog_status(*, record_path: Path | None = None) -> int:
    record_path = record_path or KLOG_RECORD
    data = read_records(record_path)
    if data is None:
        print("[klog] no kernel-log follower recorded")
        return 1
    log = Path(str(data.get("log", "")))
    size = log.stat().st_size if log.is_file() else 0
    state = "running" if any_alive(data) else "ended"
    print(f"[klog] follower {state} since {data.get('started')} target={data.get('target')} -> {log} ({size} bytes)")
    return 0 if state == "running" else 1


# --- crash dumps ----------------------------------------------------------------------------------------------


def parse_listing(text: str) -> list[tuple[str, bool, int]]:
    """(name, is_dir, size) rows of a Unix-style FTP LIST reply; . and .. are skipped."""
    rows = []
    for line in text.splitlines():
        parts = line.split(None, 8)
        if len(parts) < 9 or parts[0][:1] not in "d-l":
            continue
        name = parts[8].strip()
        if name in (".", "..") or "/" in name:
            continue
        try:
            size = int(parts[4])
        except ValueError:
            size = 0
        rows.append((name, parts[0].startswith("d"), size))
    return rows


def remote_files(run: Curl, base: str, folder: str, depth: int = DUMP_DEPTH) -> list[tuple[str, int]]:
    """Every regular file below `folder` (relative paths with sizes), at most `depth` folders deep."""
    try:
        listing = run([f"{base}{folder}/"]).decode("utf-8", errors="replace")
    except subprocess.CalledProcessError:
        return []
    found = []
    for name, is_dir, size in parse_listing(listing):
        if is_dir:
            if depth > 1:
                found += [(f"{name}/{rel}", n) for rel, n in remote_files(run, base, f"{folder}/{name}", depth - 1)]
        else:
            found.append((name, size))
    return found


def crash_pull(host: str, ftp_port: str, *, out: Path | None = None, remote: str = DUMP_ROOT, run: Curl = curl) -> int:
    """Copy dump files not pulled before (by path and size) into out/<timestamp>/; never deletes remotely."""
    out = out or CRASH_DIR
    base = f"ftp://{host}:{ftp_port}"
    state_path = out / "pulled.json"
    try:
        pulled = json.loads(state_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        pulled = {}
    files = remote_files(run, base, remote)
    new = [(rel, size) for rel, size in files if pulled.get(rel) != size]
    if not new:
        print(f"[crash] no new crash dumps under {remote} ({len(files)} file(s) seen)")
        return 0
    folder = out / time.strftime("%Y%m%d-%H%M%S")
    copied = 0
    for rel, size in new:
        target = folder / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            target.write_bytes(run([f"{base}{remote}/{rel}"]))
        except subprocess.CalledProcessError:
            print(f"[crash] could not read {remote}/{rel} (it may have been removed by the reporter)", file=sys.stderr)
            continue
        pulled[rel] = size
        copied += 1
    out.mkdir(parents=True, exist_ok=True)
    state_path.write_text(json.dumps(pulled, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    print(f"[crash] {copied} new crash dump file(s) -> {folder}")
    return 0 if copied == len(new) else 1


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    start = sub.add_parser("klog-start")
    start.add_argument("--host", default=os.environ.get("PS5_HOST", ""))
    start.add_argument("--port", default=os.environ.get("PS5_KLOG_PORT") or "3232")
    start.add_argument("--target", default=os.environ.get("GTAV_TARGET", ""))
    sub.add_parser("klog-stop")
    sub.add_parser("klog-status")
    pull = sub.add_parser("crash-pull")
    pull.add_argument("--host", default=os.environ.get("PS5_HOST", ""))
    pull.add_argument("--ftp-port", default=os.environ.get("PS5_FTP_PORT") or "2121")
    pull.add_argument("--out", type=Path)
    pull.add_argument("--remote", default=DUMP_ROOT)
    args = parser.parse_args(argv)
    if args.command in ("klog-start", "crash-pull") and not args.host.strip():
        print("[crash] set PS5_HOST=<console-ip> for console commands", file=sys.stderr)
        return 2
    if args.command == "klog-start":
        return klog_start(args.host.strip(), args.port, args.target)
    if args.command == "klog-stop":
        return klog_stop()
    if args.command == "klog-status":
        return klog_status()
    return crash_pull(args.host.strip(), args.ftp_port, out=args.out, remote=args.remote.rstrip("/"))


if __name__ == "__main__":
    raise SystemExit(main())

"""Host background processes recorded by pid + start time, so they are only ever stopped through the record.

A recorded pid alone is not an identity: after the process ends the kernel may hand the number to an
unrelated process. Every record therefore carries the process start time, and nothing is signalled
unless both still match (a recycled pid reads as "already ended").
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import time
from collections.abc import Callable
from pathlib import Path


def process_start(pid: int) -> str | None:
    """The process's start time (a stable identity beside the pid), or None when it is gone or a zombie."""
    stat = Path(f"/proc/{pid}/stat")
    if Path("/proc/self/stat").is_file():
        try:
            text = stat.read_text(encoding="ascii", errors="replace")
        except OSError:
            return None
        fields = text[text.rindex(")") + 2 :].split()
        if not fields or fields[0] in ("Z", "X"):
            return None
        return fields[19]  # field 22, starttime (fields[0] is field 3, the state)
    result = subprocess.run(["ps", "-o", "stat=,lstart=", "-p", str(pid)], capture_output=True, text=True)
    out = result.stdout.strip()
    if result.returncode != 0 or not out or out.startswith("Z"):
        return None
    return out.split(None, 1)[1] if " " in out else out


def is_alive(record: dict) -> bool:
    start = process_start(int(record["pid"]))
    return start is not None and start == record.get("start")


def stop_record(record: dict, *, grace: float = 5.0, sleep: Callable[[float], None] = time.sleep) -> str:
    """Stop one recorded process (its own process group: a follower's children go with it)."""
    pid = int(record["pid"])
    if not is_alive(record):
        return f"{record.get('role', '?')} pid {pid}: already ended"
    try:
        group = os.getpgid(pid)
    except OSError:
        return f"{record.get('role', '?')} pid {pid}: already ended"

    def send(sig: int) -> None:
        try:
            if group == pid:
                os.killpg(pid, sig)
            else:
                os.kill(pid, sig)
        except ProcessLookupError:
            pass

    send(signal.SIGTERM)
    waited = 0.0
    while waited < grace and is_alive(record):
        sleep(0.2)
        waited += 0.2
    if is_alive(record):
        send(signal.SIGKILL)
        return f"{record.get('role', '?')} pid {pid}: killed"
    return f"{record.get('role', '?')} pid {pid}: stopped"


def read_records(path: Path) -> dict | None:
    """A `{"procs": [{"pid": ..., "start": ...}, ...], ...}` record file, or None when absent or malformed."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict) or not isinstance(data.get("procs"), list):
        return None
    for record in data["procs"]:
        if not isinstance(record, dict) or not isinstance(record.get("pid"), int) or record["pid"] <= 1:
            return None
    return data


def write_records(data: dict, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(".tmp")
    temp.write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8")
    temp.replace(path)


def any_alive(data: dict | None) -> bool:
    return bool(data) and any(is_alive(record) for record in data["procs"])

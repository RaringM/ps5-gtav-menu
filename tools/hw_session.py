#!/usr/bin/env python3
"""A hardware test session as a plan: stage every pack once, then one short flow per fresh GTA process.

A plan (a plan.json in its own directory) lists processes (one fresh GTA process each), the packs each loads
together, named selections for processes with host-side steps before the load, the expectation file its
pack-notes lines are checked against (tools/pack_notes.py check), the in-game steps, and which installed ids
may be uninstalled to make room. A process may name its own "target" (default: the plan's) and ask for the
kernel log with "klog": true (risky spawns: a crash backtrace only shows in a klog collected before it).

  show PLAN [NAME]                 the processes, their packs, selections and runs
  stage PLAN [--dry-run]           make room in the console's installed list (64 max) and upload every pack of
                                   the plan that is not installed yet (no activation). Uninstalls, in this order:
                                   the plan's "uninstall" ids, then older versions (<family>-vN with a newer
                                   <family>-vM installed); never a plan pack, a "keep" id or an active pack.
                                   Uninstalled packs keep their files (a later upload restores them).
  select PLAN NAME [SELECTION]     packs/active = the selection's packs (default: "load"), verifying each remote
                                   copy byte for byte (upload_runtime_pack.py --activate-only [--activate-add])
  next PLAN NAME [SELECTION]       select, then print what to do before restarting GTA
  go PLAN NAME [--klog]            wait until GTA (the process's target) runs in single-player with player
                                   control (the cave-inject pre-flight probe, tools/read_player_ped_anchor.py),
                                   start the pack-notes follower (and the klog collector) in the background,
                                   inject (menu-ctl.sh cave-inject), print "press Load now" and the in-game
                                   steps, then show the pack lines until Ctrl-C (which stops the followers)
  check PLAN NAME [NOTES] [--keep] stop the session's followers (not with --keep) and check the lines of the
                                   latest GTA process in the session log (or NOTES) against the expectation
                                   file: PASS / MISSING / FAIL per pack-note rule. Operator observations,
                                   status/doctor output and klog still need separate review. Capture gaps
                                   leave the pack-note evidence incomplete and fail the session check.
  stop                             stop the running session's followers

`./menu-ctl.sh session ...` runs these (paths relative to the caller's directory). Logs go to
build/sessions/<plan>/<NAME>.log and <NAME>-klog.log; an older log of the same process is kept under a
timestamped name. Background processes are recorded in build/sessions/active.json (pid + process start time)
and stopped only through those records. Console commands require $PS5_HOST; $PS5_FTP_PORT and $PS5DEBUG_PORT
match menu-ctl.sh. Offline show/check/stop and next/go --dry-run need no address. stage --dry-run without an
address lists pack and uninstall candidates only; with an address it reads the console inventory first.
"""

from __future__ import annotations

import argparse
import fcntl
import json
import os
import re
import signal
import subprocess
import sys
import time
from collections.abc import Callable, Iterator, Sequence
from contextlib import contextmanager
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import pack_notes  # noqa: E402
import upload_runtime_pack as upr  # noqa: E402
from gtavmenu_tools import runtime_pack  # noqa: E402
from gtavmenu_tools.host_paths import build_dir  # noqa: E402

PACKS = build_dir(ROOT) / "custom-assets"  # GTAV_HOST_BUILD_DIR selects candidate pack inputs.
# Keep one follower/injection owner per checkout even when pack outputs are isolated.
SESSIONS = ROOT / "build/sessions"
ACTIVE_FILE = SESSIONS / "active.json"
MENU_CTL = ROOT / "menu-ctl.sh"
ANCHOR_PROBE = ROOT / "tools/read_player_ped_anchor.py"
MENU_CLIENT = ROOT / "tools/menu_client.py"
# The developer checkout's reconnecting klog collector (LF lines); a publication checkout uses menu_client.py klog.
KLOG_COLLECTOR = ROOT / "research/tools/live/collect_klog.sh"
TARGETS = ROOT / "data/targets"
DEFAULT_TARGET = "ppsa04264-01.010.002"
VERSION = re.compile(r"(.+)-v(\d+)([a-z]*)\Z")
NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}\Z")
TARGET = re.compile(r"[a-z0-9][a-z0-9.-]{0,63}\Z")
PLAN_KEYS = {"title", "target", "uninstall", "keep", "processes"}
PROCESS_KEYS = {"name", "runs", "packs", "expect", "steps", "selections", "no_validate", "target", "klog"}
# Process markers in a followed log: the worker's first pack line of every process, the follower's restart line.
ROOT_LINE = re.compile(r"\d+ GTAVMenu pack root ")
NEW_PROCESS = "# NEW PROCESS"
SEQUENCE = re.compile(r"(\d+) ")
BEFORE_LOAD = re.compile(r"\s*before load\b", re.IGNORECASE)
READY_CONFIRMATIONS = 2  # consecutive READY probes before the inject (the watch loader debounces too)
PROBE_STATES = {
    0: "READY: player control",
    3: "waiting for GTA ({title}) in the foreground",
    4: "GTA is running; waiting for player control in Story Mode",
    5: "the foreground changed while the anchor was read; retrying",
}

Runner = Callable[..., subprocess.CompletedProcess]


class PlanError(ValueError):
    pass


# --- plans -----------------------------------------------------------------------------------------------------


def _ids(where: str, value: object, *, allow_empty: bool = True) -> list[str]:
    if not isinstance(value, list) or not all(isinstance(i, str) and runtime_pack.valid_id(i) for i in value):
        raise PlanError(f"{where}: a list of valid pack ids")
    if not allow_empty and not 1 <= len(value) <= runtime_pack.ACTIVE_MAX:
        raise PlanError(f"{where}: 1..{runtime_pack.ACTIVE_MAX} valid pack ids")
    return value


def _strings(where: str, value: object) -> list[str]:
    if not isinstance(value, list) or not all(isinstance(i, str) for i in value):
        raise PlanError(f"{where}: a list of strings")
    return value


def _target(where: str, value: object) -> str:
    if not isinstance(value, str) or not TARGET.match(value) or not (TARGETS / f"{value}.json").is_file():
        raise PlanError(f"{where}: target {value!r} names no data/targets manifest")
    return value


def validate_plan(plan: object, where: str = "plan") -> dict:
    """The plan with every process's "load" selection filled in; PlanError names the first problem."""
    if not isinstance(plan, dict):
        raise PlanError(f"{where}: a JSON object")
    unknown = set(plan) - PLAN_KEYS
    if unknown:
        raise PlanError(f"{where}: unknown keys {sorted(unknown)} (known: {sorted(PLAN_KEYS)})")
    if not isinstance(plan.get("title", ""), str):
        raise PlanError(f"{where}: title is text")
    if "target" in plan:
        _target(f"{where}", plan["target"])
    _ids(f"{where}: uninstall", plan.get("uninstall", []))
    _ids(f"{where}: keep", plan.get("keep", []))
    processes = plan.get("processes")
    if not isinstance(processes, list) or not processes:
        raise PlanError(f"{where}: processes is a non-empty list")
    seen: set[str] = set()
    for process in processes:
        if not isinstance(process, dict):
            raise PlanError(f"{where}: every process is an object")
        name = process.get("name")
        if not isinstance(name, str) or not NAME.match(name):
            raise PlanError(f"{where}: process name {name!r}: letters, digits, '_', '.', '-' (a file name)")
        if name.lower() in seen:
            raise PlanError(f"{where}: process names repeat ({name})")
        seen.add(name.lower())
        here = f"{where}: {name}"
        unknown = set(process) - PROCESS_KEYS
        if unknown:
            raise PlanError(f"{here}: unknown keys {sorted(unknown)} (known: {sorted(PROCESS_KEYS)})")
        _ids(f"{here} packs", process.get("packs"), allow_empty=False)
        sels = process.setdefault("selections", {})
        if not isinstance(sels, dict):
            raise PlanError(f"{here}: selections is an object")
        sels.setdefault("load", process["packs"])
        for sel, ids in sels.items():
            if not NAME.match(sel):
                raise PlanError(f"{here}: selection name {sel!r}")
            _ids(f"{here} selection {sel}", ids, allow_empty=False)
        for sel in _strings(f"{here} no_validate", process.get("no_validate", [])):
            if sel not in sels:
                raise PlanError(f"{here}: no_validate names no selection {sel!r}")
        _strings(f"{here} runs", process.get("runs", []))
        _strings(f"{here} steps", process.get("steps", []))
        if "expect" in process:
            expect = process["expect"]
            if not isinstance(expect, str) or not expect or Path(expect).is_absolute() or ".." in Path(expect).parts:
                raise PlanError(f"{here}: expect is a file name relative to the plan's directory")
        if "target" in process:
            _target(here, process["target"])
        if not isinstance(process.get("klog", False), bool):
            raise PlanError(f"{here}: klog is true or false")
    return plan


def load_plan(path: Path) -> dict:
    try:
        plan = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise PlanError(f"{path}: {error}") from error
    return validate_plan(plan, str(path))


def plan_name(path: Path) -> str:
    """research/runs/hw-phasef-5/plan.json -> hw-phasef-5 (a plan named otherwise: its file stem)."""
    name = path.resolve().parent.name if path.name == "plan.json" else path.stem
    if not NAME.match(name):
        raise PlanError(f"{path}: the plan's directory name {name!r} is no usable session name")
    return name


def plan_packs(plan: dict) -> list[str]:
    out: list[str] = []
    for process in plan["processes"]:
        for ids in process["selections"].values():
            out += [i for i in ids if i not in out]
    return out


def process_of(plan: dict, name: str) -> dict:
    for process in plan["processes"]:
        if process["name"].lower() == name.lower():
            return process
    raise PlanError(f"no process {name!r} in the plan ({', '.join(p['name'] for p in plan['processes'])})")


def target_of(plan: dict, process: dict) -> str:
    return process.get("target") or plan.get("target") or os.environ.get("GTAV_TARGET") or DEFAULT_TARGET


def title_of(target: str) -> str:
    return target.split("-", 1)[0].upper()


def session_paths(plan_path: Path, name: str) -> dict[str, Path]:
    folder = SESSIONS / plan_name(plan_path)
    return {
        "dir": folder,
        "log": folder / f"{name}.log",
        "klog": folder / f"{name}-klog.log",
        "notes_err": folder / f"{name}.follow.err",
        "klog_err": folder / f"{name}-klog.err",
    }


# --- console endpoints -----------------------------------------------------------------------------------------


def host(*, dry_run: bool = False) -> str:
    address = os.environ.get("PS5_HOST", "").strip()
    if address:
        return address
    if dry_run:
        return "<console-ip>"
    raise PlanError("set PS5_HOST=<console-ip> for console commands")


def endpoint() -> tuple[str, list[str]]:
    port = os.environ.get("PS5_FTP_PORT") or str(upr.DEFAULT_FTP_PORT)
    return f"ftp://{host()}:{port}", ["--host", host(), "--port", port]


def probe_command(*, dry_run: bool = False) -> list[str]:
    """The cave-inject pre-flight: exact foreground target identity + the player-ped readiness anchor (read-only)."""
    port = os.environ.get("PS5DEBUG_PORT") or "744"
    return [sys.executable, str(ANCHOR_PROBE), "--host", host(dry_run=dry_run), "--port", port, "--require-ready"]


def notes_command(log: Path) -> list[str]:
    return [str(MENU_CTL), "pack-notes", "--follow", "--out", str(log)]


def klog_command(log: Path, *, dry_run: bool = False) -> list[str]:
    """The kernel-log collector: the developer checkout's reconnecting collect_klog.sh, else menu_client.py klog."""
    if KLOG_COLLECTOR.is_file():
        return ["bash", str(KLOG_COLLECTOR), "-o", str(log)]
    port = os.environ.get("PS5_KLOG_PORT") or "3232"
    return [
        sys.executable,
        str(MENU_CLIENT),
        "--host",
        host(dry_run=dry_run),
        "klog",
        "--klog-port",
        port,
        "--seconds",
        "86400",
        "--output",
        str(log),
    ]


def shown(cmd: Sequence[str]) -> str:
    """A command as printed: paths inside the checkout relative to it, the interpreter as python3."""
    prefix = f"{ROOT}/"
    return " ".join("python3" if arg == sys.executable else arg.replace(prefix, "") for arg in cmd)


def inject_command() -> list[str]:
    return [str(MENU_CTL), "cave-inject"]


# --- background processes: recorded pid + start time, stopped only through the record ------------------------


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


def read_active(path: Path | None = None) -> dict | None:
    path = path or ACTIVE_FILE
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


@contextmanager
def active_record_lock(path: Path) -> Iterator[None]:
    """Serialize record replacement and conditional cleanup, never follower shutdown."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.with_suffix(".lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def write_active(data: dict, path: Path | None = None) -> None:
    path = path or ACTIVE_FILE
    with active_record_lock(path):
        temp = path.with_suffix(".tmp")
        temp.write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8")
        temp.replace(path)


def remove_active(expected: dict | None, path: Path) -> bool:
    """Remove only the record we observed; a concurrent new session must survive."""
    if not path.exists():
        return True
    with active_record_lock(path):
        if read_active(path) != expected:
            return False
        path.unlink(missing_ok=True)
    return True


def live_session(path: Path | None = None) -> dict | None:
    """The recorded session while one of its processes runs; a record whose processes all ended is removed."""
    path = path or ACTIVE_FILE
    while True:
        data = read_active(path)
        if data is not None and any(is_alive(record) for record in data["procs"]):
            return data
        if remove_active(data, path):
            return None


def stop_record(record: dict, *, grace: float = 5.0, sleep: Callable[[float], None] = time.sleep) -> str:
    """Stop one recorded process (its own process group: the follower's curl/tee children go with it)."""
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


def normalize_klog(path: Path) -> None:
    """menu_client.py klog writes the kernel's CR-terminated lines as they come: make them LF lines."""
    try:
        raw = path.read_bytes()
    except OSError:
        return
    if b"\r" in raw:
        path.write_bytes(raw.replace(b"\r\n", b"\n").replace(b"\r", b"\n"))


def stop_session(path: Path | None = None, *, expected: dict | None = None) -> int:
    """Stop the observed session, or the current one for an explicit session stop."""
    path = path or ACTIVE_FILE
    data = expected if expected is not None else read_active(path)
    if data is None:
        remove_active(data, path)
        print("no session running")
        return 0
    for record in data["procs"]:
        print(f"session {data.get('plan_name')} {data.get('process')}: {stop_record(record)}")
        if record.get("role") == "klog" and record.get("log"):
            normalize_klog(Path(record["log"]))
    remove_active(data, path)
    for key in ("log", "klog"):
        if data.get(key):
            print(f"  {key}: {data[key]}")
    return 0


def spawn(cmd: list[str], env: dict[str, str], err: Path) -> subprocess.Popen:
    with err.open("ab") as stream:
        return subprocess.Popen(
            cmd,
            cwd=ROOT,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=stream,
            start_new_session=True,
        )


def keep_previous(path: Path) -> None:
    """Move an older log of the same process aside (<name>-<time>.log) so this run starts a fresh one."""
    if path.is_file() and path.stat().st_size:
        stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(path.stat().st_mtime))
        path.replace(path.with_name(f"{path.stem}-{stamp}{path.suffix}"))


# --- followed logs ---------------------------------------------------------------------------------------------


def latest_process_start(lines: Sequence[str]) -> int:
    """Index of the first line of the latest GTA process in a followed log.

    A follower started before the inject may first copy the pack-notes file of an older process. The latest
    process starts at its `pack root` line, after the follower's `# NEW PROCESS` line, or where the line
    sequence numbers restart.
    """
    start = 0
    previous: int | None = None
    for index, line in enumerate(lines):
        if line.startswith(NEW_PROCESS):
            start, previous = index + 1, None
            continue
        match = SEQUENCE.match(line)
        if not match:
            continue
        number = int(match.group(1))
        if ROOT_LINE.match(line) or (previous is not None and number <= previous):
            start = index
        previous = number
    return start


def check_lines(expect: Path, lines: Sequence[str]) -> tuple[bool, list[str]]:
    title, rules = pack_notes.load_rules(expect)
    ok, report = pack_notes.check(rules, list(lines))
    if any(line.startswith(pack_notes.GAP) for line in lines):
        ok = False
        report.append("INCOMPLETE: capture gaps may hide failures; collect a complete fresh process before acceptance.")
    return ok, ([title] if title else []) + report


# --- commands --------------------------------------------------------------------------------------------------


def older_versions(installed: list[str]) -> list[str]:
    """Installed ids with a newer version of the same family installed (gtavmenu-x-v1 when -v2 is there)."""
    best: dict[str, int] = {}
    for pack_id in installed:
        match = VERSION.match(pack_id)
        if match:
            best[match.group(1)] = max(best.get(match.group(1), 0), int(match.group(2)))
    out = []
    for pack_id in installed:
        match = VERSION.match(pack_id)
        if match and int(match.group(2)) < best[match.group(1)]:
            out.append(pack_id)
    return out


def choose_uninstalls(plan: dict, installed: list[str], active: list[str]) -> tuple[list[str], list[str], int]:
    """(ids to uninstall, plan packs to upload, slots still missing after the uninstalls)."""
    wanted = plan_packs(plan)
    missing = [i for i in wanted if i not in installed]
    need = len(installed) + len(missing) - upr.INSTALLED_MAX
    protected = set(wanted) | set(plan.get("keep", [])) | set(active)
    order = [i for i in plan.get("uninstall", []) if i in installed]
    order += [i for i in older_versions(installed) if i not in order]
    chosen = [i for i in order if i not in protected][: max(0, need)]
    return chosen, missing, max(0, need - len(chosen))


def cmd_show(plan: dict, name: str | None) -> int:
    for process in plan["processes"]:
        if name and process["name"].lower() != name.lower():
            continue
        extra = [target_of(plan, process)] + (["klog"] if process.get("klog") else [])
        print(f"{process['name']} ({', '.join(extra)}): {', '.join(process.get('runs', []))}")
        for sel, ids in process["selections"].items():
            print(f"  {sel:<6} {' '.join(ids)}")
        if process.get("expect"):
            print(f"  expect {process['expect']}")
        for line in process.get("steps", []):
            print(f"  - {line}")
    return 0


def cmd_stage(plan: dict, dry_run: bool) -> int:
    if dry_run and not os.environ.get("PS5_HOST", "").strip():
        print("dry run: no PS5_HOST; console inventory not checked, nothing changed")
        for pack_id in plan_packs(plan):
            available = (PACKS / pack_id / "resources/pack.cfg").is_file()
            print(f"pack      {pack_id} ({'built locally' if available else 'not built locally'})")
        for pack_id in plan.get("uninstall", []):
            print(f"uninstall candidate {pack_id}")
        print("set PS5_HOST=<console-ip> to check which uploads and uninstalls are needed")
        return 0
    base, conn = endpoint()
    installed = upr._read_ids(upr.curl, base, upr.INSTALLED) or []
    active = upr._read_ids(upr.curl, base, upr.ACTIVE) or []
    chosen, missing, short = choose_uninstalls(plan, installed, active)
    local = [i for i in missing if not (PACKS / i / "resources/pack.cfg").is_file()]
    print(f"console: {len(installed)} installed, {len(active)} active; plan packs to upload: {len(missing)}")
    if local:
        print(f"not built locally ({PACKS}): {' '.join(local)}", file=sys.stderr)
        return 1
    if short:
        print(f'{short} more slot(s) needed: add superseded ids to the plan\'s "uninstall" list', file=sys.stderr)
        return 1
    for pack_id in chosen:
        print(f"uninstall {pack_id}")
    for pack_id in missing:
        print(f"upload    {pack_id}")
    if dry_run:
        print("dry run: nothing changed")
        return 0
    for pack_id in chosen:
        if upr.main(["--uninstall", pack_id, *conn]) != 0:
            return 1
    for pack_id in missing:
        if upr.main([str(PACKS / pack_id), *conn]) != 0:
            return 1
    print(f"staged: {len(missing)} uploaded, {len(chosen)} uninstalled (files kept)")
    return 0


def cmd_select(plan: dict, name: str, selection: str) -> int:
    process = process_of(plan, name)
    sels = process["selections"]
    if selection not in sels:
        raise PlanError(f"{process['name']} has no selection {selection!r} ({', '.join(sels)})")
    _, conn = endpoint()
    extra = ["--no-validate"] if selection in process.get("no_validate", []) else []
    for n, pack_id in enumerate(sels[selection]):
        mode = ["--activate-only"] if n == 0 else ["--activate-only", "--activate-add"]
        if upr.main([str(PACKS / pack_id), *mode, *extra, *conn]) != 0:
            print(f"selection stopped at {pack_id}", file=sys.stderr)
            return 1
    print(f"{process['name']} {selection}: packs/active = {' '.join(sels[selection])}")
    return 0


def print_load_prompt(process: dict) -> None:
    steps = process.get("steps", [])
    first = [s for s in steps if BEFORE_LOAD.match(s)]
    rest = [s for s in steps if not BEFORE_LOAD.match(s)]
    if first:
        print("==> DO NOT press Load yet. First:")
        for step in first:
            print(f"    - {step}")
        print("==> then open the menu (R1 + D-pad Left) > Custom Packs > Load, and:")
    else:
        print("==> press Load now (R1 + D-pad Left > Custom Packs > Load). Then:")
    for step in rest:
        print(f"    - {step}")


def cmd_next(plan_path: Path, plan: dict, name: str, selection: str, dry_run: bool) -> int:
    process = process_of(plan, name)
    if selection not in process["selections"]:
        raise PlanError(f"{process['name']} has no selection {selection!r} ({', '.join(process['selections'])})")
    if dry_run:
        print(f"dry run: would select {process['name']} {selection}: {' '.join(process['selections'][selection])}")
    elif cmd_select(plan, name, selection) != 0:
        return 1
    target = target_of(plan, process)
    klog = " --klog" if process.get("klog") else ""
    print(f"\n{process['name']}: {', '.join(process.get('runs', []))}")
    print("Before restarting GTA:")
    print("  1. close GTA V completely (every run needs a fresh GTA process)")
    print(f"  2. run: ./menu-ctl.sh session go {plan_path} {process['name']}{klog}")
    print(f"  3. start GTA V {title_of(target)} ({target}) and enter Story Mode; `session go` injects once you")
    print("     have player control and says when to press Load")
    if process.get("klog"):
        print("  (risky process: the kernel log is collected from before the inject)")
    print("After the inject `session go` prints:")
    print_load_prompt(process)
    return 0


def wait_ready(
    env: dict[str, str],
    target: str,
    *,
    timeout: float,
    poll: float,
    run: Runner = subprocess.run,
    sleep: Callable[[float], None] = time.sleep,
    clock: Callable[[], float] = time.monotonic,
) -> bool:
    """Poll the cave-inject pre-flight probe until READY READY_CONFIRMATIONS times in a row (False: timed out)."""
    started = clock()
    confirmed = 0
    last = ""
    while True:
        try:
            code = run(probe_command(), env=env, capture_output=True, text=True, timeout=60).returncode
        except subprocess.TimeoutExpired:
            code = -1
        state = PROBE_STATES.get(code, "console not answering (ps5debug); retrying").format(title=title_of(target))
        if state != last:
            print(f"[session] {state}", flush=True)
            last = state
        confirmed = confirmed + 1 if code == 0 else 0
        if confirmed >= READY_CONFIRMATIONS:
            return True
        if timeout and clock() - started >= timeout:
            return False
        sleep(poll)


def cmd_go(plan_path: Path, plan: dict, name: str, *, klog: bool, timeout: float, poll: float, dry_run: bool) -> int:
    process = process_of(plan, name)
    name = process["name"]
    target = target_of(plan, process)
    paths = session_paths(plan_path, name)
    use_klog = klog or bool(process.get("klog"))
    env = dict(os.environ, GTAV_TARGET=target)
    print(f"{plan.get('title', plan_name(plan_path))}")
    print(f"{name} on {target}: {', '.join(process.get('runs', []))}")
    print(f"  packs: {' '.join(process['selections']['load'])}")
    if dry_run:
        flow = []
        if use_klog:
            flow.append(f"klog collector (background): {shown(klog_command(paths['klog'], dry_run=True))}")
        flow.append(f"wait for player control: GTAV_TARGET={target} {shown(probe_command(dry_run=True))}")
        flow.append(f"  every {poll:g}s until READY {READY_CONFIRMATIONS}x in a row (timeout {timeout:g}s)")
        flow.append(f"pack-notes follower (background): {shown(notes_command(paths['log']))}")
        flow.append(f"inject: GTAV_TARGET={target} {shown(inject_command())}")
        flow.append("the Load prompt, then the pack lines until Ctrl-C or `session check` stops the followers")
        print("dry run: nothing runs. The flow:")
        number = 0
        for line in flow:
            if line.startswith("  "):
                print(f"   {line}")
            else:
                number += 1
                print(f"  {number}. {line}")
        print_load_prompt(process)
        return 0
    host()  # Refuse before starting followers, waiting on the console or creating session state.
    running = live_session()
    if running:
        print(
            f"a session is running ({running.get('plan_name')} {running.get('process')}): "
            "`./menu-ctl.sh session check ...` or `./menu-ctl.sh session stop` first",
            file=sys.stderr,
        )
        return 1
    paths["dir"].mkdir(parents=True, exist_ok=True)
    keep_previous(paths["log"])
    if use_klog:
        keep_previous(paths["klog"])
    record = {
        "plan": str(plan_path),
        "plan_name": plan_name(plan_path),
        "process": name,
        "target": target,
        "log": str(paths["log"]),
        "klog": str(paths["klog"]) if use_klog else "",
        "started": time.strftime("%Y-%m-%d %H:%M:%S"),
        "procs": [],
    }
    children: list[subprocess.Popen] = []

    def start(role: str, cmd: list[str], log: Path, err: Path) -> subprocess.Popen:
        child = spawn(cmd, env, err)
        children.append(child)
        record["procs"].append({"role": role, "pid": child.pid, "start": process_start(child.pid), "log": str(log)})
        write_active(record)
        print(f"[session] {role} pid {child.pid} -> {log}", flush=True)
        return child

    def interrupted(_signum: int, _frame: object) -> None:
        raise KeyboardInterrupt

    previous = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGTERM, signal.SIGHUP)}
    try:
        if use_klog:
            start("klog", klog_command(paths["klog"]), paths["klog"], paths["klog_err"])
        print(f"[session] start GTA V {title_of(target)} and enter Story Mode. Do NOT press Load until told.")
        if not wait_ready(env, target, timeout=timeout, poll=poll):
            print(f"[session] no player control within {timeout:g}s", file=sys.stderr)
            stop_session(expected=record)
            return 1
        notes = start("notes", notes_command(paths["log"]), paths["log"], paths["notes_err"])
        if subprocess.run(inject_command(), cwd=ROOT, env=env).returncode != 0:
            print("[session] the inject failed; the followers stop (logs kept)", file=sys.stderr)
            stop_session(expected=record)
            return 1
        print()
        print_load_prompt(process)
        print(f"\n[session] pack lines follow (Ctrl-C stops; then ./menu-ctl.sh session check {plan_path} {name})")
        offset = 0
        while notes.poll() is None:
            for child in children:
                child.poll()  # reap a collector that ended by itself
            offset = show_new(paths["log"], offset)
            time.sleep(1)
        show_new(paths["log"], offset)
        current = read_active()
        if current and (current.get("started"), current.get("process")) == (record["started"], name):
            print("[session] the pack-notes follower ended by itself; the session stops", file=sys.stderr)
            stop_session(expected=record)
            return 1
        print("[session] followers stopped")
        return 0
    except KeyboardInterrupt:
        print()
        stop_session(expected=record)
        print(f"next: ./menu-ctl.sh session check {plan_path} {name}")
        return 130
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)
        for child in children:
            child.poll()


def show_new(path: Path, offset: int) -> int:
    try:
        with path.open("rb") as stream:
            stream.seek(offset)
            data = stream.read()
    except OSError:
        return offset
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
    return offset + len(data)


def cmd_check(plan_path: Path, plan: dict, name: str, notes: Path | None, keep: bool) -> int:
    process = process_of(plan, name)
    name = process["name"]
    if not process.get("expect"):
        raise PlanError(f'{name} has no expectation file ("expect")')
    expect = plan_path.parent / process["expect"]
    if not expect.is_file():
        raise PlanError(f"no expectation file {expect}")
    print("Pack-note rules only; review operator observations, status/doctor output and klog separately.")
    if notes is None:
        paths = session_paths(plan_path, name)
        running = live_session()
        if running and (running.get("plan_name"), running.get("process")) == (plan_name(plan_path), name):
            if not keep:
                stop_session(expected=running)
        elif running:
            print(f"note: the running session is {running.get('plan_name')} {running.get('process')} (left running)")
        notes = paths["log"]
        if not notes.is_file():
            target = target_of(plan, process)
            print(f"no session log {notes}: checking the console's current pack-notes for {target}")
            env = dict(os.environ, GTAV_TARGET=target)
            return subprocess.run([str(MENU_CTL), "pack-check", str(expect.resolve())], cwd=ROOT, env=env).returncode
    text = notes.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    first = latest_process_start(lines)
    run_lines = lines[first:]
    print(f"{name}: {notes} lines {first + 1}..{len(lines)} (the latest GTA process) against {expect}")
    if not [line for line in run_lines if line.strip() and not line.startswith("#")]:
        print(f"{name}: NO PACK LINES in the latest process (was Load pressed? was the follower running?)")
        return 1
    ok, report = check_lines(expect, run_lines)
    for line in report:
        print(line)
    print(f"{name}: PACK NOTES {'PASS' if ok else 'FAIL'}")
    klog = session_paths(plan_path, name)["klog"]
    if klog.is_file():
        print(f"klog: {klog}")
    return 0 if ok else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    s = sub.add_parser("show")
    s.add_argument("plan", type=Path)
    s.add_argument("name", nargs="?")
    s = sub.add_parser("stage")
    s.add_argument("plan", type=Path)
    s.add_argument("--dry-run", action="store_true")
    s = sub.add_parser("select")
    s.add_argument("plan", type=Path)
    s.add_argument("name")
    s.add_argument("selection", nargs="?", default="load")
    s = sub.add_parser("next")
    s.add_argument("plan", type=Path)
    s.add_argument("name")
    s.add_argument("selection", nargs="?", default="load")
    s.add_argument("--dry-run", action="store_true", help="print the steps; select nothing")
    s = sub.add_parser("go")
    s.add_argument("plan", type=Path)
    s.add_argument("name")
    s.add_argument("--klog", action="store_true", help='also collect the kernel log (default: the plan\'s "klog")')
    s.add_argument("--timeout", type=float, default=900.0, help="seconds to wait for player control (0: no limit)")
    s.add_argument("--poll", type=float, default=5.0, help="seconds between readiness probes")
    s.add_argument("--dry-run", action="store_true", help="print the flow; touch nothing")
    s = sub.add_parser("check")
    s.add_argument("plan", type=Path)
    s.add_argument("name")
    s.add_argument("notes", type=Path, nargs="?")
    s.add_argument("--keep", action="store_true", help="leave the session's followers running")
    sub.add_parser("stop")
    args = parser.parse_args(argv)
    if args.command == "go" and (args.poll <= 0 or args.timeout < 0):
        parser.error("--poll must be positive and --timeout not negative")
    try:
        if args.command == "stop":
            return stop_session()
        plan = load_plan(args.plan)
        if args.command == "show":
            return cmd_show(plan, args.name)
        if args.command == "stage":
            return cmd_stage(plan, args.dry_run)
        if args.command == "select":
            return cmd_select(plan, args.name, args.selection)
        if args.command == "next":
            return cmd_next(args.plan, plan, args.name, args.selection, args.dry_run)
        if args.command == "go":
            return cmd_go(
                args.plan, plan, args.name, klog=args.klog, timeout=args.timeout, poll=args.poll, dry_run=args.dry_run
            )
        return cmd_check(args.plan, plan, args.name, args.notes, args.keep)
    except PlanError as error:
        print(f"hw_session: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

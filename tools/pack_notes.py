#!/usr/bin/env python3
"""Pack notes on the host: follow the worker's pack-notes ring and check a run's lines.

The worker keeps the last 128 "GTAVMenu pack ..." lines in a ring and rewrites <custom root>/pack-notes.log
about once a second (src/module/klog.c): a header `# pack notes START..END` and the lines START..END-1. A busy
load (several packs) can push early lines out of the ring before anyone reads them.

  merge --state FILE   read one snapshot of pack-notes.log on stdin and print only the lines not printed
                       before (by sequence number, kept in FILE). A gap (lines that left the ring between two
                       snapshots) and a new GTA process (the sequence restarts or an overlapping numbered
                       line changes) are printed as `#` lines. Snapshots carry no process identity: a restart
                       with identical overlapping lines or no overlap may be indistinguishable from progress.
                       `menu-ctl.sh pack-notes --follow` runs this once per poll.
  check EXPECT [NOTES] check pack-notes lines (a file, or stdin) against an expectation file:
                         pass: REGEX       at least one line matches
                         pass>=N: REGEX    at least N lines match
                         fail: REGEX       no line may match
                         title: TEXT       printed first; `#` lines and blank lines are ignored
                       Prints PASS / MISSING / FAIL per rule (with the first matching line) and exits 0 only
                       when every rule holds. A followed log's gap lines are reported as a warning.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

HEADER = re.compile(r"# pack notes (\d+)\.\.(\d+)\s*\Z")
SEQUENCE = re.compile(r"(\d+) ")
RULE = re.compile(r"(pass|fail)(?:>=(\d+))?:\s*(.+?)\s*\Z")
GAP = "# GAP:"


def parse(text: str) -> tuple[int, list[str]] | None:
    """(sequence number of the first line, lines) of one pack-notes.log snapshot, or None when it has no header."""
    lines = text.splitlines()
    while lines and not lines[0].strip():
        lines.pop(0)
    if not lines:
        return None
    match = HEADER.match(lines[0].strip())
    if not match:
        return None
    start, end = int(match.group(1)), int(match.group(2))
    body = lines[1:]
    # A snapshot cut short (the flush is temp + rename, so normally whole): keep the lines it has.
    return start, body[: max(0, end - start)]


def merge(text: str, state: dict) -> tuple[list[str], dict]:
    """The output lines for one snapshot given the state of the previous ones, and the new state."""
    parsed = parse(text)
    if parsed is None:
        return [], state
    start, body = parsed
    end = start + len(body)
    seen = int(state.get("end", 0))
    # A flusher can skip a half-written ring slot: use the printed sequence, never the body's row index.
    fingerprints = {
        match.group(1): hashlib.sha256(line.encode("utf-8")).hexdigest()
        for line in body[-128:]
        if (match := SEQUENCE.match(line))
    }
    previous = state.get("fingerprints", {})
    changed = next((number for number, digest in fingerprints.items() if previous.get(number, digest) != digest), None)
    out: list[str] = []
    if end < seen:  # a new GTA process: its worker counts from 0 again
        out.append(f"# NEW PROCESS: the sequence restarted ({seen} -> {end})")
        seen = 0
    elif changed is not None:
        out.append(f"# NEW PROCESS: overlapping line {changed} changed ({seen} -> {end})")
        seen = 0
    if start > seen:
        out.append(f"{GAP} lines {seen}..{start - 1} left the ring before this poll")
        seen = start
    out += body[seen - start :]
    return out, {"end": max(seen, end), "fingerprints": fingerprints}


@dataclass
class Rule:
    kind: str  # pass | fail
    count: int
    pattern: re.Pattern[str]
    text: str


def load_rules(path: Path) -> tuple[str, list[Rule]]:
    title = ""
    rules: list[Rule] = []
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("title:"):
            title = line[len("title:") :].strip()
            continue
        match = RULE.match(line)
        if not match:
            raise ValueError(f"{path}:{number}: expected 'pass: REGEX', 'pass>=N: REGEX' or 'fail: REGEX'")
        kind, count, pattern = match.group(1), match.group(2), match.group(3)
        if kind == "fail" and count:
            raise ValueError(f"{path}:{number}: 'fail' takes no count")
        try:
            compiled = re.compile(pattern)
        except re.error as error:
            raise ValueError(f"{path}:{number}: bad regex: {error}") from error
        rules.append(Rule(kind, int(count or 1), compiled, pattern))
    if not rules:
        raise ValueError(f"{path}: no rules")
    return title, rules


def check(rules: list[Rule], lines: list[str]) -> tuple[bool, list[str]]:
    """(every rule holds, report lines)."""
    notes = [line for line in lines if not line.startswith("#")]
    report: list[str] = []
    ok = True
    for rule in rules:
        hits = [line for line in notes if rule.pattern.search(line)]
        if rule.kind == "pass":
            good = len(hits) >= rule.count
            status = "PASS   " if good else "MISSING"
            counted = f"{len(hits)}/{rule.count}" if rule.count > 1 or not good else f"{len(hits)}"
        else:
            good = not hits
            status = "ok     " if good else "FAIL   "
            counted = f"{len(hits)}"
        ok &= good
        report.append(f"{status} [{counted}] {rule.kind}: {rule.text}")
        if hits and (rule.kind == "fail" or rule.count == 1):
            report.append(f"          {hits[0][:200]}")
    gaps = [line for line in lines if line.startswith(GAP)]
    if gaps:
        report.append(f"WARNING: {len(gaps)} gap(s) in the followed log: lines left the ring unread")
        report += [f"          {line}" for line in gaps]
    restarts = [line for line in lines if line.startswith("# NEW PROCESS")]
    if restarts:
        report.append("WARNING: the log spans more than one GTA process: check only this run's part")
    return ok, report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    m = sub.add_parser("merge", help="print a snapshot's new lines (stdin), remembering them in --state")
    m.add_argument("--state", type=Path, required=True)
    c = sub.add_parser("check", help="check pack-notes lines against an expectation file")
    c.add_argument("expect", type=Path)
    c.add_argument("notes", type=Path, nargs="?", help="pack-notes text or a followed log (default: stdin)")
    args = parser.parse_args(argv)
    if args.command == "merge":
        try:
            state = json.loads(args.state.read_text(encoding="utf-8")) if args.state.is_file() else {}
        except (OSError, ValueError):
            state = {}
        out, state = merge(sys.stdin.read(), state)
        args.state.write_text(json.dumps(state), encoding="utf-8")
        for line in out:
            print(line)
        return 0
    try:
        title, rules = load_rules(args.expect)
    except (OSError, ValueError) as error:
        print(f"pack_notes: {error}", file=sys.stderr)
        return 2
    text = args.notes.read_text(encoding="utf-8", errors="replace") if args.notes else sys.stdin.read()
    lines = text.splitlines()
    ok, report = check(rules, lines)
    if title:
        print(title)
    print("\n".join(report))
    print("PACK NOTES: PASS" if ok else "PACK NOTES: NOT PASSED")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())

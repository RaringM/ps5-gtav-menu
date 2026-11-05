"""Fail-closed Git source identity for packaged build evidence."""

from __future__ import annotations

import subprocess
from pathlib import Path

UNKNOWN_COMMIT = "unknown"


def _git(root: Path, *arguments: str) -> str | None:
    """Return a Git command's stripped stdout, or None when Git is missing or the command fails."""
    try:
        result = subprocess.run(
            ["git", "-C", str(root), *arguments],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="surrogateescape",
        )
    except OSError:
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def source_state(root: Path) -> tuple[str, bool]:
    """Return (commit, dirty) for the checkout whose top level is exactly ``root``.

    The answer fails closed. A tree that sits inside another checkout without being its top level
    (an export staged under build/, say) would otherwise report that outer repository's HEAD and
    status, so it reads as ("unknown", True). So does a tree outside any repository, a missing Git,
    and any Git command that fails: nothing unverified ever reads as a clean, known commit.
    """
    unknown = (UNKNOWN_COMMIT, True)
    top = _git(root, "rev-parse", "--show-toplevel")
    if top is None:
        return unknown
    try:
        if Path(top).resolve() != Path(root).resolve():
            return unknown
    except (OSError, RuntimeError):
        return unknown
    commit = _git(root, "rev-parse", "--verify", "HEAD^{commit}")
    if not commit:
        return unknown
    status = _git(root, "status", "--porcelain", "--untracked-files=all", "--ignore-submodules=none")
    if status is None:
        return unknown
    return commit, bool(status)

"""Shared host build paths for isolated conversion and acceptance runs."""

from __future__ import annotations

import os
from pathlib import Path


def build_dir(root: Path) -> Path:
    """Resolve GTAV_HOST_BUILD_DIR against the checkout, defaulting to build/."""
    value = Path(os.environ.get("GTAV_HOST_BUILD_DIR") or "build")
    return (root / value).resolve()


def assets_dir(root: Path) -> Path:
    """Scratch asset outputs beneath the selected host build directory."""
    return build_dir(root) / "assets"

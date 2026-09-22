"""JSON and filesystem helpers shared across the GTAV-Menu tools."""

from __future__ import annotations

import hashlib
import json
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

HASH_CHUNK_SIZE = 8 * 1024 * 1024


def read_json(path: Path) -> dict[str, Any]:
    """Load a JSON object from ``path``."""
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, data: dict[str, Any]) -> None:
    """Write ``data`` to ``path`` as pretty JSON (2-space indent, trailing newline)."""
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def read_json_optional(path: Path) -> dict[str, Any] | None:
    """Load a JSON object from ``path``, or return ``None`` if it is missing.

    Returns ``None`` when the file does not exist or does not contain a JSON
    object (i.e. the decoded value is not a ``dict``).
    """
    if not path.exists():
        return None
    data = json.loads(path.read_text(encoding="utf-8"))
    return data if isinstance(data, dict) else None


def sha256_file(path: Path, *, chunk_size: int = HASH_CHUNK_SIZE) -> str:
    """Return the hex SHA-256 digest of ``path``, read in chunks."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(chunk_size)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def iso_mtime(path: Path) -> str:
    """Return the modification time of ``path`` as a second-precision UTC ISO string."""
    return datetime.fromtimestamp(path.stat().st_mtime, tz=UTC).replace(microsecond=0).isoformat()

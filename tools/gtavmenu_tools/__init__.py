"""Shared utilities for the GTAV-Menu tooling.

The publication branch intentionally carries only the modules needed by its
operator tools. Development-only catalog and ELF helpers are loaded lazily so
importing a production submodule does not require the private tool set.
"""

from __future__ import annotations

from importlib import import_module
from typing import Any

from .io import iso_mtime, read_json, read_json_optional, sha256_file, write_json
from .parsing import parse_int

_LAZY_EXPORTS = {
    "NativeReportError": (".elf", "NativeReportError"),
    "ProgramHeader": (".elf", "ProgramHeader"),
    "c_escape": (".catalog", "c_escape"),
    "category_order": (".catalog", "category_order"),
    "joaat": (".catalog", "joaat"),
    "load_validation": (".catalog", "load_validation"),
    "read_program_headers": (".elf", "read_program_headers"),
    "rel_to_repo": (".catalog", "rel_to_repo"),
    "render_catalog_header": (".catalog", "render_catalog_header"),
    "render_categories_header": (".catalog", "render_categories_header"),
    "write_generated": (".catalog", "write_generated"),
}


def __getattr__(name: str) -> Any:
    """Load private-only compatibility exports only when a caller requests one."""
    try:
        module_name, attribute = _LAZY_EXPORTS[name]
    except KeyError as exc:
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}") from exc
    value = getattr(import_module(module_name, __name__), attribute)
    globals()[name] = value
    return value


__all__ = [
    "NativeReportError",
    "ProgramHeader",
    "c_escape",
    "category_order",
    "iso_mtime",
    "joaat",
    "load_validation",
    "parse_int",
    "read_json",
    "read_json_optional",
    "read_program_headers",
    "rel_to_repo",
    "render_catalog_header",
    "render_categories_header",
    "sha256_file",
    "write_generated",
    "write_json",
]

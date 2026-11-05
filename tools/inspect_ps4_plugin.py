#!/usr/bin/env python3
"""Validate the OpenOrbis ELF and fake-SELF PRX used by the PS4 plugin (GoldHEN and nexHEN hosts)."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "tools"))

from gtavmenu_tools.target_profile import TargetProfileError, load_manifest  # noqa: E402


class ValidationError(RuntimeError):
    pass


BOOT_POOLS_MODES = ("off", "observe", "caps", "full")
# plugin_entry.cpp's kBootPoolsMarker: the boot-pool mode the PRX was built with.
BOOT_POOLS_MARKER = re.compile(rb"gtavmenu-boot-pools-mode=(off|observe|caps|full)\x00")


# OpenOrbis v0.5.4 (toolchain-llvm-18.tar.gz, sha256 3c7cd5bb...4526): the files that shape the PRX. The
# headers are one digest over each file's relative path and sha256 (tree_digest).
OPENORBIS_SHA256 = {
    "link.x": "e52b4bf39ecf6db66c861d130fe89a67aec6e34c5af394faf60331a1655769c1",
    "lib/libkernel.so": "0b52fd74a9ec02eed0e03a691b087cc2910f0901d0bcdd643c5d666aaa76d9c3",
    "lib/libSceLibcInternal.so": "bb320b5b16601bd3a9a4a7ed7f4a2e68bd0138373838264909f349aedb3558a3",
    "bin/linux/create-fself": "6232098c3eed1b801c97ae90f271315b898c11c5c630831abce7331b502f952c",
    "include/": "d6ff663ae0b03fb54df599829b2f7e3db2bf80b258319863c9bee7332f75b124",
}


def boot_pools_mode(data: bytes, what: str) -> str:
    """The one boot-pool mode marker in a built image; a missing or conflicting marker is an error."""
    modes = sorted({match.decode() for match in BOOT_POOLS_MARKER.findall(data)})
    if not modes:
        raise ValidationError(f"{what} carries no gtavmenu-boot-pools-mode marker")
    if len(modes) > 1:
        raise ValidationError(f"{what} carries conflicting boot-pool modes: {modes}")
    return modes[0]


@dataclass(frozen=True)
class Symbol:
    value: int
    kind: str
    binding: str
    visibility: str
    section: str
    name: str


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def readelf(path: Path, *arguments: str) -> str:
    try:
        return subprocess.check_output(["readelf", *arguments, str(path)], text=True, env={**os.environ, "LC_ALL": "C"})
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ValidationError(f"readelf failed for {path}") from exc


def symbols(path: Path) -> list[Symbol]:
    pattern = re.compile(r"^\s*\d+:\s+([0-9a-fA-F]+)\s+\d+\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(.+)$")
    result = []
    for line in readelf(path, "-sW").splitlines():
        match = pattern.match(line)
        if not match:
            continue
        value, kind, binding, visibility, section, name = match.groups()
        result.append(Symbol(int(value, 16), kind, binding, visibility, section, name))
    return result


def tree_digest(root: Path) -> str:
    """One sha256 over every file under root: its relative path and its own sha256, in path order."""
    combined = hashlib.sha256()
    for relative in sorted(path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()):
        combined.update(f"{relative}\0{digest(root / relative)}\n".encode())
    return combined.hexdigest()


def toolchain_fingerprint(toolchain: Path) -> dict[str, object]:
    """The OpenOrbis files that shape the PRX, and whether they are the pinned v0.5.4 ones."""
    files = {}
    for name in OPENORBIS_SHA256:
        path = toolchain / name
        if name.endswith("/"):
            if not path.is_dir():
                raise ValidationError(f"OpenOrbis toolchain directory is missing: {path}")
            files[name] = tree_digest(path)
        else:
            if not path.is_file():
                raise ValidationError(f"OpenOrbis toolchain file is missing: {path}")
            files[name] = digest(path)
    return {"files": files, "pinned": files == OPENORBIS_SHA256}


def compiler_identity(elf: Path) -> list[str]:
    """The compiler and linker lines the build left in the ELF's .comment section."""
    text = readelf(elf, "-p", ".comment")
    return sorted({line.strip() for line in re.findall(r"^\s*\[\s*[0-9a-fA-F]+\]\s+(.+)$", text, re.M)})


def require_export(rows: list[Symbol], name: str, *, binding: str, visibility: str) -> Symbol:
    candidates = [
        row
        for row in rows
        if row.name == name and row.section != "UND" and row.binding == binding and row.visibility == visibility
    ]
    if not candidates:
        raise ValidationError(f"missing {binding.lower()}/{visibility.lower()} export: {name}")
    return candidates[0]


def validate(elf: Path, prx: Path, target_manifest: Path, toolchain: Path | None = None) -> dict[str, object]:
    """Check the plugin ELF and PRX; with toolchain (the OpenOrbis root), also fingerprint it."""
    for path in (elf, prx, target_manifest):
        if not path.is_file():
            raise ValidationError(f"required input is missing: {path}")
    try:
        target, profile = load_manifest(target_manifest)
    except (OSError, ValueError, TargetProfileError) as exc:
        raise ValidationError(f"invalid target manifest {target_manifest}: {exc}") from None
    if profile["platform"] != "ps4":
        raise ValidationError(f"{target_manifest} is not a PS4 target manifest")

    data = elf.read_bytes()
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise ValidationError("intermediate is not a little-endian ELF64 image")
    elf_type, machine = struct.unpack_from("<HH", data, 16)
    if elf_type != 3 or machine != 62:
        raise ValidationError("intermediate must be an x86-64 ET_DYN image")

    rows = symbols(elf)
    exported = {}
    for name in ("plugin_load", "plugin_unload", "g_pluginName", "g_pluginDesc", "g_pluginAuth", "g_pluginVersion"):
        require_export(rows, name, binding="GLOBAL", visibility="DEFAULT")
        exported[name] = "global/default"
    for name in ("module_start", "module_stop"):
        candidates = [
            row
            for row in rows
            if row.name == name
            and row.section != "UND"
            and row.binding in {"WEAK", "LOCAL"}
            and row.visibility == "HIDDEN"
        ]
        if not candidates:
            raise ValidationError(f"missing weak-or-local/hidden module callback: {name}")
        exported[name] = "weak-or-local/hidden"

    dynamic = readelf(elf, "-dW")
    entries = {name: int(value, 16) for name, value in re.findall(r"\((INIT|FINI)\)\s+0x([0-9a-fA-F]+)", dynamic)}
    if entries.keys() != {"INIT", "FINI"}:
        raise ValidationError("dynamic INIT/FINI tags are missing")
    for name, tag in (("_init", "INIT"), ("_fini", "FINI")):
        candidates = [row for row in rows if row.name == name and row.section != "UND"]
        if not candidates or entries[tag] not in {row.value for row in candidates}:
            raise ValidationError(f"dynamic {tag} does not resolve to {name}")

    allowed_undefined = {"getpid"}
    undefined = sorted(
        {row.name for row in rows if row.section == "UND" and row.binding in {"GLOBAL", "WEAK"} and row.name}
    )
    unexpected = [
        name for name in undefined if name not in allowed_undefined and not name.startswith(("sceKernel", "scePthread"))
    ]
    if unexpected:
        raise ValidationError(f"unclassified PRX imports: {unexpected}")

    needed = re.findall(r"Shared library: \[([^]]+)]", dynamic)
    allowed_providers = {"libkernel.so", "libSceLibcInternal.so", "libSceLibcInternal.prx", "libc.prx"}
    if not needed or any(name not in allowed_providers for name in needed):
        raise ValidationError(f"unexpected PRX providers: {needed}")

    writable_executable = []
    for line in readelf(elf, "-lW").splitlines():
        match = re.match(r"^\s*LOAD\s+.*\s(RWE|RWX)\s", line)
        if match:
            writable_executable.append(line.strip())
    if writable_executable:
        raise ValidationError("PRX contains a writable executable LOAD segment")

    self_data = prx.read_bytes()
    if len(self_data) < 16 or self_data[:4] != bytes.fromhex("4f153d1d"):
        raise ValidationError("final PRX is not a fake SELF container")
    # The fake SELF keeps its segments plain, so the PRX must carry the ELF's marker too.
    mode = boot_pools_mode(data, "intermediate ELF")
    if boot_pools_mode(self_data, "final PRX") != mode:
        raise ValidationError("final PRX and intermediate ELF name different boot-pool modes")

    return {
        "schemaVersion": 1,
        "kind": "gtavmenu-ps4-plugin-validation",
        "hardwareValidated": False,
        "targetId": target["targetId"],
        "titleId": target["titleId"],
        "contentVersion": target["contentVersion"],
        "hosts": ["GoldHEN 2.3+", "nexHEN PS4 v1.00"],
        "bootPoolsMode": mode,
        "probeBuild": mode != "full",
        "artifacts": {
            "elf": {"path": elf.name, "size": len(data), "sha256": digest(elf)},
            "prx": {"path": prx.name, "size": len(self_data), "sha256": digest(prx)},
        },
        "exports": exported,
        "entryPoints": entries,
        "imports": undefined,
        "providers": needed,
        "writableExecutableLoad": False,
        "toolchain": {
            "compiler": compiler_identity(elf),
            "openOrbis": toolchain_fingerprint(toolchain) if toolchain is not None else None,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--prx", required=True, type=Path)
    parser.add_argument("--target-manifest", required=True, type=Path)
    parser.add_argument("--toolchain", type=Path, help="the OpenOrbis toolchain root, to fingerprint it")
    parser.add_argument("--output", type=Path, help="write validation JSON here as well as stdout")
    args = parser.parse_args()
    try:
        report = validate(args.elf, args.prx, args.target_manifest, args.toolchain)
    except (OSError, ValueError, json.JSONDecodeError, ValidationError) as exc:
        parser.error(str(exc))
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

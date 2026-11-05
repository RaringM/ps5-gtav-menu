#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate bounded SCE imports and exports for the nexHEN menu PRX.

Adapted from ampr_emu's GPL-3.0 generate_payload_exports.py. The provider
allowlist is intentionally fixed to the libraries used by the production menu.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path


def load_nid_salt(path: Path) -> bytes:
    encoded = "".join(line.partition("#")[0].strip() for line in path.read_text(encoding="ascii").splitlines())
    salt = bytes.fromhex(encoded)
    if len(salt) != 16:
        raise ValueError(f"invalid SCE NID salt length in {path}")
    return salt


NID_SALT_PATH = Path(__file__).resolve().parents[2] / "data/nexhen/nid-salt.txt"
NID_SALT = load_nid_salt(NID_SALT_PATH)
PLUGIN_EXPORTS = (
    "plugin_load",
    "plugin_unload",
    "g_pluginName",
    "g_pluginDesc",
    "g_pluginAuth",
    "g_pluginVersion",
)
# Only the broker lane exports the activation contract; stock nexHEN never reads it.
CONTRACT_EXPORT = "gtav_nexhen_contract"
EXPORTS = (*PLUGIN_EXPORTS, CONTRACT_EXPORT)


def export_names(with_contract: bool) -> tuple[str, ...]:
    return EXPORTS if with_contract else PLUGIN_EXPORTS


@dataclass(frozen=True)
class Provider:
    key: str
    name: str
    sdk_library: str
    library_id: int
    module_id: int

    @property
    def suffix(self) -> str:
        return f"#{id_char(self.library_id)}#{id_char(self.module_id)}"


PROVIDERS = (
    Provider("libc", "libSceLibcInternal", "libSceLibcInternal.so", 0, 1),
    Provider("kernel", "libkernel", "libkernel.so", 1, 2),
    Provider("pad", "libScePad", "libScePad.so", 2, 3),
    Provider("user", "libSceUserService", "libSceUserService.so", 3, 4),
    Provider("system", "libSceSystemService", "libSceSystemService.so", 4, 5),
)
EXPORT_LIBRARY_ID = 6
EXPORT_MODULE_ID = 0


def id_char(value: int) -> str:
    alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"
    if not 0 <= value < len(alphabet):
        raise ValueError(f"SCE id out of range: {value}")
    return alphabet[value]


def name_to_nid(name: str) -> str:
    digest = hashlib.sha1(name.encode("ascii") + NID_SALT).digest()
    return base64.b64encode(digest[:8][::-1], altchars=b"+-").decode().rstrip("=")


def defined_symbols(nm: Path, library: Path) -> set[str]:
    output = subprocess.check_output(
        [str(nm), "-D", "--defined-only", "--format=just-symbols", str(library)],
        text=True,
    )
    return set(output.split())


def strong_undefined_symbols(text: str) -> set[str]:
    """Return imports that need providers, excluding unresolved weak hooks."""
    symbols: set[str] = set()
    for line_number, line in enumerate(text.splitlines(), 1):
        fields = line.split()
        if not fields:
            continue
        if len(fields) != 2 or fields[0] not in {"U", "u", "w", "v"}:
            raise ValueError(f"invalid nm record at line {line_number}: {line!r}")
        symbol_type, name = fields
        if symbol_type.lower() in {"w", "v"}:
            continue
        symbols.add(name)
    return symbols


def stub(names: list[str], suffix: str) -> str:
    lines = ["/* Link-only import provider; never installed on the console. */", ".text"]
    for name in names:
        symbol = name_to_nid(name) + suffix
        lines.extend(
            [
                f'.globl "{symbol}"',
                f'.type "{symbol}", @function',
                f'"{symbol}":',
                "    ret",
                f'.size "{symbol}", .-"{symbol}"',
            ]
        )
    lines.append('.section .note.GNU-stack,"",@progbits')
    return "\n".join(lines) + "\n"


def generate(undefined: Path, directory: Path, module: str, sdk: Path, nm: Path, with_contract: bool = True) -> None:
    exports = export_names(with_contract)
    imports = sorted(strong_undefined_symbols(undefined.read_text(encoding="utf-8")))
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,63}", module):
        raise ValueError("module name must be 1-64 ASCII letters, digits or underscores")

    provider_symbols = {
        provider.key: defined_symbols(nm, sdk / "target/lib" / provider.sdk_library) for provider in PROVIDERS
    }
    assignments: dict[str, Provider] = {}
    unknown: list[str] = []
    ambiguous: dict[str, list[str]] = {}
    for symbol in imports:
        matches = [provider for provider in PROVIDERS if symbol in provider_symbols[provider.key]]
        if not matches:
            unknown.append(symbol)
        elif len(matches) > 1:
            ambiguous[symbol] = [provider.key for provider in matches]
        else:
            assignments[symbol] = matches[0]
    if unknown or ambiguous:
        raise ValueError(f"unclassified imports={unknown}, ambiguous imports={ambiguous}")

    active = [provider for provider in PROVIDERS if provider in assignments.values()]
    directory.mkdir(parents=True, exist_ok=True)
    export_suffix = f"#{id_char(EXPORT_LIBRARY_ID)}#{id_char(EXPORT_MODULE_ID)}"
    exported = [name_to_nid(name) + export_suffix for name in exports]
    rename = [f'"--redefine-sym={name}={symbol}"' for name, symbol in zip(exports, exported, strict=True)]
    for name, provider in sorted(assignments.items()):
        rename.append(f'"--redefine-sym={name}={name_to_nid(name)}{provider.suffix}"')

    metadata_names = [*(provider.name for provider in active), module, module + ".prx"]
    contents = {
        "rename.rsp": "\n".join(rename) + "\n",
        "exports.rsp": "".join(f'"--export-dynamic-symbol={name}"\n' for name in [*exported, *metadata_names]),
        "exports.map": "{\n  global:\n"
        + "".join(f'    "{name}";\n' for name in [*exported, *metadata_names])
        + "  local: *;\n};\n",
    }
    for provider in active:
        names = sorted(name for name, assigned in assignments.items() if assigned == provider)
        contents[provider.key + ".S"] = stub(names, provider.suffix)
        contents[provider.key + ".txt"] = "\n".join(names) + "\n"

    metadata = [".data"]
    for name in metadata_names:
        metadata.extend(
            [
                f'.globl "{name}"',
                f'.protected "{name}"',
                f'.type "{name}", @object',
                f'"{name}":',
                "    .byte 0",
                f'.size "{name}", 1',
            ]
        )
    metadata.append('.section .note.GNU-stack,"",@progbits')
    contents["metadata.S"] = "\n".join(metadata) + "\n"
    contents["providers.json"] = (
        json.dumps(
            {
                "module": module,
                "export_library_id": EXPORT_LIBRARY_ID,
                "export_module_id": EXPORT_MODULE_ID,
                "providers": [asdict(provider) for provider in active],
                "imports": {name: provider.key for name, provider in sorted(assignments.items())},
                "exports": list(exports),
            },
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )
    for name, content in contents.items():
        (directory / name).write_text(content, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("undefined", type=Path)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--module", required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--nm", type=Path, required=True)
    parser.add_argument(
        "--without-contract",
        action="store_true",
        help="omit the broker activation contract export (stock nexHEN and probe builds)",
    )
    args = parser.parse_args()
    generate(args.undefined, args.directory, args.module, args.sdk, args.nm, not args.without_contract)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

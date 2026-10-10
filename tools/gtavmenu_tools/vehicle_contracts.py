"""Reviewed vehicle format layouts, independent of executable or retail-instance data.

The private generator selects serialized fields and arithmetic rules. Runtime tools
read this exact reviewed projection, never the executable/disassembly inputs used
to establish it. Retail shader/material instances are separate user-supplied inputs.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from .asset_formats import AssetError

KIND = "gtavmenu-vehicle-format-contracts"
SCHEMA_VERSION = 1
DEFAULT = Path(__file__).resolve().parents[2] / "data/vehicle_contracts/ppsa04264-01.010.002.json"
MAX_FILE_BYTES = 1 << 20
# Updated only together with the reviewed generator output and its mutation tests.
REVIEWED_SHA256 = "966fe484f854d0cc191a0dcf1cce89d738a138eba859d60187051a96c54183c0"


def canonical(value) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def digest(value) -> str:
    return hashlib.sha256(canonical(value)).hexdigest()


def _unique_object(pairs):
    out = {}
    for key, value in pairs:
        if key in out:
            raise AssetError(f"vehicle contract contains duplicate key {key}")
        out[key] = value
    return out


def load(path: Path = DEFAULT) -> dict:
    """Read only the versioned, content-pinned projection; a recomputed hash is not approval."""
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise AssetError("vehicle contracts must be a regular file")
    with path.open("rb") as source:
        raw = source.read(MAX_FILE_BYTES + 1)
    if len(raw) > MAX_FILE_BYTES:
        raise AssetError("vehicle contracts exceed the input bound")
    try:
        document = json.loads(raw, object_pairs_hook=_unique_object)
    except (ValueError, UnicodeError) as exc:
        raise AssetError(f"vehicle contracts are not valid JSON: {exc}") from None
    if not isinstance(document, dict) or set(document) != {
        "kind",
        "schemaVersion",
        "attribution",
        "contractsSha256",
        "contracts",
    }:
        raise AssetError("vehicle contracts document fields differ")
    if document["kind"] != KIND or type(document["schemaVersion"]) is not int or document["schemaVersion"] != 1:
        raise AssetError("unsupported vehicle contracts kind or schema")
    contracts = document["contracts"]
    if not isinstance(contracts, dict) or set(contracts) != {"target", "pc", "native"}:
        raise AssetError("vehicle contracts sections differ")
    if digest(contracts) != document["contractsSha256"] or digest(contracts) != REVIEWED_SHA256:
        raise AssetError("vehicle contracts differ from the reviewed content identity")
    return contracts


def check_target(manifest: dict, contracts: dict | None = None) -> None:
    """Pin every target field read by vehicle conversion, allowing unrelated menu metadata edits."""
    contracts = load() if contracts is None else contracts
    loader = manifest.get("loader")
    if not isinstance(loader, dict):
        raise AssetError("vehicle conversion target has no loader identity")
    selected = {
        "targetId": manifest.get("targetId"),
        "binaryIdentity": manifest.get("binaryIdentity"),
        "loader": {"runtimeImageBias": loader.get("runtimeImageBias")},
    }
    if selected != contracts["target"]:
        raise AssetError("vehicle conversion target identity differs from the reviewed build")


def texture_reference(contracts: dict | None = None) -> bytes:
    """Build the name-only texture profile from named fields; zero pointer/class slots."""
    contracts = load() if contracts is None else contracts
    layout = contracts["pc"]["textureReference"]["native"]
    initializer = contracts["native"]["textureReference"]
    if layout["bytes"] != initializer["objectBytes"]:
        raise AssetError("texture-reference initializer extent differs from its field layout")
    raw = bytearray(layout["bytes"])
    for name, value in initializer["fields"].items():
        field = layout["fields"][name]
        raw[field["offset"] : field["offset"] + field["bytes"]] = value.to_bytes(field["bytes"], "little")
    return bytes(raw)

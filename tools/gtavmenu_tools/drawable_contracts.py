"""Frozen drawable contracts: the layouts and shader tables the PC -> PS5 drawable converters read.

The drawable converters (convert_pc_drawable and the model, ped, clothing, wheel and collision routes on
top of it) need the PC Legacy .ydr v165 field layouts (drawable, model, geometry, vertex buffer and
declaration, shader group / FX / parameters, texture base), the Gen9 layouts their writers emit
(declaration, vertex/index buffer, view, shader and shader-parameter-info) and the Legacy -> Gen9 shader
parameter mappings. They used to parse those at every run from nine pinned CodeWalker source files; the
result is frozen here as one reviewed data file, data/drawable_contracts/<name>.json:

  {"kind": "gtavmenu-drawable-contracts", "schemaVersion": 1, "attribution": {...}, "sources": [...],
   "generator": ..., "omitted": [...], "contractsSha256": "<sha256 of canonical(contracts)>",
   "contracts": {"geometry": ..., "material": ..., "mesh": ..., "model": ..., "pcMesh": ..., "shader": ...}}

`contracts` is the derivation's output with the keys in OMITTED removed (the derivation's evidence
excerpts, which quote the sources, and the MetaNames hash -> name table: nothing on the drawable route
reads them). devtools/generators/generate_drawable_contracts.py rebuilds the file from the pinned sources
(`--check` compares); this module only reads it and needs neither the sources nor a third-party package.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from .asset_formats import AssetError

KIND = "gtavmenu-drawable-contracts"
SCHEMA_VERSION = 1
CONTRACTS_DIR = Path(__file__).resolve().parents[2] / "data/drawable_contracts"
DEFAULT = CONTRACTS_DIR / "codewalker-485d56b.json"
SECTIONS = ("geometry", "material", "mesh", "model", "pcMesh", "shader")
# Keys the frozen file leaves out, wherever they occur: the derivation's evidence (source line numbers and
# quoted source excerpts) and the MetaNames table (only the research YFT material inspection reads it).
OMITTED = (
    "baseEvidence",
    "bindingEvidence",
    "evidence",
    "formulaEvidence",
    "geometryExtentEvidence",
    "metaNameEntryCount",
    "metaNames",
    "modelExtentEvidence",
    "renameEvidence",
    "stopEvidence",
    "streamParameterEvidence",
    "triangleAdmissionEvidence",
)
MAX_FILE_BYTES = 8 << 20


def canonical(value) -> bytes:
    """The hashed form: sorted keys, no whitespace, ASCII."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def digest(contracts: dict) -> str:
    return hashlib.sha256(canonical(contracts)).hexdigest()


def project(contracts: dict) -> dict:
    """The frozen form of derived contracts: OMITTED keys dropped, JSON types (tuples become lists, keys strings)."""
    if set(contracts) != set(SECTIONS):
        raise AssetError(f"drawable contracts must have exactly the sections {', '.join(SECTIONS)}")

    def strip(value):
        if isinstance(value, dict):
            return {key: strip(item) for key, item in value.items() if key not in OMITTED}
        if isinstance(value, (list, tuple)):
            return [strip(item) for item in value]
        return value

    return json.loads(canonical(strip(contracts)))


def render(header: dict, contracts: dict) -> bytes:
    """The file bytes: the header fields in their order, `omitted`, `contractsSha256`, then `contracts` with its
    keys sorted; deterministic."""
    document = dict(header) | {
        "omitted": list(OMITTED),
        "contractsSha256": digest(contracts),
        "contracts": json.loads(canonical(contracts)),
    }
    return (json.dumps(document, indent=1, allow_nan=False) + "\n").encode()


def load(path: Path = DEFAULT) -> dict:
    """The contracts of a frozen file, after its kind, version, sections and sha256 are checked."""
    data = Path(path).read_bytes()
    if len(data) > MAX_FILE_BYTES:
        raise AssetError(f"{path}: drawable contracts file is larger than {MAX_FILE_BYTES} bytes")
    try:
        document = json.loads(data)
    except ValueError as error:
        raise AssetError(f"{path}: drawable contracts file is not JSON ({error})") from None
    if not isinstance(document, dict) or document.get("kind") != KIND:
        raise AssetError(f"{path}: not a drawable contracts file (kind {KIND})")
    if document.get("schemaVersion") != SCHEMA_VERSION:
        raise AssetError(f"{path}: drawable contracts schemaVersion {document.get('schemaVersion')} is not supported")
    contracts = document.get("contracts")
    if not isinstance(contracts, dict) or set(contracts) != set(SECTIONS):
        raise AssetError(f"{path}: drawable contracts must have exactly the sections {', '.join(SECTIONS)}")
    if document.get("contractsSha256") != digest(contracts):
        raise AssetError(f"{path}: drawable contracts differ from their contractsSha256 (edited by hand?)")
    return contracts

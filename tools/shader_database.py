"""Bounded compiled shader-bank decoding using reviewed serialized layouts.

Shader programs remain opaque spans; no executable or private report is read.
Bank bytes come from the caller's verified retail template cache.
"""

from __future__ import annotations

import hashlib
import struct
from pathlib import Path

from gtavmenu_tools import retail_templates, vehicle_contracts, vehicle_templates
from gtavmenu_tools.asset_formats import AssetError
from gtavmenu_tools.io import sha256_file

BANK_DIRECTORY = "razor-shader-database-source-a"
BANK_NAMES = ("sga_prospero_final.awc", "sga_prospero_final_init.awc")


def unique(rows, label):
    if len(rows) != 1:
        raise AssetError(f"{label}: expected one item, found {len(rows)}")
    return rows[0]


def bank_path(directory, name):
    if name not in BANK_NAMES:
        raise AssetError("unreviewed compiled shader bank")
    directory = Path(directory)
    return directory / name if directory.name == BANK_DIRECTORY else directory / BANK_DIRECTORY / name


def read_banks(directory, catalog=None, *, manifest=None):
    """Read only reviewed bank names and reject changes since catalog construction."""
    blobs = {}
    for name in BANK_NAMES:
        data = vehicle_templates.verified_input(
            bank_path(directory, name), f"{BANK_DIRECTORY}/{name}", manifest=manifest
        )
        if catalog is not None:
            row = unique([r for r in catalog["banks"] if r["file"] == name], "compiled shader bank identity")
            if len(data) != row["bytes"] or hashlib.sha256(data).hexdigest() != row["sha256"]:
                raise AssetError("compiled shader source bank changed")
        blobs[name] = data
    return blobs


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, count):
        if not isinstance(count, int) or count < 0 or count > len(self.data) - self.at:
            raise AssetError(f"shader database span outside input at {self.at}: {count}")
        result = self.data[self.at : self.at + count]
        self.at += count
        return result

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def name(self):
        raw = self.take(self.take(1)[0])
        if not raw or raw[-1:] != b"\0" or b"\0" in raw[:-1]:
            raise AssetError("shader database name is not a single terminated string")
        try:
            return raw[:-1].decode("ascii")
        except UnicodeError as exc:
            raise AssetError("shader database name is not ASCII") from exc

    def block(self, stride=1):
        count = self.u32()
        at = self.at
        raw = self.take(count * stride)
        return {"offset": at, "count": count, "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}, raw


def parameters(raw, contract):
    def get(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(raw):
            raise AssetError("shader database parameter descriptor escapes its block")
        return struct.unpack_from(fmt, raw, offset)

    count = get("<H", 0)[0]
    if contract["headerBytes"] + count * contract["stride"] > len(raw):
        raise AssetError("shader database parameter table is truncated")
    result = []
    for index in range(count):
        at = contract["headerBytes"] + index * contract["stride"]
        if not get("<B", at + contract["activeOffset"])[0] & contract["activeMask"]:
            continue
        kind = (get("<B", at)[0] >> contract["kindShift"]) & contract["kindMask"]
        if kind == contract["constantKind"]:
            if not get("<H", at + contract["constantSizeOffset"])[0]:
                continue
            member_count = get("<B", at + contract["memberCountOffset"])[0]
            relative = get("<H", at + contract["membersOffset"])[0]
            if member_count and not relative:
                raise AssetError("shader database constant buffer has a null member pointer")
            entries = [
                (at + relative + i * contract["memberStride"], contract["memberHashOffset"])
                for i in range(member_count)
            ]
        else:
            entries = [(at, contract["hashOffset"])]
        for offset, hashes in entries:
            primary, secondary = get("<II", offset + hashes)
            result.append(
                {
                    "kind": kind,
                    "primaryHash": primary,
                    "secondaryHash": secondary,
                    "descriptorOffset": offset,
                    "parentIndex": index,
                }
            )
    return result


def decode(data, contract):
    r = Reader(data)
    if r.take(4).hex() != contract["header"]["magicHex"]:
        raise AssetError("shader database magic differs")
    programs = []
    for profile in contract["programClasses"]:
        count = r.u32()
        if count > len(data) // 6:
            raise AssetError("shader database program count exceeds input")
        group = []
        for _ in range(count):
            start = r.at
            name, version = r.name(), r.take(1)[0]
            if version >= profile["versionUpperBound"]:
                raise AssetError("shader database program requires an unsupported alternate encoding")
            blob, _ = r.block()
            r.take(profile["hashBytes"])
            r.take(profile["metadataBytes"])
            extra, _ = r.block()
            group.append(
                {
                    "name": name,
                    "version": version,
                    "offset": start,
                    "bytes": r.at - start,
                    "program": blob,
                    "metadataExtension": extra,
                }
            )
        programs.append(group)
    count = r.u32()
    if count > len(data) // 6:
        raise AssetError("shader database effect count exceeds input")
    effects = []
    for _ in range(count):
        start = r.at
        name, storage = r.name(), r.u32()
        arrays = []
        for group in programs:
            info, raw = r.block(4)
            indices = [v[0] for v in struct.iter_unpack("<I", raw)]
            if any(i >= len(group) for i in indices):
                raise AssetError("shader database effect refers to an absent program")
            arrays.append(info | {"indices": indices})
        for size in contract["fixedBytes"]:
            r.take(size)
        states = [r.block(stride)[0] for stride in contract["stateStrides"]]
        blocks = [r.block(stride) for stride in contract["blockStrides"]]
        optional_count = r.u32()
        if optional_count > len(data) // 4:
            raise AssetError("shader database optional blob count exceeds input")
        optional = [r.block()[0] for _ in range(optional_count)]
        final, _ = r.block()
        pointer = contract["pointerBytes"]
        computed = sum((a["count"] + 1) * pointer for a in arrays)
        computed += sum(
            info["bytes"] + info["count"] * shadow
            for (info, _), shadow in zip(blocks, contract["blockShadowBytes"], strict=True)
        )
        if optional:
            computed = (computed + pointer - 1) & -pointer
        computed += sum(b["bytes"] + pointer for b in optional) + final["bytes"]
        if computed != storage:
            raise AssetError("shader database reconstructed storage extent differs")
        effects.append(
            {
                "name": name,
                "offset": start,
                "bytes": r.at - start,
                "storageBytes": storage,
                "sha256": hashlib.sha256(data[start : r.at]).hexdigest(),
                "programArrays": arrays,
                "states": states,
                "blocks": [b[0] for b in blocks],
                "optional": optional,
                "final": final,
                "parameters": parameters(blocks[4][1], contract["parameters"]),
            }
        )
    if r.at != len(data) or len({e["name"] for e in effects}) != len(effects):
        raise AssetError("shader database has trailing bytes or duplicate effects")
    return {"programs": programs, "effects": effects, "consumedBytes": r.at}


def load(directory, material, contract=None, *, manifest=None):
    contract = vehicle_contracts.load()["native"]["shaderDatabase"] if contract is None else contract
    # Read the manifest as identity metadata only; no private source manifest is needed.
    manifest_path = manifest or retail_templates.manifest_path()
    manifest_hash = sha256_file(manifest_path)
    blobs = read_banks(directory, manifest=manifest)
    effects, banks = [], []
    needed = {row["name"] for row in material["mappings"].values()}
    for name, data in blobs.items():
        decoded = decode(data, contract)
        effects.extend(e | {"bank": name} for e in decoded["effects"] if e["name"] in needed)
        banks.append(
            {
                "file": name,
                "sha256": hashlib.sha256(data).hexdigest(),
                "bytes": len(data),
                "programCounts": [len(g) for g in decoded["programs"]],
                "effectCount": len(decoded["effects"]),
                "consumedBytes": decoded["consumedBytes"],
            }
        )
    missing = needed - {e["name"] for e in effects}
    if missing or len({e["name"] for e in effects}) != len(effects):
        raise AssetError(f"required compiled effects missing or ambiguous: {sorted(missing)}")
    if sha256_file(manifest_path) != manifest_hash or any(
        sha256_file(bank_path(directory, n)) != hashlib.sha256(b).hexdigest() for n, b in blobs.items()
    ):
        raise AssetError("compiled shader input changed during decoding")
    return {
        "sourceTemplateManifestSha256": manifest_hash,
        "provenance": "verified-runtime-template",
        "banks": banks,
        "nativeContract": contract,
        "requiredEffects": sorted(effects, key=lambda e: e["name"]),
        "qualification": {
            "allBankRecordExtentsDecoded": True,
            "requiredCompiledEffectRecordsAvailable": True,
            "runtimeRemappingQualified": False,
            "materialDefaultsQualified": False,
            "shaderEffectsExecuted": False,
            "runtimeEnabled": False,
            "nativeGpuLayoutValidated": False,
            "hardwareQualified": False,
        },
    }

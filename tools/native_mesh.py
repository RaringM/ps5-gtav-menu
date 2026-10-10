"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import struct

import native_geometry_buffers as buffers
import native_geometry_formats as formats
from gen9_mesh import (  # noqa: F401
    PACK,
    WIDTHS,
    build,
    declaration,
    digest,
    full_headers,
    get,
    inverse_vertices,
    put,
    source_bytes,
    view_bytes,
)
from gtavmenu_tools.asset_formats import AssetError


def retail_check(system: bytes, graphics: bytes, bases: list, geometries: list, contract: dict, native: dict) -> dict:
    observed = buffers.read_buffers(system, graphics, bases, geometries, native["buffers"])
    declarations = formats.read_declarations(system, bases[0], observed["buffers"], native["formats"])
    full_headers(system, bases[0], observed["buffers"], contract, True)
    for row in declarations["declarations"]:
        vertex = observed["buffers"][row["geometry"] * 2]
        start, length = vertex["dataOffset"], vertex["byteLength"]
        space = system if vertex["dataSpace"] == "system" else graphics
        at = row["declarationSystemOffset"]
        raw = system[at : at + contract["layouts"]["declaration"]["bytes"]]
        _, components = inverse_vertices(space[start : start + length], raw, row["stride"], contract)
        rebuilt, _ = declaration(components, row["stride"], contract)
        if rebuilt != raw:
            raise AssetError("complete native declaration differs from public conversion algorithm")
        geo = get(system, geometries[row["geometry"]]["systemOffset"], contract["layouts"]["geometry"])
        ib = observed["buffers"][row["geometry"] * 2 + 1]
        if (
            geo["VerticesCount"],
            geo["VertexStride"],
            geo["IndicesCount"],
            geo["TrianglesCount"],
            geo["Unknown_62h"],
        ) != (
            vertex["count"],
            vertex["stride"],
            ib["count"],
            ib["count"] // contract["triangleElements"],
            contract["triangleElements"],
        ) or ib[
            "stride"
        ] != contract[
            "indexElementBytes"
        ]:
            raise AssetError("native geometry scalar fields differ from public buffer/topology fields")
        allowed = {
            "VFT",
            "Unknown_4h",
            "VertexBufferPointer",
            "IndexBufferPointer",
            "BoneIdsPointer",
            "VertexDataPointer",
            "VerticesCount",
            "VertexStride",
            "IndicesCount",
            "TrianglesCount",
            "BoneIdsCount",
            "Unknown_62h",
        }
        if any(value for name, value in geo.items() if name not in allowed):
            raise AssetError("native retail geometry exceeds finite reserved-zero profile")
        if geo["BoneIdsCount"]:
            begin = geo["BoneIdsPointer"] - bases[0]
            if begin < 0 or begin % 2 or begin + geo["BoneIdsCount"] * contract["indexElementBytes"] > len(system):
                raise AssetError("native referenced palette exceeds system storage")
            # Retail palettes can be shared or placed separately. The public
            # writer's inline placement is an output policy, not a reader rule.
    return {
        "nativeGeometries": len(geometries),
        "completeDeclarationsMatched": declarations["declarationCount"],
        "completeBufferHeadersMatched": observed["bufferCount"],
        "completeViewsMatched": observed["bufferCount"],
        "nativeCodeExecuted": False,
    }


def verify(
    blob: bytes, placement: dict, contract: dict, native_buffers: dict, native_formats: dict, graphics_base: int
) -> dict:
    base = placement["sourceBase"]
    observed = buffers.read_buffers(blob, b"", (base, graphics_base), placement["geometries"], native_buffers)
    declarations = formats.read_declarations(blob, base, observed["buffers"], native_formats)
    full_headers(blob, base, observed["buffers"], contract, False)
    for row, decl in zip(placement["geometries"], declarations["declarations"], strict=True):
        pair = [b for b in observed["buffers"] if b["geometry"] == decl["geometry"]]
        vertex, index = pair
        if (
            vertex["count"],
            vertex["stride"],
            vertex["dataSha256"],
            index["count"],
            index["stride"],
            index["dataSha256"],
        ) != (
            row["vertexCount"],
            row["vertexStride"],
            row["nativeVertexSha256"],
            row["indexCount"],
            contract["indexElementBytes"],
            row["indicesSha256"],
        ):
            raise AssetError("native mesh reader differs from written buffer intent")
        raw = blob[vertex["dataOffset"] : vertex["dataOffset"] + vertex["byteLength"]]
        start = decl["declarationSystemOffset"]
        declaration_bytes = blob[start : start + contract["layouts"]["declaration"]["bytes"]]
        inverse, _ = inverse_vertices(raw, declaration_bytes, vertex["stride"], contract)
        if digest(inverse) != row["legacyVertexSha256"]:
            raise AssetError("native mesh readback differs from source attributes")
        geometry = get(blob, row["systemOffset"], contract["layouts"]["geometry"])
        expected_geometry = dict.fromkeys(geometry, 0) | {
            "VertexBufferPointer": base + vertex["systemOffset"],
            "IndexBufferPointer": base + index["systemOffset"],
            "VertexDataPointer": base + vertex["dataOffset"],
            "VerticesCount": vertex["count"],
            "VertexStride": vertex["stride"],
            "IndicesCount": index["count"],
            "TrianglesCount": index["count"] // contract["triangleElements"],
            "Unknown_62h": contract["triangleElements"],
            "BoneIdsCount": len(row["boneIds"]),
            "BoneIdsPointer": geometry["BoneIdsPointer"],
        }
        if geometry != expected_geometry:
            raise AssetError("native full geometry header differs from buffer and palette intent")
        palette = geometry["BoneIdsPointer"] - base
        if not row["boneIds"] and geometry["BoneIdsPointer"]:
            raise AssetError("empty native palette has a nonnull pointer")
        if row["boneIds"] and (
            palette < 0
            or palette + len(row["boneIds"]) * contract["indexElementBytes"] > len(blob)
            or list(struct.unpack_from("<" + "H" * len(row["boneIds"]), blob, palette)) != row["boneIds"]
        ):
            raise AssetError("native mesh bone palette readback differs")
        # Also reject corruption to inactive offset entries, which the selected
        # native builder does not access but the complete public writer defines.
        _, decoded_components = inverse_vertices(raw, declaration_bytes, vertex["stride"], contract)
        if declaration(decoded_components, vertex["stride"], contract)[0] != declaration_bytes:
            raise AssetError("complete written declaration differs from public conversion")
    return {
        "geometryCount": len(placement["geometries"]),
        "bufferCount": observed["bufferCount"],
        "bufferBytes": observed["totalBytes"],
        "declarationCount": declarations["declarationCount"],
        "vertexCount": sum(r["vertexCount"] for r in placement["geometries"]),
        "indexCount": sum(r["indexCount"] for r in placement["geometries"]),
        "allSourceAttributeBytesRecovered": True,
        "nativeCodeExecuted": False,
    }

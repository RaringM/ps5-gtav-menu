"""Bounded diagnostic PC draw context; topology admission remains strict."""

import re

from gtavmenu_tools.asset_formats import AssetError


def inspect(geometries: list[dict], shaders: list[dict], contract: dict) -> dict:
    if [row["index"] for row in shaders] != list(range(len(shaders))):
        raise AssetError("mesh renderer shader indices are not a complete ordered array")
    observations = []
    for g in geometries:
        index = g["shaderIndex"]
        if not 0 <= index < len(shaders):
            raise AssetError("mesh renderer shader index leaves the selected shader group")
        shader = shaders[index]
        shader_hash = shader["fileNameHash"]
        if not isinstance(shader_hash, str) or not re.fullmatch(r"0x[0-9a-f]{8}", shader_hash):
            raise AssetError("mesh renderer shader file hash is malformed")
        cable = int(shader_hash, 0) == contract["exceptionShaderFileHash"]
        # Fresh material replay proves a nonempty parameter list. For an empty
        # list, the report does not establish null versus a zero-length object.
        if cable and shader["parameterCount"] == 0:
            raise AssetError("mesh cable topology requires proven parameter-list presence")
        count = g["indexCount"]
        if not isinstance(count, int) or not 0 < count <= 0x7FFFFFFF:
            raise AssetError("mesh public renderer index count cannot be represented losslessly")
        topology = contract["exceptionTopology"] if cable else contract["defaultTopology"]
        observations.append(
            {
                "lod": g["lod"],
                "modelIndex": g["modelIndex"],
                "geometryIndex": g["geometryIndex"],
                "shaderIndex": index,
                "shaderFileNameHash": shader_hash,
                "publicReferenceTopology": topology,
                "publicReferenceDrawIndexCount": count,
                "storedTriangleCount": g["storedTriangleCount"],
                "serializedTriangleCountConsumedBySelectedPublicClass": False,
            }
        )
    return {
        "geometries": observations,
        "triangleListOccurrences": sum(g["publicReferenceTopology"] == "triangle-list" for g in observations),
        "lineListOccurrences": sum(g["publicReferenceTopology"] == "line-list" for g in observations),
        "zeroStoredTriangleCountOccurrences": sum(g["storedTriangleCount"] == 0 for g in observations),
        "scope": "Pinned public renderer source dataflow only; no rendering or retail execution",
        "assumptions": [
            "Init is followed by Load/Render without external writes to public draw-count/topology properties"
        ],
        "changesMeshAdmission": False,
    }

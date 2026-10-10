"""Strict stock-object placement snapshots from CodeWalker YMAP XML.

This is not native map conversion: streaming, flags and other native behavior
are deliberately omitted only with explicit consent. Layout field names follow
CodeWalker 485d56bec00262ed7fa472261cce7bbc6202b96e MetaTypes.cs CMapData/CEntityDef.
YmapFile.cs:1602-1613/1818-1838 invert ordinary CEntityDef quaternions; MLO differs
and is rejected. Cfx ENTITY/GetEntityRotation.md names rotation order 2 as ZXY.
The existing scene runtime uses that order for both GET and SET_ENTITY_ROTATION.
"""

from __future__ import annotations

import math
import re
import xml.etree.ElementTree as ET

from .asset_formats import AssetError, Limits
from .asset_metadata import parse_xml
from .hashes import joaat

XML_LIMITS = Limits(max_metadata_bytes=1024 * 1024, max_entries=32, max_xml_nodes=4096, max_xml_depth=16)
SNAPSHOT_POLICY = "stock-object-static-placement-snapshot-v1"
_FLOAT = re.compile(r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?\Z")
_INTEGER = re.compile(r"-?(?:0|[1-9][0-9]*)\Z")
_NAME = re.compile(r"[a-z0-9_]{1,31}\Z")
_ENTITY_FIELDS = {
    "archetypeName",
    "flags",
    "guid",
    "position",
    "rotation",
    "scaleXY",
    "scaleZ",
    "parentIndex",
    "lodDist",
    "childLodDist",
    "lodLevel",
    "numChildren",
    "priorityLevel",
    "extensions",
    "ambientOcclusionMultiplier",
    "artificialAmbientOcclusion",
    "tintValue",
}
_REQUIRED_ENTITY_FIELDS = {
    "archetypeName",
    "position",
    "rotation",
    "scaleXY",
    "scaleZ",
    "parentIndex",
    "numChildren",
    "lodLevel",
}
_EMPTY_ROOT_SECTIONS = {
    "containerLods",
    "boxOccluders",
    "occludeModels",
    "physicsDictionaries",
    "timeCycleModifiers",
    "carGenerators",
}
_EXTENTS = {"streamingExtentsMin", "streamingExtentsMax", "entitiesExtentsMin", "entitiesExtentsMax"}
_ROOT_FIELDS = (
    _EMPTY_ROOT_SECTIONS
    | _EXTENTS
    | {
        "name",
        "parent",
        "flags",
        "contentFlags",
        "entities",
        "instancedData",
        "LODLightsSOA",
        "DistantLODLightsSOA",
        "block",
    }
)
_LOD_LIGHT_ARRAYS = {
    "direction",
    "falloff",
    "falloffExponent",
    "timeAndStateFlags",
    "hash",
    "coneInnerAngle",
    "coneOuterAngleOrCapExt",
    "coronaIntensity",
}


def snapshot_import_record() -> dict:
    """Fixed policy declaration, not a claim to have rechecked original XML."""
    return {
        "inputFormat": "codewalker-ymap-xml",
        "policy": SNAPSHOT_POLICY,
        "acknowledgePlacementOnly": True,
        "sourceRotation": "ordinary-CEntityDef-inverse-quaternion",
        "targetRotation": "degrees-ZXY-order-2",
        "quaternionNormTolerance": 0.00001,
        "outputDecimalPlaces": 3,
        "nativeMapConversion": False,
        "nativeBehaviorPreserved": False,
        "hardwareOrientationQualified": False,
        "nonpreservedSemantics": [
            "map flags, parent association, content flags and streaming/entity extents",
            "entity flags, GUIDs, LOD distances and streaming priority",
            "ambient occlusion and tint overrides",
            "export block metadata",
            "native streaming/lifetime behavior; output is a frozen menu-owned static snapshot",
        ],
    }


def _fields(
    element: ET.Element, allowed: set[str], *, attributes: dict[str, str] | None = None
) -> dict[str, ET.Element]:
    if element.attrib != (attributes or {}) or (element.text or "").strip():
        raise AssetError(f"unsupported attributes or mixed content in {element.tag}")
    result = {}
    for child in element:
        if child.tag not in allowed or child.tag in result or (child.tail or "").strip():
            raise AssetError(f"unknown, duplicate or mixed-content field in {element.tag}: {child.tag}")
        result[child.tag] = child
    return result


def _empty(element: ET.Element) -> None:
    if element.attrib or len(element) or (element.text or "").strip():
        raise AssetError(f"nonempty or attributed {element.tag} is outside the placement subset")


def _text(element: ET.Element, *, maximum: int = 256) -> str:
    value = (element.text or "").strip()
    if element.attrib or len(element) or len(value) > maximum or any(not 32 <= ord(char) <= 126 for char in value):
        raise AssetError(f"invalid scalar text in {element.tag}")
    return value


def _value(element: ET.Element) -> str:
    if set(element.attrib) != {"value"} or len(element) or (element.text or "").strip():
        raise AssetError(f"{element.tag} requires only a value attribute")
    return element.attrib["value"]


def _float(value: str, label: str, *, maximum: float = 1000000.0) -> float:
    if len(value) > 64 or not _FLOAT.fullmatch(value):
        raise AssetError(f"invalid numeric {label}")
    result = float(value)
    if not math.isfinite(result) or abs(result) > maximum:
        raise AssetError(f"nonfinite or out-of-range {label}")
    return result


def _int(element: ET.Element, *, low: int = 0, high: int = 0xFFFFFFFF) -> int:
    value = _value(element)
    if len(value) > 12 or not _INTEGER.fullmatch(value) or not low <= int(value) <= high:
        raise AssetError(f"invalid bounded integer {element.tag}")
    return int(value)


def _vector(element: ET.Element, axes: str, *, maximum: float = 1000000.0) -> list[float]:
    if set(element.attrib) != set(axes) or len(element) or (element.text or "").strip():
        raise AssetError(f"{element.tag} requires exactly {axes} attributes")
    return [_float(element.attrib[axis], f"{element.tag}.{axis}", maximum=maximum) for axis in axes]


def quaternion_to_rotation(raw: list[float]) -> list[float]:
    """Invert a near-unit source quaternion and derive Rz * Rx * Ry degrees."""
    if len(raw) != 4 or any(
        type(value) not in (int, float) or abs(value) > 2 or not math.isfinite(value) for value in raw
    ):
        raise AssetError("rotation requires four finite quaternion components")
    norm = math.hypot(*raw)
    if not math.isfinite(norm) or abs(norm - 1.0) > 0.00001:
        raise AssetError("source quaternion is not near-unit (tolerance 1e-5)")
    x, y, z, w = (-raw[0] / norm, -raw[1] / norm, -raw[2] / norm, raw[3] / norm)
    r00, r01 = 1 - 2 * (y * y + z * z), 2 * (x * y - z * w)
    r10, r11 = 2 * (x * y + z * w), 1 - 2 * (x * x + z * z)
    r20, r21, r22 = 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)
    pitch = math.asin(max(-1.0, min(1.0, r21)))
    if math.hypot(r20, r22) < 1e-8:
        roll, yaw = 0.0, math.atan2(r10, r00)
    else:
        roll, yaw = math.atan2(-r20, r22), math.atan2(-r01, r11)
    return [0.0 if abs(angle) < 1e-12 else angle for angle in map(math.degrees, (pitch, roll, yaw))]


def _model(value: str, catalogue: set[str], hashes: dict[int, list[str]]) -> str:
    folded = value.lower()
    if folded in catalogue and _NAME.fullmatch(folded):
        identity = joaat(folded)
    elif re.fullmatch(r"(?:hash_|0x)[0-9a-fA-F]{1,8}", value):
        identity = int(value[5:] if value.startswith("hash_") else value[2:], 16)
    elif re.fullmatch(r"[0-9]{1,10}", value) and int(value) <= 0xFFFFFFFF:
        identity = int(value)
    else:
        raise AssetError(f"archetype is not a curated stock object: {value!r}")
    matches = hashes.get(identity, [])
    if len(matches) != 1 or not _NAME.fullmatch(matches[0]):
        raise AssetError(f"unknown or ambiguous curated object hash: 0x{identity:08x}")
    return matches[0]


def _root_metadata(fields: dict[str, ET.Element]) -> None:
    for name in _EMPTY_ROOT_SECTIONS & fields.keys():
        _empty(fields[name])
    for name in ("flags", "contentFlags"):
        if name in fields:
            _int(fields[name])
    if "parent" in fields:
        parent = _text(fields["parent"], maximum=128)
        if parent and not re.fullmatch(r"[A-Za-z0-9_]{1,128}", parent):
            raise AssetError("invalid parent map identifier")
    vectors = {name: _vector(fields[name], "xyz") for name in _EXTENTS & fields.keys()}
    for prefix in ("streaming", "entities"):
        lower, upper = prefix + "ExtentsMin", prefix + "ExtentsMax"
        if (
            lower in vectors
            and upper in vectors
            and any(a > b for a, b in zip(vectors[lower], vectors[upper], strict=True))
        ):
            raise AssetError("map extents minimum exceeds maximum")
    if "instancedData" in fields:
        children = _fields(fields["instancedData"], {"ImapLink", "PropInstanceList", "GrassInstanceList"})
        for name in ("PropInstanceList", "GrassInstanceList"):
            if name in children:
                _empty(children[name])
        if "ImapLink" in children and _text(children["ImapLink"]) not in ("", "0", "hash_00000000", "0x00000000"):
            raise AssetError("nonempty instanced map link is outside the placement subset")
    if "LODLightsSOA" in fields:
        for child in _fields(fields["LODLightsSOA"], _LOD_LIGHT_ARRAYS).values():
            _empty(child)
    if "DistantLODLightsSOA" in fields:
        children = _fields(fields["DistantLODLightsSOA"], {"position", "RGBI", "numStreetLights", "category"})
        for name, child in children.items():
            if name in ("position", "RGBI"):
                _empty(child)
            elif _int(child, high=65535) != 0:
                raise AssetError("nonzero distant light metadata is outside the placement subset")
    if "block" in fields:
        for name, child in _fields(
            fields["block"], {"version", "flags", "name", "exportedBy", "owner", "time"}
        ).items():
            _int(child) if name in ("version", "flags") else _text(child)


def read_ymap_placements(data: bytes, object_names: set[str], *, acknowledge_placement_only: bool = False) -> dict:
    """Return a normal schema-1 scene source; reject every unsupported feature."""
    if acknowledge_placement_only is not True:
        raise AssetError("YMAP import requires explicit acknowledgment of placement-only snapshot semantics")
    if type(data) is not bytes or not 0 < len(data) <= XML_LIMITS.max_metadata_bytes:
        raise AssetError("YMAP XML is empty or exceeds 1 MiB")
    try:
        text = data.decode("utf-8-sig")
        if re.search(r"\bxmlns(?:\s|:|=)", text):
            raise AssetError("XML namespaces are outside the placement subset")
        without_declaration = re.sub(r"^\s*<\?xml\s[^?]*\?>", "", text, count=1)
        if "<?" in without_declaration:
            raise AssetError("XML processing instructions are outside the placement subset")
        root = parse_xml(data, XML_LIMITS)
    except (UnicodeError, ET.ParseError) as exc:
        raise AssetError(f"invalid YMAP XML: {exc}") from exc
    if root.tag != "CMapData":
        raise AssetError("YMAP placement XML root must be CMapData")
    fields = _fields(root, _ROOT_FIELDS)
    _root_metadata(fields)
    name = _text(fields["name"], maximum=64) if "name" in fields else "YMAP placement snapshot"
    name = name or "YMAP placement snapshot"
    entities = fields.get("entities")
    if entities is None or entities.attrib or (entities.text or "").strip() or not 1 <= len(entities) <= 32:
        raise AssetError("YMAP snapshot requires one to 32 stock object entities")
    hashes: dict[int, list[str]] = {}
    for model in sorted(object_names):
        hashes.setdefault(joaat(model), []).append(model)
    output = []
    for item in entities:
        if item.tag != "Item" or (item.tail or "").strip():
            raise AssetError("entities must contain only CEntityDef Item records")
        row = _fields(item, _ENTITY_FIELDS, attributes={"type": "CEntityDef"})
        if missing := _REQUIRED_ENTITY_FIELDS - row.keys():
            raise AssetError(f"every entity needs explicit placement fields: {sorted(missing)}")
        model = _model(_text(row["archetypeName"]), object_names, hashes)
        for field in ("flags", "guid", "tintValue"):
            if field in row:
                _int(row[field])
        for field in ("ambientOcclusionMultiplier", "artificialAmbientOcclusion"):
            if field in row:
                _int(row[field], low=-(1 << 31), high=(1 << 31) - 1)
        for field in ("lodDist", "childLodDist"):
            if field in row:
                _float(_value(row[field]), field)
        for field in ("scaleXY", "scaleZ"):
            if field in row and _float(_value(row[field]), field) != 1.0:
                raise AssetError("placement snapshots support only unit scale")
        if "parentIndex" in row and _int(row["parentIndex"], low=-1, high=(1 << 31) - 1) != -1:
            raise AssetError("parented entities are outside the placement subset")
        if "numChildren" in row and _int(row["numChildren"]) != 0:
            raise AssetError("entities with children are outside the placement subset")
        if "lodLevel" in row and _text(row["lodLevel"]) not in (
            "LODTYPES_DEPTH_HD",
            "LODTYPES_DEPTH_ORPHANHD",
            "0",
            "5",
        ):
            raise AssetError("only HD or ORPHANHD entities are supported")
        if "priorityLevel" in row and _text(row["priorityLevel"]) not in (
            "PRI_REQUIRED",
            "PRI_OPTIONAL_HIGH",
            "PRI_OPTIONAL_MEDIUM",
            "PRI_OPTIONAL_LOW",
            "0",
            "1",
            "2",
            "3",
        ):
            raise AssetError("unknown entity priority enum")
        if "extensions" in row:
            _empty(row["extensions"])
        position = _vector(row["position"], "xyz", maximum=100000.0)
        rotation = quaternion_to_rotation(_vector(row["rotation"], "xyzw"))
        output.append(
            {
                "kind": "object",
                "model": model,
                "position": position,
                "rotation": rotation,
                "frozen": True,
                "placeOnGround": False,
            }
        )
    return {
        "schemaVersion": 1,
        "name": name,
        "description": "Stock-object placement snapshot imported from YMAP XML.",
        "placement": "absolute",
        "entries": output,
    }

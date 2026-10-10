"""Inspect Legacy texture usage words without admitting or mapping them.

The labels reproduce CodeWalker's TextureUsage and TextureUsageFlags vocabulary
at commit 485d56bec00262ed7fa472261cce7bbc6202b96e (Texture.cs). Labels, including
the X/Y names, are not qualified field semantics or a PS5 transfer policy.

The rounded-payload relationship is an observation from supplied mod assets.
It is not a general decoding rule: its bits can overlap named source flags.
"""

from __future__ import annotations

from .asset_formats import AssetError

MAX_LINEAR_MIP_BYTES = 256 * 1024 * 1024
_FOOTPRINT_ALIGNMENT = 4096
_EMBEDDED_SCRIPT_RT_MASK = 0x00800000
_USAGE_LABELS = (
    "UNKNOWN",
    "DEFAULT",
    "TERRAIN",
    "CLOUDDENSITY",
    "CLOUDNORMAL",
    "CABLE",
    "FENCE",
    "ENVEFF",
    "SCRIPT",
    "WATERFLOW",
    "WATERFOAM",
    "WATERFOG",
    "WATEROCEAN",
    "WATER",
    "FOAMOPACITY",
    "FOAM",
    "DIFFUSEMIPSHARPEN",
    "DIFFUSEDETAIL",
    "DIFFUSEDARK",
    "DIFFUSEALPHAOPAQUE",
    "DIFFUSE",
    "DETAIL",
    "NORMAL",
    "SPECULAR",
    "EMISSIVE",
    "TINTPALETTE",
    "SKIPPROCESSING",
    "DONOTOPTIMIZE",
    "TEST",
    "COUNT",
)
_FLAG_LABELS = (
    "NOT_HALF",
    "HD_SPLIT",
    "X2",
    "X4",
    "Y4",
    "X8",
    "X16",
    "X32",
    "X64",
    "Y64",
    "X128",
    "X256",
    "X512",
    "Y512",
    "X1024",
    "Y1024",
    "X2048",
    "Y2048",
    "EMBEDDEDSCRIPTRT",
    "UNK19",
    "UNK20",
    "UNK21",
    "FLAG_FULL",
    "MAPS_HALF",
    "UNK24",
)


def describe_legacy_usage(usage_data: int, linear_mip_bytes: int) -> dict:
    """Return bounded source diagnostics; bit indices refer to UsageData >> 5.

    Neither a matching footprint pattern nor a familiar label grants conversion
    admission. The original word and all unnamed bits remain visible.
    """

    if type(usage_data) is not int or not 0 <= usage_data <= 0xFFFFFFFF:
        raise AssetError("Legacy UsageData must be an unsigned 32-bit integer")
    if type(linear_mip_bytes) is not int or not 1 <= linear_mip_bytes <= MAX_LINEAR_MIP_BYTES:
        raise AssetError("linear mip byte count must be a positive integer of at most 256 MiB")
    usage = usage_data & 31
    flags = usage_data >> 5
    bits = [bit for bit in range(27) if flags & (1 << bit)]
    rounded = (linear_mip_bytes + _FOOTPRINT_ALIGNMENT - 1) & -_FOOTPRINT_ALIGNMENT
    footprint_word = 0x20000001 | rounded
    return {
        "rawHex": f"0x{usage_data:08x}",
        "usageLow5": usage,
        "usageSourceLabel": _USAGE_LABELS[usage] if usage < len(_USAGE_LABELS) else None,
        "flagsShiftedHex": f"0x{flags:08x}",
        "setFlagBits": bits,
        "sourceFlagLabels": [_FLAG_LABELS[bit] for bit in bits if bit < len(_FLAG_LABELS)],
        "unnamedFlagBits": [bit for bit in bits if bit >= len(_FLAG_LABELS)],
        "embeddedScriptRtFlagSet": bool(usage_data & _EMBEDDED_SCRIPT_RT_MASK),
        "linearMipBytes": linear_mip_bytes,
        "roundedFootprintBytes": rounded,
        "footprintWordHex": f"0x{footprint_word:08x}",
        "matchesObservedFootprintPattern": usage_data == footprint_word,
        "footprintOverlapsSpecialFlag": bool(rounded & _EMBEDDED_SCRIPT_RT_MASK),
        "sourceLabelsAreNotQualifiedSemantics": True,
        "targetMetadataMappingQualified": False,
    }

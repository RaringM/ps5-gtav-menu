"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import struct

from gtavmenu_tools.asset_formats import AssetError


def model(gate, request, record):
    if type(request) is not int or not 0 <= request <= 0xFFFFFFFF:
        raise AssetError("texture version comparison request must be uint32")
    if type(record) is not bytes or len(record) != gate["recordBytes"]:
        raise AssetError("texture version comparison requires the exact declared entry span")
    value = 0
    if int.from_bytes(record[:8], "little") >> 63:
        value = (record[gate["systemByte"]] & gate["systemMask"]) | (
            struct.unpack_from("<I", record, gate["graphicsWord"])[0] >> gate["graphicsShift"]
        )
    return request == 0 or value == (request & 255)

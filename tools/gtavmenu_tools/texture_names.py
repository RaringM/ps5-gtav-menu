"""Explicit, non-normalizing names for experimental static texture output.

The source-preserving policy only widens ASCII letters and a leading digit.
It does not qualify engine name handling or change case-insensitive hashes.
Package paths and dictionary logical names are separate namespaces.
"""

from __future__ import annotations

import re

from .asset_formats import AssetError

LEGACY_NAME_POLICY = "lowercase-identifiers-v1"
SOURCE_NAME_POLICY = "preserve-source-ascii-v1"
_POLICIES = {
    LEGACY_NAME_POLICY: (
        r"[a-z][a-z0-9_]{0,126}",
        "PS5 texture name must be an ordinary lowercase identifier of at most 127 bytes",
    ),
    SOURCE_NAME_POLICY: (
        r"[A-Za-z0-9][A-Za-z0-9_]{0,126}",
        "PS5 texture name must be an ordinary ASCII identifier starting with a letter or digit, at most 127 bytes",
    ),
}


def validate_name_policy(policy: object) -> None:
    """Reject implicit, malformed or unknown policy choices."""
    if type(policy) is not str or policy not in _POLICIES:
        raise AssetError("texture name policy must be an explicit supported policy")


def texture_name_issue(name: object, policy: object = LEGACY_NAME_POLICY) -> str | None:
    """Validate without changing the original spelling or hashing the name."""
    validate_name_policy(policy)
    pattern, message = _POLICIES[policy]
    if type(name) is not str or not re.fullmatch(pattern, name) or "script_rt_" in name.lower():
        return message
    return None

"""Single source of truth for the game-build "pin" the loader/tools target.

Every address, offset, and version string that is specific to one GTA V build
(initially PPSA04264 01.005.000) lives in a target manifest under
``data/targets/`` (e.g. ``data/targets/ppsa04264-01.005.000.json``). Historically
these were copied by hand into make/targets.mk, menu-ctl.sh, and ~7 Python tools;
after a game update you had to edit them all in lockstep or the tools would
disagree (some refuse, the loader injects stale addresses -> crash). This module
reads the manifests once so the Python side has ONE place to change, and
``tests/test_target_pin_single_source.py`` guards that the C/make/shell mirrors
still agree with it.

Multiple supported builds are expressed as multiple manifests. A manifest must be
"pinned" (all per-build loader addresses derived from that build's decrypted
eboot) before its addresses may be consumed; a manifest with
``loader.status == "pending"`` registers the target as version evidence only and
must not feed the loader's build-pin table or any address consumers. The loader
auto-detects the running build at inject time from the pinned builds it was built
with, and cleanly reports unsupported builds (see src/payload_loader/main.c and
devtools/generators/emit_loader_pins.py).

To re-pin for a new game build: add the manifest, derive the machine addresses
from the new eboot, fill loader/liveMapping/nativeBridge, re-run the emitter +
test suite -- the drift guard names every stale mirror.
"""

from __future__ import annotations

import os
from pathlib import Path

from .io import read_json
from .parsing import parse_int

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGETS_DIR = REPO_ROOT / "data/targets"
# The default target every tool/mirror binds to unless GTAV_TARGET or TARGET_ID is set. Moved to
# 01.010.002 on 2026-09-19 together with make/config.mk's GTAV_TARGET: the two defaults must agree, or
# `make` builds one build's worker while the host tools validate against the other's version signature.
DEFAULT_MANIFEST_STEM = "ppsa04264-01.010.002"
MANIFEST_PATH = TARGETS_DIR / f"{DEFAULT_MANIFEST_STEM}.json"


def decrypted_eboot(stem: str | None = None) -> Path | None:
    """A locally dumped, decrypted eboot for `stem`, or None if none is present.

    These images are large and gitignored, so every caller must tolerate their absence (CI has none).
    Naming is historical: the first build's image is ``eboot.ida.elf``, later ones are
    ``eboot-<version>.elf``.
    """
    stem = stem or DEFAULT_MANIFEST_STEM
    version = stem.split("-", 1)[1] if "-" in stem else stem
    analysis = REPO_ROOT / "build/analysis"
    candidates = [analysis / f"eboot-{version}.elf", analysis / f"eboot-{stem}.elf"]
    if version == "01.005.000":
        candidates += [analysis / "eboot.ida.elf", REPO_ROOT / "ref/eboot.elf", REPO_ROOT / "eboot.elf"]
    return next((c for c in candidates if c.is_file()), None)


def native_addresses_header(stem: str | None = None) -> Path:
    """The generated native-address header a build of `stem` compiles against.

    Mirrors make/config.mk: a per-target header when one exists
    (``native_addresses_<stem>_generated.h``), else the original unsuffixed file. Mirror guards need
    this, because reading the unsuffixed header unconditionally compares one build's addresses against
    another build's manifest as soon as the default target moves.
    """
    stem = stem or DEFAULT_MANIFEST_STEM
    per_target = REPO_ROOT / "include/gtavmenu" / f"native_addresses_{stem}_generated.h"
    return per_target if per_target.is_file() else REPO_ROOT / "include/gtavmenu/native_addresses_generated.h"


def manifest_path(stem: str | None = None) -> Path:
    """Path of one target manifest by stem (``ppsa04264-01.005.000``)."""
    if stem is None:
        return MANIFEST_PATH
    p = TARGETS_DIR / f"{stem}.json"
    if not p.is_file():
        raise FileNotFoundError(f"no target manifest {p}")
    return p


def all_manifest_paths() -> list[Path]:
    """Every supported-build manifest (excludes the dev-unknown template)."""
    return sorted(TARGETS_DIR.glob("ppsa04264-*.json"))


def all_manifests() -> list[dict]:
    return [read_json(p) for p in all_manifest_paths()]


def selected_stem() -> str:
    """Manifest stem selected by GTAV_TARGET/TARGET_ID (env), else the default."""
    override = os.environ.get("GTAV_TARGET") or os.environ.get("TARGET_ID")
    if not override:
        return DEFAULT_MANIFEST_STEM
    stem = override
    if stem.endswith(".json"):
        stem = stem[: -len(".json")]
    for p in all_manifest_paths():
        if p.stem == stem or read_json(p).get("targetId") == override:
            return p.stem
    raise ValueError(f"GTAV_TARGET='{override}' does not name a data/targets manifest")


def is_pinned(manifest: dict) -> bool:
    """True when the manifest has a derived, consumable loader pin set.

    A pending manifest registers version evidence only (so the project can track a
    freshly dumped build) and must never feed address consumers or the loader
    build-pin table until the per-build RE work fills the pins back in.
    """
    loader = manifest.get("loader") or {}
    if loader.get("status") == "pending":
        return False
    required = (
        "playerPedAnchor",
        "playerPedOffset",
        "versionSignatureAddr",
        "versionSignatureExpectedCompact",
        "frameHookTarget",
        "brokerContinuation",
        "brokerExpectedBytesCompact",
    )
    return all(loader.get(k) not in (None, "") for k in required) and bool(manifest.get("liveMapping"))


def pin_manifest(stem: str | None = None) -> dict:
    """The manifest selected (or named) -- must be pinned, else raise."""
    m = _manifest(stem)
    if not is_pinned(m):
        raise RuntimeError(
            f"target manifest {m.get('targetId')} is not pinned yet "
            "(loader.status=pending); derive its per-build addresses from the "
            "decrypted eboot before consuming pins"
        )
    return m


def _manifest(stem: str | None = None) -> dict:
    return read_json(manifest_path(stem if stem is not None else selected_stem()))


_TARGET_STEM = selected_stem()
if _TARGET_STEM != DEFAULT_MANIFEST_STEM:
    # Environment-selected non-default target: it must be a pinned build for the
    # module-level pin constants below to be meaningful.
    pin_manifest(_TARGET_STEM)

_M = pin_manifest(_TARGET_STEM)
_LOADER = _M["loader"]
_TEXT = _M["liveMapping"]["text"]

# Identity of the pinned build (foreground title id + content version).
TITLE_ID: str = _M["titleId"]
APP_VERSION: str = _M["contentVersion"]

# Player-ped readiness anchor: PLAYER_PED_ID resolves ped = *(*(ANCHOR)+OFFSET).
# ped != 0 is the loader's player-world/load readiness predicate.
PLAYER_PED_ANCHOR: int = parse_int(_LOADER["playerPedAnchor"])
PED_OFFSET: int = parse_int(_LOADER["playerPedOffset"])

# Pre-inject version signature (GET_FRAME_COUNT handler) + frame-hook target
# (PLAYER_PED_ID handler). Both also live in native_addresses_generated.h.
VERSION_SIGNATURE_ADDR: int = parse_int(_LOADER["versionSignatureAddr"])
# The rest of the loader's single-pin contract, so mirror guards read one source instead of
# re-parsing the manifest (or hardcoding one build's literals, which silently rot when the default
# target moves).
FRAME_HOOK_CONTINUATION: int = parse_int(_LOADER["brokerContinuation"])
FRAME_HOOK_PATCH_LEN: int = int(_LOADER["brokerPatchLen"])
FRAME_HOOK_STOLEN_LEN: int = int(_LOADER["brokerStolenLen"])
FRAME_HOOK_STOLEN_BYTES: bytes = bytes.fromhex(_LOADER["brokerExpectedBytesCompact"])
VERSION_SIGNATURE_EXPECTED: bytes = bytes.fromhex(_LOADER["versionSignatureExpectedCompact"])
if len(VERSION_SIGNATURE_EXPECTED) != 16:
    raise RuntimeError("loader versionSignatureExpectedCompact must encode exactly 16 bytes")
FRAME_HOOK_TARGET: int = parse_int(_LOADER["frameHookTarget"])

# The executable (.text) live-address range. The loader refuses any privileged
# protect/write outside this window, so a stale pin fails closed instead of
# corrupting an unrelated mapping.
TEXT_LIVE_START: int = parse_int(_TEXT["liveStart"])
TEXT_LIVE_END: int = parse_int(_TEXT["liveEnd"])


def foreground_matches(title_id: str | None, app_version: str | None) -> bool:
    """True if a live foreground query matches the pinned build."""
    return title_id == TITLE_ID and app_version == APP_VERSION


def pinned_build_pins() -> list[dict]:
    """Loader build-pin rows for every *pinned* target manifest, in path order.

    The loader table (devtools/generators/emit_loader_pins.py) is generated from these rows;
    pending manifests are version evidence only and never contribute a row.
    """
    pins = []
    for p in all_manifest_paths():
        m = read_json(p)
        if is_pinned(m):
            pins.append(build_pin(p.stem))
    return pins


def build_pin(stem: str | None = None) -> dict:
    """The loader build-pin row for a pinned manifest (used by the pins emitter).

    Returns the fields the loader table needs for one supported build:
    targetId, versionSignatureAddr/ExpectedCompact, frameHookTarget, and the
    executable liveMapping text range, plus player-ped readiness values.
    """
    m = pin_manifest(stem)
    loader = m["loader"]
    text = m["liveMapping"]["text"]
    return {
        "targetId": m["targetId"],
        "titleId": m["titleId"],
        "contentVersion": m["contentVersion"],
        "playerPedAnchor": loader["playerPedAnchor"],
        "playerPedOffset": loader["playerPedOffset"],
        "playerPedReadyMode": loader["playerPedReadyMode"],
        "frameHookTarget": loader["frameHookTarget"],
        "brokerContinuation": loader["brokerContinuation"],
        "brokerPatchLen": loader["brokerPatchLen"],
        "brokerStolenLen": loader["brokerStolenLen"],
        "brokerExpectedBytesCompact": loader["brokerExpectedBytesCompact"],
        "versionSignatureAddr": loader["versionSignatureAddr"],
        "versionSignatureExpectedCompact": loader["versionSignatureExpectedCompact"],
        "textLiveStart": text["liveStart"],
        "textLiveEnd": text["liveEnd"],
    }

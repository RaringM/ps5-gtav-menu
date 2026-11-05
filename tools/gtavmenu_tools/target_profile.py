"""Validated, reproducible build settings for one exact GTA V target."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGET_RE = re.compile(r"[a-z0-9]+-\d+\.\d+\.\d+\Z")
# PS4 Legacy titles use two-part content versions (CUSA00411 01.57).
PS4_TARGET_RE = re.compile(r"cusa\d{5}-\d{2}\.\d{2}\Z")
PLATFORMS = ("ps5", "ps4")
# PS4 build.features keys. Each one is a build gate: the capability policy's "requires" lists name
# them, tools/gtavmenu_tools/ps4_build.py maps them to -D flags, and the generated capability header
# repeats them as GTAV_CAPABILITY_GATE_* so the worker can cross-check its own flags.
PS4_FEATURE_KEYS = (
    "nativeFeatures",
    "mainThreadActions",
    "coldModelStreaming",
    "toasts",
    "workerTextOverlay",
    "hud",
    "effects",
    "vehicleClasses",
    "localization",
    "storage",
    "scriptGlobals",
    "vehiclePreview",
    "buttonGlyphs",
    "instructionalScaleform",
    "touchpad",
    "customPacks",
    "bootPools",
)
SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")


class TargetProfileError(ValueError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def integer(value: object, name: str) -> int:
    if not isinstance(value, (str, int)):
        raise TargetProfileError(f"{name} must be an integer")
    try:
        return int(value, 0) if isinstance(value, str) else value
    except ValueError as exc:
        raise TargetProfileError(f"{name} must be an integer: {value!r}") from exc


def boolean(value: object, name: str) -> int:
    if value is True or value == 1:
        return 1
    if value is False or value == 0:
        return 0
    raise TargetProfileError(f"{name} must be a boolean")


def manifest_platform(manifest: dict[str, object]) -> str:
    """The manifest's platform; PS5 manifests predate the field and omit it."""
    platform = manifest.get("platform", "ps5")
    if platform not in PLATFORMS:
        raise TargetProfileError(f"target manifest platform must be one of {PLATFORMS}: {platform!r}")
    return str(platform)


def target_platform(target: str, root: Path = REPO_ROOT) -> str:
    """The platform of target id `target`: its manifest's (data/targets/, PS4 Legacy targets in
    data/targets/ps4/) or, in a tree without that manifest, the one its id's form names."""
    for path in (root / "data/targets" / f"{target}.json", root / "data/targets/ps4" / f"{target}.json"):
        if path.is_file():
            try:
                manifest = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as exc:
                raise TargetProfileError(f"cannot read target manifest {path}: {exc}") from exc
            if not isinstance(manifest, dict):
                raise TargetProfileError(f"target manifest {path} must contain a JSON object")
            return manifest_platform(manifest)
    if PS4_TARGET_RE.fullmatch(target):
        return "ps4"
    if TARGET_RE.fullmatch(target):
        return "ps5"
    raise TargetProfileError(f"unknown target {target!r}")


def target_stem(manifest: dict[str, object]) -> str:
    title = manifest.get("titleId")
    version = manifest.get("contentVersion")
    if not isinstance(title, str) or not isinstance(version, str):
        raise TargetProfileError("target manifest is missing titleId/contentVersion")
    stem = f"{title.lower()}-{version}"
    pattern = PS4_TARGET_RE if manifest_platform(manifest) == "ps4" else TARGET_RE
    if not pattern.fullmatch(stem):
        raise TargetProfileError(f"invalid target identity: {stem}")
    return stem


def _object(parent: dict[str, object], key: str, name: str) -> dict[str, object]:
    value = parent.get(key)
    if not isinstance(value, dict):
        raise TargetProfileError(f"{name} must be an object")
    return value


def _input(build: dict[str, object], key: str, *, verify_inputs: bool) -> dict[str, str]:
    item = _object(build, key, f"build.{key}")
    source = item.get("json")
    header = item.get("header")
    expected = item.get("sha256")
    if not all(isinstance(value, str) and value for value in (source, header, expected)):
        raise TargetProfileError(f"build.{key} requires json, header, and sha256 strings")
    source_path = REPO_ROOT / source
    header_path = REPO_ROOT / "include" / header
    if not source_path.is_file():
        raise TargetProfileError(f"build.{key}.json does not exist: {source}")
    if not header_path.is_file():
        raise TargetProfileError(f"build.{key}.header does not exist: include/{header}")
    if verify_inputs:
        actual = sha256_file(source_path)
        if actual != expected:
            raise TargetProfileError(f"build.{key}.sha256 is stale for {source}: expected {expected}, got {actual}")
    return {"json": source, "header": header, "sha256": expected}


def validate_manifest(
    manifest: dict[str, object], *, manifest_path: Path | None = None, verify_inputs: bool = True
) -> dict[str, object]:
    if manifest.get("schemaVersion") != 2:
        raise TargetProfileError("target manifest schemaVersion must be 2")
    if manifest_platform(manifest) == "ps4":
        return _validate_ps4_manifest(manifest, manifest_path=manifest_path, verify_inputs=verify_inputs)
    stem = target_stem(manifest)
    if manifest_path is not None and manifest_path.stem != stem:
        raise TargetProfileError(f"target manifest path names {manifest_path.stem}, but content identifies {stem}")

    target_id = manifest.get("targetId")
    if not isinstance(target_id, str) or not target_id:
        raise TargetProfileError("target manifest is missing targetId")
    title_id = manifest.get("titleId")
    content_id = manifest.get("contentId")
    if not isinstance(title_id, str) or not title_id:
        raise TargetProfileError("target manifest is missing titleId")
    if not isinstance(content_id, str) or not content_id:
        raise TargetProfileError("target manifest is missing contentId")
    loader = _object(manifest, "loader", "loader")
    live = _object(manifest, "liveMapping", "liveMapping")
    text = _object(live, "text", "liveMapping.text")
    for key in (
        "playerPedAnchor",
        "playerPedOffset",
        "versionSignatureAddr",
        "frameHookTarget",
        "brokerContinuation",
        "brokerPatchLen",
        "brokerStolenLen",
        "tlsGameCtxOffset",
        "tlsCtxNativeThreadOffset",
    ):
        if integer(loader.get(key), f"loader.{key}") <= 0:
            raise TargetProfileError(f"loader.{key} must be positive")
    for key in ("liveStart", "liveEnd"):
        if integer(text.get(key), f"liveMapping.text.{key}") <= 0:
            raise TargetProfileError(f"liveMapping.text.{key} must be positive")
    for key in ("versionSignatureExpectedCompact", "brokerExpectedBytesCompact"):
        raw = loader.get(key)
        if not isinstance(raw, str) or len(bytes.fromhex(raw)) != 16:
            raise TargetProfileError(f"loader.{key} must encode exactly 16 bytes")

    build = _object(manifest, "build", "build")
    channel = build.get("channel")
    if channel not in {"primary", "development"}:
        raise TargetProfileError("build.channel must be primary or development")
    toolchain = _object(build, "toolchain", "build.toolchain")
    if toolchain.get("payloadSdk") != "0.44" or integer(toolchain.get("llvmMajor"), "build.toolchain.llvmMajor") != 20:
        raise TargetProfileError("build.toolchain must pin payloadSdk 0.44 and LLVM 20")

    native_input = _input(build, "nativeAddresses", verify_inputs=verify_inputs)
    globals_input = _input(build, "scriptGlobals", verify_inputs=verify_inputs)
    custom_lane_input: dict[str, str] | None = None
    if "customLane" in build:
        custom_lane_input = _input(build, "customLane", verify_inputs=verify_inputs)
    boot_pools_input: dict[str, str] | None = None
    if "bootPools" in build:
        boot_pools_input = _input(build, "bootPools", verify_inputs=verify_inputs)
    features = _object(build, "features", "build.features")
    injection = _object(build, "injection", "build.injection")
    feature_keys = (
        "nativeFeatures",
        "workerTextOverlay",
        "toasts",
        "vehiclePreview",
        "instructionalScaleform",
        "buttonGlyphs",
        "scriptGlobals",
        "phaseIntercept",
        "phaseDrawList",
        "customPacks",
    )
    feature_values = {key: boolean(features.get(key), f"build.features.{key}") for key in feature_keys}
    # Boot-time pool growth turns on per target only after its hardware gate, so it may be absent.
    feature_values["bootPools"] = boolean(features.get("bootPools", False), "build.features.bootPools")
    injection_keys = (
        "probeNoStop",
        "noStopIo",
        "noStopStrict",
        "verifyWrites",
        "caveBootstrap",
        "caveInject",
        "classicInject",
        "selfStartWorker",
        "installRenderPhase",
    )
    injection_values = {key: boolean(injection.get(key), f"build.injection.{key}") for key in injection_keys}
    lane = injection.get("lane")
    if lane not in {"ptrace-free-cave", "classic"}:
        raise TargetProfileError("build.injection.lane must be ptrace-free-cave or classic")
    if injection_values["caveInject"] + injection_values["classicInject"] != 1:
        raise TargetProfileError("exactly one build injection mode must be enabled")
    if lane == "ptrace-free-cave":
        if not injection_values["caveInject"] or injection_values["classicInject"]:
            raise TargetProfileError("ptrace-free-cave lane requires caveInject only")
        if not injection_values["caveBootstrap"] or not injection_values["selfStartWorker"]:
            raise TargetProfileError("ptrace-free-cave requires caveBootstrap and selfStartWorker")
        for key in ("probeNoStop", "noStopIo", "noStopStrict", "verifyWrites"):
            if not injection_values[key]:
                raise TargetProfileError(f"ptrace-free-cave requires build.injection.{key}")
        for key in ("caveAddress", "caveAllocationBytes"):
            if integer(loader.get(key), f"loader.{key}") <= 0:
                raise TargetProfileError(f"loader.{key} must be positive for ptrace-free-cave")
        cave = integer(loader["caveAddress"], "loader.caveAddress")
        allocation = integer(loader["caveAllocationBytes"], "loader.caveAllocationBytes")
        text_start = integer(text["liveStart"], "liveMapping.text.liveStart")
        text_end = integer(text["liveEnd"], "liveMapping.text.liveEnd")
        if cave < text_start or cave + 256 > text_end:
            raise TargetProfileError("loader cave must hold the complete bootstrap inside live text")
        if allocation & 0xFFF:
            raise TargetProfileError("loader.caveAllocationBytes must be page-aligned")
    else:
        if not injection_values["classicInject"] or injection_values["caveInject"]:
            raise TargetProfileError("classic lane requires classicInject only")
        if injection_values["caveBootstrap"] or injection_values["selfStartWorker"]:
            raise TargetProfileError("classic injection cannot enable caveBootstrap or selfStartWorker")

    if feature_values["phaseIntercept"] != injection_values["installRenderPhase"]:
        raise TargetProfileError("phaseIntercept and installRenderPhase must agree")
    if feature_values["phaseDrawList"] != feature_values["phaseIntercept"]:
        raise TargetProfileError("phaseDrawList and phaseIntercept must agree")
    if feature_values["phaseIntercept"]:
        phase = _object(manifest, "renderPhase", "renderPhase")
        for key in ("root", "leafVtable", "groupVtable", "taskId", "original"):
            if integer(phase.get(key), f"renderPhase.{key}") <= 0:
                raise TargetProfileError(f"renderPhase.{key} must be positive")
        cycle = _object(phase, "cycle", "renderPhase.cycle")
        for key in ("epoch", "reset", "producer", "consumer", "count0", "count1", "text", "counter"):
            address = integer(cycle.get(key), f"renderPhase.cycle.{key}")
            if address <= 0 or address & 3:
                raise TargetProfileError(f"renderPhase.cycle.{key} must be a positive aligned address")
        if (
            integer(cycle["consumer"], "renderPhase.cycle.consumer")
            != integer(cycle["producer"], "renderPhase.cycle.producer") + 4
        ):
            raise TargetProfileError("renderPhase.cycle selectors must be adjacent producer/consumer words")
        fingerprints = _object(phase, "fingerprints", "renderPhase.fingerprints")
        for key in ("leafInvoke", "groupInvoke", "group2Dispatch", "registration"):
            item = _object(fingerprints, key, f"renderPhase.fingerprints.{key}")
            if integer(item.get("address"), f"renderPhase.fingerprints.{key}.address") <= 0:
                raise TargetProfileError(f"renderPhase.fingerprints.{key}.address must be positive")
            raw = item.get("bytes")
            try:
                decoded = bytes.fromhex(raw) if isinstance(raw, str) else b""
            except ValueError as exc:
                raise TargetProfileError(f"renderPhase.fingerprints.{key}.bytes must be compact hex") from exc
            if not decoded or len(decoded) > 48:
                raise TargetProfileError(f"renderPhase.fingerprints.{key}.bytes must encode 1..48 bytes")
    if (feature_values["vehiclePreview"] or feature_values["instructionalScaleform"]) and not feature_values[
        "phaseDrawList"
    ]:
        raise TargetProfileError("preview and instructional Scaleform require phaseDrawList")
    # The runtime pack lane (CUSTOM_PACKS) streams through the preview natives and draws its card
    # through the phase draw list; make/flags.mk also pins it to the 01.010.002 executables.
    if feature_values["customPacks"] and not (feature_values["vehiclePreview"] and feature_values["phaseDrawList"]):
        raise TargetProfileError("customPacks requires vehiclePreview and phaseDrawList")
    if feature_values["customPacks"] and custom_lane_input is None:
        raise TargetProfileError("customPacks requires build.customLane")
    if feature_values["bootPools"]:
        if boot_pools_input is None:
            raise TargetProfileError("bootPools requires build.bootPools")
        if lane != "ptrace-free-cave" or not feature_values["customPacks"]:
            raise TargetProfileError("bootPools requires the ptrace-free-cave lane and customPacks")

    return {
        "platform": "ps5",
        "stem": stem,
        "targetId": target_id,
        "titleId": title_id,
        "contentId": content_id,
        "contentVersion": manifest["contentVersion"],
        "channel": channel,
        "nativeAddresses": native_input,
        "scriptGlobals": globals_input,
        "customLane": custom_lane_input,
        "bootPools": boot_pools_input,
        "features": feature_values,
        "injection": {**injection_values, "lane": lane},
    }


def _hex_bytes(value: object, name: str, length: int | None = None) -> bytes:
    try:
        decoded = bytes.fromhex(value) if isinstance(value, str) else b""
    except ValueError as exc:
        raise TargetProfileError(f"{name} must be compact hex") from exc
    if not decoded or (length is not None and len(decoded) != length):
        raise TargetProfileError(f"{name} must encode {length or 'some'} bytes")
    return decoded


def _string(parent: dict[str, object], key: str, name: str) -> str:
    value = parent.get(key)
    if not isinstance(value, str) or not value:
        raise TargetProfileError(f"{name} must be a non-empty string")
    return value


def _ps4_range(item: object, name: str) -> tuple[int, int, int]:
    if not isinstance(item, dict):
        raise TargetProfileError(f"{name} must be an object")
    start = integer(item.get("start"), f"{name}.start")
    end = integer(item.get("end"), f"{name}.end")
    prot = integer(item.get("prot"), f"{name}.prot")
    if start <= 0 or end <= start or start & 0x3FFF or end & 0x3FFF:
        raise TargetProfileError(f"{name} must be a non-empty 16 KiB-aligned range")
    return start, end, prot


def _validate_ps4_manifest(
    manifest: dict[str, object], *, manifest_path: Path | None, verify_inputs: bool
) -> dict[str, object]:
    """Validate a PS4 Legacy target (injected host-side through ps4debug, one-shot game-thread hook)."""
    stem = target_stem(manifest)
    if manifest_path is not None and manifest_path.stem != stem:
        raise TargetProfileError(f"target manifest path names {manifest_path.stem}, but content identifies {stem}")
    target_id = _string(manifest, "targetId", "targetId")
    title_id = _string(manifest, "titleId", "titleId")
    content_id = _string(manifest, "contentId", "contentId")

    identity = _object(manifest, "identity", "identity")
    for key in ("processName", "processPath", "sandboxPath", "protocolVersion", "branding", "capability"):
        _string(identity, key, f"identity.{key}")
    for key in ("normalizedElfSha256", "ftpExecutableSha256"):
        if not SHA256_RE.fullmatch(str(identity.get(key, ""))):
            raise TargetProfileError(f"identity.{key} must be a lowercase SHA-256")
    variants = identity.get("ftpExecutableSha256Variants", [])
    if not isinstance(variants, list) or any(not SHA256_RE.fullmatch(str(value)) for value in variants):
        raise TargetProfileError("identity.ftpExecutableSha256Variants must contain lowercase SHA-256 values")
    if integer(identity.get("firmwareBcd"), "identity.firmwareBcd") <= 0:
        raise TargetProfileError("identity.firmwareBcd must be positive")
    if integer(identity.get("platformId"), "identity.platformId") != 4:
        raise TargetProfileError("identity.platformId must be 4 for a PS4 target")

    loader = _object(manifest, "loader", "loader")
    for key in (
        "runtimeImageBias",
        "versionAddress",
        "hookAddress",
        "continuation",
        "playerGlobalAnchor",
        "playerPedOffset",
        "gameTimer",
        "frameCount",
        "textTransactionFlags",
        "gatewayHint",
        "gatewaySize",
    ):
        if integer(loader.get(key), f"loader.{key}") <= 0:
            raise TargetProfileError(f"loader.{key} must be positive")
    patch_length = integer(loader.get("patchLength"), "loader.patchLength")
    stolen_length = integer(loader.get("stolenLength"), "loader.stolenLength")
    if patch_length != 14 or stolen_length != 16:
        raise TargetProfileError("loader patchLength/stolenLength must be the reviewed 14/16 contract")
    hook_bytes = _hex_bytes(loader.get("hookBytes"), "loader.hookBytes", stolen_length)
    _hex_bytes(loader.get("versionBytes"), "loader.versionBytes", 16)
    lea = integer(loader.get("leaOffset"), "loader.leaOffset")
    if lea < 0 or lea + 7 > stolen_length or hook_bytes[lea : lea + 3] != b"\x48\x8d\x05":
        raise TargetProfileError("loader.leaOffset must name the stolen RIP-relative LEA")
    if (
        integer(loader["continuation"], "loader.continuation")
        != integer(loader["hookAddress"], "loader.hookAddress") + stolen_length
    ):
        raise TargetProfileError("loader.continuation must follow the stolen prologue")

    mappings = _object(manifest, "mappings", "mappings")
    text = _ps4_range(mappings.get("text"), "mappings.text")
    data = _ps4_range(mappings.get("data"), "mappings.data")
    segments = mappings.get("dataSegments")
    if not isinstance(segments, list) or not segments:
        raise TargetProfileError("mappings.dataSegments must list the exact RW mappings")
    cursor = data[0]
    for index, segment in enumerate(segments):
        start, end, _ = _ps4_range(segment, f"mappings.dataSegments[{index}]")
        if start != cursor:
            raise TargetProfileError("mappings.dataSegments must tile mappings.data without gaps")
        cursor = end
    if cursor != data[1]:
        raise TargetProfileError("mappings.dataSegments must end at mappings.data.end")
    hook = integer(loader["hookAddress"], "loader.hookAddress")
    if not text[0] <= hook < text[1]:
        raise TargetProfileError("loader.hookAddress must lie inside mappings.text")
    # The hook patch is swapped in and out with one aligned 8-byte exchange while game threads run it.
    if hook % 8:
        raise TargetProfileError("loader.hookAddress must be 8-byte aligned")
    if integer(loader["gatewayHint"], "loader.gatewayHint") % 0x4000:
        raise TargetProfileError("loader.gatewayHint must be 16 KiB aligned")

    build = _object(manifest, "build", "build")
    channel = build.get("channel")
    if channel not in {"primary", "development"}:
        raise TargetProfileError("build.channel must be primary or development")
    toolchain = _object(build, "toolchain", "build.toolchain")
    if toolchain.get("compiler") != "clang" or integer(toolchain.get("llvmMajor"), "build.toolchain.llvmMajor") != 20:
        raise TargetProfileError("build.toolchain must pin clang from LLVM 20")
    native_input = _input(build, "nativeAddresses", verify_inputs=verify_inputs)
    capability_input = _input(build, "capabilities", verify_inputs=verify_inputs)
    custom_lane_input = _input(build, "customLane", verify_inputs=verify_inputs) if "customLane" in build else None
    boot_pools_input = _input(build, "bootPools", verify_inputs=verify_inputs) if "bootPools" in build else None
    target_header = _string(build, "targetHeader", "build.targetHeader")
    if not (REPO_ROOT / "include" / target_header).is_file():
        raise TargetProfileError(f"build.targetHeader does not exist: include/{target_header}")
    injection = _object(build, "injection", "build.injection")
    if injection.get("lane") != "ps4debug-oneshot":
        raise TargetProfileError("build.injection.lane must be ps4debug-oneshot")
    for key in ("ptrace", "targetElevation", "rpcWorker"):
        if boolean(injection.get(key), f"build.injection.{key}"):
            raise TargetProfileError(f"build.injection.{key} must stay off for PS4 targets")
    features = _object(build, "features", "build.features")
    unknown = sorted(set(features) - set(PS4_FEATURE_KEYS))
    if unknown:
        raise TargetProfileError(f"unknown PS4 build.features keys: {unknown}")
    feature_values = {key: boolean(features.get(key), f"build.features.{key}") for key in PS4_FEATURE_KEYS}
    # PS4 custom packs build the engine lane when the target has a custom lane (ps4_build.py), the
    # engine-less shell otherwise; either way the packs live on /data and every pack row runs on the
    # game-thread drain.
    if feature_values["customPacks"] and not (feature_values["storage"] and feature_values["mainThreadActions"]):
        raise TargetProfileError("customPacks requires storage and mainThreadActions on PS4")
    if feature_values["bootPools"] and "bootPools" not in build:
        raise TargetProfileError("bootPools requires build.bootPools")
    if feature_values["mainThreadActions"] and not feature_values["nativeFeatures"]:
        raise TargetProfileError("mainThreadActions requires nativeFeatures")
    if "padReadStateGot" in loader:
        slot = integer(loader["padReadStateGot"], "loader.padReadStateGot")
        if slot % 8 or not data[0] <= slot < data[1]:
            raise TargetProfileError("loader.padReadStateGot must be an aligned slot inside a data segment")
        _string(loader, "padImportModule", "loader.padImportModule")
    if feature_values["touchpad"] and "padReadStateGot" not in loader:
        raise TargetProfileError("touchpad requires loader.padReadStateGot")
    globals_input: dict[str, str] | None = None
    if "scriptGlobals" in build:
        globals_input = _input(build, "scriptGlobals", verify_inputs=verify_inputs)
        anchors = json.loads((REPO_ROOT / globals_input["json"]).read_text(encoding="utf-8"))
        # A globals base or index from another build is a blind write, not a clean failure.
        if anchors.get("targetId") != stem:
            raise TargetProfileError(f"script-global anchors name {anchors.get('targetId')}, not {stem}")
    if feature_values["scriptGlobals"] and globals_input is None:
        raise TargetProfileError("scriptGlobals requires build.scriptGlobals")

    table = json.loads((REPO_ROOT / native_input["json"]).read_text(encoding="utf-8"))
    if table.get("target") != stem:
        raise TargetProfileError(f"native table names {table.get('target')}, not {stem}")
    if table.get("normalizedElfSha256") != identity["normalizedElfSha256"]:
        raise TargetProfileError("native table and identity disagree on the normalized ELF")
    addresses = {row["name"]: integer(row["address"], row["name"]) for row in table.get("addresses", [])}
    natives = manifest.get("enabledNatives")
    if not isinstance(natives, list) or not natives:
        raise TargetProfileError("enabledNatives must list the preflight fingerprints")
    seen: set[str] = set()
    for row in natives:
        if not isinstance(row, dict):
            raise TargetProfileError("enabledNatives rows must be objects")
        name = _string(row, "name", "enabledNatives.name")
        if name in seen:
            raise TargetProfileError(f"duplicate enabled native: {name}")
        seen.add(name)
        if addresses.get(name) != integer(row.get("address"), f"enabledNatives.{name}.address"):
            raise TargetProfileError(f"enabled native {name} disagrees with the native table")
        if not SHA256_RE.fullmatch(str(row.get("sha256", ""))):
            raise TargetProfileError(f"enabled native {name} needs its 32-byte prologue SHA-256")
    if addresses.get("PLAYER_PED_ID") != hook:
        raise TargetProfileError("loader.hookAddress must be the PLAYER_PED_ID handler")

    return {
        "platform": "ps4",
        "stem": stem,
        "targetId": target_id,
        "titleId": title_id,
        "contentId": content_id,
        "contentVersion": manifest["contentVersion"],
        "channel": channel,
        "nativeAddresses": native_input,
        "capabilities": capability_input,
        "scriptGlobals": globals_input,
        "customLane": custom_lane_input,
        "bootPools": boot_pools_input,
        "targetHeader": target_header,
        "features": feature_values,
        "injection": {"lane": "ps4debug-oneshot"},
    }


def load_manifest(path: Path, *, verify_inputs: bool = True) -> tuple[dict[str, object], dict[str, object]]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise TargetProfileError(f"cannot read target manifest {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise TargetProfileError("target manifest must contain a JSON object")
    return value, validate_manifest(value, manifest_path=path, verify_inputs=verify_inputs)


def expected_build_config(manifest: dict[str, object], delivery: str) -> dict[str, int | str]:
    profile = validate_manifest(manifest)
    loader = _object(manifest, "loader", "loader")
    features = profile["features"]
    injection = profile["injection"]
    assert isinstance(features, dict) and isinstance(injection, dict)
    expected = {
        "target_id": str(profile["targetId"]),
        "title_id": str(profile["titleId"]),
        "content_id": str(profile["contentId"]),
        "content_version": str(profile["contentVersion"]),
        "target_channel": str(profile["channel"]),
        "native_input_sha256": str(profile["nativeAddresses"]["sha256"]),
        "script_globals_input_sha256": str(profile["scriptGlobals"]["sha256"]),
        "injection_lane": str(injection["lane"]),
        "worker_context": 1,
        "native_features": int(features["nativeFeatures"]),
        "worker_text": int(features["workerTextOverlay"]),
        "toasts": int(features["toasts"]),
        "vehicle_preview": int(features["vehiclePreview"]),
        "instructional_scaleform": int(features["instructionalScaleform"]),
        "button_glyphs": int(features["buttonGlyphs"]),
        "script_globals": int(features["scriptGlobals"]),
        "phase_intercept": int(features["phaseIntercept"]),
        "phase_draw_list": int(features["phaseDrawList"]),
        "custom_packs": int(features["customPacks"]),
        "boot_pools": int(features["bootPools"]),
        "self_start": int(injection["selfStartWorker"]),
        "embedded_worker": 1,
        "loader_wait": 1,
        "loader_persistent": 1,
        "loader_sp_ready": 1,
        "loader_sp_mode": 1,
        "loader_sp_deref": 1,
        "loader_sp_addr": integer(loader["playerPedAnchor"], "loader.playerPedAnchor"),
        "loader_sp_deref_offset": integer(loader["playerPedOffset"], "loader.playerPedOffset"),
        "loader_sp_confirmations": 3,
        "loader_probe_nostop": int(injection["probeNoStop"]),
        "loader_nostop_io": int(injection["noStopIo"]),
        "loader_nostop_strict": int(injection["noStopStrict"]),
        "loader_verify_writes": int(injection["verifyWrites"]),
        "loader_cave_bootstrap": int(injection["caveBootstrap"]),
        "cave_inject": int(injection["caveInject"]),
        "classic_inject": int(injection["classicInject"]),
        "loader_broker": 1,
        "loader_phase": int(injection["installRenderPhase"]),
        "loader_guard": 1,
        "loader_verify_version": 1,
        "unvalidated_toolchain": 0,
    }
    if delivery == "etahen":
        expected["etahen_runtime"] = 1
    return expected

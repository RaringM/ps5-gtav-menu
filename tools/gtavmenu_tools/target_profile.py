"""Validated, reproducible build settings for one exact GTA V target."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGET_RE = re.compile(r"[a-z0-9]+-\d+\.\d+\.\d+\Z")


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


def target_stem(manifest: dict[str, object]) -> str:
    title = manifest.get("titleId")
    version = manifest.get("contentVersion")
    if not isinstance(title, str) or not isinstance(version, str):
        raise TargetProfileError("target manifest is missing titleId/contentVersion")
    stem = f"{title.lower()}-{version}"
    if not TARGET_RE.fullmatch(stem):
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
    stem = target_stem(manifest)
    if manifest_path is not None and manifest_path.stem != stem:
        raise TargetProfileError(f"target manifest path names {manifest_path.stem}, but content identifies {stem}")

    target_id = manifest.get("targetId")
    if not isinstance(target_id, str) or not target_id:
        raise TargetProfileError("target manifest is missing targetId")
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
    if toolchain.get("payloadSdk") != "0.43" or integer(toolchain.get("llvmMajor"), "build.toolchain.llvmMajor") != 20:
        raise TargetProfileError("build.toolchain must pin payloadSdk 0.43 and LLVM 20")

    native_input = _input(build, "nativeAddresses", verify_inputs=verify_inputs)
    globals_input = _input(build, "scriptGlobals", verify_inputs=verify_inputs)
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
    )
    feature_values = {key: boolean(features.get(key), f"build.features.{key}") for key in feature_keys}
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

    return {
        "stem": stem,
        "targetId": target_id,
        "contentVersion": manifest["contentVersion"],
        "channel": channel,
        "nativeAddresses": native_input,
        "scriptGlobals": globals_input,
        "features": feature_values,
        "injection": {**injection_values, "lane": lane},
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

#!/usr/bin/env python3
"""Convert a PC vehicle replacement mod into a stock override pack.

The mod may be a loose folder, OPEN RPF, OIV or ZIP. Reviewed target metadata preserves
stock owners, patches and HD texture companions; unknown stock models refuse. Resource
conversion uses public vehicle tools and the verified --templates cache. The stock
handling name keeps its stock values. Load before the target appears in the session.

Example: convert_vehicle_replace.py --source MOD --id my-police3 --game /path/to/app0
         --templates build/retail-templates --repair texture-stride
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from types import SimpleNamespace

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))
ROOT = _HERE.parent

import convert_vehicle as cv  # noqa: E402
import import_pc_assets  # noqa: E402
from gtavmenu_tools import asset_metadata, mod_collection, stock_members  # noqa: E402
from gtavmenu_tools.asset_formats import AssetError, Limits, RpfMember, resource_header  # noqa: E402
from gtavmenu_tools.host_paths import assets_dir, build_dir  # noqa: E402
from gtavmenu_tools.runtime_pack import NAME_MAX  # noqa: E402

CONTAINER = "x64/vehicles.rpf"  # fixture member chains: x64/vehicles.rpf!/<file> (the import layout)
SUFFIXES = (".yft", ".ytd")
META_NAME = "handling.meta"
MAX_FILE_BYTES = 256 * 1024 * 1024
MAX_TOTAL_BYTES = 1024 * 1024 * 1024
MAX_PACKAGE_MEMBERS = 10000
MODEL = re.compile(r"[a-z0-9_]{1,40}")
ARCHIVE = re.compile(r"[a-z0-9_]{1,59}\.rpf")
# Shared texture parents: resident from the first traffic car and overlaid by DLCs (section 6).
SHARED = re.compile(r"vehshare(_[a-z0-9]+)?|vehicles_[a-z0-9_]+_interior")
# Residency classes (advice only; the worker's gate refuses a resident target and the load can be retried).
LAW = {
    "police", "police2", "police3", "police4", "policeb", "policet", "policeold1", "policeold2", "sheriff",
    "sheriff2", "fbi", "fbi2", "riot", "riot2", "pranger", "ambulance", "firetruk", "lguard", "polmav", "predator",
    "pbus", "policeb2",
}  # fmt: skip
PLAYER = {"tailgater": "Michael", "buffalo2": "Franklin", "bagger": "Franklin", "bodhi2": "Trevor"}
DLC_HINTS = ("update/ps5/dlcpacks/", "dlcpacks/", "update.rpf:dlc_patch/", "update2.rpf:dlc_patch/")


class ReplaceError(mod_collection.CollectionError):
    pass


@dataclass
class Row:
    """One stock member the pack replaces."""

    member: str  # stock member name, e.g. police3_hi.pft
    source: str  # the mod file it is converted from, e.g. police3_hi.yft
    found: list[str] = field(default_factory=list)  # where the user's game has it (archive:path)


# --- 1. collect ----------------------------------------------------------------------------------------------


def collect(source: Path):
    try:
        return mod_collection.collect(source, SUFFIXES)
    except mod_collection.CollectionError as error:
        raise ReplaceError(str(error)) from error


shown = mod_collection.shown


def pick_model(files: dict[str, bytes], model: str | None) -> str:
    """The stock model the mod replaces: --model, or the one <m>.yft (not _hi) among the files."""
    stems = sorted({name[: -len(".yft")] for name in files if name.endswith(".yft") and not name.endswith("_hi.yft")})
    if model is None:
        if len(stems) != 1:
            listed = ", ".join(stems) or "none"
            raise ReplaceError(f"the mod has {len(stems)} vehicle models ({listed}); pick one with --model NAME")
        model = stems[0]
    if not MODEL.fullmatch(model):
        raise ReplaceError(f"--model {model!r}: the lowercase stock model name ([a-z0-9_], e.g. police3)")
    for need in (f"{model}.yft", f"{model}.ytd"):
        if need not in files:
            raise ReplaceError(f"the mod has no {need} (a replace mod ships {model}.yft, {model}.ytd, ...)")
    return model


# --- 2. host gates -------------------------------------------------------------------------------------------


def is_vehicle_path(path: str) -> bool:
    inner = path.rsplit(":", 1)[-1]
    return ("/vehicles.rpf/" in f"/{inner}" or "/vehicles/" in f"/{inner}") and "vehiclemods" not in inner


def is_overlay(path: str) -> bool:
    return any(hint in path for hint in DLC_HINTS)


def gate(model: str, rows: list[Row], scanned: bool) -> tuple[list[Row], list[str], list[str]]:
    """The worker's override rules on the host: (rows kept, notes, refusals)."""
    notes: list[str] = []
    refusals: list[str] = []
    if SHARED.fullmatch(model):
        refusals.append(f"{model} is a shared vehicle texture dictionary: resident and overlaid, never replaceable")
    if not scanned:
        notes.append("stock names not checked on the host (no --game and no retail listings): the console gate decides")
        return rows, notes, refusals
    kept = []
    for row in rows:
        vehicle = [p for p in row.found if is_vehicle_path(p)]
        if not vehicle:
            if row.member.endswith("+hi.ptd"):
                notes.append(
                    f"{row.member}: the stock {model} has no HD texture dictionary; {row.source} is not shipped"
                )
                continue
            where = f" (only {row.found[0]})" if row.found else ""
            refusals.append(f"{row.member} is not a stock vehicle member of your game{where}: check --model")
            continue
        overlays = sorted({overlay_label(p) for p in vehicle if is_overlay(p)})
        if overlays and all(is_overlay(p) for p in vehicle):
            notes.append(
                f"{row.member}: a DLC vehicle ({', '.join(overlays)}); its DLC archive owns it (overlay only if "
                "several DLCs ship it: the worker handles both)"
            )
        elif overlays:
            notes.append(
                f"{row.member}: re-shipped by {', '.join(overlays)} "
                "(DLC overlay: the worker removes that overlay node right before Load)"
            )
        else:
            notes.append(f"{row.member}: base archive owner ({vehicle[0].split(':', 1)[0]}); plain override")
        kept.append(row)
    return kept, notes, refusals


def overlay_label(path: str) -> str:
    """The DLC pack of an overlay path: patchDay3NG, mpbeach, a dlc_patch/<pack> of update.rpf."""
    archive, _, inner = path.partition(":")
    for marker, text in (("dlcpacks/", archive + "/" + inner), ("dlc_patch/", inner)):
        if marker in text:
            return text.split(marker, 1)[1].split("/", 1)[0]
    return archive


def residency_advice(model: str) -> str:
    if model in LAW:
        return (
            f"{model} is a law/emergency vehicle: dispatched with a wanted level and parked at stations. Load with no "
            "wanted level, away from stations (LSPD cars: Sandy Shores/Paleto use sheriff cars; sheriff cars: "
            "stay in the city), ideally right after entering Story Mode and before seeing one."
        )
    if model in PLAYER:
        return (
            f"{model} is {PLAYER[model]}'s own vehicle: created at the safehouse and often resident. Play as another "
            "character or drive far from the safehouse, wait ~2 min, then Load."
        )
    return (
        f"{model} may be in traffic: Load right after entering Story Mode, before seeing or spawning one. "
        "A refusal '<member> in use; restart GTA' means one was loaded: restart GTA and Load earlier."
    )


# --- 3. fixture ----------------------------------------------------------------------------------------------


def fixture_record(name: str, blob: bytes) -> dict:
    """The import manifest record of one loose resource (tools/import_pc_assets.py's own record code)."""
    header = resource_header(blob)
    flags = (int(header["systemFlags"], 16), int(header["graphicsFlags"], 16))
    member = RpfMember(
        name=name,
        offset=0,
        size=len(blob),
        unpacked_size=header["systemBytes"] + header["graphicsBytes"],
        resource=True,
        flags=flags,
        encrypted=False,
        compressed=True,
    )
    window = SimpleNamespace(chain=CONTAINER, read_member=lambda _member, _maximum: blob)
    _, record = import_pc_assets.selected_blob(window, member)
    record["offsetInContainer"] = None  # a loose file: no container offset
    return record


def write_fixture(output: Path, files: dict[str, bytes], model: str, source_label: str) -> None:
    """A verified vehicle import fixture of loose files (resources/x64/vehicles.rpf/<name>, manifest.json)."""
    names = sorted(files)
    identity = hashlib.sha256()
    for name in names:
        identity.update(f"{name}\0{hashlib.sha256(files[name]).hexdigest()}\n".encode())
    report = {
        "schemaVersion": 1,
        "kind": "gtavmenu-pc-asset-import",
        "operation": "vehicle",
        "model": model,
        "modelHash": f"0x{cv.joaat(model):08x}",
        "source": {
            "path": source_label,
            "kind": "loose-replace-files",
            "bytes": sum(map(len, files.values())),
            "sha256": identity.hexdigest(),  # over the sorted (name, sha256) list
            "containers": [],
        },
        "resources": [fixture_record(name, files[name]) for name in names],
        "metadata": [],
        "qualification": {"selectedMemberIdentityVerified": True, "conversionAvailable": False},
        "limitations": ["Loose PC files of a replace mod (no archive); the stock vehicle's metas are not changed"],
    }
    resources = output / "resources" / CONTAINER
    resources.mkdir(parents=True)
    for name in names:
        (resources / name).write_bytes(files[name])
    (output / "manifest.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


# --- 5. handling ---------------------------------------------------------------------------------------------


def handling_report(blob: bytes | None, model: str) -> dict:
    """What the mod's handling.meta says about the model; never shipped (the worker adds new names only)."""
    if blob is None:
        return {"shipped": False, "note": "the mod ships no handling.meta: the stock handling stays"}
    try:
        root = asset_metadata.parse_xml(blob, Limits())
    except (AssetError, ValueError) as error:
        return {"shipped": True, "applied": False, "note": f"handling.meta not readable ({error})"}
    items = root.findall("./HandlingData/Item")
    match = [i for i in items if (i.findtext("handlingName") or "").strip().lower() == model]
    report = {"shipped": True, "applied": False, "items": len(items)}
    if not match:
        report["note"] = f"handling.meta ({len(items)} items) has no handlingName {model}: nothing to report"
        return report
    fields = {}
    for child in match[0]:
        if child.tag in ("handlingName", "SubHandlingData"):
            continue
        fields[child.tag] = child.get("value") if child.get("value") is not None else dict(child.attrib) or child.text
    report.update(
        handlingName=(match[0].findtext("handlingName") or "").strip(),
        fields=fields,
        note=(
            f"handling.meta sets {len(fields)} fields for {model}; NOT applied: the pack HANDLING_FILE mounter "
            "only adds new handlingNames and skips a stock one (custom_pack_data.inc), so the stock car keeps "
            "its handling. The values are in replace-report.json."
        ),
    )
    return report


# --- pipeline ------------------------------------------------------------------------------------------------


def converter_args(args) -> SimpleNamespace:
    """The namespace convert_vehicle.py's functions read (frozen run, plain tables, no mod kit)."""
    return SimpleNamespace(
        id=args.id,
        model=args.model,
        templates=args.templates,
        repair=list(args.repair),
        parts_repair=[],
        mod_kit=False,
        parts_fixture=None,
        parts_multi_page=False,
        source=args.source,
        fixture=None,
        retail_layouts=None,
        census=[],
        output_root=args.output_root,
        dry_run=args.dry_run,
    )


def build(args) -> int:
    files, origin, notes = collect(args.source)
    model = args.model = pick_model(files, args.model)
    name = args.name or f"{model} (replace pack)"
    if not (cv.LABEL_TEXT.fullmatch(name) and len(name) <= 40):
        raise ReplaceError("--name: printable ASCII, at most 40 characters")
    archive = args.archive or f"gmrep_{model}.rpf"
    if not (ARCHIVE.fullmatch(archive) and len(archive) <= NAME_MAX):
        raise ReplaceError(f"--archive {archive!r}: a lowercase NAME.rpf ([a-z0-9_], at most 59 before .rpf)")
    sources = {f"{model}.yft": files[f"{model}.yft"], f"{model}.ytd": files[f"{model}.ytd"]}
    if f"{model}_hi.yft" in files:
        sources[f"{model}_hi.yft"] = files[f"{model}_hi.yft"]
    else:
        sources[f"{model}_hi.yft"] = files[f"{model}.yft"]
        notes.append(f"no {model}_hi.yft: {model}.yft is used for the HD fragment too")
    if f"{model}+hi.ytd" in files and args.no_hd_textures:
        notes.append(f"--no-hd-textures: {model}+hi.ytd is not shipped")
    elif f"{model}+hi.ytd" in files:
        sources[f"{model}+hi.ytd"] = files[f"{model}+hi.ytd"]
    for extra in sorted(files):
        if extra.endswith(SUFFIXES) and extra not in sources and extra != f"{model}+hi.ytd":
            notes.append(f"{extra} ({origin[extra]}) is not part of the {model} replacement: ignored")
    rows = [
        Row(f"{model}.pft", f"{model}.yft"),
        Row(f"{model}_hi.pft", f"{model}_hi.yft"),
        Row(f"{model}.ptd", f"{model}.ytd"),
        Row(f"{model}+hi.ptd", f"{model}+hi.ytd"),
    ]
    wanted = {row.member for row in rows}
    stock = stock_members.from_args(args)
    found = stock.scan(wanted)
    scope = f"reviewed {stock.target} owner metadata ({stock.digest[:12]})"
    scanned = True
    for row in rows:
        row.found = found.get(row.member, [])
    stock_hi = bool([p for p in found.get(f"{model}+hi.ptd", []) if is_vehicle_path(p)])
    shipped = [row for row in rows if row.source in sources]
    kept, gate_notes, refusals = gate(model, shipped, scanned)
    if stock_hi and f"{model}+hi.ytd" not in sources:
        gate_notes.append(
            f"the stock {model} has {model}+hi.ptd and the pack ships none: close-ups keep the stock HD textures"
        )
    advice = residency_advice(model)
    handling = handling_report(files.get(META_NAME), model)
    print(f"replace {model}: stock check against {scope}")
    for note in notes + gate_notes:
        print(f"     note: {shown(note)}")
    print(f"     residency: {advice}")
    print(f"     handling: {handling['note']}")
    if refusals:
        raise ReplaceError("refused:\n  " + "\n  ".join(refusals))
    if len(kept) > 16:
        raise ReplaceError("more than 16 override rows")

    cargs = converter_args(args)
    plan = cv.Plan(args.dry_run)
    try:
        cv.preflight(cargs)
    except cv.ConvertError as error:
        if not args.dry_run:
            raise
        print(f"warning (dry run): {error}", file=sys.stderr)
    work = assets_dir(ROOT) / f"convert-{args.id}"
    stage = work / "pack-inputs"
    if not args.dry_run and work.exists():
        raise ReplaceError(f"{cv.rel(work)} exists; choose a new --id or remove it")
    if args.templates is not None:
        plan.steps.append("stage retail templates")
        print(f"[{len(plan.steps):02}] stage retail templates from {cv.rel(args.templates)}")
        if not args.dry_run:
            cv.stage_templates(cargs)
    fixture = work / "fixtures" / f"{model}-replace"
    plan.steps.append("fixture")
    print(f"[{len(plan.steps):02}] fixture of the loose files\n     write {cv.rel(fixture)}")
    if not args.dry_run:
        write_fixture(fixture, sources, model, args.source.name)
    chain = f"{CONTAINER}!/"
    base = [f"{chain}{model}.yft", f"{chain}{model}_hi.yft"]
    texture = f"{chain}{model}.ytd"
    fixture = cv.repair_chain(plan, cargs, fixture, args.repair, base, texture, work, "base")
    if not args.dry_run:
        stage.mkdir(parents=True, exist_ok=True)
        (work / "lods").mkdir(parents=True, exist_ok=True)
    report = work / "base-materials.json"
    cv.material_report(plan, cargs, fixture, base, texture, report)
    cv.convert(plan, cargs, fixture, report, work / "base-references", not args.single_page)
    pairs = []
    outputs: dict[str, Path] = {}
    for stem in (model, f"{model}_hi"):
        lods = work / "lods" / f"{stem}.pft"
        cv.lod_fill(plan, work / "base-references" / f"{stem}.partial.pft", lods, deep_copy=stem == model)
        pairs.append((lods, stage / f"{stem}.pft"))
        outputs[f"{stem}.pft"] = stage / f"{stem}.pft"
    cv.octants(plan, pairs)
    for stem in (model, f"{model}+hi"):
        if f"{stem}.ptd" not in {row.member for row in kept}:
            continue
        command = cv.py("convert_pc_ytd_writer.py", "--ytd", cv.fixture_member(fixture, f"{chain}{stem}.ytd"))
        command += ["--output", stage / f"{stem}.ptd"]
        if args.max_texture_size:
            command += ["--max-size", str(args.max_texture_size)]
        if args.templates is not None:
            command += ["--templates", cv.staged_templates(cargs)]
        plan.run("textures" + (" (HD)" if stem.endswith("+hi") else ""), command)
        outputs[f"{stem}.ptd"] = stage / f"{stem}.ptd"
    members = [row.member for row in kept]
    command = cv.py("build_runtime_pack.py", "--id", args.id, "--archive", archive)
    for member in members:
        command += ["--member", f"{member}={outputs[member]}"]
    command += ["--override-archive", f"{archive}=" + ",".join(members)]
    command += ["--spawn", f"vehicle:{model}={name}", "--output-root", args.output_root, "--plain-toc"]
    plan.run("pack (overlay archive, override rows)", command)
    pack = args.output_root / args.id
    plan.run("validate", cv.py("tools/validate_runtime_pack.py", pack))
    if not args.dry_run:
        summary = {
            "tool": "tools/convert_vehicle_replace.py",
            "model": model,
            "pack": cv.rel(pack),
            "archive": archive,
            "inputs": {
                k: {"from": origin.get(k, "copy"), "sha256": hashlib.sha256(v).hexdigest()} for k, v in sources.items()
            },
            "stockScan": scope,
            "rows": [{"member": r.member, "source": r.source, "stock": r.found} for r in kept],
            "notes": notes + gate_notes,
            "residency": advice,
            "handling": handling,
            "repairs": list(args.repair),
            "outputs": {k: hashlib.sha256(v.read_bytes()).hexdigest() for k, v in outputs.items() if k in members},
            "steps": plan.steps,
        }
        (work / "replace-report.json").write_text(json.dumps(summary, indent=1, sort_keys=True) + "\n")
    print(f"{'planned' if args.dry_run else 'done'}: {len(plan.steps)} steps; pack {cv.rel(pack)}")
    print(f"in game: Custom Packs -> Load (see residency above), then spawn {model} (Vehicle Browser or the pack row)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, required=True, help="mod folder, PC dlc.rpf or .oiv package")
    parser.add_argument("--id", required=True, help="pack id, e.g. gtavmenu-replace-police3-v1")
    parser.add_argument("--model", help="stock model the mod replaces (default: the mod's one <m>.yft)")
    parser.add_argument("--name", help="spawn row text (default '<model> (replace pack)')")
    parser.add_argument("--archive", help="overlay archive name (default gmrep_<model>.rpf)")
    parser.add_argument("--repair", action="append", default=[], metavar="NAME[=BONE,...]", help=", ".join(cv.REPAIRS))
    parser.add_argument(
        "--no-hd-textures", action="store_true", help="leave <m>+hi.ytd out (the stock HD dictionary stays)"
    )
    parser.add_argument("--single-page", action="store_true", help="one large system page instead of multi-page")
    parser.add_argument("--max-texture-size", type=int, help="convert_pc_ytd_writer.py --max-size for both txds")
    parser.add_argument(
        "--templates",
        type=Path,
        default=build_dir(ROOT) / "retail-templates",
        help="retail template cache (./menu-ctl.sh fetch-templates)",
    )
    stock_members.arguments(parser)
    parser.add_argument("--output-root", type=Path, default=build_dir(ROOT) / "custom-assets")
    parser.add_argument("--dry-run", action="store_true", help="check the mod and the gates, print the steps")
    args = parser.parse_args(argv)
    for key in ("source", "templates", "game", "stock_cache", "stock_manifest", "output_root"):
        if getattr(args, key) is not None:
            setattr(args, key, getattr(args, key).absolute())
    if not re.fullmatch(r"[a-z0-9]([a-z0-9-]*[a-z0-9])?", args.id) or len(args.id) > 64:
        parser.error("--id: lowercase letters, digits and '-' (at most 64)")
    if args.max_texture_size is not None and not 4 <= args.max_texture_size <= 16384:
        parser.error("--max-texture-size: 4..16384")
    try:
        return build(args)
    except (
        stock_members.StockError,
        mod_collection.CollectionError,
        ReplaceError,
        cv.ConvertError,
        AssetError,
        OSError,
        ValueError,
    ) as error:
        print(f"convert_vehicle_replace: {shown(str(error))}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

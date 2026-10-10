#!/usr/bin/env python3
"""Convert a PC add-on vehicle into a runtime pack using verified retail templates.

The pipeline imports and repairs a copy of the PC assets, converts typed vehicle
components, textures and metadata, and writes a new pack. --dry-run lists the
steps without writing; existing output directories are never replaced.
"""

from __future__ import annotations

import argparse
import collections
import importlib.util
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import xml.dom.minidom
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import prepare_carcols  # noqa: E402
from gtavmenu_tools import asset_metadata, retail_templates  # noqa: E402
from gtavmenu_tools.asset_formats import AssetError, Limits, page_bytes  # noqa: E402
from gtavmenu_tools.assets import Budget, read_file  # noqa: E402
from gtavmenu_tools.hashes import joaat  # noqa: E402
from gtavmenu_tools.host_paths import assets_dir, build_dir  # noqa: E402
from gtavmenu_tools.runtime_pack import NAME_MAX, valid_id  # noqa: E402
from gtavmenu_tools.vehicle_repair_inputs import FixtureError, resource_path, validate_fixture  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
VEHSHARE = "corpus/vehshare-a.ptd"
SUPPLEMENTARY_REFERENCES = (
    "corpus/vigero.pft",
    "corpus/trailersmall.pft",
    "corpus/trailers3.pft",
    "corpus/buc2_dl_camo2.pft",
    "corpus/cheb_skirts.pft",
    "corpus/nemesis.pft",
    "corpus/armytrailer_hi.pft",
)
RETAIL_TEMPLATES = (
    "corpus/tornado6.pft",
    "corpus/tornado6.ptd",
    "corpus/banshee-a.pft",
    "corpus/banshee-a.ptd",
    "corpus/decal-schema-a.pft",
    "corpus/vehshare-a.ptd",
    *SUPPLEMENTARY_REFERENCES,
    "razor-shader-database-source-a/sga_prospero_final.awc",
    "razor-shader-database-source-a/sga_prospero_final_init.awc",
)
RETAIL_LAYOUTS = "retail-metas/vehiclelayouts.meta"
SHADER_DATABASE = "razor-shader-database-source-a"
FETCH_HINT = "./menu-ctl.sh fetch-templates --cache {cache} (GTA V running on the console), or python3 tools/fetch_retail_templates.py --source file:///path/to/app0 --cache {cache}"
TARGET = "data/targets/ppsa04264-01.010.002.json"
DATA_ORDER = (
    ("HANDLING_FILE", "handling"),
    ("CARCOLS_FILE", "carcols"),
    ("VEHICLE_LAYOUTS_FILE", "layouts"),
    ("VEHICLE_METADATA_FILE", "vehicles"),
    ("VEHICLE_VARIATION_FILE", "carvariations_kit"),
)
DEFAULT_METAS = {
    "handling": "handling.meta",
    "carcols": "carcols.meta",
    "layouts": "ai/vehiclelayouts.meta",
    "vehicles": "vehicles.meta",
    "carvariations_kit": "carvariations.meta",
}
REPAIRS = {
    "seat-mirror": ("repair_pc_seat_mirror.py", True, False),
    "triangle-counts": ("repair_pc_triangle_counts.py", False, False),
    "fragment-quirks": ("repair_pc_fragment_quirks.py", False, True),
    "duplicate-bones": ("repair_pc_duplicate_bones.py", False, False),
    "inverse-precision": ("repair_pc_inverse_precision.py", True, False),
    "pose-precision": ("repair_pc_pose_precision.py", True, False),
    "texture-stride": ("repair_pc_texture_stride.py", False, False),
    "select-model": ("repair_pc_select_model.py", False, False),
    "nonfinite-texcoords": ("repair_pc_nonfinite_texcoords.py", False, False),
    "texture-names": ("repair_pc_texture_names.py", False, False),
    "auto": ("repair_pc_auto.py", False, True),
}
REFUSAL_HINTS = (
    (
        "Legacy stride/mip spans differ from block-rounded linear layout",
        "texture-stride",
        "texture stride of another block size",
    ),
    ("skeleton duplicate bone tags or names are ambiguous", "duplicate-bones", "two bones share a lettered name"),
    (
        "drawable optional extension or nonzero model block size is unsupported",
        "fragment-quirks",
        "placeholder fragment matrices",
    ),
    ("collision nonnull empty array unsupported", "fragment-quirks", "empty collision octant with a stale pointer"),
    ("triangle-declaration-unqualified", "triangle-counts", "geometry with a zero triangle count"),
)
DIAGNOSED = (
    ("rejected skeleton/pose cannot be serialized", "bone matrices disagree with their pose", "pose"),
    ("mesh topology has rejected geometry", "rejected mesh geometry", "mesh"),
    ("not a precision error", "bone matrices off their pose beyond the precision repair", "pose"),
)
AUTO_DONE = {
    "select-model",
    "texture-stride",
    "texture-names",
    "nonfinite-texcoords",
    "duplicate-bones",
    "triangle-counts",
    "pose-precision",
    "fragment-quirks",
}
PRECISION_REPAIR_MAX = 0.0001
TAIL_LINES = 200
DIAGNOSE = "--diagnose"
WHEELS = re.compile("<Wheels>(.*?)</Wheels>", re.S)
KIT_IDS = range(4609, 65535)
TEMPLATE_REPAIRS = ("triangle-counts", "fragment-quirks", "nonfinite-texcoords", "auto")
DEFAULT_DRIVEBY = ("LOW_BUCCANEER_FRONT_LEFT", "LOW_BUCCANEER_FRONT_RIGHT")
NAME = re.compile("<Name>\\s*([^<\\s]+)\\s*</Name>")
MODEL_NAME = re.compile("<modelName>\\s*[^<\\s]+\\s*</modelName>")
DRIVEBY = re.compile("(<firstPersonDrivebyData>\\s*\\n)(.*?)(\\n[ \\t]*</firstPersonDrivebyData>)", re.S)
ITEM = re.compile("<Item>\\s*([^<\\s]+)\\s*</Item>")
MAKE = re.compile("<vehicleMakeName>[^<]*</vehicleMakeName>|<vehicleMakeName\\s*/>")
GAME_NAME_LINE = re.compile("^([ \\t]*)<gameName>[^<]*</gameName>[ \\t]*$", re.M)
LABEL_KEY = re.compile("[A-Za-z0-9_]+")
LABEL_TEXT = re.compile("[\\x20-\\x7e]+")


class ConvertError(RuntimeError):
    pass


def refusal(
    step: int, label: str, tail: list[str], flag: str = "--repair", found: dict | None = None, auto: bool = False
) -> str:
    """One line for a failed step: the gate's own message and, for a known exporter quirk, the repair to add.

    FOUND is diagnose()'s {repair: [bone, ...]} for a pose or mesh refusal ("none": what fits no repair).
    AUTO: FLAG's chain ran --repair auto (the default), which applies every bone-free repair it detects:
    a bone repair is then added beside it (naming any repair drops the default), and a bone-free quirk
    that still refuses is one auto's detector missed."""
    text = "".join(tail)
    where = f"step {step} ({label}) refused"
    keep = f" {flag} auto" if auto else ""
    for needle, why, _ in DIAGNOSED:
        if needle in text and found:
            adds = [
                f"{flag} {k}" + (f"={','.join(v)}" if REPAIRS[k][1] else "")
                for k, v in found.items()
                if k in REPAIRS and (not (auto and k in AUTO_DONE))
            ]
            odd = found.get("none")
            rest = f"; no repair fits {', '.join(odd) or 'the rest'}" if odd is not None else ""
            fix = f"; add {' '.join(adds)}{keep} and rerun" if adds else ""
            if auto and (not adds) and (odd is None):
                fix = f"; {flag} auto ran but did not repair {', '.join(found)}: record the output above"
            return f"{where}: {why} ({needle}){fix}{rest}"
    for needle, repair, why in REFUSAL_HINTS:
        if needle in text:
            if auto:
                return f"{where}: {why} ({needle}), which the {repair} detector of {flag} auto did not find: record the output above (a form of the quirk the repair does not know)"
            return f"{where}: {why} ({needle}); add {flag} {repair} and rerun"
    last = next((line.strip() for line in reversed(tail) if line.strip()), "")
    return f"step {step} ({label}) failed: {last or 'no output'}" + (" (tool output above)" if last else "")


def classify_bone(pose_row: dict, stored_local: list[float]) -> str:
    """Which repair fits one pose-rejected bone (pc_transforms_pose rows; matrices in M11..M44 order)."""
    rotation = [stored_local[i] for i in (0, 1, 2, 4, 5, 6, 8, 9, 10)]
    if [round(v, 5) + 0.0 for v in rotation] == [-1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]:
        return "seat-mirror"
    local, inverse = (pose_row["localComparison"], pose_row["inverseBindTimesGlobalComparison"])
    if max(local["maximumAbsoluteError"], inverse["maximumAbsoluteError"]) > PRECISION_REPAIR_MAX:
        return "none"
    if not local["mismatches"] and {row["field"] for row in inverse["mismatches"]} <= {"M41", "M42", "M43"}:
        return "inverse-precision"
    return "pose-precision"


def diagnose(kind: str, fixture: Path, material_report: Path, templates: Path) -> dict[str, list[str]]:
    """The converter's own PC-side walk on FIXTURE (read-only), its rejections sorted by the repair that fits.

    pose: each rejected bone by repair (seat-mirror, pose-precision, inverse-precision, none);
    mesh: triangle-counts when every rejected geometry is a zero triangle count, else none."""
    sys.path.insert(0, str(TOOLS))
    found: dict[str, set[str]] = {}
    if kind == "pose":
        from repair_pc_seat_mirror import inspect

        for pose in inspect(fixture, material_report)["resources"]:
            matrices = pose["arrays"]["TransformationsPointer"]["matrices"]
            for row in pose["rejections"]:
                index = row["boneIndex"]
                found.setdefault(classify_bone(pose["bones"][index], matrices[index]), set()).add(row["name"])
    else:
        from repair_pc_triangle_counts import collect

        probe = material_report.parent / f"{material_report.stem}-mesh-probe"
        try:
            found["triangle-counts" if collect(fixture, material_report, probe, templates) else "none"] = set()
        except SystemExit as error:
            found["none"] = {str(error)[:160]}
        finally:
            shutil.rmtree(probe, ignore_errors=True)
    return {name: sorted(values) for name, values in sorted(found.items())}


SIREN_BONE = re.compile("siren(\\d+)")
SIREN_BONES = "--siren-bones"


def siren_bones(fixture: Path, material_report: Path, member: str) -> dict[str, list[float]]:
    """Model-space positions of MEMBER's bones siren1..siren20 (the converter's own PC pose walk, read-only)."""
    sys.path.insert(0, str(TOOLS))
    from repair_pc_seat_mirror import inspect

    walk = inspect(fixture, material_report)
    skeletons = walk["freshSkinningEvidence"]["freshSkeletonEvidence"]["resources"]
    for skeleton, pose in zip(skeletons, walk["resources"], strict=True):
        if skeleton["member"] == member:
            found = {}
            for bone in pose["bones"]:
                lamp = SIREN_BONE.fullmatch(bone["name"].strip().lower())
                if lamp and 1 <= int(lamp.group(1)) <= prepare_carcols.SIREN_LAMPS:
                    found[str(int(lamp.group(1)))] = [round(v, 4) for v in bone["globalPose"][12:15]]
            return found
    raise SystemExit(f"{member}: not in the material report's skeletons")


def siren_lamps(spec: dict, bones: dict[str, list[float]]) -> str:
    """Default --siren-lamps for a car: '-' for a lamp without its bone; a spec with groups L and R puts each
    lamp on its side (bone x < 0: L, the car's left; else R), any other spec keeps its own key per lamp."""
    sides = {"L", "R"} <= set(spec["groups"])
    keys = []
    for n in range(1, prepare_carcols.SIREN_LAMPS + 1):
        position = bones.get(str(n))
        if position is None:
            keys.append("-")
        else:
            keys.append(("L" if position[0] < 0 else "R") if sides else spec["lamps"][n - 1])
    return "".join(keys)


def load_siren_spec(args) -> dict | None:
    """The checked --siren-preset / --siren-spec (prepare_carcols.siren_spec), or None without one."""
    if args.siren_preset is None and args.siren_spec is None:
        return None
    try:
        return prepare_carcols.load_siren_spec(args.siren_preset, args.siren_spec, args.siren_lamps)
    except SystemExit as error:
        raise ConvertError(str(error)) from None


def diagnosis(command: list, kind: str) -> dict | None:
    """Run diagnose() for a refused COMMAND in a child: a converter step (--pc-fixture, its material report and
    --native-reference's templates root) or a repair step (--fixture, --material-report, --templates)."""
    words = [str(c) for c in command]

    def value(*flags: str) -> str | None:
        flag = next((f for f in flags if f in words), None)
        return words[words.index(flag) + 1] if flag is not None and words.index(flag) + 1 < len(words) else None

    fixture, report = (value("--pc-fixture", "--fixture"), value("--material-report"))
    native, templates = (value("--native-reference"), value("--templates"))
    root = str(Path(native).parents[1]) if native else templates or str(assets_dir(ROOT))
    if fixture is None or report is None:
        return None
    print(f"     diagnosing the {kind} refusal (which repair fits)", flush=True)
    return child_query(DIAGNOSE, kind, fixture, report, root)


def child_query(*argv: str) -> dict | None:
    """This script's internal mode ARGV in a child (using the same public tool modules): its last
    stdout line as JSON, or None when it fails."""
    me = str(Path(__file__).resolve())
    child = [sys.executable, me, *argv]
    done = subprocess.run(child, cwd=ROOT, capture_output=True, text=True, errors="replace", check=False)
    lines = done.stdout.strip().splitlines()
    try:
        return json.loads(lines[-1]) if done.returncode == 0 and lines else None
    except json.JSONDecodeError:
        return None


@dataclass
class Plan:
    dry_run: bool
    repair_flag: str = "--repair"
    auto: dict[str, bool] = field(default_factory=dict)
    steps: list[str] = field(default_factory=list)

    def run(self, label: str, command: list[str]) -> None:
        shown = " ".join(shlex.quote(short(c)) for c in command)
        self.steps.append(f"{label}: {shown}")
        print(f"[{len(self.steps):02}] {label}\n     {shown}", flush=True)
        if self.dry_run:
            return
        tail: collections.deque[str] = collections.deque(maxlen=TAIL_LINES)
        with subprocess.Popen(
            [str(c) for c in command],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        ) as process:
            for line in process.stdout:
                print(line, end="", flush=True)
                tail.append(line)
        if process.returncode:
            text = "".join(tail)
            kind = next((kind for needle, _, kind in DIAGNOSED if needle in text), None)
            found = diagnosis(command, kind) if kind else None
            auto = self.auto.get(self.repair_flag, False)
            raise ConvertError(refusal(len(self.steps), label, list(tail), self.repair_flag, found, auto))

    def query(self, label: str, shown: str, *argv: str) -> dict:
        """A read-only look at the fixture in a child (child_query); a dry run only lists it."""
        self.steps.append(f"{label}: {shown}")
        print(f"[{len(self.steps):02}] {label}\n     {shown}", flush=True)
        if self.dry_run:
            return {}
        found = child_query(*argv)
        if found is None:
            raise ConvertError(
                f"step {len(self.steps)} ({label}) failed: rerun `{short(sys.executable)} {rel(Path(__file__))} {' '.join(map(short, argv))}` for its output"
            )
        return found

    def write(self, label: str, path: Path, text: str) -> None:
        self.steps.append(f"{label}: write {path}")
        print(f"[{len(self.steps):02}] {label}\n     write {rel(path)}", flush=True)
        if not self.dry_run:
            path.parent.mkdir(parents=True, exist_ok=True)
            xml.dom.minidom.parseString(text.encode("utf-8"))
            path.write_text(text, encoding="utf-8", newline="\n")


def staged_templates(args) -> Path:
    """The selected host build work directory's copy of the retail templates."""
    return assets_dir(ROOT) / f"convert-{args.id}" / "templates"


def template_problems(args) -> list[str]:
    """--templates: every retail template present and matching the template manifest."""
    if getattr(args, "templates", None) is None:
        return []
    hint = FETCH_HINT.format(cache=shlex.quote(str(args.templates)))
    try:
        _, rows = retail_templates.load_manifest()
    except retail_templates.TemplateError as error:
        return [str(error)]
    pinned = {row.cache: row for row in rows}
    problems = []
    for name in (*RETAIL_TEMPLATES, *([] if args.retail_layouts else [RETAIL_LAYOUTS])):
        row = pinned.get(name)
        if row is None:
            problems.append(f"retail template {name} is not in the template manifest")
        elif not retail_templates.cached_ok(args.templates, row):
            if name == RETAIL_LAYOUTS:
                continue
            state = "missing" if not (args.templates / name).exists() else "does not match the manifest sha256"
            problems.append(f"retail template {name} {state} in {rel(args.templates)}; fetch it: {hint}")
    return problems


def short(value) -> str:
    """Display form: repo-relative paths, python3 for the interpreter."""
    text = str(value)
    return "python3" if text == sys.executable else text.replace(f"{ROOT}/", "")


def rel(path: Path) -> str:
    path = Path(path)
    return str(path.relative_to(ROOT)) if path.is_absolute() and ROOT in path.parents else str(path)


def py(tool: str, *args) -> list:
    """A tool command: tools/<tool> (or a repo-relative path) run by this interpreter."""
    return [sys.executable, str(TOOLS / tool) if "/" not in tool else str(ROOT / tool), *args]


def read_meta(path: Path) -> str:
    """Read bounded, regular XML while preserving its text formatting."""
    limits = Limits()
    try:
        data = read_file(path, Budget(Limits(max_file_bytes=limits.max_metadata_bytes)))
        asset_metadata.parse_xml(data, limits)
        return data.decode("utf-8-sig").replace("\r\n", "\n")
    except (AssetError, OSError, UnicodeError, ET.ParseError) as error:
        raise ConvertError(f"{rel(path)}: invalid metadata: {error}") from error


def set_make(text: str, key: str) -> tuple[str, int]:
    """Point every <vehicleMakeName> at KEY (inserted after <gameName> where absent); returns (text, items)."""
    text, count = MAKE.subn(f"<vehicleMakeName>{key}</vehicleMakeName>", text)
    if count == 0:
        text, count = GAME_NAME_LINE.subn(f"\\g<0>\\n\\g<1><vehicleMakeName>{key}</vehicleMakeName>", text)
    if count == 0:
        raise ConvertError("vehicles.meta has neither <vehicleMakeName> nor <gameName>")
    return (text, count)


def label_spec(spec: str) -> tuple[str, str]:
    """'KEY=TEXT' of a pack label row: KEY [A-Za-z0-9_], TEXT printable ASCII, each at most NAME_MAX."""
    key, sep, text = spec.partition("=")
    if not (sep and LABEL_KEY.fullmatch(key) and LABEL_TEXT.fullmatch(text)):
        raise ConvertError(f"label {spec!r}: expected KEY=TEXT, KEY [A-Za-z0-9_], TEXT printable ASCII")
    if len(key) > NAME_MAX or len(text) > NAME_MAX:
        raise ConvertError(f"label {spec!r}: KEY and TEXT are at most {NAME_MAX} characters")
    return (key, text)


GAME_NAME_MAX = 11
RETAIL_VEHICLES = ROOT / "data/vehicles/gtav-vehicle-names.txt"
OWN_LABEL_PREFIX = "GM"


def retail_models() -> set[str]:
    """Lowercase retail vehicle model names (data/vehicles/gtav-vehicle-names.txt; empty when absent)."""
    if not RETAIL_VEHICLES.exists():
        return set()
    lines = RETAIL_VEHICLES.read_text(encoding="utf-8").splitlines()
    return {line.split("\t")[0].strip().lower() for line in lines if line.strip() and (not line.startswith("#"))}


def own_game_name(text: str, model: str) -> tuple[str, str, str | None]:
    """(vehicles.meta text, the gameName key the pack labels, a note) with a gameName the pack owns.

    A pack label never replaces game text (the worker adds a label only when its key is new), and every car of a
    gameName shows that key's text: the LaFerrari mod's gameName TURISMOR is the retail Turismo R's, so its label
    row was a no-op and the LaFerrari was named "Turismo R". The mod's gameName is kept only when it is the model's
    own name (an add-on model is new, so is its name; e.g. GTR for gtr), not a retail model name, and fits the game's
    11 characters; anything else becomes GM<MODEL> (GM + the joaat hex when that is longer than 11)."""
    found = re.search("<gameName>\\s*([^<\\s]*)\\s*</gameName>", text)
    old = found.group(1) if found else ""
    if old.lower() == model and len(old) <= GAME_NAME_MAX and (model not in retail_models()):
        return (text, old, None)
    key = f"{OWN_LABEL_PREFIX}{model.upper()}"
    if len(key) > GAME_NAME_MAX:
        key = f"{OWN_LABEL_PREFIX}{joaat(model):08X}"
    if found is None:
        text, count = re.subn(
            "^([ \\t]*)<modelName>[^<]*</modelName>[ \\t]*$",
            f"\\g<0>\\n\\g<1><gameName>{key}</gameName>",
            text,
            count=1,
            flags=re.M,
        )
        if not count:
            raise ConvertError("vehicles.meta has neither <gameName> nor <modelName>")
        return (text, key, f"gameName -> {key} (none in the mod)")
    text = text[: found.start()] + f"<gameName>{key}</gameName>" + text[found.end() :]
    why = (
        "a retail model name"
        if model in retail_models() and old.lower() == model
        else (
            f"longer than the game's {GAME_NAME_MAX} characters"
            if old.lower() == model
            else "not the model's own name: it may be a game key, whose text would win"
        )
    )
    return (text, key, f"gameName {old or '(empty)'} -> {key} (pack-owned; {old or 'it'} is {why})")


def owned_parents(model: str, names: list[str]) -> dict[str, str]:
    """Pack-owned names for the txd parent dictionaries a mod ships whose name is not its own (does not contain the
    model name): the KoRn a45 ships a copy of the retail vehicles_race_generic, and a pack member of that name
    would stand beside (or over) the game's for every retail car on it. vehicles_tfdominator_interior stays."""
    return {name: f"gm{model}_{name}" for name in names if model not in name}


def own_parent_txds(vehicles: str, owned: dict[str, str]) -> tuple[str, list[str]]:
    """Rename OWNED shipped dictionaries in vehicles.meta's txdRelationships (as parent and as child); an owned
    dictionary the file gives no parent gets vehshare (the root of every retail vehicle chain; the retail copy's
    own parent row is the game's, not the mod's)."""
    notes = []
    for old, new in owned.items():
        vehicles, count = re.subn(
            f"(<(parent|child)>\\s*){re.escape(old)}(\\s*</\\2>)", f"\\g<1>{new}\\g<3>", vehicles, flags=re.I
        )
        notes.append(f"txd {old} -> {new} (pack-owned: a game dictionary may have the mod's name; {count} rows)")
        if not re.search(f"<child>\\s*{re.escape(new)}\\s*</child>", vehicles):
            row = f"    <Item>\n      <parent>vehshare</parent>\n      <child>{new}</child>\n    </Item>\n"
            vehicles, added = re.subn(
                "([ \\t]*)</txdRelationships>", lambda m, row=row: row + m.group(0), vehicles, count=1
            )
            if not added:
                raise ConvertError(f"vehicles.meta has no <txdRelationships> for the shipped dictionary {old}")
            notes.append(f"txd parent of {new}: vehshare (the mod gives none)")
    return (vehicles, notes)


def make_label_problem(key: str) -> str | None:
    """Why a --make-label KEY must not be used (None when it is pack-owned)."""
    if not key.upper().startswith(OWN_LABEL_PREFIX):
        return f"--make-label {key}: use a pack-owned key starting with {OWN_LABEL_PREFIX} (e.g. GM<MODEL>_MAKE): a game key such as GROTTI keeps the game's text (a pack label never replaces it); --make {key} points the car at an existing make"
    return None


def remap_vehicles(
    text: str, audio: str | None, defined: set[str], driveby: tuple[str, ...], make: str | None = None
) -> tuple[str, list[str]]:
    """Retail-safe vehicles.meta: tabs -> two spaces, optional audio/make swap, undefined drive-by -> stock."""
    notes = []
    text = text.replace("\t", "  ")
    if audio:
        text, count = AUDIO_NAME.subn(f"<audioNameHash>{audio}</audioNameHash>", text)
        if count == 0:
            text, count = GAME_NAME_LINE.subn(f"\\g<0>\\n\\g<1><audioNameHash>{audio}</audioNameHash>", text)
        notes.append(f"audioNameHash -> {audio} ({count} item)")
    if make:
        text, count = set_make(text, make)
        notes.append(f"vehicleMakeName -> {make} ({count} item)")

    def block(match: re.Match) -> str:
        names = ITEM.findall(match.group(2))
        if all(n in defined for n in names):
            return match.group(0)
        indent = re.match("[ ]*", match.group(2)).group(0)
        notes.append(f"firstPersonDrivebyData {','.join(names)} (undefined) -> {','.join(driveby)}")
        return match.group(1) + "\n".join(f"{indent}<Item>{n}</Item>" for n in driveby) + match.group(3)

    return (DRIVEBY.sub(block, text), notes)


PACKAGE_SUFFIXES = (".oiv", ".zip")
PACKAGE_RPF_MAX = 4 * 1024 * 1024 * 1024
PACKAGE_MEMBERS_MAX = 4096


def package_rpf(source: Path) -> str:
    """The one add-on dlc.rpf member of an .oiv/.zip package (OIV: content/dlc*/dlc.rpf), checked only: an
    update.rpf edit or a second add-on is refused (convert one dlc.rpf at a time)."""
    try:
        package = zipfile.ZipFile(source)
    except (zipfile.BadZipFile, OSError) as error:
        raise ConvertError(f"{source.name}: not a readable .oiv/.zip package ({error})") from None
    with package:
        infos = package.infolist()
        if len(infos) > PACKAGE_MEMBERS_MAX:
            raise ConvertError(f"{source.name}: more than {PACKAGE_MEMBERS_MAX} entries")
        hits = [
            i
            for i in infos
            if not i.is_dir() and PurePosixPath(i.filename.replace("\\", "/")).name.lower() == "dlc.rpf"
        ]
        if len(hits) != 1:
            names = ", ".join(i.filename for i in hits) or "none"
            raise ConvertError(f"{source.name}: expected exactly one dlc.rpf in the package, found {names}")
        if hits[0].file_size > PACKAGE_RPF_MAX:
            raise ConvertError(f"{source.name}:{hits[0].filename}: larger than {PACKAGE_RPF_MAX} bytes")
        return hits[0].filename


def unpack_package(source: Path, target: Path) -> Path:
    """Stream the package's dlc.rpf (package_rpf) to TARGET; returns TARGET."""
    member = package_rpf(source)
    target.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(source) as package, package.open(member) as stream, target.open("xb") as sink:
        shutil.copyfileobj(stream, sink, 1 << 20)
    return target


RETAIL_AUDIO = "retail-audio/game.dat151.rel"
SOUND_CLASSES = {3: "car", 8: "helicopter", 16: "boat", 56: "bicycle", 57: "plane"}
TYPE_CLASS = {
    "VEHICLE_TYPE_HELI": 8,
    "VEHICLE_TYPE_BLIMP": 8,
    "VEHICLE_TYPE_BOAT": 16,
    "VEHICLE_TYPE_BICYCLE": 56,
    "VEHICLE_TYPE_PLANE": 57,
}
CLASS_SOUNDS = {
    "VC_COMPACT": "BLISTA",
    "VC_SEDAN": "SCHAFTER2",
    "VC_SUV": "BALLER",
    "VC_COUPE": "FELON",
    "VC_MUSCLE": "DOMINATOR",
    "VC_SPORT_CLASSIC": "MONROE",
    "VC_SPORT": "BANSHEE",
    "VC_SUPER": "ADDER",
    "VC_MOTORCYCLE": "BATI",
    "VC_OFF_ROAD": "SANDKING",
    "VC_INDUSTRIAL": "PHANTOM",
    "VC_UTILITY": "UTILLITRUCK",
    "VC_VAN": "SPEEDO",
    "VC_SERVICE": "BUS",
    "VC_EMERGENCY": "POLICE",
    "VC_MILITARY": "BARRACKS",
    "VC_COMMERCIAL": "MULE",
    "VC_CYCLE": "BMX",
    "VC_BOAT": "SQUALO",
    "VC_HELICOPTER": "MAVERICK",
    "VC_PLANE": "CUBAN800",
}
TYPE_SOUNDS = {
    "VEHICLE_TYPE_BIKE": "BATI",
    "VEHICLE_TYPE_QUADBIKE": "BLAZER",
    "VEHICLE_TYPE_BICYCLE": "BMX",
    "VEHICLE_TYPE_BOAT": "SQUALO",
    "VEHICLE_TYPE_HELI": "MAVERICK",
    "VEHICLE_TYPE_PLANE": "CUBAN800",
}
ELECTRIC_SOUND = "VOLTIC"
DEFAULT_SOUND = "SCHAFTER2"
AUDIO_NAME = re.compile("<audioNameHash>\\s*([^<]*?)\\s*</audioNameHash>|<audioNameHash\\s*/>")


@dataclass
class AudioChoice:
    name: str | None
    line: str
    donor: str | None = None


def retail_sounds(args) -> dict[int, int] | None:
    """{name hash: dat151 class} of the vehicle sounds in the base game data of the template cache, or None when
    the cache has no verified RETAIL_AUDIO (it comes from the menu export: ./menu-ctl.sh export-templates)."""
    if getattr(args, "templates", None) is None:
        return None
    try:
        _, rows = retail_templates.load_manifest()
        row = next(r for r in rows if r.cache == RETAIL_AUDIO)
    except (retail_templates.TemplateError, StopIteration):
        return None
    if not retail_templates.cached_ok(args.templates, row):
        return None
    from gtavmenu_tools import audio_rel

    chunk = audio_rel.parse((args.templates / RETAIL_AUDIO).read_bytes())
    return {h: chunk.data[at] for h, at, _ in chunk.index if chunk.data[at] in SOUND_CLASSES}


def class_sound(vehicles: str) -> tuple[str, str]:
    """(base-game sound, why) for a vehicles.meta item: its <type>, electric flag, then <vehicleClass>."""
    kind = (re.search("<type>\\s*([^<\\s]+)\\s*</type>", vehicles) or [None, ""])[1]
    flags = (re.search("<flags>([^<]*)</flags>", vehicles) or [None, ""])[1].split()
    vclass = (re.search("<vehicleClass>\\s*([^<\\s]+)\\s*</vehicleClass>", vehicles) or [None, ""])[1]
    if kind in TYPE_SOUNDS:
        return (TYPE_SOUNDS[kind], kind)
    if "FLAG_IS_ELECTRIC" in flags:
        return (ELECTRIC_SOUND, "FLAG_IS_ELECTRIC")
    if vclass in CLASS_SOUNDS:
        return (CLASS_SOUNDS[vclass], vclass)
    return (DEFAULT_SOUND, f"class {vclass or 'unknown'}")


def mod_ships_audio(fixture: Path | None) -> bool:
    """Whether the mod's content.xml loads audio data of its own (AUDIO_GAMEDATA, AUDIO_WAVEPACK, ...)."""
    content = None if fixture is None else fixture / "metadata/content.xml"
    if content is None or not content.exists():
        return False
    root = asset_metadata.parse_xml(read_meta(content).encode("utf-8"), Limits())
    return any(item.findtext("fileType", "").strip().startswith("AUDIO_") for item in root.findall("./dataFiles/Item"))


def pick_audio(args, vehicles: str, model: str, own_audio: bool, sounds: dict[int, int] | None) -> AudioChoice:
    """The vehicles.meta audioNameHash of a converted car and its one-line report.

    --audio NAME is used when the base game data has it (of the kind the vehicle type needs), refused otherwise
    unless --audio-unchecked (a DLC car's sound cannot be checked); without the list it is used unchecked.
    Without --audio the mod's own name stays when it is a base-game sound, or another car's name (a DLC car's
    sound works; it cannot be checked); a name that is the model itself, empty (the engine then tries the model
    name) or a sound the mod ships itself (its banks are not converted) is replaced by the class table's pick."""
    from gtavmenu_tools import audio_rel

    kind = (re.search("<type>\\s*([^<\\s]+)\\s*</type>", vehicles) or [None, ""])[1]
    want = TYPE_CLASS.get(kind, audio_rel.CAR)
    pick, why = class_sound(vehicles)
    hint = "./menu-ctl.sh export-templates fetches your game's base car sounds"
    unchecked = f"not checked: no {RETAIL_AUDIO} in the template cache ({hint})"

    def known(name: str) -> bool:
        return sounds is not None and sounds.get(audio_rel.name_hash(name)) == want

    if args.audio:
        if sounds is None:
            return AudioChoice(args.audio, f"{args.audio} (--audio; {unchecked})", args.audio)
        if known(args.audio):
            return AudioChoice(
                args.audio, f"{args.audio} (--audio; a {SOUND_CLASSES[want]} sound of your game)", args.audio
            )
        if args.audio_unchecked:
            return AudioChoice(args.audio, f"{args.audio} (--audio --audio-unchecked: not a base-game sound)")
        raise ConvertError(
            f"--audio {args.audio}: not a base-game {SOUND_CLASSES[want]} sound in your game's audio data ({RETAIL_AUDIO}); e.g. --audio {pick} ({why}), or add --audio-unchecked for a DLC car's sound"
        )
    match = AUDIO_NAME.search(vehicles)
    mine = (match.group(1) or "").strip() if match else ""
    if mine and known(mine):
        return AudioChoice(None, f"{mine} (the mod's; a {SOUND_CLASSES[want]} sound of your game)", mine)
    if not mine or mine.lower() == model.lower() or own_audio:
        reason = (
            "no audioNameHash (the engine would try the model name)"
            if not mine
            else f"{mine} is the mod's own engine sound ({('its audio banks are' if own_audio else 'no retail car is')} not converted)"
        )
        state = "" if sounds is not None else f"; {unchecked}"
        return AudioChoice(pick, f"{pick} ({reason} -> the {why} pick; --audio NAME to choose{state})", pick)
    if sounds is None:
        return AudioChoice(None, f"{mine} (the mod's; {unchecked})")
    return AudioChoice(
        None,
        f"{mine} (the mod's; not a base-game sound: a DLC car's sound cannot be checked; if the car has no engine sound in game, convert with --audio {pick})",
    )


def own_sound(plan: Plan, args, stage: Path, model: str, choice: AudioChoice, sounds: dict | None):
    """--audio-engine NAME: the car's sound (a base-game CarAudioSettings) cloned under a pack-owned name with NAME's
    engine and granular engine (tools/make_audio_gamedata.py car; AUDIO_GAMEDATA row). Returns
    (the new audioNameHash, the <chunk>_game.rel path)."""
    from gtavmenu_tools import audio_rel

    if sounds is None:
        raise ConvertError(
            f"--audio-engine clones a base-game sound: the template cache has no {RETAIL_AUDIO} (./menu-ctl.sh export-templates fetches it)"
        )
    for name, what in ((choice.donor, "the car's sound"), (args.audio_engine, "--audio-engine")):
        if name is None or sounds.get(audio_rel.name_hash(name)) != audio_rel.CAR:
            raise ConvertError(
                f"--audio-engine: {what} {name or '(none)'} is not a base-game car sound in your game's audio data; name one with --audio NAME / --audio-engine NAME (e.g. BANSHEE, SANCHEZ)"
            )
    slug = re.sub("[^a-z0-9]", "", model.lower())
    own = f"GMAUD_{slug.upper()}"[:NAME_MAX]
    path = stage / f"{('gmaud' + slug)[:31]}_game.rel"
    command = py(
        "tools/make_audio_gamedata.py",
        "car",
        "--source",
        args.templates / RETAIL_AUDIO,
        "--donor",
        choice.donor,
        "--name",
        own,
        "--engine-from",
        args.audio_engine,
        "--output",
        path,
    )
    plan.run(f"own car sound ({choice.donor} with the {args.audio_engine} engine)", command)
    print(
        f"     audio: {own} = {choice.donor} with the engine of {args.audio_engine} (own CarAudioSettings)", flush=True
    )
    return (own, path)


def fixture_member(fixture: Path, member: str) -> Path:
    """'x64/vehicles.rpf!/a80.ytd' -> <fixture>/resources/x64/vehicles.rpf/a80.ytd (import layout)."""
    try:
        return resource_path(fixture, member)
    except FixtureError as error:
        raise ConvertError(str(error)) from error


def fixture_manifest(fixture: Path) -> dict:
    """Validate the whole supplied fixture before interpreting any member name."""
    try:
        return validate_fixture(fixture)
    except (FixtureError, OSError) as error:
        raise ConvertError(str(error)) from error


def fixture_members(fixture: Path, model: str, dry_run: bool) -> dict[str, list[str]]:
    """Base/hi/texture/part member names of an imported fixture (from its manifest).

    Parts are every other .yft the import selected: tools/import_pc_assets.py --mod-kit takes
    vehiclemods/<model>_*.yft and any .yft the car's kits list (visibleMods, linkMods) in whatever archive
    holds it (KoRn: x64/<car>_mods.rpf; vans123: vehiclemods/tuning_mods.rpf)."""
    manifest_path = fixture / "manifest.json"
    if dry_run and (not manifest_path.exists()):
        base = "x64/vehicles.rpf!/"
        parts = [f"x64/vehiclemods/{model}_mods.rpf!/{model}_<part>.yft"]
        return {
            "base": [f"{base}{model}.yft", f"{base}{model}_hi.yft"],
            "texture": [f"{base}{model}.ytd"],
            "parts": parts,
        }
    names = sorted(row["member"] for row in fixture_manifest(fixture)["resources"])
    base = [n for n in names if n.lower().endswith((f"!/{model}.yft", f"!/{model}_hi.yft"))]
    texture = [n for n in names if n.lower().endswith(f"!/{model}.ytd")]
    pick = {
        "base": base,
        "texture": texture,
        "parts": [n for n in names if n.lower().endswith(".yft") and n not in base],
        "parents": [n for n in names if n.lower().endswith(".ytd") and n not in texture],
    }
    if not pick["base"] or len(pick["texture"]) != 1:
        raise ConvertError(f"{fixture}: no {model}.yft / single {model}.ytd in the fixture manifest")
    return pick


EMBEDDED_SIDECAR = "embedded-textures"


def embedded_members(fixture: Path, dry_run: bool) -> list[str]:
    """The fixture's .yft members with graphics pages (an embedded texture dictionary; RSC7 headers only)."""
    manifest_path = fixture / "manifest.json"
    if dry_run and (not manifest_path.exists()):
        return []
    found = []
    for row in fixture_manifest(fixture)["resources"]:
        if row["member"].lower().endswith(".yft"):
            with fixture_member(fixture, row["member"]).open("rb") as handle:
                head = handle.read(16)
            if len(head) == 16 and head[:4] == b"RSC7" and page_bytes(int.from_bytes(head[12:16], "little")):
                found.append(row["member"])
    return found


UNCOVERED = re.compile("native shader corpus does not cover every source shader type \\(name hashes ([0-9a-fx, ]+)\\)")


def uncovered_parts(error: str, report: Path, parts: list[str]) -> list[str]:
    """The PARTS whose shaders (material REPORT) use a shader type the convert step's ERROR names as missing from
    the corpus (the Dominator GTX's tfdom_interior1: vehicle_emissive_opaque); [] for any other refusal."""
    found = UNCOVERED.search(error)
    if not found:
        return []
    missing = {int(h, 16) for h in re.findall("0x[0-9a-f]+", found.group(1))}
    rows = json.loads(report.read_bytes())["resources"]
    return [
        row["member"]
        for row in rows
        if row["member"] in parts and any(int(sh["nameHash"], 16) in missing for sh in row["shaders"])
    ]


def embedded_lines(fixture: Path) -> list[str]:
    """`parts:` lines for the parts repair_pc_embedded_textures.py left out (malformed dictionaries) and one
    `textures:` line for the names it renamed (a texture of the same name and other pixels was there first)."""
    record = json.loads((fixture / "manifest.json").read_bytes()).get("embeddedTextureRepairs", {})
    lines = [
        f"parts: {row['member'].rpartition('!/')[2]} left out ({row['reason']})" for row in record.get("leftOut", [])
    ]
    renames = [
        f"{x['texture']} -> {x['name']} ({row['member'].rpartition('!/')[2]})"
        for row in record.get("resources", [])
        for x in row.get("renames", [])
    ]
    if renames:
        lines.append(
            f"textures: {len(renames)} part texture(s) renamed (another texture has the name): " + ", ".join(renames)
        )
    return lines


def embedded_sources(fixture: Path) -> list[Path]:
    """The original fragments repair_pc_embedded_textures.py kept (their textures go to the car's .ptd)."""
    return (
        sorted(p for p in (fixture / EMBEDDED_SIDECAR).rglob("*.yft")) if (fixture / EMBEDDED_SIDECAR).is_dir() else []
    )


FRAGMENT_DRAWABLE = 48
DRAWABLE_MODEL_LISTS = (80, 88, 96, 104)
MODEL_HAS_SKIN = 41


def model_skins(payload: bytes) -> list[int]:
    """The HasSkin byte of every model of a Legacy fragment payload's main drawable (IndexError when a pointer
    leaves the payload)."""
    import struct

    def at(pointer: int, size: int) -> int:
        offset = pointer - 1342177280
        if not 0 <= offset <= len(payload) - size:
            raise IndexError(pointer)
        return offset

    def u64(pointer: int) -> int:
        return struct.unpack_from("<Q", payload, at(pointer, 8))[0]

    drawable, skins = (u64(1342177280 + FRAGMENT_DRAWABLE), [])
    for slot in DRAWABLE_MODEL_LISTS:
        models = u64(drawable + slot)
        if models:
            array, count = struct.unpack_from("<QH", payload, at(models, 10))
            skins += [payload[at(u64(array + 8 * index) + MODEL_HAS_SKIN, 1)] for index in range(count)]
    return skins


def rigid_parts(fixture: Path, parts: list[str]) -> list[str]:
    """Parts with a model that is not skinned (SkeletonBinding HasSkin 0): the PC vehicle route reads skinned
    geometry only (inspect_pc_skinning_bindings: "lacks the verified skin-model binding"). The vans123 aventador's
    avnt_wg2_bd2, avnt_wg2_carb and avnt_wg_race are rigid. A part this walk cannot read stays (the converter
    judges it)."""
    from gtavmenu_tools.asset_formats import AssetError, decode_resource

    rigid = []
    for member in parts:
        try:
            skins = model_skins(decode_resource(fixture_member(fixture, member).read_bytes(), 1 << 30)[1])
        except (AssetError, IndexError, OSError):
            continue
        if 0 in skins:
            rigid.append(member)
    return rigid


def repair_chain(plan: Plan, args, fixture: Path, specs: list[str], members, texture, work: Path, tag: str) -> Path:
    for index, spec in enumerate(specs):
        name, _, bones = spec.partition("=")
        if name not in REPAIRS:
            raise ConvertError(f"unknown repair {name!r} (known: {', '.join(REPAIRS)})")
        tool, takes_bones, _ = REPAIRS[name]
        if takes_bones != bool(bones):
            raise ConvertError(f"--repair {spec}: {('needs' if takes_bones else 'takes no')} =BONE,... list")
        report = work / f"{tag}-repair{index}-materials.json"
        material_report(plan, args, fixture, members, texture, report)
        output = work / "fixtures" / f"{tag}-repair{index}-{name}"
        command = py(tool, "--fixture", fixture, "--material-report", report, "--output", output)
        for bone in filter(None, bones.split(",")):
            command += ["--bone", bone]
        if args.templates is not None and name in TEMPLATE_REPAIRS:
            command += ["--templates", staged_templates(args)]
        plan.run(f"repair {name}", command)
        fixture = output
    return fixture


LOWER_LODS = ("auto", "copy", "empty")


def lod_fill(plan: Plan, partial: Path, output: Path, deep_copy: bool, lower: str = "auto") -> None:
    """Fill the null medium/low/very-low lists: deep copies of the high list for the base fragment (`auto`: copies,
    own empty collections past the 64 MiB / 128-page bounds; `copy`: refuse there; `empty`: always empty, the
    accepted resource shape), own empty collections for _hi fragments and parts (deep_copy False)."""
    deep_copy = deep_copy and lower != "empty"
    tool = "copy_drawable_lods.py" if deep_copy else "fill_drawable_lods.py"
    label = "LOD lists (deep copies)" if deep_copy else "LOD lists (own empty collections)"
    command = py(tool, "--input", partial, "--output", output)
    plan.run(label, command + (["--no-fallback"] if deep_copy and lower == "copy" else []))


def model_split(plan: Plan, partial: Path, output: Path) -> Path:
    """Models over 127 geometries split into models of at most 127 (the engine's draw lists keep the geometry
    index in a signed byte); any other resource is written byte-identical."""
    plan.run(
        "model split (<= 127 geometries per model)",
        py("native_drawable_model_split.py", "--input", partial, "--output", output),
    )
    return output


def livery_texture(plan: Plan, args, partial: Path, original: Path, output: Path) -> Path:
    """A VMT_LIVERY_MOD part gets its own one-texture dictionary back (the engine binds entry 0 of the livery
    part's embedded dictionary as the car's livery texture; repair_pc_embedded_textures.py moved it out)."""
    command = py("native_drawable_part_textures.py", "--input", partial, "--embedded", original, "--output", output)
    command += ["--max-size", str(args.part_texture_size)] if args.part_texture_size else []
    command += ["--templates", staged_templates(args)] if args.templates is not None else []
    plan.run("livery texture (the part's own dictionary)", command)
    return output


KIT_ITEM = re.compile("<Item\\b[^>]*?/>|<Item\\b[^>]*>|</Item>")


def livery_parts(fixture: Path) -> set[str]:
    """Lower-case modelNames of the mod carcols' VMT_LIVERY_MOD visible mods (none without a carcols)."""
    found, _ = mod_metas(fixture)
    if "carcols" not in found:
        return set()
    names = set()
    for block in re.finditer("<visibleMods>(.*?)</visibleMods>", read_meta(found["carcols"]), re.S):
        body, depth, start = (block.group(1), 0, 0)
        for tag in KIT_ITEM.finditer(body):
            if tag.group(0).endswith("/>"):
                continue
            if tag.group(0).startswith("</"):
                depth -= 1
                if depth == 0:
                    item = body[start : tag.end()]
                    kind, model = (re.search("<type>\\s*(\\w+)", item), re.search("<modelName>\\s*([^<\\s]+)", item))
                    if kind and model and (kind.group(1) == "VMT_LIVERY_MOD"):
                        names.add(model.group(1).lower())
            else:
                if depth == 0:
                    start = tag.start()
                depth += 1
    return names


def octants(plan: Plan, pairs: list[tuple[Path, Path]]) -> None:
    """Fill empty collision octants and verify bounded nonempty vertex lists."""
    command = py("native_octants.py", "repair")
    for source, target in pairs:
        command += ["--member", f"{source}={target}"]
    plan.run("collision octants", command)


def build(args) -> int:
    model, plan = (args.model, Plan(args.dry_run))
    try:
        preflight(args)
    except ConvertError as error:
        if not args.dry_run:
            raise
        print(f"warning (dry run): {error}", file=sys.stderr)
    work = assets_dir(ROOT) / f"convert-{args.id}"
    stage = work / "pack-inputs"
    if not args.dry_run and (work.exists() or work.is_symlink()):
        raise ConvertError(f"{rel(work)} exists; choose a new --id or remove it")
    if not re.fullmatch("[a-z0-9_]+", model):
        raise ConvertError("--model must be the lowercase model name (e.g. a80)")
    if args.templates is not None:
        plan.steps.append("stage retail templates")
        shown = f"{rel(args.templates)}\n     write {rel(staged_templates(args))}"
        print(f"[{len(plan.steps):02}] stage retail templates from {shown}")
        if not args.dry_run:
            stage_templates(args)
    if args.source is not None and args.source.name.lower().endswith(PACKAGE_SUFFIXES):
        member = package_rpf(args.source)
        target = work / "package" / "dlc.rpf"
        plan.steps.append("unpack package")
        print(f"[{len(plan.steps):02}] unpack {member} from {args.source.name}\n     write {rel(target)}", flush=True)
        if not args.dry_run:
            args.source = unpack_package(args.source, target)
    fixture = args.fixture
    if fixture is None:
        fixture = work / "fixtures" / f"{model}-import"
        command = py("tools/import_pc_assets.py", "vehicle", args.source, "--model", model, "--output-dir", fixture)
        plan.run("import PC add-on", [*command, *(["--mod-kit"] if args.mod_kit else [])])
    members = fixture_members(fixture, model, args.dry_run)
    texture = members["texture"][0]
    if (fixture / "metadata").exists():
        found, _ = mod_metas(fixture)
        single = "vehicles" in found and len(MODEL_NAME.findall(read_meta(found["vehicles"]))) == 1
        if "carcols" in found and single:
            check_wheels(args, read_meta(found["carcols"]), report=False)
    plan.auto = {"--repair": "auto" in args.repair, "--parts-repair": "auto" in args.parts_repair}
    if embedded_members(fixture, args.dry_run):
        output = work / "fixtures" / f"{model}-embedded-textures"
        command = py("repair_pc_embedded_textures.py", "--fixture", fixture, "--output", output, "--model", model)
        command += ["--shared-dictionary", reference(args, VEHSHARE)]
        plan.run("embedded textures (fragment textures -> the car's .ptd)", command)
        fixture = output
        if not args.dry_run:
            for line in embedded_lines(fixture):
                print(f"     {line}", flush=True)
    fixture = repair_chain(plan, args, fixture, args.repair, members["base"], texture, work, "base")
    parts_fixture = args.parts_fixture or fixture
    parts = fixture_members(parts_fixture, model, args.dry_run)["parts"] if args.mod_kit or args.parts_fixture else []
    if (args.mod_kit or args.parts_fixture) and (not parts) and (not args.dry_run):
        raise ConvertError(
            "no mod-kit parts in the fixture (vehiclemods/<model>_*.yft or kit-listed .yft; import with --mod-kit)"
        )
    rigid = [] if args.dry_run else rigid_parts(parts_fixture, parts)
    if rigid:
        parts = [p for p in parts if p not in rigid]
        names = ", ".join(Path(p.split("!/")[-1]).stem for p in rigid)
        print(f"     parts: {len(rigid)} left out (rigid, unskinned models; not converted yet): {names}", flush=True)
        if not parts:
            raise ConvertError("every mod-kit part has rigid (unskinned) models; convert without --mod-kit")
    if parts:
        plan.repair_flag = "--parts-repair"
        parts_fixture = repair_chain(plan, args, parts_fixture, args.parts_repair, parts, texture, work, "parts")
        plan.repair_flag = "--repair"
    if not args.dry_run:
        stage.mkdir(parents=True, exist_ok=True)
        (work / "lods").mkdir(parents=True, exist_ok=True)
    members_out: list[tuple[str, Path]] = []
    report = base_report = work / "base-materials.json"
    material_report(plan, args, fixture, members["base"], texture, report)
    convert(plan, args, fixture, report, work / "base-references", not args.single_page)
    pairs = []
    for member in members["base"]:
        source = Path(member.split("!/")[-1]).stem
        stem = source.lower()
        out = stage / f"{stem}.pft"
        lods = work / "lods" / f"{stem}.pft"
        partial = model_split(
            plan, work / "base-references" / f"{source}.partial.pft", work / "split" / f"{source}.partial.pft"
        )
        lod_fill(plan, partial, lods, deep_copy=not stem.endswith("_hi"), lower=args.lower_lods)
        pairs.append((lods, out))
        members_out.append((f"{stem}.pft", out))
    octants(plan, pairs)
    part_names = []
    if parts:
        plan.repair_flag = "--parts-repair"
        report = work / "parts-materials.json"
        material_report(plan, args, parts_fixture, parts, texture, report)
        try:
            convert(plan, args, parts_fixture, report, work / "parts-references", args.parts_multi_page)
        except ConvertError as error:
            unknown = uncovered_parts(str(error), report, parts)
            if not unknown or len(unknown) == len(parts):
                raise
            parts = [p for p in parts if p not in unknown]
            names = ", ".join(Path(p.split("!/")[-1]).stem for p in unknown)
            print(f"     parts: {len(unknown)} left out (a shader type the converter has no reference for): {names}")
            report = work / "parts-materials-kept.json"
            material_report(plan, args, parts_fixture, parts, texture, report)
            convert(plan, args, parts_fixture, report, work / "parts-references-kept", args.parts_multi_page)
        references = work / (
            "parts-references-kept" if report.name == "parts-materials-kept.json" else "parts-references"
        )
        pairs = []
        liveries = set() if args.dry_run else livery_parts(parts_fixture)
        originals = {p.stem.lower(): p for p in embedded_sources(parts_fixture)}
        for member in parts:
            source = Path(member.split("!/")[-1]).stem
            stem = source.lower()
            lods = work / "lods" / f"{stem}.pft"
            partial = model_split(plan, references / f"{source}.partial.pft", work / "split" / f"{source}.partial.pft")
            if stem in liveries and stem in originals:
                partial = livery_texture(
                    plan, args, partial, originals[stem], work / "liveries" / f"{source}.partial.pft"
                )
            elif stem in liveries:
                print(f"     parts: {stem} is a livery part without its own texture (the livery shows nothing)")
            lod_fill(plan, partial, lods, deep_copy=False)
            pairs.append((lods, stage / f"{stem}.pft"))
            part_names.append(stem)
        octants(plan, pairs)
        plan.repair_flag = "--repair"
    ytd = fixture_member(fixture, texture)
    command = py("convert_pc_ytd_writer.py", "--ytd", ytd, "--output", stage / f"{model}.ptd")
    command += ["--max-size", str(args.max_texture_size)] if args.max_texture_size else []
    kept = {m.split("!/")[-1].lower() for m in [*members["base"], *parts]}
    embedded = [p for p in embedded_sources(parts_fixture if parts else fixture) if p.name.lower() in kept]
    for source in embedded:
        command += ["--embedded", source]
    if embedded and args.part_texture_size:
        command += ["--embedded-max-size", str(args.part_texture_size)]
    plan.run("textures", command + (["--templates", staged_templates(args)] if args.templates is not None else []))
    members_out.append((f"{model}.ptd", stage / f"{model}.ptd"))
    stems = [Path(parent.split("!/")[-1]).stem.lower() for parent in members.get("parents", [])]
    owned = owned_parents(model, stems)
    for parent in members.get("parents", []):
        name = Path(parent.split("!/")[-1]).stem.lower()
        name = owned.get(name, name)
        ptd = stage / f"{name}.ptd"
        command = py("convert_pc_ytd_writer.py", "--ytd", fixture_member(fixture, parent), "--output", ptd)
        command += ["--max-size", str(args.max_texture_size)] if args.max_texture_size else []
        plan.run(f"textures ({name})", command + (["--templates", staged_templates(args)] if args.templates else []))
        members_out.append((f"{name}.ptd", ptd))
    members_out += [(f"{p}.pft", stage / f"{p}.pft") for p in part_names]
    spec = load_siren_spec(args)
    if spec is not None and args.siren_lamps is None:
        base = next(m for m in members["base"] if m.lower().endswith(f"!/{model}.yft"))
        found = plan.query(
            "siren lamps (siren bones of the skeleton)",
            f"{rel(fixture)} {base}",
            SIREN_BONES,
            str(fixture),
            str(base_report),
            base,
        )
        if not args.dry_run:
            args.siren_lamps = siren_lamps(spec, found)
            where = ", ".join(f"siren{n} x={found[n][0]:+.2f}" for n in sorted(found, key=int)) or "none"
            print(f"     sirens: lamps {args.siren_lamps} ({where})", flush=True)
            if set(args.siren_lamps) == {"-"}:
                raise ConvertError("the car has no siren1..siren20 bones: a siren pattern would light nothing")
    data, labels = metas(plan, args, fixture, stage, part_names)
    if not args.dry_run:
        carcols = dict(data).get("CARCOLS_FILE")
        for line in livery_lines(base_report, stage / f"{model}.ptd", carcols, labels):
            print(f"     liveries: {line}", flush=True)
    command = py("build_runtime_pack.py", "--id", args.id, "--archive", args.archive or f"gm{model}.rpf")
    for name, path in members_out:
        command += ["--member", f"{name}={path}"]
    if part_names:
        archive = args.parts_archive or f"gm{model}parts.rpf"
        command += ["--extra-archive", f"{archive}=" + ",".join(f"{p}.pft" for p in part_names)]
    for type_name, path in data:
        command += ["--data", f"{type_name}={path}"]
    for label in labels:
        command += ["--label", label]
    command += ["--spawn", f"vehicle:{model}={args.name}", "--output-root", args.output_root]
    command += ["--plain-toc"]
    plan.run("pack", command)
    print(f"{('planned' if args.dry_run else 'done')}: {len(plan.steps)} steps; pack {rel(args.output_root / args.id)}")
    return 0


def mod_metas(fixture: Path) -> tuple[dict[str, Path], list[str]]:
    """The mod's data files by DATA_ORDER key, as its content.xml declares them; returns (paths, notes).

    content.xml <dataFiles> rows name each file as `dlc_<name>:/<path>` with a <fileType>; the import
    keeps every root .meta/.xml member under metadata/<path>. Add-ons lay these out freely (a80:
    data/vehicles.meta; others common/data/levels/gta5/vehicles.meta), so the rows decide. A row whose
    file the archive does not hold is left out with a note (the PC game skips it too). Without a
    content.xml, or for a type it does not list, the a80 layout under metadata/data/ is used.
    """
    base = fixture / "metadata"
    paths = {key: base / "data" / name for key, name in DEFAULT_METAS.items()}
    notes: list[str] = []
    content = base / "content.xml"
    if not content.exists():
        return ({key: path for key, path in paths.items() if path.exists()}, notes)
    root = asset_metadata.parse_xml(read_meta(content).encode("utf-8"), Limits())
    types = dict(DATA_ORDER)
    declared: dict[str, list[str]] = {}
    for item in root.findall("./dataFiles/Item"):
        kind, name = (item.findtext(tag, "").strip() for tag in ("fileType", "filename"))
        if kind in types:
            declared.setdefault(types[kind], []).append(name)
    for key, names in declared.items():
        if len(names) > 1:
            raise ConvertError(f"content.xml lists {len(names)} {key} files ({', '.join(names)}); expected one")
        device, sep, relative = names[0].partition(":/")
        rel = PurePosixPath(relative)
        parts = rel.parts
        if (
            not sep
            or not device
            or rel.is_absolute()
            or (not parts)
            or any(part in ("", ".", "..") or "/" in part or "\\" in part for part in parts)
        ):
            raise ConvertError(f"content.xml {key} file {names[0]!r} is not dlc_<name>:/<path>")
        candidate = base.joinpath(*parts).resolve()
        if not candidate.is_relative_to(base.resolve()):
            raise ConvertError(f"content.xml {key} file {names[0]!r} leaves the mod archive")
        paths[key] = candidate
        if not paths[key].exists():
            notes.append(f"content.xml lists {names[0]} ({key}) but the archive has no such file: skipped")
    return ({key: path for key, path in paths.items() if path.exists()}, notes)


def metas(plan: Plan, args, fixture: Path, stage: Path, parts: list[str]) -> tuple[list[tuple[str, Path]], list[str]]:
    model = args.model
    out = {key: stage / f"{model}_{key}.meta" for _, key in DATA_ORDER}
    probe = fixture if (fixture / "metadata").exists() or not args.dry_run else args.fixture
    if probe is None or not (probe / "metadata").exists():
        source = {key: fixture / "metadata/data" / name for key, name in DEFAULT_METAS.items()}
    else:
        found, notes = mod_metas(probe)
        source = {key: fixture / path.relative_to(probe) for key, path in found.items()}
        for note in notes:
            print(f"     metas: {note}")
    have = {key: key in source for _, key in DATA_ORDER}
    if not (have["vehicles"] and have["carvariations_kit"]):
        raise ConvertError(f"{rel(fixture / 'metadata')}: vehicles and carvariations metas are required")
    game_name, audio_row = model, None
    if not args.dry_run:
        if have["handling"]:
            plan.write("handling.meta", out["handling"], read_meta(source["handling"]))
        vehicles = read_meta(source["vehicles"])
        defined = set()
        for path in (args.retail_layouts, source.get("layouts")):
            if path and path.exists():
                defined |= set(NAME.findall(read_meta(path)))
        sys.path.insert(0, str(TOOLS))
        from prepare_vehicle_layouts import default_driveby, reparent_shipped_txds

        bike = default_driveby(vehicles)
        driveby = tuple(args.driveby or bike or DEFAULT_DRIVEBY)
        sounds = retail_sounds(args)
        choice = pick_audio(args, vehicles, model, mod_ships_audio(probe), sounds)
        print(f"     audio: {choice.line}", flush=True)
        if args.audio_engine:
            choice.name, audio_row = own_sound(plan, args, stage, model, choice, sounds)
        vehicles, notes = remap_vehicles(vehicles, choice.name, defined, driveby, make_key(args))
        vehicles, game_name, note = own_game_name(vehicles, model)
        notes += [note] if note else []
        shipped = [Path(n.split("!/")[-1]).stem.lower() for n in fixture_members(fixture, model, False)["parents"]]
        owned = owned_parents(model, shipped)
        vehicles, more = own_parent_txds(vehicles, owned)
        notes += more
        vehicles, more = reparent_shipped_txds(vehicles, {owned.get(n, n) for n in shipped})
        notes += more
        for note in notes:
            print(f"     vehicles.meta: {note}")
        target = stage / "vehicles-remapped.meta" if have["layouts"] else out["vehicles"]
        plan.write("vehicles.meta remaps", target, vehicles)
    else:
        plan.steps.append("vehicles.meta remaps")
        print(
            f"[{len(plan.steps):02}] handling.meta + vehicles.meta remaps (audio, drive-by, make)\n     write {rel(stage)}"
        )
    if have["layouts"]:
        if not args.retail_layouts:
            raise ConvertError(retail_layouts_error(args))
        command = py(
            "prepare_vehicle_layouts.py",
            "--model",
            model,
            "--layouts",
            source["layouts"],
            "--vehicles",
            stage / "vehicles-remapped.meta",
            "--retail",
            args.retail_layouts,
            "--out-layouts",
            out["layouts"],
            "--out-vehicles",
            out["vehicles"],
        )
        plan.run("vehicle layouts", command)
    labels = [f"{game_name}={args.name}"]
    labels_file = stage / f"{model}_shop_labels.txt"
    if have["carcols"]:
        carcols = None if args.dry_run else read_meta(source["carcols"])
        if carcols is not None:
            check_wheels(args, carcols)
        pick_ids(args, carcols)
        command = py(
            "prepare_carcols.py",
            "--carcols",
            source["carcols"],
            "--variations",
            source["carvariations_kit"],
            "--kit-id",
            str(args.kit_id),
            "--light-id",
            str(args.light_id),
        )
        if args.siren_preset is not None or args.siren_spec is not None:
            pattern = ["--siren-preset", args.siren_preset] if args.siren_preset else ["--siren-spec", args.siren_spec]
            command += [*pattern, "--siren-id", str(args.siren_id)]
            command += ["--siren-lamps", args.siren_lamps] if args.siren_lamps else []
        elif args.siren_id is not None:
            command += ["--keep-sirens", "--siren-id", str(args.siren_id)]
        for part in parts:
            command += ["--ship-model", part]
        if args.census:
            command += ["--census", *args.census]
        prefix = args.shop_label_prefix or f"GM{model.upper()}"
        text = mod_text(plan, args, stage.parent / "text")
        if parts or text:
            command += ["--shop-label-prefix", prefix, "--out-labels", labels_file]
        if text:
            command += ["--gxt2", text]
        command += ["--out-carcols", out["carcols"], "--out-variations", out["carvariations_kit"]]
        plan.run("carcols + kit", command)
    elif args.siren_id is not None or args.siren_preset is not None or args.siren_spec is not None:
        raise ConvertError("--siren-id/--siren-preset/--siren-spec: the mod ships no carcols.meta (no siren settings)")
    else:
        text = "" if args.dry_run else read_meta(source["carvariations_kit"])
        plan.write("carvariations.meta", out["carvariations_kit"], text)
    data = [(t, out[k]) for t, k in DATA_ORDER if have[k] or k == "carvariations_kit"]
    if audio_row is not None:
        data.append(("AUDIO_GAMEDATA", audio_row))
    if args.make_label:
        labels.append(args.make_label)
    if have["carcols"] and (not args.dry_run) and labels_file.exists():
        labels += [line for line in labels_file.read_text(encoding="utf-8").splitlines() if line.strip()]
    elif parts:
        labels.append(f"<each KEY=TEXT line of {rel(labels_file)}>")
    keys = [label.partition("=")[0] for label in labels if not label.startswith("<")]
    if len({joaat(key) for key in keys}) != len(keys):
        raise ConvertError(f"pack label keys collide (joaat, case-insensitive): {', '.join(keys)}")
    return (data, labels)


TEXT_TABLE = re.compile("(?i)(?:^|/)x64/data/lang/american[a-z_]*\\.rpf!/global\\.gxt2")


def mod_text(plan: Plan, args, target: Path) -> Path | None:
    """The mod's own English text table (x64/data/lang/american*.rpf!/global.gxt2, americandlc first), extracted
    to TARGET for prepare_carcols --gxt2 (part names of its kit labels, livery names); None without one."""
    if args.source is None:
        return None
    import import_pc_assets

    names = sorted(
        (
            row["member"]
            for row in import_pc_assets.inventory(args.source)["members"]
            if TEXT_TABLE.search(row["member"])
        ),
        key=lambda member: ("americandlc.rpf" not in member.lower(), member),
    )
    if not names:
        return None
    command = py("tools/import_pc_assets.py", "extract", args.source, "--member", names[0], "--output-dir", target)
    plan.run("mod text table (part and livery names)", command)
    return fixture_member(target, names[0])


LIVERY_SAMPLER = "diffusesampler2"
LIVERY_MAX = 30


def livery_lines(report: Path, ptd: Path, carcols: Path | None, labels: list[str]) -> list[str]:
    """The car's liveries as the game will see them: texture liveries (a DiffuseTex2 texture <prefix>_sign_N in
    the material report, counted <prefix>_sign_1..30 in the written .ptd; Plates & Livery -> Livery), livery parts
    (kept VMT_LIVERY_MOD visible mods; LS Customs slot 48) and the kit's <liveryNames> (retail keys show the game's
    text, keys the mod's text table defines get label rows)."""
    from gtavmenu_tools.ps5_texture_reader import parse_ps5_texture_dictionary

    rows = json.loads(report.read_bytes())["resources"]
    prefixes = sorted(
        {
            t["textureName"].lower().rpartition("_sign_")[0]
            for r in rows
            for shader in r["shaders"]
            for t in shader["textureParameters"]
            if t.get("textureName") and "_sign_" in t["textureName"].lower() and (LIVERY_SAMPLER in t["nameCandidates"])
        }
    )
    lines = []
    if prefixes:
        limits = Limits(max_file_bytes=1 << 30, max_total_bytes=1 << 31, max_entries=4096)
        names = {t["name"].lower() for t in parse_ps5_texture_dictionary(ptd.read_bytes(), limits)["textures"]}
        for prefix in prefixes:
            found = [n for n in range(1, LIVERY_MAX + 1) if f"{prefix}_sign_{n}" in names]
            where = f"{prefix}_sign_{found[0]}..{found[-1]}" if found else f"no {prefix}_sign_N"
            lines.append(f"{len(found)} texture liveries ({where} in {ptd.name}; Plates & Livery -> Livery)")
    text = carcols.read_text(encoding="utf-8") if carcols is not None and carcols.exists() else ""
    labelled = dict(label.partition("=")[::2] for label in labels if "=" in label)
    parts = []
    for block in re.findall("<visibleMods>(.*?)</visibleMods>", text, re.S):
        for begin, finish in prepare_carcols.items(block):
            item = block[begin:finish]
            if re.search("<type>\\s*VMT_LIVERY_MOD\\s*</type>", item):
                key = re.search("<modShopLabel>\\s*([^<]*?)\\s*</modShopLabel>", item)
                parts.append(labelled.get(key.group(1), key.group(1)) if key else "?")
    if parts:
        lines.append(f"{len(parts)} livery parts (LS Customs -> Livery, slot 48): {', '.join(parts)}")
    keys = [k for block in re.findall("<liveryNames>(.*?)</liveryNames>", text, re.S) for k in ITEM.findall(block)]
    if keys:
        own = [k for k in keys if k in labelled]
        lines.append(f"{len(keys)} livery names ({len(own)} from the mod's text, {len(keys) - len(own)} retail keys)")
    if not lines:
        lines.append("none (no DiffuseTex2 <prefix>_sign_N texture, no VMT_LIVERY_MOD part)")
    return lines


def wheel_names(carcols: str) -> list[str]:
    """<wheelName> of every carcols <Wheels> item (custom wheels), in file order."""
    wheels = WHEELS.search(carcols or "")
    return re.findall("<wheelName>\\s*([^<]*?)\\s*</wheelName>", wheels.group(1)) if wheels else []


def shipped_wheels(source: Path | None, names: list[str]) -> list[str]:
    """Members of the mod archive SOURCE that are wheel models of NAMES (<wheelName>.ydr / .yft, any archive)."""
    if source is None or not names:
        return []
    import import_pc_assets

    wanted = {f"{name.lower()}{suffix}" for name in names for suffix in (".ydr", ".yft")}
    members = import_pc_assets.inventory(source)["members"]
    return sorted(row["member"] for row in members if row["member"].rpartition("!/")[2].lower() in wanted)


def check_wheels(args, carcols: str, report: bool = True) -> None:
    """Custom wheels (carcols <Wheels> items): wheels whose model the mod does not ship are dropped by
    prepare_carcols (a <Wheels> left empty is removed, so the menu's wheel gate never sees it). Wheel models the
    mod does ship are refused unless --drop-wheels: a wheel is a <wheelName>.pdr drawable whose textures must be
    embedded (the game binds the vehicle-mod texture dictionary to it), and the drawable converter's prop route
    moves embedded textures to a separate .ptd and takes no .yft."""
    names = wheel_names(carcols)
    if not names:
        return
    shipped = shipped_wheels(args.source, names)
    if shipped and (not args.drop_wheels):
        raise ConvertError(
            f"the mod ships {len(shipped)} custom wheel model(s) ({', '.join(m.rpartition('!/')[2] for m in shipped)}) for its carcols <Wheels>: converting wheel models is not supported yet (a wheel .pdr must embed its textures); add --drop-wheels to convert the car without its custom wheels"
        )
    if not report:
        return
    why = "--drop-wheels" if shipped else "their models are not in the mod" + ("" if args.source else " (--fixture)")
    print(f"     wheels: {len(names)} carcols <Wheels> item(s) dropped ({why}): {', '.join(names)}", flush=True)


def packs_ids(roots: list[Path], skip: str) -> dict[str, dict[int, list[str]]]:
    """Kit/light/siren ids of the CARCOLS rows of the packs under ROOTS (a pack folder, or a folder of packs such
    as build/custom-assets), pack SKIP left out: {"kits"|"lights"|"sirens": {id: [pack, ...]}}."""
    used: dict[str, dict[int, list[str]]] = {"kits": {}, "lights": {}, "sirens": {}}
    for root in roots:
        single = root / "resources/pack.cfg"
        for cfg in [single] if single.is_file() else sorted(root.glob("*/resources/pack.cfg")):
            rows = [line.split("\t") for line in cfg.read_text(encoding="utf-8", errors="replace").splitlines()]
            pack = next((row[1] for row in rows if len(row) == 2 and row[0] == "pack"), cfg.parent.parent.name)
            if pack == skip:
                continue
            for row in rows:
                if len(row) == 5 and row[:2] == ["data", "CARCOLS_FILE"] and ("/" not in row[4]):
                    meta = cfg.parent / row[4]
                    if meta.is_file():
                        for key, ids in prepare_carcols.census([meta]).items():
                            for value in ids:
                                used[key].setdefault(value, []).append(pack)
    return used


def free_id(ids: range, used: dict[int, list[str]], count: int = 1) -> int | None:
    """The lowest FIRST in IDS with FIRST .. FIRST+COUNT-1 all in IDS and unused."""
    return next((v for v in ids if all((v + i in ids and v + i not in used for i in range(count)))), None)


def carcols_counts(carcols: str | None) -> dict[str, int]:
    """Kits, Lights and Sirens items of a carcols text (prepare_carcols numbers kits and lights id, id+1, ...)."""
    counts = {}
    for key, tag in (("kits", "Kits"), ("lights", "Lights")):
        block = re.search(f"<{tag}>(.*?)</{tag}>", carcols or "", re.S)
        counts[key] = len(prepare_carcols.items(block.group(1))) if block else 0
    counts["sirens"] = len(prepare_carcols.siren_ids(carcols)) if carcols is not None else 0
    return counts


def pick_ids(args, carcols: str | None) -> None:
    """Fill --kit-id, --light-id and (when the mod ships Sirens items, or with a siren pattern) --siren-id with the
    lowest runs of the reserved ranges that no pack under --ids-from uses (default --output-root): one kit id per
    Kits item and one light id per Lights item (prepare_carcols numbers them id, id+1, ...), one siren id per kept
    Sirens item (one for a pattern). CARCOLS: the mod's carcols.meta text (None in a dry run: counts of 1)."""
    counts = carcols_counts(carcols)
    pattern = getattr(args, "siren_preset", None) is not None or getattr(args, "siren_spec", None) is not None
    sirens = 1 if pattern else counts["sirens"]
    want_siren = args.siren_id is None and (not args.drop_sirens) and (sirens > 0)
    if args.kit_id is not None and args.light_id is not None and (not want_siren):
        return
    roots = args.ids_from or [args.output_root]
    used = packs_ids(roots, args.id)
    picked = []
    for name, key, ids, count in (
        ("kit_id", "kits", KIT_IDS, max(1, counts["kits"])),
        ("light_id", "lights", prepare_carcols.GTAVMENU_IDS, max(1, counts["lights"])),
        ("siren_id", "sirens", prepare_carcols.GTAVMENU_IDS, sirens),
    ):
        if getattr(args, name) is not None or (name == "siren_id" and (not want_siren)):
            continue
        value = free_id(ids, used[key], count)
        if value is None:
            run = f"run of {count} free {key[:-1]} ids" if count > 1 else f"free {key[:-1]} id"
            raise ConvertError(
                f"no {run} in {ids.start}-{ids.stop - 1} beside the packs in {', '.join(rel(r) for r in roots)}: pass --{name.replace('_', '-')} (or --ids-from fewer packs)"
            )
        setattr(args, name, value)
        span = f"{value}-{value + count - 1}" if count > 1 else str(value)
        picked.append(f"{key[:-1]} {span}")
    taken = {key: sorted(ids) for key, ids in used.items() if ids}
    print(
        f"     ids: {', '.join(picked)} (lowest free of the reserved ranges; in use by packs in {', '.join(rel(r) for r in roots)}: {taken or 'none'})",
        flush=True,
    )


def retail_layouts_error(args) -> str:
    if args.templates is None:
        return "the mod ships vehiclelayouts.meta: pass --retail-layouts (decrypted retail file)"
    return f"the mod ships vehiclelayouts.meta: retail template {RETAIL_LAYOUTS} is not in {rel(args.templates)}. It is encrypted in the game: with GTA V running and the menu injected, ./menu-ctl.sh export-templates --cache {args.templates} exports it, or pass --retail-layouts FILE"


def make_key(args) -> str | None:
    """vehicleMakeName to write: the --make-label key, the retail --make name, or None (unchanged)."""
    return args.make_label.partition("=")[0] if args.make_label else args.make


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument(
        "--source", type=Path, help="PC add-on dlc.rpf, or an .oiv/.zip package holding one (tools/import_pc_assets.py)"
    )
    source.add_argument("--fixture", type=Path, help="already imported (and repaired) fixture directory")
    parser.add_argument("--model", required=True, help="model name, e.g. a80")
    parser.add_argument("--id", required=True, help="pack id, e.g. gtavmenu-a80-v7")
    parser.add_argument("--name", required=True, help="display name (label for gameName, spawn row text)")
    parser.add_argument("--mod-kit", action="store_true", help="also convert vehiclemods/<model>_*.yft parts")
    parser.add_argument("--parts-fixture", type=Path, help="separate fixture for the mod-kit parts")
    parser.add_argument(
        "--repair",
        action="append",
        default=[],
        metavar="NAME[=BONE,...]",
        help=f"exporter-quirk repairs in order (default auto; none = no repair): {', '.join(REPAIRS)}",
    )
    parser.add_argument(
        "--parts-repair", action="append", default=[], metavar="NAME[=BONE,...]", help="the same for the parts"
    )
    parser.add_argument("--single-page", action="store_true", help="one large system page instead of multi-page")
    parser.add_argument("--parts-multi-page", action="store_true", help="multi-page output for parts too")
    parser.add_argument("--kit-id", type=int, help="free u16 mod kit id (default: the lowest free from 4609)")
    parser.add_argument("--light-id", type=int, help="free u8 light settings id (default: the lowest free of 224-254)")
    sirens = parser.add_mutually_exclusive_group()
    sirens.add_argument(
        "--siren-id",
        type=int,
        help="keep the mod's siren settings, renumbered from this free u8 id (default when the mod ships Sirens items: the lowest free run of 224-254)",
    )
    sirens.add_argument(
        "--drop-sirens", action="store_true", help="drop the mod's Sirens items (retail references stay)"
    )
    pattern = parser.add_mutually_exclusive_group()
    pattern.add_argument(
        "--siren-preset",
        choices=sorted(prepare_carcols.SIREN_PRESETS),
        help="replace the mod's Sirens with one item from this pattern (prepare_carcols.py --list-siren-presets); its id is --siren-id (default the lowest free); every non-zero sirenSettings points at it",
    )
    pattern.add_argument("--siren-spec", type=Path, help="the same from a JSON pattern spec (prepare_carcols.py)")
    parser.add_argument(
        "--siren-lamps",
        help="20 group keys for siren1..siren20 ('-' unused; default from the skeleton: a missing bone '-', with groups L/R each lamp on its side, x < 0 = L)",
    )
    parser.add_argument(
        "--drop-wheels", action="store_true", help="drop custom wheels even when the mod ships their models"
    )
    parser.add_argument("--max-texture-size", type=int, help="drop texture mips above this size (power of two)")
    parser.add_argument(
        "--part-texture-size",
        type=int,
        help="the same for the textures LSC parts (or the car's fragments) carry themselves, which go into the car's .ptd: livery parts carry a 2048x2048 texture each (default --max-texture-size)",
    )
    parser.add_argument(
        "--lower-lods",
        choices=LOWER_LODS,
        default="auto",
        help="the base fragment's medium/low/very-low LOD lists: copy = deep copies of the high list (refused past 64 MiB / 128 pages), empty = own empty lists (the high list draws at every distance; a heavy model then stays its own size), auto = copy unless past those bounds (default)",
    )
    parser.add_argument(
        "--ids-from",
        type=Path,
        action="append",
        default=[],
        help="packs whose carcols kit/light/siren ids the defaults avoid: a pack folder or a folder of packs (repeatable; default --output-root)",
    )
    parser.add_argument("--census", type=Path, action="append", default=[], help="retail carcols for id checks")
    parser.add_argument("--shop-label-prefix", help="LSC part label prefix (default GM<MODEL>)")
    parser.add_argument(
        "--audio",
        help="retail audioNameHash to use (e.g. BANSHEE; default: the mod's when it names another car, else a base-game sound by vehicle class); checked against the template cache's base car sounds",
    )
    parser.add_argument(
        "--audio-unchecked", action="store_true", help="use --audio although it is not a base-game sound (DLC cars)"
    )
    parser.add_argument(
        "--audio-engine",
        metavar="NAME",
        help="own CarAudioSettings: the car's (base-game) sound with the engine of base-game car NAME (AUDIO_GAMEDATA)",
    )
    make = parser.add_mutually_exclusive_group()
    make.add_argument(
        "--make-label",
        metavar="KEY=TEXT",
        help="vehicleMakeName -> new pack-owned label KEY with TEXT (e.g. GMA80_MAKE=Toyota)",
    )
    make.add_argument("--make", metavar="NAME", help="vehicleMakeName -> existing retail make label (e.g. KARIN)")
    parser.add_argument("--driveby", action="append", help="stock first-person drive-by names (default LOW_BUCCANEER)")
    parser.add_argument("--retail-layouts", type=Path, help="decrypted retail data/ai/vehiclelayouts.meta")
    parser.add_argument(
        "--templates",
        type=Path,
        default=build_dir(ROOT) / "retail-templates",
        help="Verified retail template cache from fetch-templates or export-templates",
    )
    parser.add_argument("--archive", help="main archive name (default gm<model>.rpf)")
    parser.add_argument("--parts-archive", help="parts archive name (default gm<model>parts.rpf)")
    parser.add_argument("--output-root", type=Path, default=build_dir(ROOT) / "custom-assets")
    parser.add_argument("--dry-run", action="store_true", help="print the steps, write nothing")
    args = parser.parse_args(argv)
    if not valid_id(args.id):
        parser.error("--id must be a valid pack identifier")
    if not re.fullmatch(r"[a-z0-9_]+", args.model):
        parser.error("--model must be the lowercase model name")
    for name in ("repair", "parts_repair"):
        specs = getattr(args, name)
        if "none" in specs and len(specs) > 1:
            parser.error(f"--{name.replace('_', '-')} none goes alone")
        setattr(args, name, [] if specs == ["none"] else specs or ["auto"])
    if not (args.mod_kit or args.parts_fixture):
        args.parts_repair = [spec for spec in args.parts_repair if spec != "auto"]
    if (args.siren_preset or args.siren_spec) and args.drop_sirens:
        parser.error("--siren-preset/--siren-spec replace the mod's sirens; --drop-sirens drops them: pick one")
    if args.siren_lamps is not None and (not (args.siren_preset or args.siren_spec)):
        parser.error("--siren-lamps goes with --siren-preset or --siren-spec")
    for flag, size in (("--max-texture-size", args.max_texture_size), ("--part-texture-size", args.part_texture_size)):
        if size is not None and (size < 4 or size & size - 1):
            parser.error(f"{flag} must be a power of two >= 4 (e.g. 1024)")
    if args.audio_unchecked and (not args.audio):
        parser.error("--audio-unchecked goes with --audio NAME")
    for flag, value in (("--audio", args.audio), ("--audio-engine", args.audio_engine)):
        if value is not None and (not (LABEL_KEY.fullmatch(value) and len(value) <= NAME_MAX)):
            parser.error(f"{flag} takes a retail car name ([A-Za-z0-9_], e.g. BANSHEE)")
    if args.source and args.parts_fixture:
        parser.error("--parts-fixture goes with --fixture")
    for name in ("source", "fixture", "parts_fixture", "retail_layouts", "output_root", "siren_spec"):
        if getattr(args, name) is not None:
            setattr(args, name, getattr(args, name).resolve())
    args.census = [path.resolve() for path in args.census]
    args.ids_from = [path.resolve() for path in args.ids_from]
    args.templates = args.templates.resolve() if args.templates is not None else None
    if args.templates is not None and args.retail_layouts is None:
        layouts = args.templates / RETAIL_LAYOUTS
        try:
            _, rows = retail_templates.load_manifest()
            row = next(r for r in rows if r.cache == RETAIL_LAYOUTS)
            if retail_templates.cached_ok(args.templates, row):
                args.retail_layouts = layouts
        except (retail_templates.TemplateError, StopIteration):
            pass
    try:
        if args.make_label:
            args.make_label = "=".join(label_spec(args.make_label))
            problem = make_label_problem(args.make_label.partition("=")[0])
            if problem:
                raise ConvertError(problem)
            if len(args.make_label.partition("=")[0]) > GAME_NAME_MAX:
                print(
                    f"note: --make-label key {args.make_label.partition('=')[0]} is longer than {GAME_NAME_MAX} characters: the game cuts the make key to 11, so its own vehicle-name popup and shops show no make (the menu's names come from the pack); prefer a key like GMA80_MAKE",
                    flush=True,
                )
        if args.make and (not (LABEL_KEY.fullmatch(args.make) and len(args.make) <= NAME_MAX)):
            raise ConvertError(f"--make {args.make!r}: a label key [A-Za-z0-9_] of at most {NAME_MAX}")
        load_siren_spec(args)
        return build(args)
    except (ConvertError, OSError) as error:
        print(f"convert_vehicle: {error}", file=sys.stderr)
        return 1


def reference(args, name: str) -> Path:
    """Use the verified runtime template copy for every retail input."""
    return staged_templates(args) / name


def stage_templates(args) -> None:
    """Stage regular verified template files without any private report manifests."""
    target = staged_templates(args)
    for name in RETAIL_TEMPLATES:
        out = target / name
        out.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.link(args.templates / name, out)
        except OSError:
            shutil.copyfile(args.templates / name, out)


def material_report(plan: Plan, args, fixture: Path, members: list[str], texture: str, output: Path) -> None:
    command = py("inspect_pc_vehicle_materials.py", "--pc-fixture", fixture)
    for member in members:
        command += ["--resource-member", member]
    command += ["--texture-member", texture, "--templates", staged_templates(args), "--output", output]
    plan.run("material report", command)


def convert(plan: Plan, args, fixture: Path, report: Path, output: Path, multi_page: bool) -> None:
    command = py(
        "convert_pc_vehicle.py",
        "references",
        "--pc-fixture",
        fixture,
        "--material-report",
        report,
        "--target",
        ROOT / TARGET,
        "--templates",
        staged_templates(args),
        "--output",
        output,
    )
    if multi_page:
        command.append("--multi-page")
    plan.run("convert (references stage" + (", multi-page)" if multi_page else ")"), command)


def preflight(args) -> None:
    """Check published tool dependencies and exact template identities before writing."""
    from gtavmenu_tools import drawable_contracts, vehicle_contracts
    from gtavmenu_tools.asset_formats import AssetError

    problems = template_problems(args)
    if importlib.util.find_spec("numpy") is None:
        problems.append(f"numpy is not importable by {sys.executable}; install the texture conversion dependency")
    inputs = [
        ROOT / TARGET,
        *(p for p in (args.source, args.fixture, args.parts_fixture, args.retail_layouts, *args.census) if p),
    ]
    problems += [f"missing input {rel(path)}" for path in dict.fromkeys(inputs) if not path.exists()]
    try:
        contracts = vehicle_contracts.load()
        vehicle_contracts.check_target(json.loads((ROOT / TARGET).read_bytes()), contracts)
        drawable_contracts.load()
    except (AssetError, OSError, ValueError) as error:
        problems.append(str(error))
    if (args.parts_repair or args.parts_multi_page) and not (args.mod_kit or args.parts_fixture):
        problems.append("--parts-repair/--parts-multi-page need --mod-kit (or --parts-fixture)")
    pack = args.output_root / args.id
    if (pack.exists() or pack.is_symlink()) and not args.dry_run:
        problems.append(f"{rel(pack)} exists; choose a new --id or remove it")
    if problems:
        raise ConvertError("preflight failed:\n  " + "\n  ".join(problems))


if __name__ == "__main__":
    if sys.argv[1:2] == [DIAGNOSE] and len(sys.argv) == 6:
        print(json.dumps(diagnose(sys.argv[2], *(Path(a) for a in sys.argv[3:]))))
        raise SystemExit(0)
    if sys.argv[1:2] == [SIREN_BONES] and len(sys.argv) == 5:
        print(json.dumps(siren_bones(Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4])))
        raise SystemExit(0)
    raise SystemExit(main())

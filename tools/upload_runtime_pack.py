#!/usr/bin/env python3
"""Upload a runtime pack to the console's pack root over FTP, optionally marking it active.

The worker reads /data/gtavmenu/custom/packs/<id>/resources/{pack.cfg, <archives>, <data files>}
and lists every installed pack on the menu's Custom Packs page, where packs are selected and loaded.
Every file named by pack.cfg is checked against its descriptor hash, uploaded read-only and verified
by download. Without --activate/--activate-add nothing else changes: select the pack in the menu.

  --activate      write packs/active = this pack only
  --activate-add  add this pack to packs/active (the worker loads up to 8 active packs together)
  --activate-only skip uploads; verify the remote copy byte-for-byte, then activate
  --uninstall ID  remove pack ID from packs/active and packs/installed (its files stay)
  --purge         with --uninstall: also delete the pack's directory on the console
  --list          print packs/installed and packs/active from the console (no changes)

The menu's own uninstall (Custom Packs -> Manage Packs -> Uninstall) also renames the pack's resources/pack.cfg to
pack.cfg.uninstalled. Uploading the pack again restores it: pack.cfg is written anew and the
leftover pack.cfg.uninstalled is deleted. A local pack directory that holds only a
pack.cfg.uninstalled (copied back from the console) uploads it as pack.cfg.

packs/active is written last and only after every upload or remote check verified. Before an
upload the pack is validated like tools/validate_runtime_pack.py does, and with --activate-add
it must also merge with the packs already active on the console (including the carcols id rules:
the other active packs' carcols/carvariations rows are read back and no kit, light, siren or wheel key
may repeat, gtavmenu_tools.carcols_ids; and together they must fit the fixed engine tables their model,
weapon and components rows fill, gtavmenu_tools.engine_caps); --no-validate skips both. The
FTP endpoint defaults to $PS5_HOST and $PS5_FTP_PORT (2121), as in menu-ctl.sh.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import os
import subprocess
import sys
import tempfile
from collections.abc import Callable, Sequence
from pathlib import Path

from gtavmenu_tools import carcols_ids, engine_caps, runtime_pack

PACK_ROOT = "/data/gtavmenu/custom/packs"
ACTIVE = f"{PACK_ROOT}/active"
# One pack id per line, sorted: the menu's installed-pack list when the game cannot list the
# directory itself (MULTIPACK-run2: in-game opendir found nothing while fopen worked).
INSTALLED = f"{PACK_ROOT}/installed"
INSTALLED_MAX = 64  # custom_pack_select.inc GTAV_PACK_INSTALLED_MAX; the menu pages the list
DEFAULT_FTP_PORT = 2121

Curl = Callable[..., bytes]


def curl(*args: str) -> bytes:
    """One quiet, failing curl transfer; returns stdout."""
    return subprocess.run(["curl", "-s", "--fail", *args], check=True, capture_output=True, timeout=300).stdout


def load_pack(pack_dir: Path) -> tuple[runtime_pack.RuntimePack, dict[str, bytes]]:
    """Parse resources/pack.cfg; return it and the upload set (named files first, pack.cfg last)."""
    resources = pack_dir / "resources"
    descriptor = runtime_pack.descriptor_path(resources).read_text(encoding="ascii")  # uploaded as pack.cfg
    pack = runtime_pack.parse(descriptor)
    files: dict[str, bytes] = {}
    for name, size, sha in runtime_pack.files(pack):
        blob = (resources / name).read_bytes()
        if len(blob) != size or hashlib.sha256(blob).hexdigest() != sha:
            raise SystemExit(f"local {name} does not match pack.cfg")
        files[name] = blob
    files["pack.cfg"] = descriptor.encode("ascii")  # last: the pack is listed once its files are in place
    return pack, files


def active_ids(current: Sequence[str], pack_id: str, *, add: bool) -> list[str]:
    """New packs/active rows: this pack alone, or appended to `current` without repeating an id."""
    ids = list(dict.fromkeys(i for i in current if i != pack_id)) if add else []
    ids.append(pack_id)
    if len(ids) > runtime_pack.ACTIVE_MAX:
        raise SystemExit(f"active would list {len(ids)} packs (max {runtime_pack.ACTIVE_MAX})")
    return ids


def _put(run: Curl, base: str, remote: str, blob: bytes, *extra: str) -> None:
    with tempfile.NamedTemporaryFile() as handle:
        handle.write(blob)
        handle.flush()
        run(*extra, "-T", handle.name, f"{base}{remote}")


def _replace(run: Curl, base: str, remote: str, blob: bytes) -> None:
    """Store a small control file, deleting it first when the server refuses the overwrite.

    The console's FTP server can answer "550 Permission denied" to STOR over an existing file that
    a STOR of a new name accepts (OVR-run1, packs/active); DELE + STOR then succeeds.
    """
    try:
        _put(run, base, remote, blob)
    except subprocess.CalledProcessError:
        directory = remote.rpartition("/")[0]
        run("-Q", f"DELE {remote}", f"{base}{directory}/")
        _put(run, base, remote, blob)


def upload(run: Curl, base: str, pack_id: str, files: dict[str, bytes]) -> bool:
    """Upload each file read-only and read it back; returns whether every readback matched."""
    remote_dir = f"{PACK_ROOT}/{pack_id}/resources"
    ok = True
    for name, blob in files.items():
        remote = f"{remote_dir}/{name}"
        with contextlib.suppress(subprocess.CalledProcessError):  # first upload: no file to unlock yet
            run("-Q", f"SITE CHMOD 644 {remote}", f"{base}{remote_dir}/")
        _put(run, base, remote, blob, "--ftp-create-dirs")
        run("-Q", f"SITE CHMOD 444 {remote}", f"{base}{remote_dir}/")
        readback = run(f"{base}{remote}")
        match = readback == blob
        ok &= match
        print(f"uploaded {remote} bytes={len(readback)} verified={match}")
    for path in (f"{PACK_ROOT}/{pack_id}", remote_dir):
        run("-Q", f"SITE CHMOD 755 {path}", f"{base}{remote_dir}/")
    if ok:
        clear_uninstalled(run, base, pack_id)
    return ok


def clear_uninstalled(run: Curl, base: str, pack_id: str) -> bool:
    """Delete the pack.cfg.uninstalled a menu uninstall left; returns whether there was one.

    Called once pack.cfg is uploaded again, so the pack is listed and the leftover only misleads.
    Uploads are read-only (444), so a refused DELE is retried after unlocking the file.
    """
    remote_dir = f"{PACK_ROOT}/{pack_id}/resources"
    remote = f"{remote_dir}/{runtime_pack.UNINSTALLED_DESCRIPTOR}"
    try:
        run("-Q", f"DELE {remote}", f"{base}{remote_dir}/")
    except subprocess.CalledProcessError:
        try:
            run("-Q", f"SITE CHMOD 644 {remote}", f"{base}{remote_dir}/")
            run("-Q", f"DELE {remote}", f"{base}{remote_dir}/")
        except subprocess.CalledProcessError:
            return False  # nothing left behind: the usual case
    print(f"removed {remote} (left by the menu's uninstall; pack.cfg is restored)")
    return True


def verify_remote(run: Curl, base: str, pack_id: str, files: dict[str, bytes]) -> bool:
    """Compare the installed copy byte-for-byte without uploading."""
    remote_dir = f"{PACK_ROOT}/{pack_id}/resources"
    ok = True
    for name, blob in files.items():
        try:
            match = run(f"{base}{remote_dir}/{name}") == blob
        except subprocess.CalledProcessError:
            match = False
        ok &= match
        print(f"remote {remote_dir}/{name} verified={match}")
    if not ok and _exists(run, base, f"{remote_dir}/{runtime_pack.UNINSTALLED_DESCRIPTOR}"):
        print(
            f"{pack_id} was uninstalled in the menu ({runtime_pack.UNINSTALLED_DESCRIPTOR}); "
            "upload it again without --activate-only to restore it",
            file=sys.stderr,
        )
    return ok


def _exists(run: Curl, base: str, remote: str) -> bool:
    try:
        run(f"{base}{remote}")
    except subprocess.CalledProcessError:
        return False
    return True


def activate(run: Curl, base: str, pack_id: str, *, add: bool) -> bool:
    """Write packs/active (this pack, or added to the current list) and read it back."""
    current: list[str] = []
    if add:
        try:
            current = run(f"{base}{ACTIVE}").decode("ascii").split()
        except subprocess.CalledProcessError:
            current = []
    body = "".join(f"{i}\n" for i in active_ids(current, pack_id, add=add)).encode("ascii")
    _replace(run, base, ACTIVE, body)
    run("-Q", f"SITE CHMOD 644 {ACTIVE}", f"{base}{PACK_ROOT}/")
    readback = run(f"{base}{ACTIVE}")
    print(f"active packs = {' '.join(readback.decode('ascii', errors='replace').split())}")
    return readback == body


def installed_ids(current: Sequence[str], pack_id: str) -> list[str]:
    """New packs/installed rows: `current` plus this pack, sorted, without repeats."""
    ids = sorted({*current, pack_id})
    if len(ids) > INSTALLED_MAX:
        raise SystemExit(f"installed would list {len(ids)} packs (the menu shows at most {INSTALLED_MAX})")
    return ids


def record_installed(run: Curl, base: str, pack_id: str) -> bool:
    """Add this pack to packs/installed (one id per line; invalid lines dropped) and read it back."""
    try:
        lines = run(f"{base}{INSTALLED}").decode("ascii", errors="replace").splitlines()
    except subprocess.CalledProcessError:
        lines = []
    current = [line.strip() for line in lines if runtime_pack.valid_id(line.strip())]
    if pack_id in current and len(current) == len(lines):
        return True  # already listed and the file is clean
    body = "".join(f"{i}\n" for i in installed_ids(current, pack_id)).encode("ascii")
    _replace(run, base, INSTALLED, body)
    run("-Q", f"SITE CHMOD 644 {INSTALLED}", f"{base}{PACK_ROOT}/")
    return run(f"{base}{INSTALLED}") == body


def _read_ids(run: Curl, base: str, remote: str) -> list[str] | None:
    """The ids of a one-id-per-line control file, or None when it does not exist."""
    try:
        return run(f"{base}{remote}").decode("ascii", errors="replace").split()
    except subprocess.CalledProcessError:
        return None


def check_active_merge(run: Curl, base: str, pack: runtime_pack.RuntimePack, pack_dir: Path) -> list[str]:
    """Why `pack` would not load together with the packs active on the console (empty when it would)."""
    others: list[runtime_pack.RuntimePack] = []
    problems: list[str] = []
    for pack_id in dict.fromkeys(_read_ids(run, base, ACTIVE) or []):
        if pack_id == pack.pack_id:
            continue
        try:
            text = run(f"{base}{PACK_ROOT}/{pack_id}/resources/pack.cfg").decode("ascii")
            others.append(runtime_pack.parse(text))
        except (subprocess.CalledProcessError, UnicodeDecodeError, runtime_pack.RuntimePackError):
            problems.append(f"active pack {pack_id}: pack.cfg is missing or invalid on the console")
    return (
        problems
        + runtime_pack.merge_conflicts([*others, pack])
        + carcols_merge(run, base, others, pack, pack_dir)
        + engine_merge(run, base, others, pack, pack_dir)
    )


def engine_merge(
    run: Curl, base: str, others: Sequence[runtime_pack.RuntimePack], pack: runtime_pack.RuntimePack, pack_dir: Path
) -> list[str]:
    """Fixed engine tables the active packs plus `pack` would overfill (gtavmenu_tools.engine_caps),
    reading back the active packs' model and components rows (only when `pack` adds to a table)."""
    if not any(row.type in engine_caps.ROW_TYPES for row in pack.data):
        return []
    usages, problems = [], []
    for other in others:
        data = []
        for row in other.data:
            if row.type not in engine_caps.ROW_TYPES:
                continue
            blob = None
            if row.type in engine_caps.COUNTED_TYPES:
                try:
                    blob = run(f"{base}{PACK_ROOT}/{other.pack_id}/resources/{row.file}")
                except subprocess.CalledProcessError:
                    problems.append(f"active pack {other.pack_id}: {row.file} is missing on the console")
                    continue
            data.append((row.type, row.file, blob))
        usage, errors = engine_caps.pack_usage(other.pack_id, data)
        usages.append((other.pack_id, usage))
        problems += errors
    local = [
        (row.type, row.file, (pack_dir / "resources" / row.file).read_bytes())
        for row in pack.data
        if row.type in engine_caps.ROW_TYPES
    ]
    usage, errors = engine_caps.pack_usage(pack.pack_id, local)
    usages.append((pack.pack_id, usage))
    return problems + errors + [f"engine table: {p}" for p in engine_caps.problems(usages)]


def carcols_merge(
    run: Curl, base: str, others: Sequence[runtime_pack.RuntimePack], pack: runtime_pack.RuntimePack, pack_dir: Path
) -> list[str]:
    """Carcols id collisions with the active packs, reading their carcols/carvariations rows back
    (only when the new pack has such rows: nothing else can collide)."""
    kinds = (carcols_ids.CARCOLS, carcols_ids.VARIATION)
    if not any(row.type in kinds for row in pack.data):
        return []
    rows, problems = [], []
    for other in others:
        data = []
        for row in other.data:
            if row.type not in kinds:
                continue
            try:
                data.append((row.type, row.file, run(f"{base}{PACK_ROOT}/{other.pack_id}/resources/{row.file}")))
            except subprocess.CalledProcessError:
                problems.append(f"active pack {other.pack_id}: {row.file} is missing on the console")
        other_rows, errors = carcols_ids.pack_rows(other.pack_id, data)
        rows += other_rows
        problems += errors
    local = [
        (row.type, row.file, (pack_dir / "resources" / row.file).read_bytes()) for row in pack.data if row.type in kinds
    ]
    own_rows, errors = carcols_ids.pack_rows(pack.pack_id, local)
    return problems + errors + [f"carcols: {p}" for p in carcols_ids.problems([*rows, *own_rows])]


def validate_upload(run: Curl, base: str, args: argparse.Namespace) -> None:
    """Refuse an invalid pack, or with --activate-add one that cannot join the console's active set."""
    pack, errors, warnings = runtime_pack.check_directory(args.pack)
    if pack is not None and args.activate_add and not errors:
        errors = [f"with the active packs: {p}" for p in check_active_merge(run, base, pack, args.pack)]
    elif pack is not None and not errors:
        errors = engine_merge(run, base, [], pack, args.pack)  # a pack that cannot load even alone
    for warning in warnings:
        print(f"warning: {args.pack}: {warning}", file=sys.stderr)
    if errors:
        for error in errors:
            print(f"error: {args.pack}: {error}", file=sys.stderr)
        raise SystemExit(f"{args.pack}: not uploaded, the pack is invalid (--no-validate uploads anyway)")


def _remove_id(run: Curl, base: str, remote: str, pack_id: str) -> bool:
    """Drop `pack_id` (and invalid lines) from a control file; delete the file when nothing is left."""
    ids = _read_ids(run, base, remote)
    if ids is None or pack_id not in ids:
        print(f"{pack_id} is not listed in {remote}")
        return True
    kept = list(dict.fromkeys(i for i in ids if i != pack_id and runtime_pack.valid_id(i)))
    directory = remote.rpartition("/")[0]
    if not kept:
        run("-Q", f"DELE {remote}", f"{base}{directory}/")
        ok = _read_ids(run, base, remote) is None
        print(f"removed {pack_id} from {remote} (now empty, deleted) verified={ok}")
        return ok
    body = "".join(f"{i}\n" for i in kept).encode("ascii")
    _replace(run, base, remote, body)
    run("-Q", f"SITE CHMOD 644 {remote}", f"{base}{directory}/")
    ok = run(f"{base}{remote}") == body
    print(f"removed {pack_id} from {remote} (left: {' '.join(kept)}) verified={ok}")
    return ok


def _purge_tree(run: Curl, base: str, path: str) -> None:
    """Delete the directory `path` and everything below it."""
    listing = run("--list-only", f"{base}{path}/").decode("utf-8", errors="replace")
    for name in (line.strip().rsplit("/", 1)[-1] for line in listing.splitlines()):
        if name in ("", ".", ".."):
            continue
        child = f"{path}/{name}"
        try:
            run("-Q", f"DELE {child}", f"{base}{path}/")
            continue
        except subprocess.CalledProcessError:
            pass
        try:
            run("--list-only", f"{base}{child}/")
        except subprocess.CalledProcessError:  # a file whose DELE was refused: unlock it (uploads are 444)
            run("-Q", f"SITE CHMOD 644 {child}", f"{base}{path}/")
            run("-Q", f"DELE {child}", f"{base}{path}/")
            continue
        _purge_tree(run, base, child)
    run("-Q", f"RMD {path}", f"{base}{path.rpartition('/')[0]}/")


def purge(run: Curl, base: str, pack_id: str) -> bool:
    """Delete <pack root>/<id>/ on the console; returns whether it is gone."""
    path = f"{PACK_ROOT}/{pack_id}"
    try:
        run("--list-only", f"{base}{path}/")
    except subprocess.CalledProcessError:
        print(f"{path} does not exist")
        return True
    try:
        _purge_tree(run, base, path)
    except subprocess.CalledProcessError as exc:
        print(f"could not delete {path}: curl exit {exc.returncode}", file=sys.stderr)
    try:
        run("--list-only", f"{base}{path}/")
    except subprocess.CalledProcessError:
        print(f"deleted {path}")
        return True
    print(f"{path} is still present", file=sys.stderr)
    return False


def uninstall(run: Curl, base: str, pack_id: str, *, purge_files: bool) -> bool:
    """Deselect and unlist a pack (active first: no load may name an unlisted pack), then purge."""
    ok = _remove_id(run, base, ACTIVE, pack_id)
    ok = _remove_id(run, base, INSTALLED, pack_id) and ok
    if ok and purge_files:
        ok = purge(run, base, pack_id)
    elif not ok and purge_files:
        print("files left in place: the index update did not verify", file=sys.stderr)
    if ok:
        print(f"uninstalled {pack_id}; a GTA session that already loaded it keeps it until restart")
    return ok


def list_packs(run: Curl, base: str) -> list[str]:
    """Report lines for the console's installed and active packs, in the Custom Packs order.

    The menu lists the installed ids (sorted) whose pack.cfg it can open; the index is the one
    `menu-ctl.sh pack-select N` takes. `*` marks a pack in packs/active, `!` an installed id whose
    pack.cfg is missing (the menu hides it), `?` an active id that packs/installed does not list.
    """
    installed = sorted({i for i in _read_ids(run, base, INSTALLED) or [] if runtime_pack.valid_id(i)})
    active = list(dict.fromkeys(_read_ids(run, base, ACTIVE) or []))
    lines = [f"{base}{PACK_ROOT}: {len(installed)} installed, {len(active)} active (max {runtime_pack.ACTIVE_MAX})"]
    width = max((len(i) for i in [*installed, *active]), default=0)
    index = 0
    for pack_id in installed:
        mark = "*" if pack_id in active else " "
        note = f"load order {active.index(pack_id) + 1}" if pack_id in active else ""
        resources = f"{PACK_ROOT}/{pack_id}/resources"
        if not _exists(run, base, f"{resources}/pack.cfg"):
            why = "pack.cfg missing: the menu hides it"
            if _exists(run, base, f"{resources}/{runtime_pack.UNINSTALLED_DESCRIPTOR}"):
                why = "uninstalled in the menu (pack.cfg.uninstalled): re-upload restores it"
            lines.append(f"   - ! {pack_id:<{width}}  {why} {note}".rstrip())
            continue
        lines.append(f"  {index:>2} {mark} {pack_id:<{width}}  {note}".rstrip())
        index += 1
    for pack_id in active:
        if pack_id not in installed:
            lines.append(f"   ? * {pack_id:<{width}}  in packs/active but not in packs/installed")
    if not installed and not active:
        lines.append("  no packs installed (packs/installed and packs/active are absent)")
    else:
        lines.append("  * = selected (packs/active); N = ./menu-ctl.sh pack-select N; load: Custom Packs -> Load")
    return lines


def parse_args(argv: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pack", type=Path, nargs="?", help="pack directory (contains resources/pack.cfg)")
    parser.add_argument("--activate", action="store_true", help="write packs/active = this pack id")
    parser.add_argument(
        "--activate-add", action="store_true", help="add this pack id to packs/active (at most 8 active packs)"
    )
    parser.add_argument(
        "--activate-only", action="store_true", help="skip uploads; activate after verifying the remote pack"
    )
    parser.add_argument(
        "--no-validate", action="store_true", help="upload without the validator and active-set merge checks"
    )
    parser.add_argument(
        "--uninstall", metavar="ID", help="remove pack ID from packs/active and packs/installed (files stay)"
    )
    parser.add_argument("--purge", action="store_true", help="with --uninstall: also delete <pack root>/ID/")
    parser.add_argument("--list", action="store_true", help="print the console's installed and active packs")
    parser.add_argument("--host", default=os.environ.get("PS5_HOST"), help="console address (default: $PS5_HOST)")
    parser.add_argument(
        "--port",
        type=int,
        default=int(os.environ.get("PS5_FTP_PORT") or DEFAULT_FTP_PORT),
        help=f"FTP port (default: $PS5_FTP_PORT or {DEFAULT_FTP_PORT})",
    )
    args = parser.parse_args(argv)
    if args.list:
        others = (args.uninstall, args.activate, args.activate_add, args.activate_only, args.no_validate, args.purge)
        if args.pack is not None or any(o not in (None, False) for o in others):
            parser.error("--list takes no pack directory and no other mode option")
    elif args.uninstall is not None:
        if args.pack is not None or args.activate or args.activate_add or args.activate_only:
            parser.error("--uninstall takes a pack id and no pack directory or --activate option")
        if not runtime_pack.valid_id(args.uninstall):
            parser.error(f"--uninstall: {args.uninstall!r} is not a pack id")
    elif args.purge:
        parser.error("--purge needs --uninstall ID")
    elif args.pack is None:
        parser.error("pass a pack directory or --uninstall ID")
    if not args.host:
        parser.error("pass --host or set PS5_HOST")
    return args


def main(argv: Sequence[str] | None = None, run: Curl = curl) -> int:
    args = parse_args(argv)
    base = f"ftp://{args.host}:{args.port}"
    if args.list:
        print("\n".join(list_packs(run, base)))
        return 0
    if args.uninstall is not None:
        return 0 if uninstall(run, base, args.uninstall, purge_files=args.purge) else 1
    if not args.no_validate:
        validate_upload(run, base, args)
    try:
        pack, files = load_pack(args.pack)
    except (OSError, UnicodeDecodeError, runtime_pack.RuntimePackError) as exc:
        raise SystemExit(f"{args.pack}: {exc}") from exc
    transfer = verify_remote if args.activate_only else upload
    ok = transfer(run, base, pack.pack_id, files)
    if ok:
        ok = record_installed(run, base, pack.pack_id)
    wants_active = args.activate or args.activate_add or args.activate_only
    if ok and wants_active:
        ok = activate(run, base, pack.pack_id, add=args.activate_add)
    elif not ok and wants_active:
        print("packs/active left unchanged: verification failed", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())

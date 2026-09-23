#!/usr/bin/env python3
"""Install a locally staged GTAVMenu standalone bundle onto a PS5 over FTP.

This standard-library-only helper is used by the ``./gtavmenu`` front-end from a source
checkout. Public release archives stay minimal and carry manual setup in ``README.md``.

The file-to-destination mapping is read from internal verification metadata stored beside
the staged bundle. Only entries with a ``remote`` path are uploaded.

Subcommands::

    install_release.py preflight --host <ip>   # check ports are reachable
    install_release.py install   --host <ip>   # upload every bundled file over FTP
    install_release.py status    --host <ip>   # tail the on-console loader/module logs
"""

from __future__ import annotations

import argparse
import contextlib
import ftplib
import hashlib
import json
import socket
from pathlib import Path

DEFAULT_FTP_PORT = 1337
DEFAULT_PS5DEBUG_PORT = 744
DEFAULT_KLOG_PORT = 9081
# Repo-root-relative location of the staged bundle. Anchored on this file's location so
# ``./gtavmenu`` works from any working directory.
DEFAULT_BUNDLE = Path(__file__).resolve().parent.parent / "build/pkg/gtavmenu-payload"
MANIFEST_SUFFIX = ".package-manifest.json"
EXPECTED_TARGET_ID = "PPSA04264_01.010.002_DISC"
EXPECTED_CONTENT_VERSION = "01.010.002"


class InstallError(RuntimeError):
    pass


# --------------------------------------------------------------------------- manifest


def package_manifest_path(bundle: Path) -> Path:
    return bundle.with_name(f"{bundle.name}{MANIFEST_SUFFIX}")


def load_manifest(bundle: Path) -> dict:
    manifest_path = package_manifest_path(bundle)
    if not manifest_path.is_file():
        raise InstallError(f"no staging metadata at {manifest_path} -- run `make package-payload` first")
    return json.loads(manifest_path.read_text(encoding="utf-8"))


def upload_entries(manifest: dict) -> list[dict]:
    """Manifest file entries that have a real remote destination (skip read-me/local)."""
    out = []
    for entry in manifest.get("files", []):
        remote = entry.get("remote")
        if remote and remote.startswith("/"):
            out.append(entry)
    return out


def validate_bundle_safety(bundle: Path, manifest: dict) -> None:
    """Validate the staged runtime files before opening an FTP connection."""
    if (
        manifest.get("kind") != "gtavmenu-standalone-production"
        or manifest.get("profile") != "production"
        or manifest.get("targetId") != EXPECTED_TARGET_ID
        or manifest.get("contentVersion") != EXPECTED_CONTENT_VERSION
    ):
        raise InstallError("bundle identity/profile is not the supported 01.010.002 production release")

    roles = {str(entry.get("role")) for entry in manifest.get("files", [])}
    required_roles = {"payload-loader", "menu-worker", "documentation"}
    missing_roles = sorted(required_roles - roles)
    if missing_roles:
        raise InstallError(f"bundle is missing required roles: {', '.join(missing_roles)}")

    for entry in manifest.get("files", []):
        relative = Path(str(entry.get("path") or ""))
        if relative.is_absolute() or ".." in relative.parts:
            raise InstallError(f"unsafe package path: {relative}")
        local = bundle / relative
        if not local.is_file():
            raise InstallError(f"bundle file missing: {local}")
        digest = hashlib.sha256(local.read_bytes()).hexdigest()
        if digest != entry.get("sha256") or local.stat().st_size != entry.get("size"):
            raise InstallError(f"bundle hash/size mismatch: {relative}")


def _print_post_install_guidance(manifest: dict, host: str, uploaded: int) -> None:
    print(f"\nInstalled {uploaded} file(s) to {host}.")

    release_channel = manifest.get("releaseChannel")
    if release_channel:
        print(f"Release channel: {release_channel}")
    if manifest.get("requiresHardwareValidation"):
        print(
            "Note: this locally built artifact still needs the documented fresh-process hardware\n"
            "regression before publication."
        )

    if manifest.get("ps5debugDependency") is False:
        print("ps5debug-ng is not required for this end-user runtime path.")

    loader_elf = manifest.get("loaderElfPath")
    launch_surface = manifest.get("primaryLaunchSurface") or "ps5-payload-manager"
    if loader_elf:
        print("Next: start GTA V, enter Story Mode, and wait for player control.")
        print(f"Then run {loader_elf} from {launch_surface} and open the menu with R1 + D-pad Left.")


# --------------------------------------------------------------------------- preflight


def _port_open(host: str, port: int, timeout: float = 4.0) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def preflight(host: str, ftp_port: int, *, strict: bool = True) -> bool:
    """Print pass/fail for the ports the install needs. Returns True if good to go."""
    ftp_ok = _port_open(host, ftp_port)
    print(f"[{'ok' if ftp_ok else 'FAIL'}] FTP {host}:{ftp_port}")
    if not ftp_ok:
        print(
            "       FTP is required to upload files. Start your FTP server on the PS5\n"
            "       (etaHEN's embedded FTP is 1337; zftpd is often 2122) and confirm the\n"
            "       IP. Pass --ftp-port if yours differs."
        )

    # Informational only -- not required for install, handy for diagnosing the session.
    for label, port in (("ps5debug-ng", DEFAULT_PS5DEBUG_PORT), ("etaHEN klog", DEFAULT_KLOG_PORT)):
        ok = _port_open(host, port)
        print(f"[{'ok' if ok else '--'}] {label} {host}:{port} (optional)")

    if strict and not ftp_ok:
        raise InstallError("preflight failed: FTP unreachable")
    return ftp_ok


# ----------------------------------------------------------------------------- upload


def _ftp_connect(host: str, port: int) -> ftplib.FTP:
    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=15)
    ftp.login()  # most PS5 FTP servers accept anonymous login
    return ftp


def _ensure_remote_dirs(ftp: ftplib.FTP, remote_path: str) -> None:
    """MKD each parent of ``remote_path`` (ignoring 'already exists' errors)."""
    parts = [p for p in remote_path.split("/")[:-1] if p]
    path = ""
    for part in parts:
        path += "/" + part
        with contextlib.suppress(ftplib.error_perm):
            ftp.mkd(path)  # error_perm: already exists, or no-permission-but-present


def install(bundle: Path, host: str, ftp_port: int) -> int:
    manifest = load_manifest(bundle)
    validate_bundle_safety(bundle, manifest)
    entries = upload_entries(manifest)
    if not entries:
        raise InstallError("manifest lists no uploadable files")

    preflight(host, ftp_port, strict=True)

    uploaded = 0
    ftp = _ftp_connect(host, ftp_port)
    try:
        for entry in entries:
            local = bundle / entry["path"]
            remote = entry["remote"]
            if not local.is_file():
                raise InstallError(f"bundle file missing: {local}")
            _ensure_remote_dirs(ftp, remote)
            with local.open("rb") as fh:
                ftp.storbinary(f"STOR {remote}", fh)
            # Verify size where the server supports SIZE.
            expected = local.stat().st_size
            try:
                actual = ftp.size(remote)
            except ftplib.all_errors:
                actual = None
            if actual is not None and actual != expected:
                raise InstallError(f"size mismatch for {remote}: sent {expected}, server has {actual}")
            print(f"  uploaded {entry['path']} -> {remote} ({expected} bytes)")
            uploaded += 1
    finally:
        try:
            ftp.quit()
        except ftplib.all_errors:
            ftp.close()

    _print_post_install_guidance(manifest, host, uploaded)
    return 0


# ----------------------------------------------------------------------------- status


def _ftp_tail(ftp: ftplib.FTP, remote: str, max_lines: int) -> str | None:
    chunks: list[bytes] = []
    try:
        ftp.retrbinary(f"RETR {remote}", chunks.append)
    except ftplib.all_errors:
        return None
    text = b"".join(chunks).decode("utf-8", "replace")
    lines = text.splitlines()
    return "\n".join(lines[-max_lines:])


def status(bundle: Path, host: str, ftp_port: int, max_lines: int = 25) -> int:
    manifest = load_manifest(bundle)
    logs = [p for p in (manifest.get("loaderLog"), manifest.get("moduleLog")) if p]
    if not logs:
        logs = ["/data/etaHEN/plloader_plugin.log", "/data/etaHEN/gtav-menu-module.log"]

    if not preflight(host, ftp_port, strict=False):
        raise InstallError("status failed: FTP unreachable")

    ftp = _ftp_connect(host, ftp_port)
    try:
        for remote in logs:
            print(f"\n=== {remote} (last {max_lines} lines) ===")
            tail = _ftp_tail(ftp, remote, max_lines)
            print(tail if tail else "  (not present yet -- loader/module may not have run)")
    finally:
        try:
            ftp.quit()
        except ftplib.all_errors:
            ftp.close()
    print("\n(best-effort log read over FTP; this is not a live telemetry channel)")
    return 0


# ------------------------------------------------------------------------------- cli


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Install the GTAVMenu release bundle onto a PS5 over FTP")
    parser.add_argument("command", choices=("preflight", "install", "status"))
    parser.add_argument("--host", required=True, help="PS5 IP address")
    parser.add_argument("--ftp-port", type=int, default=DEFAULT_FTP_PORT)
    parser.add_argument("--bundle", type=Path, default=DEFAULT_BUNDLE)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "preflight":
            return 0 if preflight(args.host, args.ftp_port, strict=True) else 1
        if args.command == "install":
            return install(args.bundle, args.host, args.ftp_port)
        if args.command == "status":
            return status(args.bundle, args.host, args.ftp_port)
    except InstallError as exc:
        print(f"error: {exc}")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Build or verify a stock-envelope, pixel-only PS5 PTD candidate offline.

The source is never modified and output publication is create-new.  Success
proves only a bounded byte-preservation contract, not GPU or runtime behavior.
"""

from __future__ import annotations

import argparse
import json
import os
import tempfile
from pathlib import Path

from gtavmenu_tools.asset_formats import AssetError, Limits
from gtavmenu_tools.custom_pack_schema import MAX_JSON_BYTES, CustomPackSchemaError, read_regular
from gtavmenu_tools.stock_texture_envelope import (
    build_pc_stock_texture_envelope_candidate,
    build_stock_texture_envelope_candidate,
    build_visual_oracle_stock_texture_envelope_candidate,
    publish_pc_stock_texture_envelope_package,
    publish_visual_oracle_stock_texture_envelope_package,
    verify_pc_stock_texture_envelope_candidate,
    verify_pc_stock_texture_envelope_package,
    verify_stock_texture_envelope_candidate,
    verify_visual_oracle_stock_texture_envelope_candidate,
    verify_visual_oracle_stock_texture_envelope_package,
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TARGET = ROOT / "data/targets/ppsa04264-01.010.002.json"
LIMITS = Limits(max_file_bytes=256 * 1024 * 1024, max_total_bytes=1024 * 1024 * 1024, max_entries=64)


def _read(path: Path, maximum: int) -> bytes:
    return read_regular(path, maximum=maximum)


def _publish(path: Path, data: bytes) -> None:
    if path.exists() or path.is_symlink():
        raise AssetError(f"refusing to replace an existing candidate: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix=".stock-texture-", dir=path.parent) as temporary:
        temporary.write(data)
        temporary.flush()
        os.fsync(temporary.fileno())
        temporary.seek(0)
        if temporary.read(len(data) + 1) != data:
            raise AssetError("temporary candidate readback differs")
        os.link(temporary.name, path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build", help="create a new canonical candidate file")
    verify = commands.add_parser("verify", help="independently verify an existing candidate file")
    build_pc = commands.add_parser("build-from-pc", help="tile one exact PC texture into a stock envelope")
    verify_pc = commands.add_parser("verify-from-pc", help="verify a candidate against fresh original PC mips")
    package_pc = commands.add_parser(
        "build-package-from-pc",
        help="create one inactive source-bound package around a stock-envelope candidate",
    )
    verify_package_pc = commands.add_parser(
        "verify-package-from-pc",
        help="rebuild both source bindings and compare every inactive package byte",
    )
    build_visual = commands.add_parser(
        "build-visual-oracle",
        help="generate and tile an exact BC3 quadrant oracle into a stock envelope",
    )
    verify_visual = commands.add_parser(
        "verify-visual-oracle",
        help="freshly regenerate, untile and decode an existing BC3 quadrant candidate",
    )
    package_visual = commands.add_parser(
        "build-visual-oracle-package",
        help="create one inactive stock-bound BC3 visual-oracle package",
    )
    verify_package_visual = commands.add_parser(
        "verify-visual-oracle-package",
        help="freshly rebuild and compare every visual-oracle package byte",
    )
    for command in (build, verify):
        command.add_argument("source", type=Path, help="strict stock PS5 v5 PTD")
        command.add_argument("--texture", required=True, help="exact ordinary texture name")
        command.add_argument(
            "--replacement",
            type=Path,
            required=True,
            help="exact same-size, already-tiled complete graphics allocation",
        )
    build.add_argument("--output", type=Path, required=True, help="new candidate PTD; never replaced")
    verify.add_argument("--candidate", type=Path, required=True)
    for command in (build_pc, verify_pc, package_pc, verify_package_pc):
        command.add_argument("stock", type=Path, help="strict stock PS5 v5 PTD")
        command.add_argument("--stock-texture", required=True, help="exact stock texture name")
        command.add_argument("--pc-source", type=Path, required=True, help="original complete PC Legacy YTD")
        command.add_argument("--pc-texture", required=True, help="exact PC source texture name")
    build_pc.add_argument("--output", type=Path, required=True, help="new candidate PTD; never replaced")
    verify_pc.add_argument("--candidate", type=Path, required=True)
    for command in (package_pc, verify_package_pc):
        command.add_argument("--target", type=Path, default=DEFAULT_TARGET, help="exact target manifest")
        command.add_argument("--pack-id", required=True)
        command.add_argument("--logical-name", required=True)
        command.add_argument(
            "--resource-path",
            help="canonical path below resources/ (default: resources/<logical-name>.ptd)",
        )
    package_pc.add_argument("--output-dir", type=Path, required=True, help="new package directory; never replaced")
    verify_package_pc.add_argument("--package", type=Path, required=True)
    for command in (build_visual, verify_visual, package_visual, verify_package_visual):
        command.add_argument("stock", type=Path, help="strict stock PS5 v5 PTD")
        command.add_argument("--stock-texture", required=True, help="exact one-mip BC3 stock texture name")
    build_visual.add_argument("--output", type=Path, required=True, help="new candidate PTD; never replaced")
    verify_visual.add_argument("--candidate", type=Path, required=True)
    for command in (package_visual, verify_package_visual):
        command.add_argument("--target", type=Path, default=DEFAULT_TARGET, help="exact target manifest")
        command.add_argument("--pack-id", required=True)
        command.add_argument("--logical-name", required=True)
        command.add_argument(
            "--resource-path",
            help="canonical path below resources/ (default: resources/<logical-name>.ptd)",
        )
    package_visual.add_argument("--output-dir", type=Path, required=True, help="new package directory; never replaced")
    verify_package_visual.add_argument("--package", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command in ("build", "verify"):
            source = _read(args.source, LIMITS.max_file_bytes)
            replacement = _read(args.replacement, LIMITS.max_file_bytes)
        elif args.command in (
            "build-visual-oracle",
            "verify-visual-oracle",
            "build-visual-oracle-package",
            "verify-visual-oracle-package",
        ):
            stock = _read(args.stock, LIMITS.max_file_bytes)
            if args.command in ("build-visual-oracle-package", "verify-visual-oracle-package"):
                target_manifest = _read(args.target, MAX_JSON_BYTES)
        else:
            stock = _read(args.stock, LIMITS.max_file_bytes)
            pc_source = _read(args.pc_source, LIMITS.max_file_bytes)
            if args.command in ("build-package-from-pc", "verify-package-from-pc"):
                target_manifest = _read(args.target, MAX_JSON_BYTES)
        if args.command == "build":
            candidate, report = build_stock_texture_envelope_candidate(
                source, texture_name=args.texture, replacement=replacement, limits=LIMITS
            )
            _publish(args.output, candidate)
        elif args.command == "verify":
            report = verify_stock_texture_envelope_candidate(
                source,
                _read(args.candidate, LIMITS.max_file_bytes),
                texture_name=args.texture,
                replacement=replacement,
                limits=LIMITS,
            )
        elif args.command == "build-from-pc":
            candidate, report = build_pc_stock_texture_envelope_candidate(
                pc_source,
                stock,
                pc_texture_name=args.pc_texture,
                stock_texture_name=args.stock_texture,
                limits=LIMITS,
            )
            _publish(args.output, candidate)
        elif args.command == "verify-from-pc":
            report = verify_pc_stock_texture_envelope_candidate(
                pc_source,
                stock,
                _read(args.candidate, LIMITS.max_file_bytes),
                pc_texture_name=args.pc_texture,
                stock_texture_name=args.stock_texture,
                limits=LIMITS,
            )
        elif args.command == "build-package-from-pc":
            resource_path = args.resource_path or f"resources/{args.logical_name}.ptd"
            report = publish_pc_stock_texture_envelope_package(
                args.output_dir,
                pc_source,
                stock,
                target_manifest,
                pack_id=args.pack_id,
                resource_path=resource_path,
                logical_name=args.logical_name,
                pc_texture_name=args.pc_texture,
                stock_texture_name=args.stock_texture,
                limits=LIMITS,
            )
        elif args.command == "verify-package-from-pc":
            resource_path = args.resource_path or f"resources/{args.logical_name}.ptd"
            report = verify_pc_stock_texture_envelope_package(
                args.package,
                pc_source,
                stock,
                target_manifest,
                pack_id=args.pack_id,
                resource_path=resource_path,
                logical_name=args.logical_name,
                pc_texture_name=args.pc_texture,
                stock_texture_name=args.stock_texture,
                limits=LIMITS,
            )
        elif args.command == "build-visual-oracle":
            candidate, report = build_visual_oracle_stock_texture_envelope_candidate(
                stock,
                stock_texture_name=args.stock_texture,
                limits=LIMITS,
            )
            _publish(args.output, candidate)
        elif args.command == "verify-visual-oracle":
            report = verify_visual_oracle_stock_texture_envelope_candidate(
                stock,
                _read(args.candidate, LIMITS.max_file_bytes),
                stock_texture_name=args.stock_texture,
                limits=LIMITS,
            )
        elif args.command == "build-visual-oracle-package":
            resource_path = args.resource_path or f"resources/{args.logical_name}.ptd"
            report = publish_visual_oracle_stock_texture_envelope_package(
                args.output_dir,
                stock,
                target_manifest,
                pack_id=args.pack_id,
                resource_path=resource_path,
                logical_name=args.logical_name,
                stock_texture_name=args.stock_texture,
                limits=LIMITS,
            )
        else:
            resource_path = args.resource_path or f"resources/{args.logical_name}.ptd"
            report = verify_visual_oracle_stock_texture_envelope_package(
                args.package,
                stock,
                target_manifest,
                pack_id=args.pack_id,
                resource_path=resource_path,
                logical_name=args.logical_name,
                stock_texture_name=args.stock_texture,
                limits=LIMITS,
            )
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    except (AssetError, CustomPackSchemaError, OSError) as exc:
        print(f"stock texture envelope failed: {exc}", file=os.sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

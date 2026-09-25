#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Validate build directory and compilation configuration for clang-tidy ratchet lanes (ADR-1323).

Ensures that:
1. GPU lanes ('cuda', 'hip', 'sycl') use a build directory OUTSIDE the repository root,
   preventing generated translation units (e.g. *_hsaco.c, *.json.c) from polluting the measured
   source set.
2. GPU lanes are configured with -Db_lto=false (checked via meson-info/intro-buildoptions.json),
   avoiding compiler option rejection by clang (such as -flto=4).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Sequence

GPU_LANES = {"cuda", "hip", "sycl"}


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--lane",
        default="cpu",
        help="ratchet lane (cpu, cuda, hip, sycl, arm64)",
    )
    parser.add_argument(
        "--build-dir",
        required=True,
        type=Path,
        help="path to meson build directory",
    )
    parser.add_argument(
        "--repo-root",
        default=Path(),
        type=Path,
        help="path to repository root",
    )
    return parser.parse_args(argv)


def check_build_dir(lane: str, build_dir: Path, repo_root: Path) -> list[str]:
    """Validate build directory configuration for the given lane. Returns error messages."""
    errors: list[str] = []
    lane_norm = lane.lower().strip()
    build_dir_resolved = build_dir.resolve()
    repo_root_resolved = repo_root.resolve()

    if lane_norm in GPU_LANES:
        # 1. Enforce out-of-repo build directory
        try:
            is_inside = (
                build_dir_resolved == repo_root_resolved
                or build_dir_resolved.is_relative_to(repo_root_resolved)
            )
        except AttributeError:
            # Python < 3.9 fallback if ever run under older pythons
            try:
                build_dir_resolved.relative_to(repo_root_resolved)
                is_inside = True
            except ValueError:
                is_inside = False

        if is_inside:
            errors.append(
                f"lane '{lane_norm}' requires an out-of-repo build directory (e.g. /tmp/tidy-{lane_norm}) "
                f"to prevent generated files from entering the measured source set (ADR-1323). "
                f"Current build directory '{build_dir}' is inside repository '{repo_root_resolved}'."
            )

    # 2. Enforce -Db_lto=false
    options_path = build_dir_resolved / "meson-info" / "intro-buildoptions.json"
    if not options_path.exists():
        errors.append(
            f"cannot inspect build options for lane '{lane_norm}': "
            f"'{options_path}' not found. Ensure the directory was configured with 'meson setup' (ADR-1323)."
        )
    else:
        try:
            options = json.loads(options_path.read_text(encoding="utf-8"))
            b_lto_opt = next((opt for opt in options if opt.get("name") == "b_lto"), None)
            if b_lto_opt is None:
                errors.append(
                    f"lane '{lane_norm}' requires -Db_lto=false (ADR-1172, ADR-1323). "
                    f"Build directory '{build_dir}' is missing the b_lto option."
                )
            elif b_lto_opt.get("value") is not False:
                errors.append(
                    f"lane '{lane_norm}' requires -Db_lto=false (ADR-1172, ADR-1323). "
                    f"Build directory '{build_dir}' is configured with b_lto={b_lto_opt.get('value')}."
                )
        except Exception as exc:
            errors.append(f"failed to read build options from '{options_path}': {exc}")

    return errors


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    errors = check_build_dir(args.lane, args.build_dir, args.repo_root)
    if errors:
        for err in errors:
            print(f"check-tidy-build-dir: error: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

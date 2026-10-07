#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Build the one Rust archive libvmaf links, for Meson (ADR-1713).

Runs ``cargo build --release --locked --offline -p vmafx-core-rs`` against the
libvmaf-linked workspace (``core/src/rust/Cargo.toml``), copies ``libvmafx_core_rs.a`` to the Meson output and
writes a Meson depfile from cargo's dep-info, so ninja rebuilds the archive
exactly when a Rust source of its crates changes.

``--offline`` is deliberate: no crate libvmaf links may depend on anything
outside the workspace, so the build never needs the network (container and
distribution builds). A dependency that breaks this fails here, by name.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

CRATE = "vmafx-core-rs"
ARCHIVE = "libvmafx_core_rs.a"
CARGO_TIMEOUT_SECONDS = 1800


def parse_args(argv: list[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cargo", required=True, help="cargo executable")
    ap.add_argument("--manifest", required=True, type=Path, help="core/src/rust/Cargo.toml")
    ap.add_argument("--target-dir", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path, help="archive Meson expects")
    ap.add_argument("--depfile", required=True, type=Path, help="Meson depfile to write")
    return ap.parse_args(argv)


def run_cargo(args: argparse.Namespace) -> int:
    cmd = [
        args.cargo,
        "build",
        "--release",
        "--locked",
        "--offline",
        "-p",
        CRATE,
        "--manifest-path",
        str(args.manifest),
        "--target-dir",
        str(args.target_dir),
    ]
    # Plain subprocess, not scripts/lib/safe_subprocess: that helper resolves
    # symlinks before exec, and a rustup install's `cargo` is a symlink to
    # rustup, which picks the tool from argv[0]. The argv is fixed above; the
    # executable is the one Meson found.
    try:
        proc = subprocess.run(cmd, check=False, timeout=CARGO_TIMEOUT_SECONDS)  # noqa: S603
    except subprocess.TimeoutExpired:
        print(
            f"build_staticlib: cargo did not finish in {CARGO_TIMEOUT_SECONDS} s", file=sys.stderr
        )
        return 1
    if proc.returncode != 0:
        print(f"build_staticlib: {' '.join(cmd)} failed ({proc.returncode})", file=sys.stderr)
    return proc.returncode


def write_depfile(dep_info: Path, output: Path, depfile: Path) -> int:
    """Rewrite cargo's ``<archive>: <deps>`` line with Meson's output as target."""

    try:
        text = dep_info.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"build_staticlib: cannot read {dep_info}: {exc}", file=sys.stderr)
        return 1
    head, sep, deps = text.partition(": ")
    if not sep or not head:
        print(f"build_staticlib: unexpected dep-info format in {dep_info}", file=sys.stderr)
        return 1
    target = str(output).replace(" ", "\\ ")
    depfile.write_text(f"{target}: {deps.splitlines()[0] if deps else ''}\n", encoding="utf-8")
    return 0


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    rc = run_cargo(args)
    if rc != 0:
        return rc
    built = args.target_dir / "release" / ARCHIVE
    if not built.is_file():
        print(f"build_staticlib: cargo produced no {built}", file=sys.stderr)
        return 1
    shutil.copyfile(built, args.output)
    return write_depfile(built.with_suffix(".d"), args.output, args.depfile)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

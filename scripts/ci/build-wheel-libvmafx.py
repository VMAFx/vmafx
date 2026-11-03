#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Build libvmafx shared library for bundling into Python binary wheels."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

MESON_TIMEOUT_S = 3600


def repo_root() -> Path:
    """Return the repository root directory."""
    return Path(__file__).resolve().parents[2]


def clean_build_artifacts(root: Path) -> int:
    """Clean the wheel build directory and bundled libraries."""
    build_dir = root / "core" / "build-wheel"
    if build_dir.exists():
        shutil.rmtree(build_dir, ignore_errors=True)
    pkg_dir = root / "bindings" / "python" / "vmafx"
    if pkg_dir.exists():
        for p in pkg_dir.iterdir():
            if p.suffix in {".so", ".dylib", ".dll"} or ".so." in p.name:
                p.unlink(missing_ok=True)
    return 0


def ensure_build_tools() -> tuple[str, str]:
    """Locate or install meson and ninja."""
    meson_bin = shutil.which("meson")
    ninja_bin = shutil.which("ninja")
    if not meson_bin or not ninja_bin:
        lock = Path(__file__).resolve().parents[2] / "requirements" / "locks" / "build.txt"
        subprocess.run(  # noqa: S603 -- running interpreter, argv list, no shell
            [sys.executable, "-m", "pip", "install", "--require-hashes", "-r", str(lock)],
            check=True,
            timeout=MESON_TIMEOUT_S,
        )
        meson_bin = shutil.which("meson") or "meson"
        ninja_bin = shutil.which("ninja") or "ninja"
    return meson_bin, ninja_bin


def setup_meson(root: Path, meson_bin: str) -> None:
    """Configure core/build-wheel with minimal dependencies for wheels."""
    build_dir = root / "core" / "build-wheel"
    cmd = [
        meson_bin,
        "setup",
        str(root / "core"),
        str(build_dir),
        "--buildtype=release",
        "--default-library=shared",
        "-Denable_float=true",
        "-Denable_cuda=false",
        "-Denable_sycl=false",
        "-Denable_hip=false",
        "-Denable_tests=false",
        "-Denable_docs=false",
        "-Denable_tools=false",
        "-Denable_zimg=false",
    ]
    if sys.platform == "darwin":
        cmd.append("-Denable_metal=disabled")
    if build_dir.exists():
        cmd.append("--reconfigure")
    subprocess.run(cmd, check=True, timeout=MESON_TIMEOUT_S)  # noqa: S603 -- meson argv, no shell


def compile_library(root: Path, meson_bin: str) -> None:
    """Compile the vmafx shared library target."""
    build_dir = root / "core" / "build-wheel"
    subprocess.run(  # noqa: S603 -- resolved meson path, argv list, no shell
        [meson_bin, "compile", "-C", str(build_dir), "vmafx"],
        check=True,
        timeout=MESON_TIMEOUT_S,
    )


def find_built_library(build_dir: Path) -> list[Path]:
    """Locate all produced shared library files in the build tree."""
    candidates: list[Path] = []
    for candidate in build_dir.rglob("*"):
        if not candidate.is_file():
            continue
        name = candidate.name
        if (
            (sys.platform == "darwin" and (name.endswith(".dylib") and "vmafx" in name))
            or (sys.platform == "win32" and (name.endswith(".dll") and "vmafx" in name))
            or (sys.platform.startswith("linux") and ("libvmafx.so" in name))
        ):
            candidates.append(candidate)
    return candidates


def copy_bundled_libraries(root: Path) -> list[Path]:
    """Copy compiled shared libraries into bindings/python/vmafx/."""
    build_dir = root / "core" / "build-wheel"
    pkg_dir = root / "bindings" / "python" / "vmafx"
    pkg_dir.mkdir(parents=True, exist_ok=True)
    built = find_built_library(build_dir)
    if not built:
        raise RuntimeError(f"No built libvmafx library found in {build_dir}")

    copied: list[Path] = []
    for src in built:
        dest = pkg_dir / src.name
        # Copy real content even if src is symlink
        shutil.copyfile(src.resolve(), dest)
        dest.chmod(0o755)
        copied.append(dest)

    # Ensure canonical name exists on Linux/macOS
    if sys.platform.startswith("linux"):
        for name in ("libvmafx.so.1", "libvmafx.so"):
            dest = pkg_dir / name
            if not dest.exists():
                shutil.copyfile(copied[0], dest)
                dest.chmod(0o755)
                copied.append(dest)
    elif sys.platform == "darwin":
        for name in ("libvmafx.1.dylib", "libvmafx.dylib"):
            dest = pkg_dir / name
            if not dest.exists():
                shutil.copyfile(copied[0], dest)
                dest.chmod(0o755)
                copied.append(dest)
    return copied


def main() -> int:
    """Build and stage libvmafx for wheel bundling."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--clean",
        action="store_true",
        help="Remove build artifacts and bundled libraries",
    )
    args = parser.parse_args()
    root = repo_root()
    if args.clean:
        return clean_build_artifacts(root)

    meson_bin, _ = ensure_build_tools()
    setup_meson(root, meson_bin)
    compile_library(root, meson_bin)
    copied = copy_bundled_libraries(root)
    for c in copied:
        print(f"Bundled {c.relative_to(root)} ({c.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

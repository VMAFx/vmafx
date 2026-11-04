#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Build docs from packaged Rust crates, enforce size and documentation-header drift."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tarfile
import tempfile
from collections.abc import Callable
from pathlib import Path, PurePosixPath
from typing import IO

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import (  # noqa: E402
    CommandFailed,
    CommandResult,
    CommandTimedOut,
    CommandValidationError,
)
from scripts.lib.safe_subprocess import run as safe_run  # noqa: E402

CRATES = ("vmafx-sys", "vmafx")
MAX_CRATE_BYTES = 10 * 1024 * 1024
MAX_UNCOMPRESSED_CRATE_BYTES = 64 * 1024 * 1024
MAX_CRATE_MEMBERS = 2048
CHUNK_BYTES = 64 * 1024
DOC_HEADERS = ("libvmaf.h", "feature.h", "model.h", "picture.h", "macros.h")
Runner = Callable[..., CommandResult]


class PackageError(Exception):
    """A named package acceptance failure."""


def sync_headers(root: Path, *, write: bool = False) -> None:
    """Copy only bindgen's public header closure; preserve upstream licence notices."""
    source = root / "core/include/libvmaf"
    destination = root / "bindings/rust/vmafx-sys/include/libvmaf"
    if write:
        destination.mkdir(parents=True, exist_ok=True)
    for name in DOC_HEADERS:
        expected = (source / name).read_bytes()
        target = destination / name
        if write:
            target.write_bytes(expected)
        elif not target.is_file() or target.read_bytes() != expected:
            raise PackageError(f"header drift: {target}; run this script with --sync-headers")


def run_command(
    command: list[str], cwd: Path, env: dict[str, str], runner: Runner, stage: str
) -> str:
    """Run bounded commands and preserve their diagnostics under a named failure."""
    try:
        result = runner(
            command,
            allowed_executables=(command[0],),
            cwd=cwd,
            env=env,
            check=True,
            capture_output=True,
            text=True,
            timeout_seconds=600,
        )
    except CommandFailed as error:
        stdout = (
            error.result.stdout
            if isinstance(error.result.stdout, str)
            else error.result.stdout.decode(errors="replace")
        )
        stderr = (
            error.result.stderr
            if isinstance(error.result.stderr, str)
            else error.result.stderr.decode(errors="replace")
        )
        raise PackageError(f"{stage}: {stdout or ''}{stderr or ''}") from error
    except (OSError, CommandTimedOut, CommandValidationError) as error:
        raise PackageError(f"{stage}: {error}") from error
    return result.stdout if isinstance(result.stdout, str) else result.stdout.decode()


def resolve_executable(name: str) -> Path:
    """Resolve an executable or environment override to an absolute path."""
    override = os.environ.get(name.upper()) or name
    candidate: str | None = override
    if "/" not in override and "\\" not in override:
        candidate = shutil.which(override)
    if candidate is None:
        raise PackageError(f"required executable not found on PATH: {name}")
    return Path(candidate).absolute()


def resolve_cargo(rust_bin: Path) -> Path:
    """Resolve cargo to an absolute path, preferring the active toolchain."""
    suffix = ".exe" if os.name == "nt" else ""
    toolchain_cargo = rust_bin / f"cargo{suffix}"
    if toolchain_cargo.is_file():
        return toolchain_cargo.absolute()
    from_env = os.environ.get("CARGO")
    if from_env:
        if "/" not in from_env and "\\" not in from_env:
            on_path = shutil.which(from_env)
            if on_path:
                return Path(on_path).absolute()
        elif Path(from_env).is_file():
            return Path(from_env).absolute()
    on_path = shutil.which("cargo")
    if on_path:
        return Path(on_path).absolute()
    raise PackageError("required executable not found on PATH: cargo")


def docs_environment(root: Path, temporary: Path, runner: Runner) -> dict[str, str]:
    """Keep the checkout's compiler and expose only Rust and CPU build tools on PATH."""
    env = os.environ.copy()
    rustc = resolve_executable("rustc")
    sysroot = run_command([str(rustc), "--print", "sysroot"], root, env, runner, "Rust toolchain")
    rust_bin = Path(sysroot.strip()) / "bin"
    tools = temporary / "tools"
    tools.mkdir()
    suffix = ".exe" if os.name == "nt" else ""
    for name in ("cargo", "rustc", "rustdoc"):
        executable = rust_bin / f"{name}{suffix}"
        (tools / executable.name).symlink_to(executable)
    for name in ("cc", "gcc", "clang", "ld", "lld", "ld.lld", "as", "ar", "ranlib", "llvm-config"):
        executable_path = shutil.which(name)
        if executable_path:
            (tools / f"{name}{suffix}").symlink_to(executable_path)
    cargo = resolve_cargo(rust_bin)
    env.update(
        PATH=str(tools),
        RUSTC=str(rust_bin / f"rustc{suffix}"),
        RUSTDOC=str(rust_bin / f"rustdoc{suffix}"),
        CARGO=str(cargo),
        DOCS_RS="1",
        CARGO_BUILD_JOBS="4",
        CARGO_TARGET_DIR=str(temporary / "target"),
    )
    return env


def check_size(archive: Path) -> None:
    """Acceptance requires strictly less than 10 MiB, including at the boundary."""
    size = archive.stat().st_size
    if size >= MAX_CRATE_BYTES:
        raise PackageError(f"oversized crate: {archive.name}: {size} bytes >= {MAX_CRATE_BYTES}")


def safe_member(member: tarfile.TarInfo, crate_name: str) -> bool:
    path = PurePosixPath(member.name)
    return (
        not path.is_absolute()
        and ".." not in path.parts
        and "\\" not in member.name
        and bool(path.parts)
        and path.parts[0] == crate_name
        and (member.isfile() or member.isdir())
    )


def extract_member(source: IO[bytes], target: Path, max_size: int, member_name: str) -> None:
    """Copy member bytes with bounded chunks, enforcing the declared size."""
    target.parent.mkdir(parents=True, exist_ok=True)
    copied = 0
    max_chunks = max_size // CHUNK_BYTES + 2
    with target.open("wb") as output:
        for _ in range(max_chunks):
            chunk = source.read(CHUNK_BYTES)
            if not chunk:
                return
            copied += len(chunk)
            if copied > max_size:
                raise PackageError(f"member expanded beyond declared size: {member_name}")
            output.write(chunk)
    raise PackageError(f"member expanded beyond declared size: {member_name}")


def unpack_crate(archive: Path, destination: Path) -> Path:
    """Extract bounded regular files/directories under the expected crate root."""
    crate = destination / archive.stem
    member_count = 0
    total_uncompressed = 0
    with tarfile.open(archive, "r:gz") as contents:
        for member in contents:
            member_count += 1
            if member_count > MAX_CRATE_MEMBERS:
                raise PackageError(f"too many crate members: {archive.name}")
            if not safe_member(member, archive.stem):
                raise PackageError(f"unsafe crate member: {member.name}")
            if member.size < 0:
                raise PackageError(f"invalid member size: {member.name}")
            total_uncompressed += member.size
            if total_uncompressed > MAX_UNCOMPRESSED_CRATE_BYTES:
                raise PackageError(f"oversized uncompressed crate: {archive.name}")
            target = destination.joinpath(*PurePosixPath(member.name).parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            source = contents.extractfile(member)
            if source is None:
                raise PackageError(f"unreadable crate member: {member.name}")
            with source:
                extract_member(source, target, member.size, member.name)
    if not (crate / "Cargo.toml").is_file():
        raise PackageError(f"missing packaged Cargo.toml: {archive.name}")
    return crate


def packaged_crate(package_dir: Path, name: str) -> Path:
    archives = list(package_dir.glob(f"{name}-[0-9]*.crate"))
    if len(archives) != 1:
        raise PackageError(f"missing or ambiguous packaged crate: {name}")
    archive = archives[0]
    check_size(archive)
    print(f"{name}: {archive.stat().st_size} bytes < {MAX_CRATE_BYTES}", flush=True)
    return archive


def check_packages(root: Path = ROOT, runner: Runner = safe_run) -> None:
    """Package both unpublished crates together, then document their extracted contents."""
    with tempfile.TemporaryDirectory(prefix="vmafx-crate-package-") as directory:
        temporary = Path(directory)
        if temporary.resolve().is_relative_to(root.resolve()):
            raise PackageError(
                "temporary directory must be outside the checkout; choose another TMPDIR"
            )
        env = docs_environment(root, temporary, runner)
        cargo = env["CARGO"]
        command = [cargo, "package", "--allow-dirty", "-p", CRATES[0], "-p", CRATES[1], "-j4"]
        run_command(command, root, env, runner, "cargo package failed")
        extracted = {}
        for name in CRATES:
            archive = packaged_crate(temporary / "target/package", name)
            extracted[name] = unpack_crate(archive, temporary / "extracted")
        for name, crate in extracted.items():
            command = [cargo, "doc", "--no-deps", "-j4"]
            if name == "vmafx":
                # The dependency is unpublished: patch only to the extracted sys crate.
                patch = json.dumps(str(extracted["vmafx-sys"]))
                command.extend(["--config", f"patch.crates-io.vmafx-sys.path={patch}"])
            run_command(command, crate, env, runner, f"DOCS_RS doc build failed ({name})")
            print(f"{name}: packaged DOCS_RS documentation passed (CPU-only PATH)", flush=True)
        sync_headers(root)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--sync-headers", action="store_true", help="refresh the packaged headers")
    mode.add_argument(
        "--check-headers", action="store_true", help="only check the packaged headers for drift"
    )
    args = parser.parse_args()
    try:
        if args.sync_headers:
            sync_headers(ROOT, write=True)
        elif args.check_headers:
            sync_headers(ROOT)
        else:
            check_packages()
    except (PackageError, OSError, tarfile.TarError) as error:
        print(f"rust-crate-package: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

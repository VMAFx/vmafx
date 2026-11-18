#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Install a digest-pinned Linux ONNX Runtime NuGet package."""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
import re
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any, BinaryIO
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from scripts.lib.safe_subprocess import CommandFailed
from scripts.lib.safe_subprocess import run as run_command

PINS = Path(__file__).with_name("ort-nuget-pins.json")
ATTEMPTS = 3
TIMEOUT = 300
CHUNK = 65536
MAX_MEMBERS = 4096
MAX_EXPANDED = 1024 * 1024 * 1024
LIBRARIES = ("libonnxruntime.so", "libonnxruntime_providers_shared.so")
GPU_LIBRARIES = (
    *LIBRARIES,
    "libonnxruntime_providers_cuda.so",
    "libonnxruntime_providers_tensorrt.so",
)
NOTICES = ("LICENSE", "ThirdPartyNotices.txt")


def verify_file(path: Path, size: int, expected: str) -> bool:
    """Return True when path matches the pinned byte length and SHA-256."""
    if not path.is_file() or path.stat().st_size != size:
        return False
    with path.open("rb") as handle:
        return verify_handle(handle, size, expected)


def verify_handle(handle: BinaryIO, size: int, expected: str) -> bool:
    """Verify the same open file that will be passed to the ZIP reader."""
    digest = hashlib.sha256()
    count = 0
    for _ in range(size // CHUNK + 2):
        data = handle.read(CHUNK)
        if not data:
            break
        count += len(data)
        digest.update(data)
    handle.seek(0)
    return count == size and digest.hexdigest() == expected


def download(address: str, output: Path, size: int, expected: str) -> None:
    """Stream a bounded HTTPS response and verify it before use."""
    url = urlsplit(address)
    if url.scheme != "https" or not url.hostname or url.username or url.password:
        raise ValueError(f"invalid HTTPS source: {address}")
    for attempt in range(ATTEMPTS):
        try:
            conn = http.client.HTTPSConnection(url.hostname, url.port, timeout=TIMEOUT)
            try:
                conn.request("GET", url.path)
                response = conn.getresponse()
                if response.status in (404, 410):
                    raise ValueError(f"source gone (HTTP {response.status}): {address}")
                if response.status != http.client.OK:
                    raise OSError(f"HTTP {response.status}: {address}")
                save_body(response, output, size, expected)
                return
            finally:
                conn.close()
        except (OSError, http.client.HTTPException) as exc:
            output.unlink(missing_ok=True)
            if attempt + 1 == ATTEMPTS:
                raise ValueError(f"download failed after {ATTEMPTS} attempts: {exc}") from exc
        except ValueError:
            output.unlink(missing_ok=True)
            raise


def save_body(response: http.client.HTTPResponse, output: Path, size: int, expected: str) -> None:
    """Bound the response by its pinned length and check its digest."""
    digest = hashlib.sha256()
    count = 0
    length = getattr(response, "length", None)
    with output.open("wb") as out:
        for _ in range(size // CHUNK + 2):
            data = response.read(min(CHUNK, size + 1 - count))
            if not data:
                break
            count += len(data)
            out.write(data)
            digest.update(data)
            if count > size:
                break
    if count <= size and length is not None and count < length:
        raise http.client.IncompleteRead(b"", length - count)
    if count != size:
        raise ValueError(f"size mismatch: expected {size}, actual {count}")
    actual = digest.hexdigest()
    if actual != expected:
        raise ValueError(f"sha256 mismatch: expected {expected}, actual {actual}")


def fetch_cached(address: str, cache_dir: Path, size: int, expected: str) -> Path:
    """Reuse a verified cached package or delete and re-download once."""
    cache_dir.mkdir(parents=True, exist_ok=True)
    name = PurePosixPath(urlsplit(address).path).name or "package.nupkg"
    cached = cache_dir / name
    if cached.exists():
        if verify_file(cached, size, expected):
            return cached
        cached.unlink(missing_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".ort-", dir=cache_dir)
    os.close(fd)
    private = Path(temporary)
    try:
        download(address, private, size, expected)
        private.replace(cached)
    finally:
        private.unlink(missing_ok=True)
    return cached


def measure_pin(address: str) -> tuple[str, int]:
    """Stream an HTTPS package to compute its SHA-256 and byte size."""
    url = urlsplit(address)
    if url.scheme != "https" or not url.hostname or url.username or url.password:
        raise ValueError(f"invalid HTTPS source: {address}")
    conn = http.client.HTTPSConnection(url.hostname, url.port, timeout=TIMEOUT)
    try:
        conn.request("GET", url.path)
        response = conn.getresponse()
        if response.status != http.client.OK:
            raise ValueError(f"HTTP {response.status}: {address}")
        digest = hashlib.sha256()
        count = 0
        length = getattr(response, "length", None)
        for _ in range(MAX_EXPANDED // CHUNK + 2):
            data = response.read(CHUNK)
            if not data:
                if length is not None and count < length:
                    raise http.client.IncompleteRead(b"", length - count)
                return digest.hexdigest(), count
            count += len(data)
            if count > MAX_EXPANDED:
                raise ValueError("package size limit exceeded")
            digest.update(data)
        raise ValueError("package size limit exceeded")
    finally:
        conn.close()


def _member_dest(name: str, header_parts: tuple[str, ...], lib_paths: set[str]) -> Path | None:
    """Map an archive member path to its relative install destination."""
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "\\" in name:
        raise ValueError(f"unsafe member path: {name}")
    if path.parts[:-1] == header_parts and path.suffix == ".h":
        return Path("include") / path.name
    if name in lib_paths:
        return Path("lib") / path.name
    if name in NOTICES:
        return Path(name)
    return None


def selected_members(
    archive: zipfile.ZipFile,
    header_dir: str = "build/native/include",
    libraries: tuple[str, ...] = LIBRARIES,
) -> list[tuple[zipfile.ZipInfo, Path]]:
    """Validate every name and select only required files, before extraction."""
    infos = archive.infolist()
    if len(infos) > MAX_MEMBERS:
        raise ValueError(f"too many members: {len(infos)}")
    if sum(info.file_size for info in infos) > MAX_EXPANDED:
        raise ValueError("expanded size limit exceeded")
    header_parts = PurePosixPath(header_dir).parts
    lib_paths = {f"runtimes/linux-x64/native/{lib}" for lib in libraries}
    selected: list[tuple[zipfile.ZipInfo, Path]] = []
    seen: set[Path] = set()
    for info in infos:
        dest = _member_dest(info.filename, header_parts, lib_paths)
        if not info.is_dir() and dest is not None:
            if dest in seen:
                raise ValueError(f"duplicate member: {info.filename}")
            seen.add(dest)
            selected.append((info, dest))
    required = {Path("lib") / lib for lib in libraries} | {Path(n) for n in NOTICES}
    if not required.issubset(seen) or not any(p.parts[0] == "include" for p in seen):
        raise ValueError("package missing required headers, libraries or notices")
    return selected


def soname(library: Path) -> str:
    """Read the ELF SONAME and reject names that could escape lib/."""
    try:
        result = run_command(
            ["readelf", "-d", str(library)],
            allowed_executables={"readelf"},
            capture_output=True,
            text=True,
            check=True,
            timeout_seconds=30,
        )
    except CommandFailed as exc:
        detail = exc.result.stderr
        if isinstance(detail, bytes):
            detail = detail.decode(errors="replace")
        raise ValueError(f"readelf failed: {detail}") from exc
    match = re.search(r"\(SONAME\).*\[([^]]+)\]", result.stdout)
    if not match or not re.fullmatch(r"libonnxruntime\.so(?:\.[A-Za-z0-9_-]+)+", match.group(1)):
        raise ValueError("missing or unsafe libonnxruntime SONAME")
    return match.group(1)


def install(
    archive_path: Path | BinaryIO,
    prefix: Path,
    pkgconfig: Path,
    version: str,
    header_dir: str = "build/native/include",
    libraries: tuple[str, ...] = LIBRARIES,
) -> None:
    """Extract selected package files and expose the native loader name."""
    with zipfile.ZipFile(archive_path) as archive:
        members = selected_members(archive, header_dir=header_dir, libraries=libraries)
        for info, dest in members:
            target = prefix / dest
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as src, target.open("wb") as dst:
                shutil.copyfileobj(src, dst, CHUNK)
    lib = prefix / "lib" / libraries[0]
    link = lib.parent / soname(lib)
    link.unlink(missing_ok=True)
    link.symlink_to(lib.name)
    pkgconfig.mkdir(parents=True, exist_ok=True)
    text = (
        f"prefix={prefix}\nexec_prefix=${{prefix}}\nlibdir=${{exec_prefix}}/lib\n"
        f"includedir=${{prefix}}/include\n\nName: libonnxruntime\n"
        f"Description: ONNX Runtime C API\nVersion: {version}\n"
        "Libs: -L${libdir} -lonnxruntime\nCflags: -I${includedir}\n"
    )
    (pkgconfig / "libonnxruntime.pc").write_text(text, encoding="utf-8")


def _load_package(package_name: str) -> dict[str, Any]:
    """Load the package configuration entry from the pin file."""
    table = json.loads(PINS.read_text(encoding="utf-8"))
    packages = table.get("packages")
    if isinstance(packages, dict):
        pkg = packages.get(package_name)
    elif package_name == "cpu" and "versions" in table:
        pkg = {
            "source": table.get("source", ""),
            "header_dir": "build/native/include",
            "libraries": list(LIBRARIES),
            "versions": table["versions"],
        }
    else:
        pkg = None
    if not isinstance(pkg, dict):
        raise ValueError(f"unknown ONNX Runtime package: {package_name}")
    return pkg


def _run_install(pkg: dict[str, Any], args: argparse.Namespace) -> None:
    """Verify the requested version pin, fetch the archive, and install it."""
    if not args.version or args.prefix is None:
        raise ValueError("--version and --prefix are required")
    pin = pkg["versions"].get(args.version)
    if pin is None:
        raise ValueError(f"unknown ONNX Runtime version: {args.version}")
    if not re.fullmatch(r"[0-9a-f]{64}", pin["sha256"]) or int(pin["size"]) <= 0:
        raise ValueError(f"malformed pin for {args.version}")
    address = pkg["source"].format(version=args.version)
    header_dir = str(pkg["header_dir"])
    libraries = tuple(pkg["libraries"])
    if args.cache_dir is not None:
        package = fetch_cached(address, args.cache_dir, int(pin["size"]), pin["sha256"])
        with package.open("rb") as handle:
            if not verify_handle(handle, int(pin["size"]), pin["sha256"]):
                raise ValueError("cached package size mismatch or sha256 mismatch")
            install(
                handle,
                args.prefix.resolve(),
                args.pkgconfig_dir,
                args.version,
                header_dir,
                libraries,
            )
        return
    with tempfile.TemporaryDirectory(prefix="ort-nuget-") as tmp:
        package = Path(tmp) / "package.nupkg"
        download(address, package, int(pin["size"]), pin["sha256"])
        install(
            package, args.prefix.resolve(), args.pkgconfig_dir, args.version, header_dir, libraries
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", default="cpu")
    parser.add_argument("--version", default=None)
    parser.add_argument("--prefix", type=Path, default=None)
    parser.add_argument("--pkgconfig-dir", type=Path, default=Path("/usr/local/lib/pkgconfig"))
    parser.add_argument("--cache-dir", type=Path, default=None)
    parser.add_argument("--print-pin", nargs="?", const="", default=None, metavar="VERSION")
    args = parser.parse_args(argv)
    try:
        pkg = _load_package(args.package)
        if args.print_pin is not None:
            version = args.print_pin or args.version
            if not version:
                raise ValueError("--print-pin requires a version")
            sha256, size = measure_pin(pkg["source"].format(version=version))
            print(json.dumps({version: {"sha256": sha256, "size": size}}))
            return 0
        _run_install(pkg, args)
    except (
        OSError,
        ValueError,
        KeyError,
        zipfile.BadZipFile,
        http.client.HTTPException,
        CommandFailed,
    ) as exc:
        print(f"ort_nuget_install: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

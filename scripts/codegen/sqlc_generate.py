#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Generate and check the sqlc query packages (ADR-2350 D1).

Every sqlc configuration listed in CONFIGS is generated with the sqlc release
pinned in build-config.env (SQLC_VERSION and the per-platform SHA-256 of its
release archive), and every generated Go file gets the fork's SPDX header,
which sqlc cannot emit. Generated files are committed and never hand-edited.

Usage::

    python3 scripts/codegen/sqlc_generate.py --write   # regenerate in place
    python3 scripts/codegen/sqlc_generate.py --check   # fail on any difference

sqlc is taken from $VMAFX_SQLC when set, else from the tool cache
(${XDG_CACHE_HOME:-~/.cache}/vmafx-tools/sqlc-<version>/sqlc), downloaded
there and verified against the pinned digest when missing. When no pinned
binary can be obtained (no digest for this platform, no network) --check exits
77 and prints why, so a gate that did not run is never reported as passing.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# sqlc configuration (relative to the repository) -> its output directories
# (relative to the configuration's directory).
CONFIGS: dict[str, tuple[str, ...]] = {
    "cmd/vmafx-controller/store/sqlc.yaml": ("pgdb",),
}

HEADER = "// SPDX-License-Identifier: EUPL-1.2\n// Copyright 2026 Lusoris\n\n"

SKIP = 77
DOWNLOAD_TIMEOUT_S = 120
GENERATE_TIMEOUT_S = 300
MAX_ARCHIVE_BYTES = 64 * 1024 * 1024
RELEASE_URL = "https://github.com/sqlc-dev/sqlc/releases/download/v{v}/sqlc_{v}_{os}_{arch}.tar.gz"


class Unavailable(Exception):
    """The pinned sqlc cannot be obtained here; the check is skipped."""


def build_config(path: Path) -> dict[str, str]:
    """Read KEY="value" lines of build-config.env."""
    values: dict[str, str] = {}
    pattern = re.compile(r'^([A-Z0-9_]+)="([^"]*)"')
    for line in path.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def platform_key() -> tuple[str, str]:
    """The (os, arch) names of sqlc's release archives for this host."""
    systems = {"linux": "linux", "darwin": "darwin"}
    arches = {"x86_64": "amd64", "amd64": "amd64", "aarch64": "arm64", "arm64": "arm64"}
    machine = platform.machine().lower()
    if sys.platform not in systems or machine not in arches:
        raise Unavailable(f"no pinned sqlc release for {sys.platform}/{machine}")
    return systems[sys.platform], arches[machine]


def pinned(config: dict[str, str]) -> tuple[str, str, str, str]:
    """(version, os, arch, sha256) of the pinned sqlc for this host."""
    version = config.get("SQLC_VERSION", "")
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise SystemExit("sqlc_generate: build-config.env needs SQLC_VERSION as X.Y.Z")
    os_name, arch = platform_key()
    digest = config.get(f"SQLC_SHA256_{os_name.upper()}_{arch.upper()}", "")
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise Unavailable(f"build-config.env pins no sqlc digest for {os_name}/{arch}")
    return version, os_name, arch, digest


def fetch(version: str, os_name: str, arch: str, digest: str, dest: Path) -> None:
    """Download the release archive, verify its digest, extract sqlc to dest."""
    url = RELEASE_URL.format(v=version, os=os_name, arch=arch)
    try:
        with urllib.request.urlopen(url, timeout=DOWNLOAD_TIMEOUT_S) as resp:  # noqa: S310
            data = resp.read(MAX_ARCHIVE_BYTES + 1)
    except OSError as exc:
        raise Unavailable(f"cannot download {url}: {exc}") from exc
    if len(data) > MAX_ARCHIVE_BYTES:
        raise SystemExit(f"sqlc_generate: {url} is larger than {MAX_ARCHIVE_BYTES} bytes")
    got = hashlib.sha256(data).hexdigest()
    if got != digest:
        raise SystemExit(f"sqlc_generate: {url} has SHA-256 {got}, build-config.env pins {digest}")
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        member = tar.getmember("sqlc")
        stream = tar.extractfile(member)
        if stream is None:
            raise SystemExit(f"sqlc_generate: {url} holds no sqlc binary")
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(stream.read())
    dest.chmod(0o755)


def sqlc_binary(config: dict[str, str]) -> Path:
    """The pinned sqlc, downloading it into the tool cache when missing."""
    version = config.get("SQLC_VERSION", "")
    explicit = os.environ.get("VMAFX_SQLC")
    if explicit:
        binary = Path(explicit)
    else:
        cache = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
        binary = cache / "vmafx-tools" / f"sqlc-{version}" / "sqlc"
        if not binary.exists():
            fetch(*pinned(config), binary)
    # The binary is the pinned, digest-checked sqlc or the one $VMAFX_SQLC names.
    out = subprocess.run(  # noqa: S603
        [str(binary), "version"], capture_output=True, text=True, timeout=60, check=False
    )
    if out.returncode != 0 or out.stdout.strip() != f"v{version}":
        raise SystemExit(f"sqlc_generate: {binary} reports {out.stdout.strip()!r}, need v{version}")
    return binary


def with_header(text: str) -> str:
    """A generated Go file with the SPDX header in front (idempotent)."""
    return text if text.startswith(HEADER) else HEADER + text


def generate(binary: Path, root: Path, config: str, outputs: tuple[str, ...]) -> dict[str, str]:
    """Generate one configuration in a scratch copy; return {path: text}."""
    src = root / config
    with tempfile.TemporaryDirectory(prefix="sqlc-") as tmp:
        work = Path(tmp) / "cfg"
        shutil.copytree(src.parent, work, ignore=shutil.ignore_patterns(*outputs))
        subprocess.run(  # noqa: S603 (the pinned sqlc, see sqlc_binary)
            [str(binary), "generate", "-f", str(work / src.name)],
            check=True,
            timeout=GENERATE_TIMEOUT_S,
        )
        files: dict[str, str] = {}
        for out in outputs:
            for path in sorted((work / out).glob("*.go")):
                rel = (src.parent / out / path.name).relative_to(root).as_posix()
                files[rel] = with_header(path.read_text(encoding="utf-8"))
        return files


def committed(root: Path, config: str, outputs: tuple[str, ...]) -> dict[str, str]:
    """The generated files the tree holds for one configuration."""
    files: dict[str, str] = {}
    base = (root / config).parent
    for out in outputs:
        for path in sorted((base / out).glob("*.go")):
            files[path.relative_to(root).as_posix()] = path.read_text(encoding="utf-8")
    return files


def differences(want: dict[str, str], have: dict[str, str]) -> list[str]:
    """Paths that are missing, stale or different."""
    out = [f"missing: {p}" for p in sorted(want.keys() - have.keys())]
    out += [f"stale: {p}" for p in sorted(have.keys() - want.keys())]
    out += [f"differs: {p}" for p in sorted(want.keys() & have.keys()) if want[p] != have[p]]
    return out


def write(root: Path, want: dict[str, str], have: dict[str, str]) -> None:
    """Make the tree hold exactly want."""
    for path in have.keys() - want.keys():
        (root / path).unlink()
    for path, text in want.items():
        (root / path).parent.mkdir(parents=True, exist_ok=True)
        (root / path).write_text(text, encoding="utf-8")


def run(root: Path, mode: str) -> int:
    """Generate every configuration and write or check it."""
    try:
        binary = sqlc_binary(build_config(root / "build-config.env"))
    except Unavailable as exc:
        if mode == "check":
            print(f"sqlc_generate: SKIP: {exc}")
            return SKIP
        raise SystemExit(f"sqlc_generate: {exc}") from exc
    problems: list[str] = []
    for config, outputs in CONFIGS.items():
        want = generate(binary, root, config, outputs)
        have = committed(root, config, outputs)
        if mode == "write":
            write(root, want, have)
        else:
            problems += differences(want, have)
    for line in problems:
        print(f"sqlc_generate: {line}")
    if problems:
        print("sqlc_generate: run python3 scripts/codegen/sqlc_generate.py --write")
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--write", action="store_true", help="regenerate in place")
    group.add_argument("--check", action="store_true", help="fail on any difference")
    parser.add_argument("--root", type=Path, default=REPO, help="repository root")
    args = parser.parse_args(argv)
    return run(args.root.resolve(), "write" if args.write else "check")


if __name__ == "__main__":
    sys.exit(main())

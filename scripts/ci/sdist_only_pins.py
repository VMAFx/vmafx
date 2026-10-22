#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Record the locked pins that have no wheel for the CI interpreter, and check their backends.

A hash-locked install of a pin with only an sdist builds it with
``--no-build-isolation``, so the build backend named in the sdist's
``[build-system] requires`` must itself be in the lock.  ``reuse==6.2.0`` ships a
cp310 wheel only; on Python 3.14 pip built its sdist and failed for want of
``poetry-core``.

``check`` is offline: it compares every sdist-only pin recorded in
``scripts/ci/sdist_only_pins.json`` with ``requirements/locks/package-build.in`` and
``package-build.txt``.  ``write`` is the networked refresh: it reads the package
index for every pin of every lock and rewrites the record.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import io
import json
import os
import re
import sys
import tarfile
import tomllib
import urllib.request
import zipfile
from pathlib import Path
from typing import Any

from packaging import markers, tags, utils

RECORD = Path("scripts/ci/sdist_only_pins.json")
BUILD_IN = Path("requirements/locks/package-build.in")
BUILD_LOCK = Path("requirements/locks/package-build.txt")
CI_PYTHON = (3, 14)
CI_GLIBC_MINORS = range(17, 40)  # ubuntu-24.04 runners: glibc 2.39
PIN_RE = re.compile(r"^([A-Za-z0-9][A-Za-z0-9_.-]*)==([^\s;\\]+)\s*(?:;\s*([^\\]*?))?\s*\\?$")
SKIP_DIRS = {"node_modules", ".git", ".venv", ".claude", ".corpus", ".workingdir"}
INDEX_TIMEOUT_S = 60
MAX_WORKERS = 8
MAX_SDIST_BYTES = 200_000_000
MAX_PINS = 5000


def ci_tags() -> set[tags.Tag]:
    """Wheel tags the CI interpreter (CPython 3.14, manylinux x86_64) accepts."""
    plats = [f"manylinux_2_{m}_x86_64" for m in CI_GLIBC_MINORS]
    plats += ["manylinux2014_x86_64", "manylinux2010_x86_64", "manylinux1_x86_64", "linux_x86_64"]
    found = set(tags.cpython_tags(CI_PYTHON, ["cp314"], plats))
    found |= set(tags.compatible_tags(CI_PYTHON, "cp314", plats))
    return found


CI_ENV = {
    "os_name": "posix",
    "sys_platform": "linux",
    "platform_system": "Linux",
    "platform_machine": "x86_64",
    "platform_python_implementation": "CPython",
    "implementation_name": "cpython",
    "python_version": "3.14",
    "python_full_version": "3.14.7",
}


def norm(name: str) -> str:
    return str(utils.canonicalize_name(name))


def lock_files(root: Path) -> list[Path]:
    """Every hash lock: ``requirements/locks/*.txt`` and each ``*-lock.txt``."""
    found: set[Path] = set((root / "requirements/locks").glob("*.txt"))
    for here, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.startswith("build")]
        found.update(Path(here) / f for f in files if f.endswith("-lock.txt"))
    return sorted(found)


def lock_pins(path: Path) -> set[tuple[str, str]]:
    """``(name, version)`` of each pin whose marker holds on the CI interpreter."""
    pins: set[tuple[str, str]] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        match = PIN_RE.match(line.strip())
        if match is None:
            continue
        marker = match.group(3)
        if marker and not markers.Marker(marker).evaluate(CI_ENV):
            continue
        pins.add((norm(match.group(1)), match.group(2)))
    return pins


def all_pins(root: Path) -> set[tuple[str, str]]:
    pins: set[tuple[str, str]] = set()
    for lock in lock_files(root):
        pins |= lock_pins(lock)
    return pins


def requirement_names(lines: list[str]) -> set[str]:
    """Normalised project names of requirement lines (``name==1`` or ``name>=1 ; marker``)."""
    names: set[str] = set()
    for line in lines:
        text = line.split("#", 1)[0].strip()
        match = re.match(r"[A-Za-z0-9][A-Za-z0-9_.-]*", text)
        if match:
            names.add(norm(match.group(0)))
    return names


def missing_backends(
    record: dict[str, list[str]], build_in: set[str], build_lock: set[str]
) -> list[str]:
    """One finding per sdist-only pin whose backend is not in the input and the lock."""
    findings = []
    for pin in sorted(record):
        for backend in record[pin]:
            name = norm(backend)
            if name not in build_in:
                findings.append(f"{pin}: backend {name} is not in {BUILD_IN}")
            elif name not in build_lock:
                findings.append(f"{pin}: backend {name} is not in {BUILD_LOCK}")
    return findings


def stale_entries(record: dict[str, list[str]], pins: set[tuple[str, str]]) -> list[str]:
    live = {f"{n}=={v}" for n, v in pins}
    return [
        f"{pin}: recorded, but no lock pins it (rerun write)"
        for pin in sorted(record)
        if pin not in live
    ]


def check(root: Path) -> list[str]:
    doc = json.loads((root / RECORD).read_text(encoding="utf-8"))
    record: dict[str, list[str]] = doc["sdist_only"]
    build_in = requirement_names((root / BUILD_IN).read_text(encoding="utf-8").splitlines())
    build_lock = {n for n, _ in lock_pins(root / BUILD_LOCK)}
    return missing_backends(record, build_in, build_lock) + stale_entries(record, all_pins(root))


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=INDEX_TIMEOUT_S) as resp:  # noqa: S310
        return bytes(resp.read(MAX_SDIST_BYTES))


def build_requires(sdist: bytes, filename: str) -> list[str]:
    """``[build-system] requires`` of an sdist, ``["setuptools"]`` when it declares none."""
    text = None
    if filename.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(sdist)) as zf:
            hit = sorted(
                n for n in zf.namelist() if n.count("/") == 1 and n.endswith("/pyproject.toml")
            )
            text = zf.read(hit[0]).decode() if hit else None
    else:
        with tarfile.open(fileobj=io.BytesIO(sdist)) as tf:
            hit = sorted(
                m for m in tf.getnames() if m.count("/") == 1 and m.endswith("/pyproject.toml")
            )
            member = tf.extractfile(hit[0]) if hit else None
            text = member.read().decode() if member else None
    if text is None:
        return ["setuptools"]
    requires = tomllib.loads(text).get("build-system", {}).get("requires")
    return sorted(requirement_names(requires)) if requires else ["setuptools"]


def has_ci_wheel(files: list[dict[str, Any]], accepted: set[tags.Tag]) -> bool:
    for entry in files:
        name = entry["filename"]
        if name.endswith(".whl") and not entry.get("yanked"):
            if accepted & set(utils.parse_wheel_filename(name)[3]):
                return True
    return False


def inspect_pin(pin: tuple[str, str], accepted: set[tags.Tag]) -> tuple[str, list[str] | None]:
    name, version = pin
    meta = json.loads(fetch(f"https://pypi.org/pypi/{name}/{version}/json"))
    files = meta["urls"]
    key = f"{name}=={version}"
    if has_ci_wheel(files, accepted):
        return key, None
    sdists = [f for f in files if f["packagetype"] == "sdist"]
    if not sdists:
        raise SystemExit(f"{key}: neither a CI wheel nor an sdist on the index")
    return key, build_requires(fetch(sdists[0]["url"]), sdists[0]["filename"])


def write(root: Path) -> int:
    pins = sorted(all_pins(root))
    if len(pins) > MAX_PINS:
        raise SystemExit(f"{len(pins)} pins exceeds the bound {MAX_PINS}")
    accepted = ci_tags()
    with concurrent.futures.ThreadPoolExecutor(MAX_WORKERS) as pool:
        results = list(pool.map(lambda p: inspect_pin(p, accepted), pins))
    record = {key: reqs for key, reqs in results if reqs is not None}
    doc = {
        "comment": "Pins of the hash locks with no wheel for CPython 3.14 on manylinux x86_64, "
        "with the sdist's build-system requires. Regenerate: scripts/ci/sdist_only_pins.py write",
        "python": "3.14",
        "platform": "manylinux x86_64 (glibc <= 2.39)",
        "sdist_only": record,
    }
    (root / RECORD).write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"{len(pins)} pins, {len(record)} sdist-only")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("mode", choices=("check", "write"))
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args(argv)
    if args.mode == "write":
        return write(args.root)
    findings = check(args.root)
    for finding in findings:
        print(finding, file=sys.stderr)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

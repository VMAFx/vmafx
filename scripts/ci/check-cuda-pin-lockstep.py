#!/usr/bin/env python3
"""Check that every CUDA release literal names build-config.env's CUDA_VERSION.

Copyright 2026 Lusoris
SPDX-License-Identifier: EUPL-1.2

CUDA is not an image tag; it is a coordinated pin. One release is named in
four places across three files, in three different spellings:

  * ``CUDA_VERSION="13.4.2"`` in ``build-config.env`` -- the authority.
  * ``cuda-toolkit-13-4`` -- the apt package name, in ``CUDA_APT_PACKAGE`` and
    literally in ``dev/Containerfile``.
  * ``"VMAFX production CUDA 13.4.2 runtime"`` -- the published OCI description
    label on the CUDA runtime image. The residual sweep below is what found
    this one; it was in no inventory of the pin.

It was sixteen places in seven spellings until ADR-1300 and ADR-1306. The CI legs
stopped spelling the release at all: the Linux legs call
``scripts/ci/install-cuda-toolkit.sh`` and the Windows legs
``scripts/ci/install-cuda-toolkit.ps1``, and both read ``build-config.env`` at
run time instead of repeating the version, the series and the
``CUDA_PATH_V<major>_<minor>`` name in each workflow. ADR-1306 dropped the
``nvidia/cuda`` base images in favor of digest-pinned Ubuntu 26.04 with explicit
toolkit installation via ``scripts/ci/install-cuda-toolkit.sh``, retiring six
image sites from the coordinated pin. A site that reads the authority is not a
site that can drift from it.

``check-base-image-single-source.sh`` enforces that ``CUDA_BUILDER`` and
``CUDA_RUNTIME`` match ``DEV_UBUNTU``. A Renovate pull request or bump must
keep the single-source authority and derived spellings in lockstep.
``CUDA_APT_PACKAGE`` is held in step by this gate.

This gate closes all sides. Every site is discovered by shape, every site is
compared against ``CUDA_VERSION``, and a residual sweep fails on any *other*
CUDA release literal in scope, so a site added in a new spelling or a new file
is a build failure rather than a silent copy.

``--write`` rewrites the two derived spellings from ``CUDA_VERSION``. It
deliberately does not touch ``CUDA_VERSION`` (it is the authority).

Usage: check-cuda-pin-lockstep.py [--write]
Exit: 0 clean, 1 drift or an unrecognised site, 2 the config is missing.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

GIT = shutil.which("git") or "/usr/bin/git"

# Files that may legitimately pin a CUDA release. Prose under docs/ names old
# releases on purpose and is not a pin, so it stays out; scripts/ mention
# "cuda:8.6" compute capabilities, which are not releases either.
SCOPE_PATTERNS = (
    re.compile(r"^build-config\.env$"),
    re.compile(r"^Dockerfile(\.[^/]+)?$"),
    re.compile(r"^docker/[^/]*Dockerfile[^/]*$"),
    re.compile(r"^dev/Containerfile(\.[^/]+)?$"),
    re.compile(r"^\.github/workflows/[^/]+\.ya?ml$"),
)

# One regex per spelling. Each carries a `value` group covering exactly the
# characters --write may replace.
CONFIG_RE = re.compile(r'(?m)^CUDA_VERSION="(?P<value>[0-9][0-9.]*)"')
IMAGE_RE = re.compile(r"nvidia/cuda:(?P<value>[0-9]+\.[0-9]+\.[0-9]+)-")
APT_RE = re.compile(r"cuda-toolkit-(?P<value>[0-9]+-[0-9]+)")
LABEL_RE = re.compile(r"(?m)^\s*LABEL\s+[^\n]*\bCUDA (?P<value>[0-9]+\.[0-9]+\.[0-9]+)\b")

# Five spellings used to live here and no longer have a single site in the
# tree, so they are gone: a shape that matches nothing looks wired and does
# nothing, which is the failure this gate exists to prevent.
#
#   action     `cuda: '13.3.1'`      -- the Jimver/cuda-toolkit input, removed
#                                       with the action itself (ADR-1300).
#   installer  `$cudaVersion = ...`  -- both Windows legs now call
#   series     `$cudaMajorMinor ...`    scripts/ci/install-cuda-toolkit.ps1,
#   envvar     `CUDA_PATH_V13_4`        which reads CUDA_VERSION at run time
#                                       and derives the series and the
#                                       CUDA_PATH_V<major>_<minor> name from it.
#   image      `nvidia/cuda:...`     -- dropped in ADR-1306; CUDA builders
#                                       and runtimes build FROM digest-pinned
#                                       Ubuntu 26.04 and install the toolkit
#                                       via scripts/ci/install-cuda-toolkit.sh.
#
# Deleting a shape does not un-gate the spelling. RESIDUAL_RE below sweeps every
# scoped file for any CUDA release literal a shape did not claim, so
# re-introducing `$cudaVersion = '13.4.1'` or `nvidia/cuda:13.4.1-...` fails as an
# unrecognised pin rather than passing silently.
SITE_SHAPES = (
    ("config", CONFIG_RE),
    ("apt", APT_RE),
    ("label", LABEL_RE),
)

# Kinds --write may rewrite: everything that is a pure function of CUDA_VERSION
# and carries no digest.
DERIVED_KINDS = frozenset({"apt", "label"})

# The residual sweep. A CUDA release has a two-digit major (10.x through 15.x),
# which separates it from the action's own `v0.2.36`, from `cuda-keyring_1.1-1`,
# and from Ubuntu LTS releases (20.04, 22.04, 24.04, 26.04).
# The lookarounds keep `ubuntu26.04` and `ubuntu2404` out: a release literal is
# never glued to a word character.
RESIDUAL_RE = re.compile(r"(?<![\w.])(?P<value>1[0-5]\.[0-9]+(?:\.[0-9]+)?)(?![\w.])")


@dataclass(frozen=True)
class Site:
    """One literal naming the CUDA release, located to the character."""

    path: str
    line: int
    column: int
    kind: str
    value: str
    text: str

    @property
    def where(self) -> str:
        return f"{self.path}:{self.line}"


def strip_comment(line: str) -> str:
    """Blank out a trailing `#` comment, leaving column positions intact.

    Quote-aware, because a `#` inside a shell or YAML string is data. Replacing
    with spaces rather than truncating keeps every column index usable against
    the original line.
    """
    quote: str | None = None
    for index, char in enumerate(line):
        if quote is not None:
            if char == quote:
                quote = None
        elif char in "\"'":
            quote = char
        elif char == "#":
            return line[:index] + " " * (len(line) - index)
    return line


def scoped_files(root: Path) -> list[str]:
    """Tracked files that may carry a CUDA pin, newest layout discovered live."""
    listing = subprocess.run(  # noqa: S603 -- fixed read-only Git command
        [GIT, "-C", str(root), "ls-files"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()
    return sorted(path for path in listing if any(p.match(path) for p in SCOPE_PATTERNS))


def find_sites(root: Path) -> list[Site]:
    """Every recognised CUDA release literal in scope, in file order."""
    sites: list[Site] = []
    for path in scoped_files(root):
        try:
            text = (root / path).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        for number, raw in enumerate(text.splitlines(), start=1):
            line = strip_comment(raw)
            for kind, pattern in SITE_SHAPES:
                for match in pattern.finditer(line):
                    sites.append(
                        Site(
                            path=path,
                            line=number,
                            column=match.start("value"),
                            kind=kind,
                            value=match.group("value"),
                            text=raw.strip(),
                        )
                    )
    return sites


def find_unrecognised(root: Path, sites: list[Site]) -> list[Site]:
    """CUDA-looking release literals on CUDA lines that no shape claimed."""
    claimed = {(site.path, site.line, site.column) for site in sites}
    # The apt spelling has no dot, so its own span never collides with the
    # residual regex; guard the whole `cuda-toolkit-NN-N` token instead.
    for site in sites:
        if site.kind == "apt":
            claimed.add((site.path, site.line, site.column))
    found: list[Site] = []
    for path in scoped_files(root):
        try:
            text = (root / path).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        for number, raw in enumerate(text.splitlines(), start=1):
            line = strip_comment(raw)
            if "cuda" not in line.lower():
                continue
            for match in RESIDUAL_RE.finditer(line):
                if (path, number, match.start("value")) in claimed:
                    continue
                found.append(
                    Site(
                        path=path,
                        line=number,
                        column=match.start("value"),
                        kind="unrecognised",
                        value=match.group("value"),
                        text=raw.strip(),
                    )
                )
    return found


def load_cuda_version(root: Path) -> str:
    config = root / "build-config.env"
    if not config.is_file():
        print(f"check-cuda-pin-lockstep: {config} not found", file=sys.stderr)
        raise SystemExit(2)
    match = CONFIG_RE.search(config.read_text(encoding="utf-8"))
    if not match:
        print(
            "check-cuda-pin-lockstep: build-config.env declares no CUDA_VERSION",
            file=sys.stderr,
        )
        raise SystemExit(2)
    return match.group("value")


def expected_value(kind: str, version: str) -> str:
    """What a site of this kind must read, given the authoritative version."""
    series = ".".join(version.split(".")[:2])
    if kind == "apt":
        return series.replace(".", "-")
    return version


def drifted(sites: list[Site], version: str) -> list[Site]:
    return [site for site in sites if site.value != expected_value(site.kind, version)]


def rewrite(root: Path, sites: list[Site], version: str) -> list[str]:
    """Rewrite the derived spellings in place; return the edits made."""
    edits: list[str] = []
    by_path: dict[str, list[Site]] = {}
    for site in drifted(sites, version):
        if site.kind in DERIVED_KINDS:
            by_path.setdefault(site.path, []).append(site)
    for path, group in by_path.items():
        file = root / path
        lines = file.read_text(encoding="utf-8").splitlines(keepends=True)
        # Right to left so an earlier edit cannot move a later column.
        for site in sorted(group, key=lambda s: (s.line, s.column), reverse=True):
            want = expected_value(site.kind, version)
            line = lines[site.line - 1]
            start, end = site.column, site.column + len(site.value)
            lines[site.line - 1] = line[:start] + want + line[end:]
            edits.append(f"{site.where} {site.kind}: {site.value} -> {want}")
        file.write_text("".join(lines), encoding="utf-8")
    return edits


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--write",
        action="store_true",
        help="rewrite the derived CUDA spellings from CUDA_VERSION",
    )
    args = parser.parse_args()

    root = Path(
        subprocess.run(  # noqa: S603 -- fixed read-only Git command
            [GIT, "rev-parse", "--show-toplevel"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    )
    version = load_cuda_version(root)
    sites = find_sites(root)

    if args.write:
        for edit in rewrite(root, sites, version):
            print(f"check-cuda-pin-lockstep: rewrote {edit}")
        sites = find_sites(root)

    problems: list[str] = []
    for site in drifted(sites, version):
        want = expected_value(site.kind, version)
        problems.append(
            f"{site.where} {site.kind} pin reads '{site.value}'; "
            f"CUDA_VERSION={version} requires '{want}'  |  {site.text}"
        )
    for site in find_unrecognised(root, sites):
        problems.append(
            f"{site.where} names CUDA version '{site.value}' in a spelling this gate "
            f"does not know  |  {site.text}"
        )

    for problem in problems:
        print(f"::error title=CUDA pin drift::{problem}", file=sys.stderr)
    if problems:
        print(
            "\nCUDA is a coordinated pin: every site moves in one commit.\n"
            "  Derived spellings:  scripts/ci/check-cuda-pin-lockstep.py --write\n"
            "  Image pins:         edit CUDA_BUILDER / CUDA_RUNTIME, then\n"
            "                      scripts/ci/check-base-image-single-source.sh --write\n"
            "  A new spelling must be taught to this gate and to renovate.json's\n"
            "  CUDA manager in the same change (ADR-1285).",
            file=sys.stderr,
        )
        return 1
    print(f"check-cuda-pin-lockstep: OK ({len(sites)} sites on CUDA {version})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

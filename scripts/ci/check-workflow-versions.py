#!/usr/bin/env python3
"""Check that CI workflows take toolchain versions from build-config.env.

Copyright 2026 Lusoris
SPDX-License-Identifier: BSD-2-Clause-Patent

Dockerfiles are only half of the single-source problem (ADR-1231). The same
versions appear in `.github/workflows/`, and they drifted there too:

  * ROCm was 7.2.4 in build.yml and 7.2.3 in libvmaf-build-matrix.yml, so CI
    validated a ROCm the published images never shipped. ADR-1225 has since
    moved ROCm off the apt repo onto digest-pinned images, so its version is
    now governed by the image pins in build-config.env and checked by
    check-base-image-single-source.sh instead.
  * The Level Zero loader existed at FOUR versions simultaneously -- v1.18.5,
    v1.28.0 (twice), v1.29.0 and 1.32.0. A skew there does not fail a build; it
    surfaces at runtime as "No device of requested type available".

Workflow `run:` steps now source the config directly:

    set -a; . ./build-config.env; set +a

This module fails the build if a literal version reappears. It is a separate
script rather than more shell in check-base-image-single-source.sh because the
Level Zero clone puts `--branch vX.Y.Z` and the repository URL on different
lines, which a line-oriented grep cannot match.

Exit: 0 clean, 1 a literal disagrees with the config, 2 the config is missing.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

# A `git clone` invocation, following backslash continuations, that mentions
# level-zero somewhere in it.
# Consume any number of backslash-continued lines, then the final one. The
# naive `[^\n]*(?:\\\n[^\n]*)*` swallows the trailing backslash itself and
# then cannot match the continuation, silently seeing only the first line.
GIT = shutil.which("git") or "/usr/bin/git"  # absolute path: ruff S607

CLONE_RE = re.compile(r"git clone(?:[^\n]*\\\n)*[^\n]*")
BRANCH_RE = re.compile(r'--branch\s+"?v([0-9][0-9.]*)')
RUN_RE = re.compile(r"(?m)^RUN(?:[^\n]*\\\n)*[^\n]*")
LEVEL_ZERO_CONFIG = "/opt/vmafx/build-config.env"
LEVEL_ZERO_URL = "https://github.com/oneapi-src/level-zero/releases/download/"
LEVEL_ZERO_VERSION_FIELDS = 2  # release tag and package filename in each URL


def load_config(root: Path) -> dict[str, str]:
    """Read build-config.env into a dict without executing it."""
    config = root / "build-config.env"
    if not config.is_file():
        print(f"check-workflow-versions: {config} not found", file=sys.stderr)
        raise SystemExit(2)
    values: dict[str, str] = {}
    for line in config.read_text(encoding="utf-8").splitlines():
        match = re.match(r'^([A-Z][A-Z0-9_]*)="([^"]*)"', line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def check_level_zero_container(root: Path) -> list[str]:
    """The SDK stage consumes the shared value at RUN time; no ARG is needed."""
    path = root / "dev" / "Containerfile"
    if not path.is_file():
        return []
    text = path.read_text(encoding="utf-8")
    problems = []
    copy = re.search(rf"(?m)^COPY\s+build-config\.env\s+{re.escape(LEVEL_ZERO_CONFIG)}\s*$", text)
    if not copy:
        problems.append("dev/Containerfile must COPY build-config.env for the Level Zero SDK step")
    if re.search(r"(?im)^\s*ARG\s+LEVEL_ZERO_VER(?:SION)?(?:=|\s|$)", text):
        problems.append(
            "dev/Containerfile must not duplicate the shared Level Zero version as an ARG"
        )
    downloads = [run for run in RUN_RE.finditer(text) if LEVEL_ZERO_URL in run.group(0)]
    if len(downloads) != 1:
        problems.append(
            "dev/Containerfile needs one Level Zero download RUN sourced from the shared config"
        )
        return problems
    run = downloads[0]
    source = f". {LEVEL_ZERO_CONFIG}"
    command = run.group(0)
    if (
        not copy
        or copy.end() > run.start()
        or source not in command
        or command.index(source) > command.index(LEVEL_ZERO_URL)
    ):
        problems.append(
            "dev/Containerfile must source the copied config before downloading Level Zero"
        )
    if re.search(r"LEVEL_ZERO_VER\b", command) or re.search(
        re.escape(LEVEL_ZERO_URL) + r"v[0-9]", command
    ):
        problems.append(
            "dev/Containerfile Level Zero downloads must use LEVEL_ZERO_VERSION from build-config.env"
        )
    urls = re.findall(re.escape(LEVEL_ZERO_URL) + r'[^"\s]+', command)
    if any(url.count("${LEVEL_ZERO_VERSION}") != LEVEL_ZERO_VERSION_FIELDS for url in urls):
        problems.append("dev/Containerfile does not consume LEVEL_ZERO_VERSION in every SDK URL")
    return problems


def main() -> int:
    # S603: the argument vector is a fixed literal -- no user input reaches it.
    root = Path(
        subprocess.run(  # noqa: S603
            [GIT, "rev-parse", "--show-toplevel"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    )
    cfg = load_config(root)
    lz_want = cfg.get("LEVEL_ZERO_VERSION")

    problems = check_level_zero_container(root)
    for path in sorted((root / ".github" / "workflows").glob("*.yml")):
        text = path.read_text(encoding="utf-8")
        rel = path.relative_to(root)

        if lz_want:
            for clone in CLONE_RE.finditer(text):
                blob = clone.group(0)
                if "level-zero" not in blob:
                    continue
                branch = BRANCH_RE.search(blob)
                if branch and branch.group(1) != lz_want:
                    line_no = text[: clone.start()].count("\n") + 1
                    problems.append(
                        f"{rel}:{line_no} clones Level Zero v{branch.group(1)}; "
                        f"build-config.env says {lz_want}"
                    )

    for problem in problems:
        print(f"::error title=toolchain version drift::{problem}", file=sys.stderr)
    if problems:
        print(
            "\nTake the version from the config instead of a literal:\n"
            "    set -a; . ./build-config.env; set +a\n"
            "The Windows leg runs under cmd and cannot source it, so it mirrors\n"
            "the value by hand -- this check is what keeps that mirror honest.",
            file=sys.stderr,
        )
        return 1
    print(f"check-workflow-versions: OK (Level Zero {lz_want})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

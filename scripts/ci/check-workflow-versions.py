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
PYTHON_RE = re.compile(r'python-version:\s*[\'"]?([0-9]+\.[0-9]+\.[0-9]+)[\'"]?')


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


def check_workflows(root: Path, lz_want: str | None, py_want: str | None) -> list[str]:
    problems: list[str] = []
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

        if py_want:
            for m in PYTHON_RE.finditer(text):
                ver = m.group(1)
                if ver != py_want:
                    line_no = text[: m.start()].count("\n") + 1
                    problems.append(
                        f"{rel}:{line_no} pins python-version '{ver}'; "
                        f"build-config.env says '{py_want}'"
                    )
    return problems


def check_vmafx_version(root: Path, ver_want: str | None) -> list[str]:
    if not ver_want:
        return []
    problems: list[str] = []
    meson_file = root / "core" / "meson.build"
    if meson_file.is_file():
        m = re.search(r"version\s*:\s*'([^']+)'", meson_file.read_text(encoding="utf-8"))
        if m and m.group(1) != ver_want:
            problems.append(
                f"core/meson.build specifies version '{m.group(1)}'; "
                f"build-config.env says '{ver_want}'"
            )

    compat_init = root / "compat" / "python-vmaf" / "__init__.py"
    if compat_init.is_file():
        m = re.search(r'__version__\s*=\s*"([^"]+)"', compat_init.read_text(encoding="utf-8"))
        if m and m.group(1) != ver_want:
            problems.append(
                f"compat/python-vmaf/__init__.py specifies __version__ '{m.group(1)}'; "
                f"build-config.env says '{ver_want}'"
            )
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
    py_want = cfg.get("PYTHON_CI_VERSION")
    ver_want = cfg.get("VMAFX_VERSION")

    problems: list[str] = []
    problems.extend(check_workflows(root, lz_want, py_want))
    problems.extend(check_vmafx_version(root, ver_want))

    for problem in problems:
        print(f"::error title=version drift::{problem}", file=sys.stderr)
    if problems:
        print(
            "\nVersion drift detected. Align literal versions with build-config.env.",
            file=sys.stderr,
        )
        return 1
    print(
        f"check-workflow-versions: OK (Level Zero {lz_want}, Python CI {py_want}, VMAFx {ver_want})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

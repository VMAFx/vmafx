#!/usr/bin/env python3
"""Check that CI workflows take toolchain versions from build-config.env.

Copyright 2026 Lusoris
SPDX-License-Identifier: EUPL-1.2

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

`[tool.mypy] python_version` is the same problem inside one file. It promises
to track `requires-python` in a comment and nothing compared the two, so it sat
at `3.10` against a `>=3.14` floor until ADR-1282; below 3.12 mypy refuses to
parse the PEP 695 `type` statement in numpy's bundled stubs and aborts the
`ai/src/` pass entirely. `check_mypy_python_version` compares the pin to the
floor beside it and to `PYTHON_CI_VERSION`.

The formatter pins are the same problem one directory over. The Makefile
installs ruff and black at versions it promises are identical to
`.pre-commit-config.yaml`, and Renovate only ever moved the pre-commit side:
by 2026-09 the hook ran ruff 0.16.8 while `make lint-tools` installed 0.16.5
and a hint still said 0.15.17. `check_formatter_pins` compares the two files
and rejects a literal `ruff==` / `black==` in the Makefile.

Exit: 0 clean, 1 a literal disagrees with the config, 2 the config is missing.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

# tomllib sorts here rather than with the stdlib imports above because ruff is still
# configured with `target-version = "py310"`, where tomllib did not yet exist. That pin
# is the drift ADR-1282 deliberately left alone; move this import up when it is raised.
import tomllib

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


FORMATTER_PINS = {
    # Makefile variable -> (pre-commit repository, tool name in `pip install`)
    "RUFF_VERSION": ("https://github.com/astral-sh/ruff-pre-commit", "ruff"),
    "BLACK_VERSION": ("https://github.com/psf/black", "black"),
}


def precommit_rev(text: str, repo: str) -> str | None:
    """Return the `rev:` of one pre-commit repository, without a leading v."""
    match = re.search(
        rf"(?m)^\s*-\s*repo:\s*{re.escape(repo)}\s*\n\s*rev:\s*['\"]?v?([0-9][^'\"\s]*)", text
    )
    return match.group(1) if match else None


def check_formatter_pins(root: Path) -> list[str]:
    """The Makefile installs the formatters at the versions pre-commit runs."""
    makefile = root / "Makefile"
    hooks = root / ".pre-commit-config.yaml"
    if not makefile.is_file() or not hooks.is_file():
        return []
    make_text = makefile.read_text(encoding="utf-8")
    hook_text = hooks.read_text(encoding="utf-8")
    problems: list[str] = []
    for variable, (repo, tool) in FORMATTER_PINS.items():
        pin = re.search(rf"(?m)^{variable}\s*:?=\s*(\S+)\s*$", make_text)
        want = precommit_rev(hook_text, repo)
        if pin and want and pin.group(1) != want:
            problems.append(
                f"Makefile sets {variable} := {pin.group(1)}; "
                f".pre-commit-config.yaml runs {tool} {want}"
            )
        for literal in re.finditer(rf"\b{tool}==([0-9][0-9A-Za-z.]*)", make_text):
            line_no = make_text[: literal.start()].count("\n") + 1
            problems.append(
                f"Makefile:{line_no} installs {tool}=={literal.group(1)} as a literal; "
                f"use $({variable})"
            )
    return problems


# `requires-python` is a PEP 440 specifier set. Only the clauses that state a
# lower bound say where the project's floor is; `<`, `<=` and `!=` clauses are
# ignored. `==3.14.*` is a floor as well as a ceiling.
REQUIRES_FLOOR_RE = re.compile(r"(>=|~=|==)\s*([0-9]+)\.([0-9]+)")


def requires_python_floor(spec: str) -> str | None:
    """The oldest `major.minor` language version a `requires-python` admits.

    A specifier set is a conjunction, so when more than one clause states a
    lower bound the strictest of them is the effective floor -- `>=3.9,>=3.14`
    admits nothing below 3.14.
    """
    floors = [
        (int(match.group(2)), int(match.group(3))) for match in REQUIRES_FLOOR_RE.finditer(spec)
    ]
    if not floors:
        return None
    major, minor = max(floors)
    return f"{major}.{minor}"


def mypy_pin_problem(modelled: object, requires: str, floor: str) -> str | None:
    """Describe how `[tool.mypy] python_version` fails to match the floor, if it does."""
    if modelled is None:
        # Deleting the key is the other way the two drift apart: mypy then
        # models whichever interpreter the caller happens to run. ADR-1282
        # rejected that explicitly.
        return (
            f"pyproject.toml: [tool.mypy] has no python_version; requires-python "
            f"('{requires}') floors the project at {floor}, and ADR-1282 requires the "
            "value be pinned rather than inherited from the running interpreter"
        )
    if not isinstance(modelled, str):
        return (
            f"pyproject.toml: [tool.mypy] python_version = {modelled!r} must be a quoted "
            f'string, as in python_version = "{floor}"'
        )
    if modelled != floor:
        return (
            f"pyproject.toml: [tool.mypy] python_version = '{modelled}' but "
            f"requires-python = '{requires}' floors the project at {floor}; "
            "raise them in lockstep (ADR-1282)"
        )
    return None


def check_mypy_python_version(root: Path, py_ci: str | None = None) -> list[str]:
    """`[tool.mypy] python_version` tracks `requires-python` (ADR-1282).

    The pin sat at `3.10` for as long as it did because nothing compared it to
    the floor beside it. A silent revert has no other guard: mypy accepts any
    version it knows, and below 3.12 it refuses to parse the PEP 695 `type`
    statement in numpy's bundled `__init__.pyi`, which aborts the whole
    `ai/src/` pass in every checkout that has numpy installed instead of
    reporting anything about this file.
    """
    path = root / "pyproject.toml"
    if not path.is_file():
        return []
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as exc:
        return [f"pyproject.toml is not valid TOML: {exc}"]
    project = data.get("project")
    mypy = data.get("tool", {}).get("mypy")
    requires = project.get("requires-python") if isinstance(project, dict) else None
    if not isinstance(mypy, dict) or not isinstance(requires, str):
        return []

    floor = requires_python_floor(requires)
    if floor is None:
        return [
            f"pyproject.toml: requires-python = '{requires}' states no lower bound, "
            "so [tool.mypy] python_version cannot be checked against it"
        ]

    modelled = mypy.get("python_version")
    problem = mypy_pin_problem(modelled, requires, floor)
    if problem is None and py_ci and ".".join(py_ci.split(".")[:2]) != modelled:
        problem = (
            f"pyproject.toml: [tool.mypy] python_version = '{modelled}' but "
            f"build-config.env says PYTHON_CI_VERSION = '{py_ci}'; "
            "the checker must model the version CI installs (ADR-1282)"
        )
    return [problem] if problem is not None else []


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
    problems.extend(check_level_zero_container(root))
    problems.extend(check_workflows(root, lz_want, py_want))
    problems.extend(check_vmafx_version(root, ver_want))
    problems.extend(check_formatter_pins(root))
    problems.extend(check_mypy_python_version(root, py_want))

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

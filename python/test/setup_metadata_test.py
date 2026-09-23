# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Regression tests for the compatibility package's setup metadata."""

from __future__ import annotations

import subprocess
import sys
import tomllib
from pathlib import Path

import pytest

# `packaging` is a hard (non-extra) dependency of pytest itself
# (`pytest` requires `packaging>=22`), so it is available wherever this test
# runs and needs no entry in python/requirements.txt.
from packaging.requirements import Requirement
from packaging.version import InvalidVersion, Version

import vmaf

REPO_ROOT = Path(__file__).resolve().parents[2]
PYPROJECT = REPO_ROOT / "python" / "pyproject.toml"
PACKAGE_PYPROJECTS = (
    REPO_ROOT / "ai" / "pyproject.toml",
    REPO_ROOT / "dev-llm" / "pyproject.toml",
    REPO_ROOT / "mcp-server" / "vmaf-mcp" / "pyproject.toml",
    REPO_ROOT / "python" / "pyproject.toml",
    REPO_ROOT / "tools" / "ensemble-training-kit" / "pyproject.toml",
    REPO_ROOT / "tools" / "vmaf-roi-score" / "pyproject.toml",
    REPO_ROOT / "tools" / "vmaf-tune" / "pyproject.toml",
)

# The first setuptools release that parses a PEP 639 SPDX license expression
# (a bare string in `[project].license`). 77.0.0 was yanked, so 77.0.1 is the
# first one a resolver will actually hand you.
PEP639_SETUPTOOLS_FLOOR = "77.0.1"
# The newest setuptools that predates PEP 639 support, plus the version Ubuntu
# 24.04 ships in /usr/lib/python3/dist-packages. Both must be excluded by the
# declared floor, or `setup.py` aborts before it can report a version.
PRE_PEP639_SETUPTOOLS = ("76.1.0", "68.1.2")


def _setup_py_version() -> str:
    """What `setup.py --version` reports, i.e. the version setuptools ships."""
    result = subprocess.run(
        [sys.executable, "setup.py", "--version"],
        cwd=REPO_ROOT / "python",
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        # `check=True` would raise CalledProcessError, whose message carries
        # the exit status and nothing else -- setuptools writes every
        # configuration error to stderr, which capture_output() then swallows.
        # That is how a metadata form the ambient setuptools could not parse
        # reached CI as a bare "returned non-zero exit status 1".
        raise AssertionError(
            f"`{sys.executable} setup.py --version` exited {result.returncode} "
            f"in {REPO_ROOT / 'python'}\n"
            f"--- stdout ---\n{result.stdout}\n"
            f"--- stderr ---\n{result.stderr}"
        )
    return result.stdout.strip()


def _build_requirement(name: str) -> Requirement:
    """The `[build-system].requires` entry for `name`, as a parsed Requirement."""
    build_requires = tomllib.loads(PYPROJECT.read_text(encoding="utf-8"))["build-system"][
        "requires"
    ]
    for raw in build_requires:
        requirement = Requirement(raw)
        if requirement.name.lower() == name:
            return requirement
    raise AssertionError(f"{PYPROJECT} declares no {name!r} build requirement: {build_requires}")


def test_setup_metadata_version_matches_package_marker():
    """The release-please marker comment must not become the package version.

    `vmaf.__version__` carries the SemVer string release-please writes, which
    for a release candidate is a hyphenated prerelease such as ``1.0.0-rc.1``
    (ADR-1201). setuptools canonicalises whatever it is handed to PEP 440
    before publishing it, so the same release reaches `setup.py --version` as
    ``1.0.0rc1``. Comparing the two raw strings therefore fails on every RC
    even though nothing is wrong -- it did, on eleven build lanes at once, on
    the 1.0.0-rc.1 release PR.

    Comparing the parsed versions keeps what this test is actually for: a
    marker comment or a mangled substitution leaking into the shipped version
    still fails, either as an inequality or as `InvalidVersion`.
    """
    setup_version = _setup_py_version()

    assert Version(setup_version) == Version(vmaf.__version__), (
        f"setup.py ships {setup_version!r} but the package marker says "
        f"{vmaf.__version__!r}; these must describe the same release"
    )


def test_package_marker_is_a_valid_version():
    """A mangled release-please substitution must not parse as a version."""
    try:
        Version(vmaf.__version__)
    except InvalidVersion as exc:  # pragma: no cover - only on a broken release
        pytest.fail(f"vmaf.__version__ is not a valid version: {vmaf.__version__!r} ({exc})")

    assert "x-release-please" not in vmaf.__version__
    assert "${" not in vmaf.__version__


def test_build_backend_floor_covers_the_license_metadata_form():
    """`[build-system].requires` must exclude a setuptools that cannot read us.

    `[project].license` is a PEP 639 SPDX expression -- a bare string. Every
    setuptools before 77.0.1 rejects that shape outright, and the rejection is
    fatal: `setup.py --version` never gets as far as printing a version, so a
    stale build backend looks like a broken package rather than a stale build
    backend. `make cythonize` and the CI lanes run `setup.py` against the
    ambient interpreter, with no PEP 517 isolation to fetch a newer backend on
    their own, so the floor has to be declared here and honoured there.

    The table form (`license = { text = ... }`) needs no floor -- it is what
    old setuptools wants -- so this only asserts anything while we ship the
    SPDX expression.
    """
    project = tomllib.loads(PYPROJECT.read_text(encoding="utf-8"))["project"]
    if not isinstance(project["license"], str):
        return

    specifier = _build_requirement("setuptools").specifier
    assert specifier.contains(PEP639_SETUPTOOLS_FLOOR), (
        f"{PYPROJECT} ships a PEP 639 license expression but its setuptools "
        f"requirement {str(specifier)!r} excludes {PEP639_SETUPTOOLS_FLOOR}, "
        "the first release that can parse one"
    )
    for version in PRE_PEP639_SETUPTOOLS:
        assert not specifier.contains(version), (
            f"{PYPROJECT} ships a PEP 639 license expression but its setuptools "
            f"requirement {str(specifier)!r} still admits {version}, which "
            "cannot parse one -- setup.py would abort with a configuration error"
        )


def test_all_python_packages_use_pep639_license_expression():
    """Every published Python package exposes the repository SPDX license."""
    for pyproject in PACKAGE_PYPROJECTS:
        project = tomllib.loads(pyproject.read_text(encoding="utf-8"))["project"]
        assert project.get("license") == "BSD-2-Clause-Patent", (
            f"{pyproject} must use the PEP 639 SPDX string form; "
            f"found {project.get('license')!r}"
        )

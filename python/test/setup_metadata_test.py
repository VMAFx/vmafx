# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
"""Regression tests for the compatibility package's setup metadata."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

# `packaging` is a hard (non-extra) dependency of pytest itself
# (`pytest` requires `packaging>=22`), so it is available wherever this test
# runs and needs no entry in python/requirements.txt.
from packaging.version import InvalidVersion, Version

import vmaf

REPO_ROOT = Path(__file__).resolve().parents[2]


def _setup_py_version() -> str:
    """What `setup.py --version` reports, i.e. the version setuptools ships."""
    result = subprocess.run(
        [sys.executable, "setup.py", "--version"],
        cwd=REPO_ROOT / "python",
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


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

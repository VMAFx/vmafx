# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
"""Regression tests for the compatibility package's setup metadata."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import vmaf

REPO_ROOT = Path(__file__).resolve().parents[2]


def test_setup_metadata_version_matches_package_marker():
    """The release-please marker comment must not become the package version."""
    result = subprocess.run(
        [sys.executable, "setup.py", "--version"],
        cwd=REPO_ROOT / "python",
        check=True,
        capture_output=True,
        text=True,
    )

    assert result.stdout.strip() == vmaf.__version__

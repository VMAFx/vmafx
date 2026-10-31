# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every Windows job installs zimg through one script, with its C runtime's triplet."""

from __future__ import annotations

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
WORKFLOWS = REPO / ".github" / "workflows"
CALL = re.compile(r"setup-zimg-windows\.ps1 -Triplet (\S+)")
DIRECT_INSTALL = re.compile(r"vcpkg install [\"']?zimg:")
EXPECTED_CALLS = {
    "build.yml": ["x64-windows-static-md"],
    "ffmpeg-integration.yml": ["x64-windows-static-md"],
    "libvmaf-build-matrix.yml": ["x64-windows-static-md", "arm64-windows-static-md"],
    "windows-tester-bundle.yml": ["$env:ZIMG_TRIPLET"],
}


def direct_installs(text: str) -> list[str]:
    return [line.strip() for line in text.splitlines() if DIRECT_INSTALL.search(line)]


def test_no_workflow_installs_zimg_on_its_own() -> None:
    found = {
        path.name: direct_installs(path.read_text(encoding="utf-8"))
        for path in sorted(WORKFLOWS.glob("*.yml"))
    }
    assert {name: lines for name, lines in found.items() if lines} == {}


def test_a_direct_install_is_refused() -> None:
    assert direct_installs('  vcpkg install "zimg:$triplet"\n') != []
    assert direct_installs("  vcpkg install zimg:x64-windows-static\n") != []


def test_a_script_call_is_not_a_direct_install() -> None:
    assert direct_installs("  run: pwsh -File scripts/ci/setup-zimg-windows.ps1 -Triplet x\n") == []


@pytest.mark.parametrize(("workflow", "triplets"), sorted(EXPECTED_CALLS.items()))
def test_each_windows_workflow_names_its_runtime_triplet(
    workflow: str, triplets: list[str]
) -> None:
    text = (WORKFLOWS / workflow).read_text(encoding="utf-8")
    assert CALL.findall(text) == triplets


def test_the_tester_zip_triplet_follows_the_zip_runtime() -> None:
    text = (WORKFLOWS / "windows-tester-bundle.yml").read_text(encoding="utf-8")
    assert (
        "ZIMG_TRIPLET: ${{ matrix.gpu == 'sycl' && 'x64-windows-static-md'"
        " || format('{0}-windows-static', matrix.arch) }}" in text
    )
    options = (REPO / "scripts" / "ci" / "build-windows-tester-bundle.py").read_text(
        encoding="utf-8"
    )
    assert '"-Db_vscrt=mt"' in options
    assert 'SYCL_OPTIONS = ("-Denable_sycl=true", "-Db_vscrt=md"' in options

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the release-phase tool inventory."""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaf_rc1_tester.tools_catalog import (
    REPO_TOOLS,
    get_tools_catalog,
    render_tools_markdown,
)


def test_catalog_has_required_fields_and_phases() -> None:
    catalog = get_tools_catalog()
    assert {item["phase"] for item in catalog} == {"RC1", "RC2", "RC3"}
    for item in catalog:
        assert item["name"]
        assert item["path_or_command"]
        assert item["hardware_scope"]
        assert item["notes"]
        assert isinstance(item["is_rc1_ready"], bool)


def test_only_rc1_tools_are_marked_ready() -> None:
    assert all(tool.is_rc1_ready for tool in REPO_TOOLS if tool.phase == "RC1")
    assert all(not tool.is_rc1_ready for tool in REPO_TOOLS if tool.phase != "RC1")


def test_catalog_discloses_benchmark_subset_gaps() -> None:
    bench = next(tool for tool in REPO_TOOLS if tool.name == "vmaf_bench")
    assert bench.phase == "RC2"
    assert "does not cover HIP or Metal" in bench.notes
    assert "SYCL devices only" in bench.notes


def test_render_tools_markdown_states_boundary() -> None:
    markdown = render_tools_markdown()
    assert "Repository tool inventory by release phase" in markdown
    assert "| **RC1** |" in markdown
    assert "| **RC2** |" in markdown
    assert "| **RC3** |" in markdown
    assert "Performance benchmarking/tuning starts in RC2" in markdown
    assert "real model training starts in RC3" in markdown

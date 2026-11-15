#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the glossary-link checker (issue 2332)."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

import scripts.docs.check_glossary_links as checker
from scripts.docs.check_glossary_links import (
    EXIT_FAILURE,
    EXIT_SUCCESS,
    glossary_anchors,
    main,
    term_pattern,
)

REPO_ROOT = Path(__file__).resolve().parents[3]


def test_real_tree_passes() -> None:
    """The checked-in glossary, metric pages and term list hold together."""
    assert main([]) == EXIT_SUCCESS


def test_every_known_term_has_a_glossary_entry() -> None:
    anchors = glossary_anchors(checker._read(checker.GLOSSARY))
    missing = {term: anchor for term, anchor in checker.TERMS.items() if anchor not in anchors}
    assert missing == {}, f"terms without a glossary entry: {missing}"


def test_term_without_entry_fails(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """A known term whose glossary entry is missing fails the gate."""
    monkeypatch.setattr(checker, "GLOSSARY", tmp_path / "glossary.md")
    monkeypatch.setattr(checker, "METRICS_DIR", tmp_path / "metrics")
    (tmp_path / "metrics").mkdir()
    (tmp_path / "glossary.md").write_text(
        "# Glossary\n\n## Pooling {#pooling}\n\nCollapse per-frame scores.\n",
        encoding="utf-8",
    )
    # NEG is known but has no entry here.
    assert main([]) == EXIT_FAILURE


def test_metric_page_using_a_term_without_link_fails(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(checker, "GLOSSARY", tmp_path / "glossary.md")
    monkeypatch.setattr(checker, "METRICS_DIR", tmp_path / "metrics")
    (tmp_path / "metrics").mkdir()
    (tmp_path / "glossary.md").write_text(
        "# Glossary\n\n## NEG {#neg}\n\nNo Enhancement Gain.\n"
        "## CTC {#ctc}\n\nCommon test conditions.\n"
        "## HDR {#hdr}\n## SDR {#sdr}\n## Pooling {#pooling}\n## 3H {#3h}\n",
        encoding="utf-8",
    )
    (tmp_path / "metrics" / "widget.md").write_text(
        "# Widget\n\nThe NEG variant penalises sharpening.\n",
        encoding="utf-8",
    )
    assert main([]) == EXIT_FAILURE

    (tmp_path / "metrics" / "widget.md").write_text(
        "# Widget\n\nThe [NEG](../reference/glossary.md#neg) variant "
        "penalises sharpening. See the [glossary](../reference/glossary.md).\n",
        encoding="utf-8",
    )
    assert main([]) == EXIT_SUCCESS


def test_metric_page_without_glossary_link_fails(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(checker, "GLOSSARY", tmp_path / "glossary.md")
    monkeypatch.setattr(checker, "METRICS_DIR", tmp_path / "metrics")
    (tmp_path / "metrics").mkdir()
    (tmp_path / "glossary.md").write_text(
        "# Glossary\n\n## Pooling {#pooling}\n## NEG {#neg}\n## CTC {#ctc}\n"
        "## HDR {#hdr}\n## SDR {#sdr}\n## 3H {#3h}\n",
        encoding="utf-8",
    )
    (tmp_path / "metrics" / "plain.md").write_text(
        "# Plain\n\nNo glossary terms used here.\n", encoding="utf-8"
    )
    assert main([]) == EXIT_FAILURE


def test_term_pattern_is_whole_word() -> None:
    assert term_pattern("NEG").search("the NEG models")
    assert term_pattern("NEG").search("negative") is None
    assert term_pattern("3H").search("viewed at 3H away")
    assert term_pattern("3H").search("the 3Height") is None
    assert term_pattern("pooling").search("Pooling is applied")
    assert term_pattern("pooling").search("pooled") is None


def test_subdirectory_pages_link_with_correct_depth() -> None:
    """ctc/aom.md sits one level deeper and needs ../../reference paths."""
    page = checker.ROOT / "docs" / "metrics" / "ctc" / "aom.md"
    text = checker._read(page)
    assert "../../reference/glossary.md#neg" in text
    assert "../reference/glossary.md#neg}" not in text

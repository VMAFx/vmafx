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
    EXIT_UNREADABLE,
    glossary_anchors,
    main,
    term_pattern,
)

REPO_ROOT = Path(__file__).resolve().parents[3]

GLOSSARY = (
    "# Glossary\n\n"
    "## Viewing condition {#viewing-condition}\n\nViewing setup.\n\n"
    "## 3H {#3h}\n\nThree screen heights.\n\n"
    "## Phone model {#phone-model}\n\nPhone viewing model.\n\n"
    "## Default model {#default-model}\n\nThe default model.\n\n"
    "## Feature extractor {#feature-extractor}\n\nA feature extractor.\n\n"
    "## Motion {#motion}\n\nTemporal motion.\n\n"
    "## Pooling {#pooling}\n\nCollapse per-frame scores.\n\n"
    "## NEG {#neg}\n\nNo Enhancement Gain.\n\n"
    "## CTC {#ctc}\n\nCommon test conditions.\n\n"
    "## HDR {#hdr}\n\nHigh dynamic range.\n\n"
    "## SDR {#sdr}\n\nStandard dynamic range.\n\n"
    "## PQ / HLG {#pqr-hlg}\n\nHDR transfer functions.\n\n"
    "## BT.709 / BT.2020 {#bt709-bt2020}\n\nColour primaries.\n\n"
    "## Range {#range}\n\nLimited or full range.\n\n"
    "## Frame alignment {#frame-alignment}\n\nSame frames on both sides.\n\n"
    "## Scaling {#scaling}\n\nResampling before scoring.\n\n"
    "## Interlacing {#interlacing}\n\nField order.\n\n"
    "## Chroma subsampling {#chroma-subsampling}\n\n420, 422 or 444.\n\n"
    "## 10-bit {#10-bit}\n\nTen bits per component.\n\n"
    "## Confidence interval {#confidence-interval}\n\nBootstrap interval.\n\n"
    "## Score transform {#score-transform}\n\nScore mapping.\n"
)


def _make_tree(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    page_rel: str,
    page_text: str,
) -> Path:
    """Create a temp docs tree with the glossary and one metric page."""
    glossary = tmp_path / "reference" / "glossary.md"
    glossary.parent.mkdir(parents=True)
    glossary.write_text(GLOSSARY, encoding="utf-8")
    page = tmp_path / page_rel
    page.parent.mkdir(parents=True, exist_ok=True)
    page.write_text(page_text, encoding="utf-8")
    monkeypatch.setattr(checker, "ROOT", tmp_path)
    monkeypatch.setattr(checker, "GLOSSARY", glossary)
    monkeypatch.setattr(checker, "METRICS_DIR", tmp_path / "metrics")
    return page


def test_real_tree_passes() -> None:
    """The checked-in glossary, metric pages and term list hold together."""
    assert main([]) == EXIT_SUCCESS


def test_every_known_term_has_a_glossary_entry() -> None:
    anchors = glossary_anchors(checker._read(checker.GLOSSARY))
    missing = {term: anchor for term, anchor in checker.TERMS.items() if anchor not in anchors}
    assert missing == {}, f"terms without a glossary entry: {missing}"


def test_every_glossary_entry_has_a_term() -> None:
    """A new glossary entry without a term would pass gate 2 unnoticed."""
    anchors = glossary_anchors(checker._read(checker.GLOSSARY))
    assert anchors <= set(checker.TERMS.values())


def test_term_without_entry_fails(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """A known term whose glossary entry is missing fails the gate."""
    _make_tree(
        tmp_path,
        monkeypatch,
        "metrics/widget.md",
        "# Widget\n\nThe [NEG](../reference/glossary.md#neg) variant.\n",
    )
    (tmp_path / "reference" / "glossary.md").write_text(
        "# Glossary\n\n## Pooling {#pooling}\n\nCollapse per-frame scores.\n",
        encoding="utf-8",
    )
    # NEG is known but has no entry here.
    assert main([]) == EXIT_FAILURE


def test_metric_page_using_a_term_without_link_fails(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    page = _make_tree(
        tmp_path,
        monkeypatch,
        "metrics/widget.md",
        "# Widget\n\nThe NEG variant penalises sharpening.\n",
    )
    assert main([]) == EXIT_FAILURE

    page.write_text(
        "# Widget\n\nThe [NEG](../reference/glossary.md#neg) variant "
        "penalises sharpening. See the [glossary](../reference/glossary.md).\n",
        encoding="utf-8",
    )
    assert main([]) == EXIT_SUCCESS


def test_metric_page_without_glossary_link_fails(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _make_tree(
        tmp_path,
        monkeypatch,
        "metrics/plain.md",
        "# Plain\n\nNo glossary terms used here.\n",
    )
    assert main([]) == EXIT_FAILURE


def test_term_pattern_is_whole_word() -> None:
    assert term_pattern("NEG").search("the NEG models")
    assert term_pattern("NEG").search("negative") is None
    assert term_pattern("3H").search("viewed at 3H away")
    assert term_pattern("3H").search("the 3Height") is None
    assert term_pattern("pooling").search("Pooling is applied")
    assert term_pattern("pooling").search("pooled") is None


def test_subdirectory_page_needs_two_levels(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """ctc/aom.md sits one level deeper and needs ../../reference paths."""
    page = _make_tree(
        tmp_path,
        monkeypatch,
        "metrics/ctc/aom.md",
        "# AOM\n\nThe [NEG](../../reference/glossary.md#neg) variant. "
        "See the [glossary](../../reference/glossary.md).\n",
    )
    assert main([]) == EXIT_SUCCESS

    page.write_text(
        "# AOM\n\nThe [NEG](../reference/glossary.md#neg) variant. "
        "See the [glossary](../reference/glossary.md).\n",
        encoding="utf-8",
    )
    assert main([]) == EXIT_FAILURE


def test_link_inside_html_comment_does_not_count(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A commented-out link is not a live link for gate 2."""
    _make_tree(
        tmp_path,
        monkeypatch,
        "metrics/widget.md",
        "# Widget\n\n<!-- [NEG](../reference/glossary.md#neg) -->\n"
        "The NEG variant penalises sharpening. See the "
        "[glossary](../reference/glossary.md).\n",
    )
    assert main([]) == EXIT_FAILURE


def test_wrong_anchor_prefix_does_not_count(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A link to a nonexistent anchor that shares the first letters fails."""
    _make_tree(
        tmp_path,
        monkeypatch,
        "metrics/widget.md",
        "# Widget\n\nThe [NEG](../reference/glossary.md#negative-model-typo) "
        "variant. See the [glossary](../reference/glossary.md).\n",
    )
    assert main([]) == EXIT_FAILURE


def test_invalid_utf8_page_is_unreadable(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """A page that is not valid UTF-8 exits 2 instead of raising."""
    page = _make_tree(
        tmp_path,
        monkeypatch,
        "metrics/widget.md",
        "# Widget\n\nThe [NEG](../reference/glossary.md#neg) variant.\n",
    )
    page.write_bytes(b"# Widget\n\n\xff\xfe broken\n")
    assert main([]) == EXIT_UNREADABLE

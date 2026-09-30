# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Markdown shape of the predictor model cards (ADR-1351).

praetor's locked documentation gate (``make docs-lint``) lints the committed
``model/predictor_*_card.md`` files with markdownlint. The cards come from
``predictor_train._write_model_card``, which used to produce three findings:
two blank lines before "## 1. Purpose" when the synthetic-stub warning is
absent (MD012), a metrics table whose rows did not line up with its aligned
header (MD060), and an architecture fence without a language (MD040). These
tests pin the template and every committed card against all three.
"""

from __future__ import annotations

import io
import re
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.predictor_train import _write_model_card

_REPO_ROOT = _HERE.parents[2]
_CARDS = sorted((_REPO_ROOT / "model").glob("predictor_*_card.md"))


def _render(corpus_kind: str) -> str:
    out = io.StringIO()
    _write_model_card(
        out,
        codec="libx264",
        opset=18,
        node_count=10,
        n_train=80,
        n_val=20,
        plcc=0.99,
        srocc=0.97,
        rmse=1.25,
        onnx_sha256="0" * 64,
        onnx_bytes=21877,
        op_allowlist_ok=True,
        forbidden_ops=(),
        corpus_kind=corpus_kind,
    )
    return out.getvalue()


def _check_markdown_shape(text: str) -> None:
    assert "\n\n\n" not in text, "two consecutive blank lines (MD012)"
    fences = re.findall(r"^```(.*)$", text, re.MULTILINE)
    assert len(fences) % 2 == 0, "unbalanced code fence"
    assert all(lang.strip() for lang in fences[::2]), "fence without a language (MD040)"
    assert "| Metric | Value |\n| --- | --- |\n" in text, "metrics table not compact (MD060)"
    for label in ("PLCC", "SROCC", "RMSE"):
        row = rf"^\| {label} \| [0-9.]+( VMAF)? \|$"
        assert re.search(row, text, re.MULTILINE), f"{label} row not compact (MD060)"


def test_real_corpus_card_has_one_blank_line_before_purpose() -> None:
    text = _render("real-N=2592")
    assert "Warning" not in text
    assert "`\n\n## 1. Purpose\n" in text
    _check_markdown_shape(text)


def test_synthetic_card_keeps_warning_between_single_blank_lines() -> None:
    text = _render("synthetic-stub-N=100")
    assert "`\n\n> **Warning — synthetic-stub model.**" in text
    assert "`predictor_train.py` against it.\n\n## 1. Purpose\n" in text
    _check_markdown_shape(text)


def test_shape_check_rejects_the_previous_template() -> None:
    old = _render("real-N=10").replace("`\n\n## 1. Purpose", "`\n\n\n## 1. Purpose")
    with pytest.raises(AssertionError, match="MD012"):
        _check_markdown_shape(old)
    old = _render("real-N=10").replace("```text\n", "```\n")
    with pytest.raises(AssertionError, match="MD040"):
        _check_markdown_shape(old)
    old = _render("real-N=10").replace("| --- | --- |", "|--------|-------|")
    with pytest.raises(AssertionError, match="MD060"):
        _check_markdown_shape(old)


def test_every_committed_card_is_found() -> None:
    assert len(_CARDS) >= 14, f"expected the 14 shipped predictor cards, found {len(_CARDS)}"


@pytest.mark.parametrize("card", _CARDS, ids=lambda p: p.name)
def test_committed_card_markdown_shape(card: Path) -> None:
    _check_markdown_shape(card.read_text(encoding="utf-8"))

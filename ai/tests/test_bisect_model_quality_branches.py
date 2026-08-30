# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Branch-coverage tests for :mod:`vmaf_train.bisect_model_quality`.

Complements ``test_bisect_model_quality.py`` by exercising:
* ``BisectResult.to_dict`` serialisation (lines 46-51),
* the ``min_srocc`` gate branch (line 59),
* the unknown-threshold-kind error path (line 62),
* the cache-hit branch in the inner ``check`` helper (line 110).

These run without ``onnxruntime`` by stubbing :func:`evaluate_onnx`.
"""

from __future__ import annotations

from pathlib import Path
from unittest.mock import patch

import numpy as np
import pytest

import vmaf_train.bisect_model_quality as bmq
from vmaf_train.bisect_model_quality import (
    BisectResult,
    BisectStep,
    _gate,
    _resolve_threshold,
    bisect_model_quality,
)
from vmaf_train.eval import EvalReport


def _report(plcc: float = 0.99, srocc: float = 0.98, rmse: float = 0.01) -> EvalReport:
    return EvalReport(plcc=plcc, srocc=srocc, rmse=rmse, n=10)


def test_resolve_threshold_with_min_srocc() -> None:
    kind, value = _resolve_threshold(None, 0.85, None)
    assert kind == "min_srocc"
    assert value == 0.85


def test_resolve_threshold_with_min_plcc() -> None:
    kind, value = _resolve_threshold(0.9, None, None)
    assert kind == "min_plcc"
    assert value == 0.9


def test_resolve_threshold_with_max_rmse() -> None:
    kind, value = _resolve_threshold(None, None, 5.0)
    assert kind == "max_rmse"
    assert value == 5.0


def test_resolve_threshold_rejects_zero_gates() -> None:
    with pytest.raises(ValueError, match="exactly one"):
        _resolve_threshold(None, None, None)


def test_resolve_threshold_rejects_multiple_gates() -> None:
    with pytest.raises(ValueError, match="exactly one"):
        _resolve_threshold(0.9, 0.85, None)


def test_gate_min_srocc_branch() -> None:
    """Cover the ``kind == 'min_srocc'`` arm of ``_gate``."""
    good = _report(srocc=0.99)
    bad = _report(srocc=0.50)
    assert _gate("min_srocc", 0.9, good) is True
    assert _gate("min_srocc", 0.9, bad) is False


def test_gate_min_plcc_branch() -> None:
    assert _gate("min_plcc", 0.9, _report(plcc=0.95)) is True
    assert _gate("min_plcc", 0.9, _report(plcc=0.10)) is False


def test_gate_max_rmse_branch() -> None:
    assert _gate("max_rmse", 0.05, _report(rmse=0.01)) is True
    assert _gate("max_rmse", 0.05, _report(rmse=1.00)) is False


def test_gate_rejects_unknown_kind() -> None:
    """Cover the explicit ``ValueError`` raise on unknown threshold_kind."""
    with pytest.raises(ValueError, match="unknown threshold_kind"):
        _gate("never_used", 0.0, _report())


def test_bisect_result_to_dict_serialises_paths(tmp_path: Path) -> None:
    """Cover ``BisectResult.to_dict`` (lines 45-51)."""
    m_a = tmp_path / "a.onnx"
    m_b = tmp_path / "b.onnx"
    result = BisectResult(
        threshold_kind="min_plcc",
        threshold_value=0.9,
        n_models=2,
        first_bad_index=1,
        first_bad_model=m_b,
        last_good_index=0,
        last_good_model=m_a,
        steps=[
            BisectStep(index=0, model=m_a, report=_report(), passed=True),
            BisectStep(index=1, model=m_b, report=_report(plcc=0.1), passed=False),
        ],
        verdict="ok",
    )
    d = result.to_dict()
    # Paths get stringified.
    assert d["first_bad_model"] == str(m_b)
    assert d["last_good_model"] == str(m_a)
    assert d["steps"][0]["model"] == str(m_a)
    assert d["steps"][1]["model"] == str(m_b)
    assert d["threshold_kind"] == "min_plcc"


def test_bisect_result_to_dict_handles_none_paths() -> None:
    """``None`` model paths must serialise to ``None`` (not the string 'None')."""
    result = BisectResult(threshold_kind="min_plcc", threshold_value=0.9, n_models=0)
    d = result.to_dict()
    assert d["first_bad_model"] is None
    assert d["last_good_model"] is None
    assert d["steps"] == []


def test_bisect_caches_repeated_index_visits(tmp_path: Path) -> None:
    """Cover the ``if idx in cache: return cache[idx]`` branch."""
    paths = [tmp_path / f"m{i}.onnx" for i in range(5)]
    feats = np.zeros((4, 6), dtype=np.float32)
    targets = np.zeros(4, dtype=np.float32)

    # Build a deterministic "good before idx 3, bad from idx 3" schedule.
    call_counts: dict[int, int] = {}

    def _fake_eval(model, _features, _targets, input_name="input"):
        # Map model path back to its index.
        idx = int(model.stem.lstrip("m"))
        call_counts[idx] = call_counts.get(idx, 0) + 1
        return _report(plcc=0.99 if idx < 3 else 0.10)

    with patch.object(bmq, "evaluate_onnx", side_effect=_fake_eval):
        result = bisect_model_quality(paths, feats, targets, min_plcc=0.9)

    # No index visited more than once → cache reuse worked.
    assert all(c == 1 for c in call_counts.values())
    # The bisect must have localised the transition correctly.
    assert result.first_bad_index == 3
    assert result.last_good_index == 2
    # All recorded steps are unique by index.
    seen = [s.index for s in result.steps]
    assert len(seen) == len(set(seen))


def test_bisect_with_srocc_gate(tmp_path: Path) -> None:
    """Drive a full bisect through the min_srocc path."""
    paths = [tmp_path / f"m{i}.onnx" for i in range(4)]
    feats = np.zeros((4, 6), dtype=np.float32)
    targets = np.zeros(4, dtype=np.float32)

    def _fake_eval(model, *_args, **_kwargs):
        idx = int(model.stem.lstrip("m"))
        return _report(srocc=0.95 if idx < 2 else 0.30)

    with patch.object(bmq, "evaluate_onnx", side_effect=_fake_eval):
        result = bisect_model_quality(paths, feats, targets, min_srocc=0.8)

    assert result.threshold_kind == "min_srocc"
    assert result.first_bad_index == 2
    assert result.last_good_index == 1


def test_bisect_step_dataclass_holds_components(tmp_path: Path) -> None:
    step = BisectStep(index=3, model=tmp_path / "x.onnx", report=_report(), passed=True)
    assert step.index == 3
    assert step.passed is True

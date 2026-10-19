# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Cross-backend parity gate — CPU reference vs other ORT providers.

Most CI runners only ship the CPUExecutionProvider, so these tests
exercise the graph-shape inference, missing-provider bookkeeping, and
self-compare path (CPU vs CPU == 0 diff). The real CUDA / OpenVINO
parity check runs downstream where those providers are installed.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

import numpy as np
import onnx
import onnxruntime as ort
import pandas as pd
import pytest
from onnx import TensorProto, helper

from vmaf_train.cross_backend import CPU_PROVIDER, compare_backends, render_table
from vmaf_train.features import FEATURE_COLUMNS


def _tiny_mlp(path: Path, in_features: int = 6) -> None:
    x = helper.make_tensor_value_info("x", TensorProto.FLOAT, ["N", in_features])
    y = helper.make_tensor_value_info("y", TensorProto.FLOAT, ["N", 1])
    w = helper.make_tensor("W", TensorProto.FLOAT, [in_features, 1], [0.1] * in_features)
    b = helper.make_tensor("B", TensorProto.FLOAT, [1], [0.0])
    node = helper.make_node("Gemm", ["x", "W", "B"], ["y"])
    graph = helper.make_graph([node], "mlp", [x], [y], [w, b])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    onnx.save(model, str(path))


def test_cpu_vs_cpu_is_zero_diff(tmp_path: Path) -> None:
    """Using the reference as the 'other' provider must give exactly 0 error."""
    p = tmp_path / "m.onnx"
    _tiny_mlp(p)
    report = compare_backends(p, providers=[CPU_PROVIDER])
    assert len(report.comparisons) == 1
    c = report.comparisons[0]
    assert c.provider == CPU_PROVIDER
    assert c.max_abs_error == 0.0
    assert c.mean_abs_error == 0.0
    assert report.ok


def test_missing_provider_recorded_and_fails_closed(tmp_path: Path) -> None:
    p = tmp_path / "m.onnx"
    _tiny_mlp(p)
    report = compare_backends(p, providers=["DoesNotExistExecutionProvider"])
    assert "DoesNotExistExecutionProvider" in report.missing
    assert report.comparisons == []
    # Nothing was compared, so nothing passed: `all([])` is True and used to make
    # this report ok (T-TINY-AI-CROSS-DEVICE-PARITY-UNGATED-2026-09-25).
    assert not report.ok
    assert report.to_dict()["ok"] is False


def test_cpu_only_install_with_default_providers_fails_closed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """No `--provider` on a host whose only provider is the CPU compares nothing: not ok."""
    p = tmp_path / "m.onnx"
    _tiny_mlp(p)
    monkeypatch.setattr(ort, "get_available_providers", lambda: [CPU_PROVIDER])
    report = compare_backends(p)
    assert report.comparisons == [] and report.missing == []
    assert not report.ok


def test_one_missing_provider_fails_even_when_another_compared(tmp_path: Path) -> None:
    p = tmp_path / "m.onnx"
    _tiny_mlp(p)
    report = compare_backends(p, providers=[CPU_PROVIDER, "DoesNotExistExecutionProvider"])
    assert len(report.comparisons) == 1 and report.missing
    assert not report.ok


def test_session_that_falls_back_to_cpu_is_reported_unbound(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """ORT builds a session for an unavailable-at-runtime EP on the CPU; that is not a comparison."""
    p = tmp_path / "m.onnx"
    _tiny_mlp(p)
    real = ort.InferenceSession

    class Fallback:
        def __init__(self, path: str, providers: list[str]) -> None:
            self._s = real(path, providers=[CPU_PROVIDER])

        def get_inputs(self) -> Any:
            return self._s.get_inputs()

        def run(self, *a: Any, **k: Any) -> Any:
            return self._s.run(*a, **k)

        def get_providers(self) -> list[str]:
            return [CPU_PROVIDER]

    monkeypatch.setattr(
        ort, "get_available_providers", lambda: [CPU_PROVIDER, "FakeExecutionProvider"]
    )
    calls = {"n": 0}

    def factory(path: str, providers: list[str]) -> Any:
        calls["n"] += 1
        return (
            real(path, providers=providers)
            if providers == [CPU_PROVIDER] and calls["n"] == 1
            else Fallback(path, providers)
        )

    monkeypatch.setattr(ort, "InferenceSession", factory)
    report = compare_backends(p, providers=["FakeExecutionProvider"])
    assert report.unbound == ["FakeExecutionProvider"]
    assert report.comparisons == []
    assert not report.ok


def test_synthetic_shape_inference(tmp_path: Path) -> None:
    p = tmp_path / "m.onnx"
    _tiny_mlp(p, in_features=6)
    report = compare_backends(p, providers=[CPU_PROVIDER])
    # Dynamic "N" gets resolved to 4 by the shape inference heuristic.
    assert report.comparisons[0].shape == (4, 6)


def test_explicit_synthetic_shape(tmp_path: Path) -> None:
    p = tmp_path / "m.onnx"
    _tiny_mlp(p, in_features=6)
    report = compare_backends(p, providers=[CPU_PROVIDER], shape=(16, 6))
    assert report.comparisons[0].shape == (16, 6)


def test_features_parquet_path(tmp_path: Path) -> None:
    p = tmp_path / "m.onnx"
    _tiny_mlp(p, in_features=len(FEATURE_COLUMNS))
    rng = np.random.default_rng(0)
    df = pd.DataFrame({c: rng.standard_normal(50).astype(np.float32) for c in FEATURE_COLUMNS})
    parquet = tmp_path / "f.parquet"
    df.to_parquet(parquet)
    report = compare_backends(p, providers=[CPU_PROVIDER], features=parquet, n_rows=10)
    assert report.comparisons[0].shape == (10, len(FEATURE_COLUMNS))


def test_render_table_and_json(tmp_path: Path) -> None:
    p = tmp_path / "m.onnx"
    _tiny_mlp(p)
    report = compare_backends(p, providers=[CPU_PROVIDER])
    table = render_table(report)
    assert "provider" in table and "max" in table
    data = report.to_dict()
    assert data["ok"] is True
    assert data["reference_provider"] == CPU_PROVIDER
    assert isinstance(data["comparisons"][0]["shape"], list)


def test_atol_gate_flags_over_threshold(tmp_path: Path) -> None:
    """Fake a diff by feeding the 'other' run a different input shape.

    We can't induce a real CPU-vs-CPU diff, so this test instead verifies
    the ok-property arithmetic by constructing a report where a
    comparison's max_abs_error exceeds atol.
    """
    from vmaf_train.cross_backend import BackendComparison, CrossBackendReport

    r = CrossBackendReport(model=Path("x.onnx"), atol=1e-4)
    r.comparisons.append(
        BackendComparison(provider="X", max_abs_error=1e-3, mean_abs_error=1e-4, shape=(1,))
    )
    assert not r.ok
    r2 = CrossBackendReport(model=Path("x.onnx"), atol=1e-2)
    r2.comparisons.append(
        BackendComparison(provider="X", max_abs_error=1e-3, mean_abs_error=1e-4, shape=(1,))
    )
    assert r2.ok


def test_features_parquet_missing_columns(tmp_path: Path) -> None:
    p = tmp_path / "m.onnx"
    _tiny_mlp(p, in_features=len(FEATURE_COLUMNS))
    parquet = tmp_path / "bad.parquet"
    pd.DataFrame({"not_a_feature": [1.0, 2.0]}).to_parquet(parquet)
    with pytest.raises(ValueError):
        compare_backends(p, providers=[CPU_PROVIDER], features=parquet)

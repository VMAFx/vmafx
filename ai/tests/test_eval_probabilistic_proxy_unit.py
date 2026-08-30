# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for :mod:`ai.scripts.eval_probabilistic_proxy`.

Covers the pure-numpy helpers (``_z_for_coverage``, ``_coverage_metrics``,
``_synthesize_smoke_corpus``, ``_predict_ensemble``) plus the ``main``
entry point under ORT / pandas mocks.
"""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
from unittest.mock import MagicMock, patch

import numpy as np
import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "ai" / "scripts" / "eval_probabilistic_proxy.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("epp_under_test", _SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


EPP = _load_module()


# ---------------------------------------------------------------------------
# _z_for_coverage
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "cov,expected",
    [
        (0.50, 0.6744897501960817),
        (0.80, 1.2815515655446004),
        (0.90, 1.6448536269514722),
        (0.95, 1.959963984540054),
        (0.99, 2.5758293035489004),
    ],
)
def test_z_for_coverage_tabulated_levels(cov: float, expected: float) -> None:
    assert EPP._z_for_coverage(cov) == pytest.approx(expected)


def test_z_for_coverage_non_tabulated_body() -> None:
    # 0.85 is not in the table — uses Acklam approximation.
    z = EPP._z_for_coverage(0.85)
    # Reference value from scipy.stats.norm.ppf(0.925) ≈ 1.4395
    assert z == pytest.approx(1.4395314709384563, rel=1e-6)


def test_z_for_coverage_increases_with_level() -> None:
    levels = [0.50, 0.60, 0.70, 0.80, 0.90, 0.95, 0.99]
    zs = [EPP._z_for_coverage(c) for c in levels]
    assert zs == sorted(zs), "z must monotonically increase with coverage"


# ---------------------------------------------------------------------------
# _synthesize_smoke_corpus
# ---------------------------------------------------------------------------


def test_synthesize_smoke_corpus_shapes() -> None:
    features, codec_onehot, target = EPP._synthesize_smoke_corpus(n_rows=50, num_codecs=4)
    assert features.shape == (50, 6)
    assert codec_onehot.shape == (50, 4)
    assert target.shape == (50,)


def test_synthesize_smoke_corpus_target_in_range() -> None:
    _, _, target = EPP._synthesize_smoke_corpus(n_rows=200, num_codecs=6)
    assert target.min() >= 0.0
    assert target.max() <= 100.0


def test_synthesize_smoke_corpus_deterministic_under_same_seed() -> None:
    a = EPP._synthesize_smoke_corpus(n_rows=20, num_codecs=3, seed=99)
    b = EPP._synthesize_smoke_corpus(n_rows=20, num_codecs=3, seed=99)
    np.testing.assert_array_equal(a[0], b[0])
    np.testing.assert_array_equal(a[1], b[1])
    np.testing.assert_array_equal(a[2], b[2])


def test_synthesize_smoke_corpus_codec_onehot_is_onehot() -> None:
    _, codec_onehot, _ = EPP._synthesize_smoke_corpus(n_rows=40, num_codecs=5)
    # exactly one nonzero per row
    nonzero = (codec_onehot != 0).sum(axis=1)
    assert (nonzero == 1).all()


# ---------------------------------------------------------------------------
# _coverage_metrics
# ---------------------------------------------------------------------------


def test_coverage_metrics_perfect_coverage() -> None:
    n = 100
    mu = np.full(n, 50.0)
    sigma = np.full(n, 5.0)
    # target is in the interval mu +/- z*sigma for any positive z when target == mu
    target = np.full(n, 50.0)
    out = EPP._coverage_metrics(mu, sigma, target)
    for key in ("50", "80", "95"):
        assert out["gaussian"][key]["empirical_coverage"] == pytest.approx(1.0)


def test_coverage_metrics_zero_coverage_when_target_outside() -> None:
    n = 50
    mu = np.zeros(n)
    sigma = np.ones(n) * 0.1
    target = np.full(n, 1000.0)  # absurdly far
    out = EPP._coverage_metrics(mu, sigma, target)
    assert out["gaussian"]["95"]["empirical_coverage"] == pytest.approx(0.0)


def test_coverage_metrics_width_grows_with_level() -> None:
    n = 30
    mu = np.zeros(n)
    sigma = np.ones(n)
    target = mu.copy()
    out = EPP._coverage_metrics(mu, sigma, target)
    w50 = out["gaussian"]["50"]["mean_width"]
    w80 = out["gaussian"]["80"]["mean_width"]
    w95 = out["gaussian"]["95"]["mean_width"]
    assert w50 < w80 < w95


def test_coverage_metrics_conformal_row_present_when_q_given() -> None:
    n = 40
    mu = np.zeros(n)
    sigma = np.ones(n)
    target = mu.copy()
    out = EPP._coverage_metrics(mu, sigma, target, conformal_q=1.5)
    assert "conformal" in out
    assert out["conformal"]["q"] == pytest.approx(1.5)
    assert out["conformal"]["empirical_coverage"] == pytest.approx(1.0)


def test_coverage_metrics_conformal_row_absent_when_q_none() -> None:
    out = EPP._coverage_metrics(
        np.zeros(5),
        np.ones(5),
        np.zeros(5),
        conformal_q=None,
    )
    assert "conformal" not in out


# ---------------------------------------------------------------------------
# _predict_ensemble
# ---------------------------------------------------------------------------


def test_predict_ensemble_stacks_sessions() -> None:
    n_rows = 7
    num_codecs = 3

    class FakeSession:
        def __init__(self, bias: float) -> None:
            self.bias = bias

        def run(self, _outputs, _feeds):
            n = _feeds["features"].shape[0]
            return [np.full((n, 1), self.bias, dtype=np.float32)]

    sessions = [FakeSession(b) for b in (10.0, 20.0, 30.0)]
    features = np.zeros((n_rows, 6), dtype=np.float32)
    onehot = np.zeros((n_rows, num_codecs), dtype=np.float32)

    preds = EPP._predict_ensemble(sessions, features, onehot)
    assert preds.shape == (3, n_rows)
    assert preds[0].mean() == pytest.approx(10.0)
    assert preds[1].mean() == pytest.approx(20.0)
    assert preds[2].mean() == pytest.approx(30.0)


# ---------------------------------------------------------------------------
# main entry point (smoke + missing manifest + bad parquet)
# ---------------------------------------------------------------------------


def _write_manifest(path: Path, num_codecs: int = 3, conformal_q: float | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # Each ensemble member references its own ONNX file beside the manifest.
    members = [{"onnx": f"member_{i}.onnx"} for i in range(2)]
    for m in members:
        (path.parent / m["onnx"]).write_bytes(b"\x00")  # placeholder
    payload: dict = {
        "members": members,
        "feature_mean": [0.5] * 6,
        "feature_std": [0.2] * 6,
        "codec_vocab": [f"c{i}" for i in range(num_codecs)],
    }
    if conformal_q is not None:
        payload["confidence"] = {"conformal_q_residual": conformal_q}
    path.write_text(json.dumps(payload))


def _make_fake_session(bias: float, num_codecs: int = 3):
    sess = MagicMock()

    def _run(_outputs, feeds):
        n = feeds["features"].shape[0]
        return [np.full((n, 1), bias, dtype=np.float32) + feeds["features"][:, :1]]

    sess.run.side_effect = _run
    # The main() path calls sessions[0].get_inputs() to derive codec_onehot dim.
    codec_input = MagicMock()
    codec_input.name = "codec_onehot"
    codec_input.shape = [None, num_codecs]
    sess.get_inputs.return_value = [codec_input]
    return sess


def test_main_exits_2_when_manifest_missing(tmp_path: Path, capsys: pytest.CaptureFixture) -> None:
    rc = EPP.main(["--manifest", str(tmp_path / "nope.json"), "--smoke"])
    assert rc == 2
    err = capsys.readouterr().err
    assert "manifest missing" in err


def test_main_smoke_path_returns_zero(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    manifest = tmp_path / "ens.json"
    _write_manifest(manifest, num_codecs=3, conformal_q=1.7)

    sessions = [_make_fake_session(0.0), _make_fake_session(5.0)]
    fake_load = MagicMock(return_value=(json.loads(manifest.read_text()), sessions))

    with patch.object(EPP, "_load_ensemble", fake_load):
        rc = EPP.main(["--manifest", str(manifest), "--smoke"])
    assert rc == 0
    fake_load.assert_called_once()


def test_main_writes_metrics_out(tmp_path: Path) -> None:
    manifest = tmp_path / "ens.json"
    _write_manifest(manifest, num_codecs=3)
    metrics_out = tmp_path / "report.json"

    sessions = [_make_fake_session(0.0), _make_fake_session(5.0)]
    fake_load = MagicMock(return_value=(json.loads(manifest.read_text()), sessions))
    with patch.object(EPP, "_load_ensemble", fake_load):
        rc = EPP.main(
            [
                "--manifest",
                str(manifest),
                "--smoke",
                "--metrics-out",
                str(metrics_out),
            ]
        )
    assert rc == 0
    assert metrics_out.is_file()
    report = json.loads(metrics_out.read_text())
    assert "coverage" in report
    assert report["ensemble_size"] == 2

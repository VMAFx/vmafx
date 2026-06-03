# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Local sidecar training — unit tests.

Pins (per ADR-0325 / Research-0086):

* :class:`SidecarPredictor` cold-starts to *zero* correction — the
  composed predictor is bit-equivalent to the bare :class:`Predictor`
  before any captures land.
* After ``N`` captures, the sidecar's prediction is closer to the
  observed VMAF than the bare predictor's (residual-RMS reduction).
* Save / load round-trips preserve weights, ``A_inv``, history, and
  ``n_updates`` exactly.
* The anonymous host UUID is stable across :class:`SidecarPredictor`
  reconstructions within the same install (i.e. against the same
  ``cache_dir``).
* Bumping the predictor version invalidates the saved sidecar state
  cleanly — load returns a cold-start model rather than fitting a
  fresh predictor against a stale correction.

All tests are CPU-only, sub-second, and write to a pytest ``tmp_path``
— they never touch the operator's real ``~/.cache/vmaf-tune/``.
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.predictor import Predictor, ShotFeatures  # noqa: E402
from vmaftune.sidecar import (  # noqa: E402
    FEATURE_DIM,
    SIDECAR_SCHEMA_VERSION,
    SidecarConfig,
    SidecarModel,
    SidecarPredictor,
    get_or_create_host_uuid,
)

# ----------------------------------------------------------------------
# Helpers.
# ----------------------------------------------------------------------


def _make_features(probe_kbps: float = 3000.0) -> ShotFeatures:
    """Return a deterministic, plausible :class:`ShotFeatures`."""
    return ShotFeatures(
        probe_bitrate_kbps=probe_kbps,
        probe_i_frame_avg_bytes=10000.0,
        probe_p_frame_avg_bytes=2000.0,
        probe_b_frame_avg_bytes=1000.0,
        saliency_mean=0.3,
        saliency_var=0.05,
        frame_diff_mean=2.5,
        y_avg=128.0,
        y_var=400.0,
        shot_length_frames=120,
        fps=24.0,
        width=1920,
        height=1080,
    )


def _config(tmp_path: Path, predictor_version: str = "predictor_v1") -> SidecarConfig:
    """Pin the cache_dir into a pytest-managed tmp dir."""
    return SidecarConfig(
        cache_dir=tmp_path / "vmaf-tune-sidecar",
        predictor_version=predictor_version,
        # Tighten history so save/load round-trip is cheap to assert.
        max_history_rows=32,
    )


# ----------------------------------------------------------------------
# Cold-start.
# ----------------------------------------------------------------------


def test_cold_start_passes_through(tmp_path: Path):
    """Empty sidecar adds zero correction; composed prediction == bare."""
    p = Predictor()
    cfg = _config(tmp_path)
    sp = SidecarPredictor.for_codec(p, codec="libx264", config=cfg)

    feats = _make_features()
    crf = 28
    base = p.predict_vmaf(feats, crf, "libx264")
    composed = sp.predict_vmaf(feats, crf)

    assert sp.model.predict_correction(feats, crf) == 0.0
    assert composed == pytest.approx(base, abs=1e-12)
    assert sp.model.n_updates == 0
    assert sp.model.recent_residual_rms == 0.0


def test_cold_start_feature_vector_dim_matches_published_constant(tmp_path: Path):
    """``FEATURE_DIM`` is the load-bearing column-index pin."""
    cfg = _config(tmp_path)
    model = SidecarModel(config=cfg)
    assert len(model.weights) == FEATURE_DIM
    assert len(model.a_inv) == FEATURE_DIM
    assert all(len(row) == FEATURE_DIM for row in model.a_inv)


# ----------------------------------------------------------------------
# Update reduces residual.
# ----------------------------------------------------------------------


def test_update_then_predict_reduces_residual(tmp_path: Path):
    """Feed N captures with a constant +5 VMAF bias; assert later
    sidecar predictions are closer to observed than the bare predictor.
    """
    p = Predictor()
    cfg = _config(tmp_path)
    sp = SidecarPredictor.for_codec(p, codec="libx264", config=cfg)

    crf = 28
    bias = 5.0
    # Simulate N encodes where the operator's content runs ~5 VMAF
    # higher than the shipped predictor expects (e.g. their content is
    # easier-to-encode than the training corpus).
    train_features = [_make_features(probe_kbps=2000.0 + 200.0 * i) for i in range(40)]
    for f in train_features:
        predicted = p.predict_vmaf(f, crf, "libx264")
        observed = predicted + bias
        sp.record_capture(f, crf=crf, observed_vmaf=observed, persist=False)

    # Now evaluate on a held-out feature that is similar in scale.
    held = _make_features(probe_kbps=4500.0)
    bare_pred = p.predict_vmaf(held, crf, "libx264")
    composed_pred = sp.predict_vmaf(held, crf)
    observed = bare_pred + bias

    bare_residual = abs(observed - bare_pred)
    composed_residual = abs(observed - composed_pred)
    # Sidecar should absorb at least half the constant bias after 40
    # captures. Tightening this further is not the scaffold's job; the
    # contract is "later prediction is closer than bare".
    assert composed_residual < bare_residual
    assert composed_residual < bias * 0.5
    assert sp.model.n_updates == len(train_features)
    # Drift hook: rolling residual RMS should be positive (we *do* see
    # residuals during training).
    assert sp.model.recent_residual_rms > 0.0


# ----------------------------------------------------------------------
# Save / load round-trip.
# ----------------------------------------------------------------------


def test_save_load_round_trip(tmp_path: Path):
    """Train, save, load — assert weights / A_inv / history restored."""
    p = Predictor()
    cfg = _config(tmp_path)
    sp = SidecarPredictor.for_codec(p, codec="libx264", config=cfg)

    crf = 28
    for i in range(10):
        f = _make_features(probe_kbps=2000.0 + 100.0 * i)
        predicted = p.predict_vmaf(f, crf, "libx264")
        sp.record_capture(f, crf=crf, observed_vmaf=predicted + 3.0, persist=False)

    sp.save()
    assert sp.state_path.is_file()

    # Reload via a fresh SidecarPredictor.for_codec — this is the
    # operator-facing path on a subsequent process invocation.
    sp2 = SidecarPredictor.for_codec(p, codec="libx264", config=cfg)

    assert sp2.model.weights == pytest.approx(sp.model.weights, abs=0.0)
    for r1, r2 in zip(sp2.model.a_inv, sp.model.a_inv, strict=True):
        assert r1 == pytest.approx(r2, abs=0.0)
    assert sp2.model.n_updates == sp.model.n_updates
    assert len(sp2.model.history) == len(sp.model.history)

    # And the prediction itself round-trips.
    held = _make_features(probe_kbps=4200.0)
    assert sp2.predict_vmaf(held, crf) == pytest.approx(sp.predict_vmaf(held, crf), abs=1e-9)


def test_save_load_handles_corrupted_json_as_cold_start(tmp_path: Path):
    """Garbage JSON falls back to cold-start (corrupted file preserved)."""
    p = Predictor()
    cfg = _config(tmp_path)
    sp = SidecarPredictor.for_codec(p, codec="libx264", config=cfg)
    sp.state_path.parent.mkdir(parents=True, exist_ok=True)
    sp.state_path.write_text("{ not valid json", encoding="utf-8")

    sp2 = SidecarPredictor.for_codec(p, codec="libx264", config=cfg)
    assert sp2.model.n_updates == 0
    feats = _make_features()
    assert sp2.model.predict_correction(feats, crf=28) == 0.0


def test_save_writes_strict_json_and_invalid_nonfinite_loads_cold(tmp_path: Path):
    """Persistence JSON is portable; non-finite state reloads as cold-start."""
    cfg = _config(tmp_path)
    state_path = cfg.cache_dir / cfg.predictor_version / "libx264" / "state.json"
    model = SidecarModel(config=cfg)
    model.weights[0] = math.nan
    model.history.append(
        {
            "residual": math.inf,
            "predicted_vmaf": 91.0,
            "observed_vmaf": math.nan,
        }
    )

    model.save(state_path)
    raw = state_path.read_text(encoding="utf-8")
    assert "NaN" not in raw
    assert "Infinity" not in raw
    payload = json.loads(raw)
    assert payload["weights"][0] is None
    assert payload["history"][0]["residual"] is None
    assert payload["history"][0]["observed_vmaf"] is None

    reloaded = SidecarModel.load(state_path, cfg)
    assert reloaded.n_updates == 0
    assert reloaded.weights == [0.0] * FEATURE_DIM


# ----------------------------------------------------------------------
# Anonymous host UUID — stability + non-machine-derivation.
# ----------------------------------------------------------------------


def test_anonymised_host_uuid_stable_within_install(tmp_path: Path):
    """The UUID persists across SidecarPredictor reconstructions."""
    p = Predictor()
    cfg = _config(tmp_path)
    sp1 = SidecarPredictor.for_codec(p, codec="libx264", config=cfg)
    sp2 = SidecarPredictor.for_codec(p, codec="libx265", config=cfg)
    assert sp1.host_uuid == sp2.host_uuid
    # And the persisted file contains it.
    persisted = (cfg.cache_dir / "host-uuid").read_text(encoding="utf-8").strip()
    assert persisted == sp1.host_uuid


def test_host_uuid_is_random_per_install(tmp_path: Path):
    """Two separate cache dirs ⇒ two distinct UUIDs.

    Confirms the UUID is not derived from machine-identifying info
    (MAC / hostname / machine-id) — those would yield the same value
    for both installs on the same host.
    """
    a = get_or_create_host_uuid(tmp_path / "install_a")
    b = get_or_create_host_uuid(tmp_path / "install_b")
    assert a != b
    # 32 hex chars = 128 bits.
    assert len(a) == 32
    assert len(b) == 32
    # Hex-only.
    int(a, 16)
    int(b, 16)


# ----------------------------------------------------------------------
# Predictor-version invalidation.
# ----------------------------------------------------------------------


def test_predictor_version_change_invalidates_sidecar(tmp_path: Path):
    """Bumping the predictor version triggers a clean sidecar."""
    p = Predictor()
    cfg_v1 = _config(tmp_path, predictor_version="predictor_v1")
    sp = SidecarPredictor.for_codec(p, codec="libx264", config=cfg_v1)
    crf = 28
    for i in range(8):
        f = _make_features(probe_kbps=2000.0 + 100.0 * i)
        predicted = p.predict_vmaf(f, crf, "libx264")
        sp.record_capture(f, crf=crf, observed_vmaf=predicted + 4.0, persist=False)
    sp.save()
    assert sp.model.n_updates == 8

    # Same cache dir, bumped predictor version. The state from v1
    # lives at <cache>/predictor_v1/libx264/state.json; v2 looks at
    # <cache>/predictor_v2/libx264/state.json which doesn't exist →
    # cold-start.
    cfg_v2 = SidecarConfig(
        cache_dir=cfg_v1.cache_dir,
        predictor_version="predictor_v2",
        max_history_rows=cfg_v1.max_history_rows,
    )
    sp2 = SidecarPredictor.for_codec(p, codec="libx264", config=cfg_v2)
    assert sp2.model.n_updates == 0
    assert sp2.model.predict_correction(_make_features(), crf=crf) == 0.0
    # Host UUID is preserved across the version bump (it lives at the
    # cache-dir root, above the per-version subtree).
    assert sp2.host_uuid == sp.host_uuid


def test_state_load_rejects_mismatched_predictor_version_in_payload(tmp_path: Path):
    """A hand-crafted state file with a stale ``predictor_version`` is
    discarded — defends against an operator copying state between
    predictor versions out-of-band."""
    import json as _json

    cfg = _config(tmp_path, predictor_version="predictor_v2")
    state_path = cfg.cache_dir / cfg.predictor_version / "libx264" / "state.json"
    state_path.parent.mkdir(parents=True, exist_ok=True)
    # Synthesise a payload that *claims* to be from predictor_v1 but
    # lives at the v2 path.
    payload = {
        "schema_version": SIDECAR_SCHEMA_VERSION,
        "predictor_version": "predictor_v1",
        "feature_dim": FEATURE_DIM,
        "lambda_l2": 1.0,
        "weights": [0.1] * FEATURE_DIM,
        "a_inv": [[1.0 if i == j else 0.0 for j in range(FEATURE_DIM)] for i in range(FEATURE_DIM)],
        "history": [],
        "n_updates": 5,
    }
    state_path.write_text(_json.dumps(payload), encoding="utf-8")

    model = SidecarModel.load(state_path, cfg)
    # Cold-start because predictor_version mismatched.
    assert model.n_updates == 0
    assert model.weights == [0.0] * FEATURE_DIM

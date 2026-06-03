# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Phase 3 of ADR-0325 — synthetic-corpus smoke + gate tests for the MOS head.

The trainer in :mod:`ai.scripts.train_konvid_mos_head` produces a
deterministic-seeded ONNX from a synthetic MOS corpus only when
``--smoke`` is explicit. This test suite pins:

* The trainer is importable and exposes the documented constants
  (FEATURE_COLUMNS, ENCODER_VOCAB_V4, gate thresholds).
* ``_synthesize_corpus`` is bit-stable under a fixed seed.
* ``_kfold_indices`` produces non-overlapping (train, val) folds
  whose union covers every row.
* ``_load_corpus`` accepts the Phase-1/2 KonViD JSONL row shape
  (`mos`, `corpus`, ...) and projects to ``(features, encoder, mos)``
  with content-independent zero fills for the columns the row does
  not yet carry.
* The synthetic-corpus 5-fold gate clears mean PLCC ≥ 0.75 (the
  placeholder threshold for Phase 3 prior to real-corpus data).
* The shipped checkpoint round-trips through :mod:`onnx.checker`
  and emits only ops on the fork's allowlist.

Real-corpus PLCC ≥ 0.85 / SROCC ≥ 0.82 / RMSE ≤ 0.45 is the
production-flip gate from ADR-0325; that gate fires when the real
KonViD JSONL drop is on disk and the trainer is invoked with
``--konvid-150k <path>``. Per the ``feedback_no_test_weakening``
memory, the gate is **not** lowered if the real-corpus retrain
misses — the model ships ``Status: Proposed`` instead.
"""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "scripts"))

np = pytest.importorskip("numpy")
torch = pytest.importorskip("torch")

import train_konvid_mos_head as trainer  # noqa: E402

_CHUG_SCRIPT_PATH = REPO_ROOT / "ai" / "scripts" / "train_chug_hdr_mos_head.py"
_CHUG_SPEC = importlib.util.spec_from_file_location("train_chug_hdr_mos_head", _CHUG_SCRIPT_PATH)
assert _CHUG_SPEC is not None and _CHUG_SPEC.loader is not None
chug_trainer = importlib.util.module_from_spec(_CHUG_SPEC)
sys.modules[_CHUG_SPEC.name] = chug_trainer
_CHUG_SPEC.loader.exec_module(chug_trainer)

# ---------------------------------------------------------------------
# Schema pins — fail loudly if any constant drifts under us.
# ---------------------------------------------------------------------


def test_feature_columns_layout() -> None:
    assert trainer.CANONICAL_6 == (
        "adm2",
        "vif_scale0",
        "vif_scale1",
        "vif_scale2",
        "vif_scale3",
        "motion2",
    )
    assert trainer.EXTRA_FEATURES == (
        "saliency_mean",
        "saliency_var",
        "shot_count_norm",
        "shot_mean_len_norm",
        "shot_cut_density",
    )
    assert trainer.FEATURE_COLUMNS == trainer.CANONICAL_6 + trainer.EXTRA_FEATURES
    assert trainer.N_FEATURES == 11
    assert trainer.FEATURE_SCHEMA_KONVID_V1 == "konvid-v1"
    assert trainer.FEATURE_SCHEMAS[trainer.FEATURE_SCHEMA_KONVID_V1] == trainer.FEATURE_COLUMNS


def test_chug_hdr_wide_feature_schema_layout() -> None:
    expected_temporal = tuple(
        f"{name}_{stat}" for name in trainer.CANONICAL_6 for stat in ("p10", "p90", "std")
    )
    assert trainer.FEATURE_SCHEMA_CHUG_HDR_WIDE_V1 == "chug-hdr-wide-v1"
    assert expected_temporal == trainer.CHUG_HDR_TEMPORAL_FEATURES
    assert trainer.CHUG_HDR_METADATA_FEATURES == (
        "chug_bitrate_mbps",
        "chug_orientation_portrait",
        "chug_ref",
        "distorted_width_norm",
        "distorted_height_norm",
        "reference_width_norm",
        "reference_height_norm",
        "duration_s_norm",
        "n_feature_frames_norm",
        "feature_bitdepth_norm",
    )
    assert trainer.CHUG_HDR_FEATURE_COLUMNS == (
        trainer.CANONICAL_6
        + trainer.CHUG_HDR_TEMPORAL_FEATURES
        + trainer.CHUG_HDR_METADATA_FEATURES
    )
    assert len(trainer.CHUG_HDR_FEATURE_COLUMNS) == 34
    assert (
        trainer.FEATURE_SCHEMAS[trainer.FEATURE_SCHEMA_CHUG_HDR_WIDE_V1]
        == trainer.CHUG_HDR_FEATURE_COLUMNS
    )


def test_chug_hdr_display_feature_schema_layout() -> None:
    assert trainer.FEATURE_SCHEMA_CHUG_HDR_DISPLAY_V1 == "chug-hdr-display-v1"
    assert trainer.CHUG_HDR_DISPLAY_FEATURES == (
        "display_peak_luminance_nits_norm",
        "display_black_luminance_nits_norm",
        "display_contrast_ratio_log_norm",
        "display_ambient_lux_norm",
        "display_bt2020_coverage",
        "display_p3_coverage",
        "display_panel_oled",
        "display_panel_qled",
        "display_panel_lcd",
        "display_local_dimming",
        "display_tone_mapping_dynamic",
    )
    assert trainer.CHUG_HDR_DISPLAY_FEATURE_COLUMNS == (
        trainer.CHUG_HDR_FEATURE_COLUMNS + trainer.CHUG_HDR_DISPLAY_FEATURES
    )
    assert len(trainer.CHUG_HDR_DISPLAY_FEATURE_COLUMNS) == 45
    assert (
        trainer.FEATURE_SCHEMAS[trainer.FEATURE_SCHEMA_CHUG_HDR_DISPLAY_V1]
        == trainer.CHUG_HDR_DISPLAY_FEATURE_COLUMNS
    )


def test_encoder_vocab_v4_single_slot() -> None:
    assert trainer.ENCODER_VOCAB_V4 == ("ugc-mixed",)
    assert trainer.ENCODER_VOCAB_V4_VERSION == 4
    assert trainer.N_ENCODERS == 1


def test_mos_range() -> None:
    assert trainer.MOS_MIN == 1.0
    assert trainer.MOS_MAX == 5.0


def test_gate_thresholds_match_adr_0325() -> None:
    # ADR-0325 §Production-flip gate, mirrored verbatim.
    assert pytest.approx(0.85) == trainer.GATE_MEAN_PLCC
    assert pytest.approx(0.82) == trainer.GATE_SROCC
    assert pytest.approx(0.45) == trainer.GATE_RMSE_MAX
    assert pytest.approx(0.005) == trainer.GATE_SPREAD_MAX
    # Synthetic-corpus surrogate threshold (placeholder per the
    # task brief; replaced when real-corpus runs land).
    assert pytest.approx(0.75) == trainer.SYNTHETIC_GATE_PLCC


# ---------------------------------------------------------------------
# Synthetic-corpus determinism + shape.
# ---------------------------------------------------------------------


def test_synthesize_corpus_is_deterministic_under_seed() -> None:
    a_feat, a_enc, a_mos = trainer._synthesize_corpus(n_rows=200, seed=42)
    b_feat, b_enc, b_mos = trainer._synthesize_corpus(n_rows=200, seed=42)
    np.testing.assert_array_equal(a_feat, b_feat)
    np.testing.assert_array_equal(a_enc, b_enc)
    np.testing.assert_array_equal(a_mos, b_mos)


def test_synthesize_corpus_shape_and_range() -> None:
    feat, enc, mos = trainer._synthesize_corpus(n_rows=300, seed=0)
    assert feat.shape == (300, trainer.N_FEATURES)
    assert enc.shape == (300, trainer.N_ENCODERS)
    assert mos.shape == (300,)
    assert mos.min() >= trainer.MOS_MIN - 1e-6
    assert mos.max() <= trainer.MOS_MAX + 1e-6
    # The single ENCODER_VOCAB v4 slot is always asserted on.
    assert (enc == 1.0).all()


# ---------------------------------------------------------------------
# K-fold indices.
# ---------------------------------------------------------------------


def test_kfold_indices_partition_rows() -> None:
    folds = trainer._kfold_indices(100, k=5, seed=7)
    assert len(folds) == 5
    seen: set[int] = set()
    for train_idx, val_idx in folds:
        # train + val partition all rows for this fold.
        assert len(set(train_idx) & set(val_idx)) == 0
        assert len(train_idx) + len(val_idx) == 100
        seen.update(int(i) for i in val_idx)
    assert seen == set(range(100))


# ---------------------------------------------------------------------
# JSONL loader — accepts the Phase-1/2 row shape.
# ---------------------------------------------------------------------


def _write_konvid_jsonl(path: Path, n: int) -> None:
    rows = []
    for i in range(n):
        rows.append(
            {
                "src": f"clip_{i}.mp4",
                "src_sha256": "a" * 64,
                "src_size_bytes": 1024,
                "width": 540,
                "height": 960,
                "framerate": 30.0,
                "duration_s": 8.0,
                "pix_fmt": "yuv420p",
                "encoder_upstream": "h264",
                "mos": 1.5 + 3.0 * (i / max(1, n - 1)),
                "mos_std_dev": 0.4,
                "n_ratings": 5,
                "corpus": "konvid-1k",
                "corpus_version": "1.0",
                "ingested_at_utc": "2026-05-08T12:00:00Z",
            }
        )
    with path.open("w", encoding="utf-8") as fh:
        for row in rows:
            fh.write(json.dumps(row) + "\n")


def test_load_corpus_accepts_phase_1_jsonl_shape(tmp_path: Path) -> None:
    p = tmp_path / "konvid_1k.jsonl"
    _write_konvid_jsonl(p, n=20)
    feat, enc, mos = trainer._load_corpus([p])
    assert feat.shape == (20, trainer.N_FEATURES)
    assert enc.shape == (20, trainer.N_ENCODERS)
    assert mos.shape == (20,)
    # Phase-1 rows do not carry canonical-6 / saliency / shot columns
    # yet → defaults zero-fill.
    assert (feat == 0.0).all()
    # MOS round-trips.
    assert mos.min() >= trainer.MOS_MIN
    assert mos.max() <= trainer.MOS_MAX


def test_load_corpus_preserves_explicit_split_labels(tmp_path: Path) -> None:
    p = tmp_path / "chug_features.jsonl"
    rows = [
        {"src": "a.mp4", "mos": 3.0, "adm2": 0.9, "split": "train"},
        {"src": "b.mp4", "mos": 2.8, "adm2": 0.7, "split": "val"},
        {"src": "c.mp4", "mos": 2.5, "adm2": 0.6, "split": "test"},
        {"src": "d.mp4", "mos": 2.0, "adm2": 0.5},
    ]
    p.write_text("\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8")

    arrays = trainer._load_corpus_arrays([p])

    assert arrays.features.shape == (4, trainer.N_FEATURES)
    assert arrays.splits.tolist() == ["train", "val", "test", ""]


def test_chug_hdr_wide_projection_uses_temporal_and_hdr_metadata() -> None:
    row = {
        "mos": 2.5,
        "adm2": 0.80,
        "vif_scale0": 0.30,
        "vif_scale1": 0.40,
        "vif_scale2": 0.50,
        "vif_scale3": 0.60,
        "motion2": 7.0,
        "adm2_p10": 0.70,
        "adm2_p90": 0.90,
        "adm2_std": 0.05,
        "chug_bitrate_label": "0.5M",
        "chug_orientation": "Portrait",
        "chug_ref": 0,
        "width": 1080,
        "height": 1920,
        "feature_width": 1920,
        "feature_height": 1080,
        "duration_s": 6.0,
        "n_feature_frames": 180,
        "feature_bitdepth": 10,
    }

    projected = trainer._row_to_features(
        row,
        feature_columns=trainer.CHUG_HDR_FEATURE_COLUMNS,
    )

    assert projected is not None
    features, mos = projected
    idx = trainer.CHUG_HDR_FEATURE_COLUMNS.index
    assert features.shape == (34,)
    assert mos == pytest.approx(2.5)
    assert features[idx("adm2_p10")] == pytest.approx(0.70)
    assert features[idx("adm2_p90")] == pytest.approx(0.90)
    assert features[idx("adm2_std")] == pytest.approx(0.05)
    assert features[idx("chug_bitrate_mbps")] == pytest.approx(0.5)
    assert features[idx("chug_orientation_portrait")] == pytest.approx(1.0)
    assert features[idx("distorted_width_norm")] == pytest.approx(1080.0 / 3840.0)
    assert features[idx("distorted_height_norm")] == pytest.approx(1920.0 / 2160.0)
    assert features[idx("reference_width_norm")] == pytest.approx(1920.0 / 3840.0)
    assert features[idx("reference_height_norm")] == pytest.approx(1080.0 / 2160.0)
    assert features[idx("duration_s_norm")] == pytest.approx(0.1)
    assert features[idx("n_feature_frames_norm")] == pytest.approx(0.1)
    assert features[idx("feature_bitdepth_norm")] == pytest.approx(0.5)


def test_display_profile_payload_normalises_panel_context() -> None:
    values = trainer._normalise_display_profile_payload(
        {
            "display": {
                "peak_nits": 1000.0,
                "black_nits": 0.005,
                "ambient_lux": 25.0,
                "bt2020_coverage": 72.5,
                "p3_coverage": 0.95,
                "panel_type": "QLED LCD",
                "local_dimming": True,
                "tone_mapping": "HDR10+ dynamic",
            }
        }
    )

    assert values["display_peak_luminance_nits_norm"] == pytest.approx(0.1)
    assert values["display_black_luminance_nits_norm"] == pytest.approx(0.005)
    assert values["display_contrast_ratio_log_norm"] == pytest.approx(5.30103 / 7.0)
    assert values["display_ambient_lux_norm"] == pytest.approx(0.025)
    assert values["display_bt2020_coverage"] == pytest.approx(0.725)
    assert values["display_p3_coverage"] == pytest.approx(0.95)
    assert values["display_panel_oled"] == pytest.approx(0.0)
    assert values["display_panel_qled"] == pytest.approx(1.0)
    assert values["display_panel_lcd"] == pytest.approx(1.0)
    assert values["display_local_dimming"] == pytest.approx(1.0)
    assert values["display_tone_mapping_dynamic"] == pytest.approx(1.0)


def test_chug_hdr_display_projection_uses_row_then_profile_fallback() -> None:
    display_profile = trainer._normalise_display_profile_payload(
        {
            "peak_nits": 800.0,
            "black_nits": 0.01,
            "ambient_lux": 40.0,
            "bt2020_coverage": 0.80,
            "p3_coverage": 0.92,
            "panel_type": "OLED",
            "local_dimming": False,
            "tone_mapping": "static HDR10",
        }
    )
    row = {
        "mos_raw_0_100": 50.0,
        "adm2": 0.80,
        "display_peak_luminance_nits": 1200.0,
    }

    projected = trainer._row_to_features(
        row,
        feature_columns=trainer.CHUG_HDR_DISPLAY_FEATURE_COLUMNS,
        display_profile=display_profile,
    )

    assert projected is not None
    features, mos = projected
    idx = trainer.CHUG_HDR_DISPLAY_FEATURE_COLUMNS.index
    assert mos == pytest.approx(3.0)
    assert features[idx("display_peak_luminance_nits_norm")] == pytest.approx(0.12)
    assert features[idx("display_black_luminance_nits_norm")] == pytest.approx(0.01)
    assert features[idx("display_panel_oled")] == pytest.approx(1.0)
    assert features[idx("display_panel_lcd")] == pytest.approx(0.0)
    assert features[idx("display_tone_mapping_dynamic")] == pytest.approx(0.0)


def test_chug_hdr_entrypoint_accepts_feature_jsonl_and_custom_model_id(
    tmp_path: Path,
) -> None:
    p = tmp_path / "chug_features.jsonl"
    rows = []
    for idx in range(6):
        split = "train" if idx < 4 else "val"
        rows.append(
            {
                "src": f"chug-{idx}.mp4",
                "corpus": "chug",
                "mos": 2.0 + 0.3 * idx,
                "adm2": 0.55 + 0.05 * idx,
                "vif_scale0": 0.30 + 0.04 * idx,
                "vif_scale1": 0.35 + 0.04 * idx,
                "vif_scale2": 0.40 + 0.04 * idx,
                "vif_scale3": 0.45 + 0.04 * idx,
                "motion2": 1.0 + idx,
                "split": split,
            }
        )
    p.write_text("\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8")
    onnx_path = tmp_path / "chug_hdr_mos_head_v1.onnx"
    manifest_path = tmp_path / "chug_hdr_mos_head_v1.json"

    rc = chug_trainer.main(
        [
            "--feature-jsonl",
            str(p),
            "--model-id",
            "chug_hdr_mos_head_v1",
            "--epochs",
            "1",
            "--k-folds",
            "2",
            "--out-onnx",
            str(onnx_path),
            "--out-manifest",
            str(manifest_path),
            "--device",
            "cpu",
        ]
    )

    assert rc == 0
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["id"] == "chug_hdr_mos_head_v1"
    assert manifest["feature_schema"] == trainer.FEATURE_SCHEMA_CHUG_HDR_WIDE_V1
    assert manifest["feature_order"] == list(trainer.CHUG_HDR_FEATURE_COLUMNS)
    assert manifest["training_recipe"]["n_rows"] == 4
    assert manifest["folds"][0]["validation_policy"] == "explicit-corpus-split"
    assert manifest["run_provenance"]["entrypoint"]["path"].endswith(
        "ai/scripts/train_chug_hdr_mos_head.py"
    )
    assert manifest["run_provenance"]["shared_trainer"]["path"].endswith(
        "ai/scripts/train_konvid_mos_head.py"
    )
    assert manifest["run_provenance"]["inputs"]["feature_jsonl"][0]["kind"] == "file"


def test_chug_hdr_entrypoint_display_profile_selects_display_schema(
    tmp_path: Path,
) -> None:
    p = tmp_path / "chug_features.jsonl"
    rows = []
    for idx in range(6):
        split = "train" if idx < 4 else "val"
        rows.append(
            {
                "src": f"chug-display-{idx}.mp4",
                "corpus": "chug",
                "mos": 2.1 + 0.2 * idx,
                "adm2": 0.55 + 0.05 * idx,
                "vif_scale0": 0.30 + 0.04 * idx,
                "vif_scale1": 0.35 + 0.04 * idx,
                "vif_scale2": 0.40 + 0.04 * idx,
                "vif_scale3": 0.45 + 0.04 * idx,
                "motion2": 1.0 + idx,
                "split": split,
            }
        )
    p.write_text("\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8")
    profile_path = tmp_path / "display-profile.json"
    profile_path.write_text(
        json.dumps(
            {
                "display": {
                    "peak_nits": 1000.0,
                    "black_nits": 0.005,
                    "ambient_lux": 25.0,
                    "bt2020_coverage": 0.72,
                    "p3_coverage": 0.95,
                    "panel_type": "OLED",
                    "local_dimming": False,
                    "tone_mapping": "HDR10",
                }
            }
        ),
        encoding="utf-8",
    )
    onnx_path = tmp_path / "chug_hdr_display_mos_head_v1.onnx"
    manifest_path = tmp_path / "chug_hdr_display_mos_head_v1.json"

    rc = chug_trainer.main(
        [
            "--feature-jsonl",
            str(p),
            "--display-profile-json",
            str(profile_path),
            "--epochs",
            "1",
            "--k-folds",
            "2",
            "--out-onnx",
            str(onnx_path),
            "--out-manifest",
            str(manifest_path),
            "--device",
            "cpu",
        ]
    )

    assert rc == 0
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["feature_schema"] == trainer.FEATURE_SCHEMA_CHUG_HDR_DISPLAY_V1
    assert manifest["feature_order"] == list(trainer.CHUG_HDR_DISPLAY_FEATURE_COLUMNS)
    assert manifest["display_profile"]["schema"] == "chug-display-profile-v1"
    assert manifest["display_profile"]["source_path"] == str(profile_path)
    assert manifest["display_profile"]["feature_values"][
        "display_peak_luminance_nits_norm"
    ] == pytest.approx(0.1)
    assert manifest["training_recipe"]["n_rows"] == 4
    assert manifest["run_provenance"]["entrypoint"]["path"].endswith(
        "ai/scripts/train_chug_hdr_mos_head.py"
    )
    assert manifest["run_provenance"]["inputs"]["display_profile_json"]["sha256"]


def test_load_corpus_accepts_full_features_parquet(tmp_path: Path) -> None:
    pd = pytest.importorskip("pandas")
    p = tmp_path / "full_features.parquet"
    pd.DataFrame(
        [
            {
                "clip_name": "a.mp4",
                "mos": 3.2,
                "adm2_mean": 0.91,
                "vif_scale0_mean": 0.71,
                "vif_scale1_mean": 0.72,
                "vif_scale2_mean": 0.73,
                "vif_scale3_mean": 0.74,
                "motion2_mean": 3.5,
                "split": "train",
            },
            {
                "clip_name": "b.mp4",
                "mos_raw_0_100": 50.0,
                "adm2": 0.62,
                "motion2_mean": 1.5,
                "split": "val",
            },
        ]
    ).to_parquet(p)

    arrays = trainer._load_corpus_arrays([p])

    adm_idx = trainer.FEATURE_COLUMNS.index("adm2")
    motion_idx = trainer.FEATURE_COLUMNS.index("motion2")
    assert arrays.features.shape == (2, trainer.N_FEATURES)
    assert arrays.features[0, adm_idx] == pytest.approx(0.91)
    assert arrays.features[0, motion_idx] == pytest.approx(3.5)
    assert arrays.features[1, adm_idx] == pytest.approx(0.62)
    assert arrays.mos.tolist() == pytest.approx([3.2, 3.0])
    assert arrays.splits.tolist() == ["train", "val"]


def test_heldout_split_indices_prefer_validation_split() -> None:
    splits = np.asarray(["train", "train", "val", "test", ""], dtype="<U5")

    train_idx, val_idx = trainer._heldout_split_indices(splits)

    np.testing.assert_array_equal(train_idx, [0, 1])
    np.testing.assert_array_equal(val_idx, [2])


def test_heldout_split_indices_falls_back_to_test_split() -> None:
    splits = np.asarray(["train", "train", "test", ""], dtype="<U5")

    train_idx, val_idx = trainer._heldout_split_indices(splits)

    np.testing.assert_array_equal(train_idx, [0, 1])
    np.testing.assert_array_equal(val_idx, [2])


def test_load_corpus_drops_out_of_range_rows(tmp_path: Path) -> None:
    p = tmp_path / "garbled.jsonl"
    p.write_text(
        "\n".join(
            [
                json.dumps({"src": "a.mp4", "mos": 0.5}),  # below MOS_MIN
                json.dumps({"src": "b.mp4", "mos": 6.0}),  # above MOS_MAX
                json.dumps({"src": "c.mp4", "mos": 3.0}),  # OK
                json.dumps({"src": "d.mp4"}),  # missing MOS
            ]
        )
        + "\n",
        encoding="utf-8",
    )
    feat, _enc, mos = trainer._load_corpus([p])
    assert feat.shape[0] == 1
    assert mos[0] == pytest.approx(3.0)


def test_load_corpus_handles_missing_paths(tmp_path: Path) -> None:
    nonexistent = tmp_path / "nope.jsonl"
    feat, _enc, _mos = trainer._load_corpus([nonexistent])
    assert feat.shape == (0, trainer.N_FEATURES)


def test_main_rejects_empty_real_corpus_without_synthetic_fallback(tmp_path: Path) -> None:
    empty = tmp_path / "empty.jsonl"
    empty.write_text("", encoding="utf-8")

    rc = trainer.main(
        [
            "--konvid-1k",
            str(empty),
            "--konvid-150k",
            str(tmp_path / "missing.jsonl"),
            "--out-onnx",
            str(tmp_path / "out.onnx"),
            "--out-manifest",
            str(tmp_path / "out.json"),
            "--device",
            "cpu",
        ]
    )

    assert rc == 2
    assert not (tmp_path / "out.onnx").exists()
    assert not (tmp_path / "out.json").exists()


# ---------------------------------------------------------------------
# Gate evaluator — synthetic-corpus surrogate threshold.
# ---------------------------------------------------------------------


def test_evaluate_gate_synthetic_surrogate_pass() -> None:
    folds = [
        {"plcc": 0.80, "srocc": 0.80, "rmse": 0.40},
        {"plcc": 0.82, "srocc": 0.82, "rmse": 0.41},
        {"plcc": 0.79, "srocc": 0.81, "rmse": 0.42},
    ]
    verdict = trainer._evaluate_gate(folds, synthetic=True)
    assert verdict["passed"] is True
    assert verdict["gate"]["kind"] == "synthetic"


def test_evaluate_gate_synthetic_surrogate_fail() -> None:
    folds = [
        {"plcc": 0.50, "srocc": 0.60, "rmse": 0.50},
        {"plcc": 0.55, "srocc": 0.62, "rmse": 0.49},
        {"plcc": 0.60, "srocc": 0.65, "rmse": 0.48},
    ]
    verdict = trainer._evaluate_gate(folds, synthetic=True)
    assert verdict["passed"] is False


def test_evaluate_gate_real_threshold_unchanged() -> None:
    # 0.85 PLCC / 0.82 SROCC / 0.45 RMSE / 0.005 spread — the gate
    # the trainer carries against real KonViD data. A run that misses
    # is reported as failing; the threshold itself is **not** lowered
    # here (memory `feedback_no_test_weakening`).
    folds = [
        {"plcc": 0.83, "srocc": 0.81, "rmse": 0.46},
        {"plcc": 0.84, "srocc": 0.82, "rmse": 0.45},
        {"plcc": 0.82, "srocc": 0.80, "rmse": 0.47},
    ]
    verdict = trainer._evaluate_gate(folds, synthetic=False)
    assert verdict["passed"] is False
    assert verdict["gate"]["plcc_min"] == pytest.approx(0.85)
    assert verdict["gate"]["spread_max"] == pytest.approx(0.005)


def test_run_argv_json_must_decode_to_string_list(capsys: pytest.CaptureFixture[str]) -> None:
    rc = trainer.main(["--run-argv-json", '{"not": "a-list"}', "--no-export"])

    assert rc == 2
    assert "--run-argv-json must decode to list[str]" in capsys.readouterr().err


# ---------------------------------------------------------------------
# End-to-end smoke train — produces an ONNX, gate evaluates, ops are
# allowlist-conformant. This is the regression test the CI gate runs.
# ---------------------------------------------------------------------


def test_smoke_run_produces_allowlist_conformant_onnx(tmp_path: Path) -> None:
    onnx_pkg = pytest.importorskip("onnx")
    onnx_path = tmp_path / "out.onnx"
    manifest_path = tmp_path / "out.json"
    rc = trainer.main(
        [
            "--smoke",
            "--smoke-epochs",
            "30",
            "--out-onnx",
            str(onnx_path),
            "--out-manifest",
            str(manifest_path),
            "--device",
            "cpu",
        ]
    )
    assert rc == 0
    assert onnx_path.is_file()
    assert manifest_path.is_file()

    manifest = json.loads(manifest_path.read_text())
    assert manifest["id"] == "konvid_mos_head_v1"
    assert manifest["encoder_vocab"] == ["ugc-mixed"]
    assert manifest["mos_range"] == [1.0, 5.0]
    # Synthetic-corpus surrogate gate must clear with the deterministic
    # 30-epoch recipe; this is the placeholder gate per the task brief.
    assert manifest["gate"]["mean_plcc"] >= trainer.SYNTHETIC_GATE_PLCC
    assert manifest["gate"]["passed"] is True
    assert manifest["run_provenance"]["schema"] == "ai-run-provenance-v1"
    assert manifest["run_provenance"]["entrypoint"]["path"].endswith(
        "ai/scripts/train_konvid_mos_head.py"
    )
    assert manifest["run_provenance"]["args"]["smoke"] is True

    # ONNX op-allowlist conformance — every op must appear in
    # core/src/dnn/op_allowlist.c.
    model = onnx_pkg.load(str(onnx_path))
    onnx_pkg.checker.check_model(model)
    ops = {n.op_type for n in model.graph.node}
    allowlist_text = (REPO_ROOT / "core" / "src" / "dnn" / "op_allowlist.c").read_text()
    for op in ops:
        assert f'"{op}"' in allowlist_text, f"op {op} missing from op_allowlist.c"


def test_smoke_run_is_deterministic(tmp_path: Path) -> None:
    """Two back-to-back smoke runs must produce bit-identical weights.

    PyTorch's ONNX exporter writes weights to a sibling ``.onnx.data``
    external-data file and bakes the *filename* into the ONNX graph
    (``location: a.onnx.data``). The wrapper ONNX therefore differs
    between two runs that target different output filenames; the
    weights file (which is what the inference engine actually loads)
    is the one that must be bit-identical. We compare it explicitly,
    plus the recorded sha256 in the manifest as a belt-and-braces
    check on the wrapper graph minus the location string.
    """
    a_path = tmp_path / "a.onnx"
    b_path = tmp_path / "b.onnx"
    for out_path in (a_path, b_path):
        rc = trainer.main(
            [
                "--smoke",
                "--smoke-epochs",
                "30",
                "--out-onnx",
                str(out_path),
                "--out-manifest",
                str(out_path.with_suffix(".json")),
                "--device",
                "cpu",
            ]
        )
        assert rc == 0
    a_data = a_path.with_name(a_path.name + ".data")
    b_data = b_path.with_name(b_path.name + ".data")
    if a_data.is_file() and b_data.is_file():
        # The 5081-parameter MLP triggers torch's external-data path;
        # weights live in the .onnx.data sibling.
        assert a_data.read_bytes() == b_data.read_bytes()
    else:
        # Smaller graphs inline weights into the .onnx file itself.
        assert a_path.read_bytes() == b_path.read_bytes()

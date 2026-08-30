# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for direct ensemble manifest provenance."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

# pylint: disable=wrong-import-position
import train_fr_regressor_v2_ensemble as ensemble


def test_build_manifest_records_run_provenance_when_supplied() -> None:
    provenance = {
        "schema": "ai-run-provenance-v1",
        "entrypoint": {"path": "ai/scripts/train_fr_regressor_v2_ensemble.py"},
    }

    manifest = ensemble._build_manifest(
        "fr_regressor_v2_ensemble_v1",
        [{"id": "seed0", "onnx": "seed0.onnx", "seed": 0, "sha256": "a" * 64}],
        standardisation={
            "feature_mean": [0.0] * len(ensemble.CANONICAL_6),
            "feature_std": [1.0] * len(ensemble.CANONICAL_6),
        },
        codec_vocab=("unknown",),
        codec_vocab_version=1,
        encoder_vocab=("unknown",),
        nominal_coverage=0.95,
        conformal_q=None,
        smoke=True,
        eval_metrics={"in_sample_plcc": 1.0},
        run_provenance=provenance,
    )

    assert manifest["run_provenance"] == provenance


def test_build_manifest_omits_run_provenance_when_not_supplied() -> None:
    manifest = ensemble._build_manifest(
        "fr_regressor_v2_ensemble_v1",
        [{"id": "seed0", "onnx": "seed0.onnx", "seed": 0, "sha256": "a" * 64}],
        standardisation={
            "feature_mean": [0.0] * len(ensemble.CANONICAL_6),
            "feature_std": [1.0] * len(ensemble.CANONICAL_6),
        },
        codec_vocab=("unknown",),
        codec_vocab_version=1,
        encoder_vocab=("unknown",),
        nominal_coverage=0.95,
        conformal_q=None,
        smoke=True,
        eval_metrics={"in_sample_plcc": 1.0},
    )

    assert "run_provenance" not in manifest

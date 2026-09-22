# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Regression tests for :mod:`ai.scripts.train_fr_regressor`."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "scripts"))
sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))

_SCRIPT_PATH = REPO_ROOT / "ai" / "scripts" / "train_fr_regressor.py"
_SPEC = importlib.util.spec_from_file_location("train_fr_regressor", _SCRIPT_PATH)
assert _SPEC is not None and _SPEC.loader is not None
trainer = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = trainer
_SPEC.loader.exec_module(trainer)


def test_standardize_accepts_read_only_arrays_without_mutating_inputs() -> None:
    train = np.asarray([[1.0, 10.0], [3.0, 14.0]], dtype=np.float64)
    validation = np.asarray([[5.0, 18.0]], dtype=np.float64)
    train.setflags(write=False)
    validation.setflags(write=False)

    train_scaled, validation_scaled, report = trainer._standardize(train, validation)

    np.testing.assert_array_equal(train, [[1.0, 10.0], [3.0, 14.0]])
    np.testing.assert_array_equal(validation, [[5.0, 18.0]])
    np.testing.assert_allclose(train_scaled, [[-1.0, -1.0], [1.0, 1.0]])
    np.testing.assert_allclose(validation_scaled, [[3.0, 3.0]])
    assert report == {"mean": [2.0, 12.0], "std": [1.0, 2.0]}

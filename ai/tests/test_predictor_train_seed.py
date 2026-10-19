# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""``_set_seed`` of the predictor trainer: optional packages are skipped by name.

Kept apart from ``test_predictor_train.py``, which skips as a module without torch;
these cases need only numpy.
"""

from __future__ import annotations

import importlib.machinery
import importlib.util
import sys

import pytest

pytest.importorskip("numpy")

from vmaf_train import predictor_train


def test_set_seed_seeds_the_installed_generators_and_skips_the_missing(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    import random

    import numpy as np

    predictor_train._set_seed(7)
    first = (random.random(), float(np.random.random()))
    predictor_train._set_seed(7)
    assert (random.random(), float(np.random.random())) == first

    # A missing optional package is skipped by name, not by swallowing an ImportError.
    real = importlib.util.find_spec
    monkeypatch.setattr(
        importlib.util,
        "find_spec",
        lambda name, *a: None if name == "torch" else real(name, *a),
    )
    monkeypatch.setitem(sys.modules, "torch", None)  # an import would raise
    predictor_train._set_seed(7)


def test_set_seed_does_not_hide_a_broken_installed_package(monkeypatch: pytest.MonkeyPatch) -> None:
    class Broken:
        @staticmethod
        def seed(_: int) -> None:
            raise ImportError("numpy is installed but broken")

    broken = type(sys)("numpy")
    broken.random = Broken  # type: ignore[attr-defined]
    broken.__spec__ = importlib.machinery.ModuleSpec("numpy", None)
    monkeypatch.setitem(sys.modules, "numpy", broken)
    with pytest.raises(ImportError, match="installed but broken"):
        predictor_train._set_seed(7)

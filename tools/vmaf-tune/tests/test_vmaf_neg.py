# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for VMAF NEG (No Enhancement Gain) flag — ADR-0622.

Covers:
- ``neg_model_for()`` mapping and idempotency.
- ``_resolve_vmaf_model()`` CLI helper with and without ``--neg``.
- ``--neg`` flag presence in every affected subparser
  (``recommend``, ``tune-per-shot``, ``compare``, ``ladder``).
- Model-path routing: standard ``vmaf_v0.6.1`` ↔ ``vmaf_v0.6.1neg``
  and 4K variant ``vmaf_4k_v0.6.1`` ↔ ``vmaf_4k_v0.6.1neg``.
- make_default_sampler propagates vmaf_model through to CorpusOptions.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.cli import _build_parser, _resolve_vmaf_model
from vmaftune.resolution import (
    MODEL_4K,
    MODEL_4K_NEG,
    MODEL_1080P,
    MODEL_1080P_NEG,
    neg_model_for,
)

# ---------------------------------------------------------------------------
# neg_model_for — unit tests
# ---------------------------------------------------------------------------


class TestNegModelFor:
    def test_standard_1080p_maps_to_neg(self):
        assert neg_model_for(MODEL_1080P) == MODEL_1080P_NEG

    def test_standard_4k_maps_to_neg(self):
        assert neg_model_for(MODEL_4K) == MODEL_4K_NEG

    def test_idempotent_already_neg(self):
        """Calling neg_model_for on a NEG string must not double-suffix it."""
        assert neg_model_for(MODEL_1080P_NEG) == MODEL_1080P_NEG
        assert neg_model_for(MODEL_4K_NEG) == MODEL_4K_NEG

    def test_path_override_passthrough(self):
        """Pre-formatted path= / version= strings must not be modified."""
        path_override = "path=/abs/model/vmaf_v0.6.1.json"
        assert neg_model_for(path_override) == path_override

    def test_version_key_passthrough(self):
        version_override = "version=vmaf_v0.6.1"
        assert neg_model_for(version_override) == version_override

    def test_unknown_model_appends_neg_suffix(self):
        """Unknown models get the suffix appended so libvmaf surfaces a clear error."""
        result = neg_model_for("vmaf_custom_v1.0")
        assert result == "vmaf_custom_v1.0neg"

    def test_constants_have_expected_values(self):
        # ADR-1169: the production models moved to the v1.0.16 generation
        # while the NEG constants did NOT, because Netflix published no NEG
        # counterpart to any vmaf_v1.0.16_* model. The two therefore name
        # different generations on purpose -- asking for NEG changes generation.
        assert MODEL_1080P == "vmaf_v1.0.16_3d0h"
        assert MODEL_4K == "vmaf_v1.0.16_1d5h_2160"
        assert MODEL_1080P_NEG == "vmaf_v0.6.1neg"
        assert MODEL_4K_NEG == "vmaf_4k_v0.6.1neg"

    def test_neg_is_not_derived_by_appending_to_the_default(self):
        # Guards the specific bug the default flip would otherwise have
        # introduced: DEFAULT_MODEL + "neg" is a model that does not exist.
        assert MODEL_1080P_NEG != MODEL_1080P + "neg"
        assert neg_model_for(MODEL_1080P) == MODEL_1080P_NEG


# ---------------------------------------------------------------------------
# _resolve_vmaf_model — CLI helper
# ---------------------------------------------------------------------------


class TestResolveVmafModel:
    def _ns(self, vmaf_model: str = MODEL_1080P, neg: bool = False) -> argparse.Namespace:
        return argparse.Namespace(vmaf_model=vmaf_model, neg=neg)

    def test_no_neg_returns_model_unchanged(self):
        ns = self._ns(vmaf_model=MODEL_1080P, neg=False)
        assert _resolve_vmaf_model(ns) == MODEL_1080P

    def test_neg_routes_1080p_to_neg_variant(self):
        ns = self._ns(vmaf_model=MODEL_1080P, neg=True)
        assert _resolve_vmaf_model(ns) == MODEL_1080P_NEG

    def test_neg_routes_4k_to_neg_variant(self):
        ns = self._ns(vmaf_model=MODEL_4K, neg=True)
        assert _resolve_vmaf_model(ns) == MODEL_4K_NEG

    def test_neg_false_on_4k_keeps_4k(self):
        ns = self._ns(vmaf_model=MODEL_4K, neg=False)
        assert _resolve_vmaf_model(ns) == MODEL_4K

    def test_missing_neg_attr_defaults_to_no_neg(self):
        """Subcommands that haven't been updated yet should not crash."""
        ns = argparse.Namespace(vmaf_model=MODEL_1080P)  # no .neg attribute
        # Should not raise; defaults to standard model.
        assert _resolve_vmaf_model(ns) == MODEL_1080P


# ---------------------------------------------------------------------------
# --neg flag presence in every subparser
# ---------------------------------------------------------------------------


class TestNegFlagParsing:
    """Verify every affected subcommand accepts --neg without error."""

    def setup_method(self):
        self.parser = _build_parser()

    def _parse(self, argv: list[str]) -> argparse.Namespace:
        return self.parser.parse_args(argv)

    def test_recommend_accepts_neg(self):
        ns = self._parse(
            [
                "recommend",
                "--source",
                "ref.yuv",
                "--width",
                "1920",
                "--height",
                "1080",
                "--neg",
            ]
        )
        assert ns.neg is True

    def test_recommend_neg_default_false(self):
        ns = self._parse(
            [
                "recommend",
                "--source",
                "ref.yuv",
                "--width",
                "1920",
                "--height",
                "1080",
            ]
        )
        assert ns.neg is False

    def test_tune_per_shot_accepts_neg(self):
        ns = self._parse(
            [
                "tune-per-shot",
                "--src",
                "ref.yuv",
                "--width",
                "1920",
                "--height",
                "1080",
                "--encoder",
                "libx264",
                "--neg",
            ]
        )
        assert ns.neg is True

    def test_tune_per_shot_neg_default_false(self):
        ns = self._parse(
            [
                "tune-per-shot",
                "--src",
                "ref.yuv",
                "--width",
                "1920",
                "--height",
                "1080",
                "--encoder",
                "libx264",
            ]
        )
        assert ns.neg is False

    def test_compare_accepts_neg(self):
        ns = self._parse(["compare", "--src", "ref.yuv", "--neg"])
        assert ns.neg is True

    def test_compare_neg_default_false(self):
        ns = self._parse(["compare", "--src", "ref.yuv"])
        assert ns.neg is False

    def test_ladder_accepts_neg(self):
        ns = self._parse(
            [
                "ladder",
                "--src",
                "ref.yuv",
                "--resolutions",
                "1920x1080",
                "--target-vmafs",
                "90",
                "--neg",
            ]
        )
        assert ns.neg is True

    def test_ladder_neg_default_false(self):
        ns = self._parse(
            [
                "ladder",
                "--src",
                "ref.yuv",
                "--resolutions",
                "1920x1080",
                "--target-vmafs",
                "90",
            ]
        )
        assert ns.neg is False

    def test_compare_neg_overrides_vmaf_model(self):
        """--neg + --vmaf-model vmaf_v0.6.1 must resolve to vmaf_v0.6.1neg."""
        ns = self._parse(["compare", "--src", "ref.yuv", "--neg", "--vmaf-model", "vmaf_v0.6.1"])
        assert _resolve_vmaf_model(ns) == MODEL_1080P_NEG

    def test_compare_neg_with_4k_model(self):
        """--neg + --vmaf-model vmaf_4k_v0.6.1 must route to the 4K NEG variant."""
        ns = self._parse(["compare", "--src", "ref.yuv", "--neg", "--vmaf-model", "vmaf_4k_v0.6.1"])
        assert _resolve_vmaf_model(ns) == MODEL_4K_NEG


# ---------------------------------------------------------------------------
# make_default_sampler — vmaf_model threading
# ---------------------------------------------------------------------------


class TestMakeDefaultSamplerVmafModel:
    """Verify that make_default_sampler propagates vmaf_model into CorpusOptions."""

    def test_sampler_threads_standard_model(self, tmp_path: Path):
        """Without --neg the sampler should use the standard model."""
        from vmaftune.ladder import make_default_sampler

        _corpus_opts_seen: list[object] = []

        def _fake_iter_rows(job: object, opts: object, **_kw):
            _corpus_opts_seen.append(opts)
            return iter([])

        from vmaftune import corpus as _corpus_mod

        original_iter_rows = _corpus_mod.iter_rows

        def _patched_iter_rows(job, opts, **kw):
            _corpus_opts_seen.append(opts)
            return iter([])

        _corpus_mod.iter_rows = _patched_iter_rows  # type: ignore[assignment]
        try:
            sampler = make_default_sampler(vmaf_model=MODEL_1080P)
            # Calling the sampler should try to call iter_rows.
            try:
                sampler(tmp_path / "ref.yuv", "libx264", 1920, 1080, 90.0)
            except Exception:
                pass  # iter_rows returns empty; pick_target_vmaf will raise
        finally:
            _corpus_mod.iter_rows = original_iter_rows  # type: ignore[assignment]

        assert len(_corpus_opts_seen) >= 1
        opts = _corpus_opts_seen[0]
        assert getattr(opts, "vmaf_model", None) == MODEL_1080P

    def test_sampler_threads_neg_model(self, tmp_path: Path):
        """With NEG model string, the sampler must forward it to CorpusOptions."""
        from vmaftune.ladder import make_default_sampler

        _corpus_opts_seen: list[object] = []

        from vmaftune import corpus as _corpus_mod

        original_iter_rows = _corpus_mod.iter_rows

        def _patched_iter_rows(job, opts, **kw):
            _corpus_opts_seen.append(opts)
            return iter([])

        _corpus_mod.iter_rows = _patched_iter_rows  # type: ignore[assignment]
        try:
            sampler = make_default_sampler(vmaf_model=MODEL_1080P_NEG)
            try:
                sampler(tmp_path / "ref.yuv", "libx264", 1920, 1080, 90.0)
            except Exception:
                pass
        finally:
            _corpus_mod.iter_rows = original_iter_rows  # type: ignore[assignment]

        assert len(_corpus_opts_seen) >= 1
        opts = _corpus_opts_seen[0]
        assert getattr(opts, "vmaf_model", None) == MODEL_1080P_NEG


# ---------------------------------------------------------------------------
# NEG model files exist in-tree
# ---------------------------------------------------------------------------


class TestNegModelFilesExist:
    """Sanity check: the JSON model files must exist in model/."""

    def _model_dir(self) -> Path:
        here = Path(__file__).resolve()
        for parent in here.parents:
            candidate = parent / "model"
            if candidate.is_dir() and (candidate / "vmaf_v0.6.1.json").exists():
                return candidate
        pytest.skip("model/ directory not found from test file location")
        return Path()  # unreachable

    def test_1080p_neg_json_exists(self):
        model_dir = self._model_dir()
        p = model_dir / "vmaf_v0.6.1neg.json"
        assert p.exists(), f"expected NEG model at {p}"

    def test_4k_neg_json_exists(self):
        model_dir = self._model_dir()
        p = model_dir / "vmaf_4k_v0.6.1neg.json"
        assert p.exists(), f"expected 4K NEG model at {p}"

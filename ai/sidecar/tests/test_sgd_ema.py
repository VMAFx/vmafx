# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# ai/sidecar/tests/test_sgd_ema.py — unit tests for SGDEMATrainer.
#
# All tests run CPU-only and complete in well under 10 s on any modern
# laptop.  PyTorch is required; the tests are skipped when it is absent.
#
# ADR-0781: sidecar online training — SGD + EMA + replay buffer.

import os
import pathlib
import tempfile

import pytest

torch = pytest.importorskip("torch", reason="PyTorch not installed — skipping SGD+EMA tests")
import torch.nn as nn  # noqa: E402 — after pytest.importorskip guard

from ai.sidecar.sgd_ema import SGDEMAConfig, SGDEMATrainer  # noqa: E402

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

N_FEATURES = 16


def _tiny_model() -> nn.Module:
    return nn.Sequential(nn.Linear(N_FEATURES, 8), nn.ReLU(), nn.Linear(8, 1))


def _batch(n: int = 8):
    feats = torch.randn(n, N_FEATURES)
    scores = torch.rand(n) * 100.0
    return feats, scores


# ---------------------------------------------------------------------------
# Construction / configuration
# ---------------------------------------------------------------------------


class TestSGDEMAConfig:
    def test_defaults(self):
        cfg = SGDEMAConfig()
        assert cfg.lr == pytest.approx(1e-4)
        assert cfg.ema_decay == pytest.approx(0.999)
        assert cfg.max_grad_norm == pytest.approx(1.0)
        assert cfg.warmup_steps == 100

    def test_custom_values(self):
        cfg = SGDEMAConfig(lr=0.01, optimizer="adam", ema_decay=0.99)
        assert cfg.lr == pytest.approx(0.01)
        assert cfg.optimizer == "adam"


class TestSGDEMATrainerInit:
    def test_constructs_without_error(self):
        model = _tiny_model()
        trainer = SGDEMATrainer(model)
        assert trainer.step_count == 0

    def test_ema_starts_equal_to_live(self):
        model = _tiny_model()
        trainer = SGDEMATrainer(model)
        for p_live, p_ema in zip(
            trainer.model.parameters(), trainer.ema_model.parameters(), strict=True
        ):
            assert torch.allclose(p_live.data, p_ema.data)

    def test_ema_params_have_no_grad(self):
        model = _tiny_model()
        trainer = SGDEMATrainer(model)
        for p in trainer.ema_model.parameters():
            assert not p.requires_grad


# ---------------------------------------------------------------------------
# Step mechanics
# ---------------------------------------------------------------------------


class TestSGDEMAStep:
    def test_step_returns_scalar_loss(self):
        model = _tiny_model()
        trainer = SGDEMATrainer(model)
        feats, scores = _batch()
        loss = trainer.step(feats, scores)
        assert isinstance(loss, float)
        assert loss >= 0.0

    def test_step_increments_counter(self):
        model = _tiny_model()
        trainer = SGDEMATrainer(model)
        for _i in range(5):
            trainer.step(*_batch())
        assert trainer.step_count == 5

    def test_ema_diverges_from_live_after_steps(self):
        """After gradient steps the live weights change; EMA lags behind."""
        cfg = SGDEMAConfig(lr=0.1, ema_decay=0.9, warmup_steps=0)
        model = _tiny_model()
        trainer = SGDEMATrainer(model, cfg)

        # Run many steps so the live model moves significantly.
        for _ in range(50):
            trainer.step(*_batch(32))

        # Collect max abs diff across all parameter tensors.
        max_diff = max(
            (p_live.data - p_ema.data).abs().max().item()
            for p_live, p_ema in zip(
                trainer.model.parameters(), trainer.ema_model.parameters(), strict=True
            )
        )
        # EMA should not be identical to live after 50 high-LR steps.
        assert (
            max_diff > 1e-6
        ), "EMA and live weights are identical after 50 steps — EMA update likely broken"

    def test_ema_update_formula(self):
        """Verify the EMA formula θ_ema ← β·θ_ema + (1−β)·θ_live numerically."""
        cfg = SGDEMAConfig(lr=1.0, ema_decay=0.9, warmup_steps=0, max_grad_norm=1e9)
        model = nn.Linear(4, 1, bias=False)
        # Fix initial weights to known values.
        with torch.no_grad():
            model.weight.fill_(1.0)
        trainer = SGDEMATrainer(model, cfg)
        # Record EMA weight before the step.
        ema_before = trainer.ema_model.weight.data.clone()

        feats = torch.ones(1, 4)
        scores = torch.zeros(1)
        trainer.step(feats, scores)

        live_after = trainer.model.weight.data
        ema_after = trainer.ema_model.weight.data
        expected = 0.9 * ema_before + 0.1 * live_after
        assert torch.allclose(
            ema_after, expected, atol=1e-5
        ), f"EMA mismatch: got {ema_after}, expected {expected}"


# ---------------------------------------------------------------------------
# Warmup
# ---------------------------------------------------------------------------


class TestWarmup:
    def test_lr_is_zero_at_step_zero(self):
        """Before any step, warmup has not yet applied — first step uses scale=1/warmup."""
        cfg = SGDEMAConfig(lr=1.0, warmup_steps=10, optimizer="sgd")
        model = _tiny_model()
        trainer = SGDEMATrainer(model, cfg)
        # After step 1 the LR should be 1/10 = 0.1.
        trainer.step(*_batch())
        for pg in trainer._opt.param_groups:
            assert abs(pg["lr"] - 0.1) < 1e-6

    def test_lr_reaches_full_after_warmup(self):
        cfg = SGDEMAConfig(lr=0.5, warmup_steps=5, optimizer="sgd")
        model = _tiny_model()
        trainer = SGDEMATrainer(model, cfg)
        for _ in range(5):
            trainer.step(*_batch())
        for pg in trainer._opt.param_groups:
            assert abs(pg["lr"] - 0.5) < 1e-5


# ---------------------------------------------------------------------------
# Checkpoint
# ---------------------------------------------------------------------------


class TestCheckpoint:
    def test_should_checkpoint_false_initially(self):
        cfg = SGDEMAConfig(checkpoint_interval_s=3600, min_samples_per_checkpoint=1000)
        trainer = SGDEMATrainer(_tiny_model(), cfg)
        assert not trainer.should_checkpoint()

    def test_export_onnx_writes_file(self):
        cfg = SGDEMAConfig(checkpoint_interval_s=0.0, min_samples_per_checkpoint=0)
        trainer = SGDEMATrainer(_tiny_model(), cfg)
        # Force a step so there are no untrained parameters issues.
        trainer.step(*_batch())
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "model.onnx")
            trainer.export_onnx(path, n_features=N_FEATURES)
            assert pathlib.Path(path).exists()
            assert pathlib.Path(path).stat().st_size > 0

    def test_export_onnx_is_loadable(self):
        """The exported ONNX file should be parseable by onnx."""
        onnx = pytest.importorskip("onnx", reason="onnx not installed")
        cfg = SGDEMAConfig(checkpoint_interval_s=0.0, min_samples_per_checkpoint=0)
        trainer = SGDEMATrainer(_tiny_model(), cfg)
        trainer.step(*_batch())
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "model.onnx")
            trainer.export_onnx(path, n_features=N_FEATURES)
            model_proto = onnx.load(path)
            onnx.checker.check_model(model_proto)

    def test_state_dict_round_trip(self):
        """state_dict / load_state_dict preserves step count and weights."""
        cfg = SGDEMAConfig(checkpoint_interval_s=0.0, min_samples_per_checkpoint=0)
        trainer = SGDEMATrainer(_tiny_model(), cfg)
        for _ in range(3):
            trainer.step(*_batch())
        state = trainer.state_dict()
        assert state["step_count"] == 3

        # Restore into a fresh trainer.
        trainer2 = SGDEMATrainer(_tiny_model(), cfg)
        trainer2.load_state_dict(state)
        assert trainer2.step_count == 3
        for p1, p2 in zip(trainer.model.parameters(), trainer2.model.parameters(), strict=True):
            assert torch.allclose(p1.data, p2.data)


# ---------------------------------------------------------------------------
# Adam optimizer variant
# ---------------------------------------------------------------------------


class TestAdamVariant:
    def test_adam_optimizer_steps_without_error(self):
        cfg = SGDEMAConfig(lr=1e-3, optimizer="adam")
        trainer = SGDEMATrainer(_tiny_model(), cfg)
        for _ in range(10):
            loss = trainer.step(*_batch())
        assert loss >= 0.0
        assert trainer.step_count == 10

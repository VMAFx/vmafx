# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# ai/sidecar/sgd_ema.py — Online SGD optimizer with EMA (Polyak averaging).
#
# Wraps a PyTorch nn.Module with:
#   - standard SGD (or Adam) gradient updates on the *live* model copy;
#   - an exponential moving average (EMA) shadow maintained in parallel;
#   - gradient clipping to prevent pathological updates from outlier
#     feature vectors;
#   - linear learning-rate warmup for the first `warmup_steps` steps to
#     protect the base-model weights.
#
# The EMA model (not the live model) is exported to ONNX at checkpoint time.
# This follows the Mean Teacher pattern (Tarvainen & Valpola 2017) and is
# standard practice in online video-quality fine-tuning.
#
# ADR-0781: sidecar online training — SGD + EMA + replay buffer.

from __future__ import annotations

import copy
import time
from dataclasses import dataclass, field
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    import torch
    import torch.nn as nn


@dataclass
class SGDEMAConfig:
    """Hyperparameters for the online SGD + EMA trainer.

    All defaults match the Research-0733 recommendations except where noted.
    """

    lr: float = 1e-4
    """Initial learning rate for SGD/Adam."""

    momentum: float = 0.9
    """SGD momentum factor (unused when optimizer='adam')."""

    weight_decay: float = 1e-5
    """L2 regularisation strength."""

    optimizer: str = "sgd"
    """'sgd' or 'adam'. SGD is the default per ADR-0781 (lighter per-step)."""

    ema_decay: float = 0.999
    """EMA decay beta.  0.999 ≈ 1000-step half-life; standard for online VMAF."""

    max_grad_norm: float = 1.0
    """Gradient-norm clipping threshold.  Prevents outlier-triple blowups."""

    warmup_steps: int = 100
    """Linear LR warmup steps from 0 → lr.  Protects base-model weights."""

    checkpoint_interval_s: float = 600.0
    """Minimum seconds between checkpoint exports (default 10 min)."""

    min_samples_per_checkpoint: int = 1_000
    """Minimum new samples required before exporting a checkpoint."""

    extra: dict = field(default_factory=dict)  # forward-compat catch-all


class SGDEMATrainer:
    """Online SGD trainer with an EMA shadow model.

    Parameters
    ----------
    model:
        A ``torch.nn.Module`` that accepts a single float tensor of shape
        ``(batch, n_features)`` and returns a ``(batch, 1)`` predicted score
        tensor.  Must already be on the target device.
    config:
        ``SGDEMAConfig`` instance.  Constructed from defaults when omitted.

    Thread safety
    -------------
    ``step()`` and ``export_onnx()`` both acquire ``self._lock`` so that a
    background checkpoint thread cannot race with an in-progress gradient step.
    Callers that want to call ``step()`` concurrently from multiple threads
    should use one ``SGDEMATrainer`` per thread and merge EMA weights externally
    (not supported in v1 — single-threaded training loop only).
    """

    def __init__(
        self,
        model: "nn.Module",
        config: SGDEMAConfig | None = None,
    ) -> None:
        import torch

        self._cfg = config or SGDEMAConfig()
        self._model = model
        self._ema_model = copy.deepcopy(model)

        # Freeze EMA model — we manage weights manually.
        for p in self._ema_model.parameters():
            p.requires_grad_(False)

        self._opt = self._build_optimizer()
        self._step_count: int = 0
        self._samples_since_ckpt: int = 0
        self._last_ckpt_time: float = 0.0

        import threading

        self._lock = threading.Lock()

        # Loss function: MSE is standard for VMAF regression.
        self._loss_fn = torch.nn.MSELoss()

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def step(self, features: "torch.Tensor", targets: "torch.Tensor") -> float:
        """Perform one gradient step on a batch.

        Parameters
        ----------
        features:
            Float tensor of shape ``(batch, n_features)``.
        targets:
            Float tensor of shape ``(batch,)`` or ``(batch, 1)`` with the
            ground-truth VMAF scores.

        Returns
        -------
        float
            The scalar MSE loss for this batch (for logging).
        """
        import torch

        with self._lock:
            self._model.train()
            self._opt.zero_grad()

            preds = self._model(features)
            loss = self._loss_fn(preds.squeeze(-1), targets.float().squeeze(-1))
            loss.backward()

            # Gradient clipping — protects against outlier feature vectors.
            import torch.nn.utils as nn_utils

            nn_utils.clip_grad_norm_(self._model.parameters(), self._cfg.max_grad_norm)

            # Apply warmup LR scaling before the optimizer step.
            self._apply_warmup_lr()
            self._opt.step()
            self._step_count += 1

            # EMA update: θ_ema ← β·θ_ema + (1−β)·θ_live
            beta = self._cfg.ema_decay
            with torch.no_grad():
                for p_live, p_ema in zip(
                    self._model.parameters(), self._ema_model.parameters(), strict=True
                ):
                    p_ema.data.mul_(beta).add_(p_live.data, alpha=1.0 - beta)

            batch_size = features.shape[0]
            self._samples_since_ckpt += batch_size

        return float(loss.item())

    def should_checkpoint(self) -> bool:
        """Return True when both checkpoint conditions are satisfied."""
        elapsed = time.monotonic() - self._last_ckpt_time
        return (
            elapsed >= self._cfg.checkpoint_interval_s
            and self._samples_since_ckpt >= self._cfg.min_samples_per_checkpoint
        )

    def export_onnx(
        self,
        path: str,
        n_features: int,
        opset: int = 17,
    ) -> None:
        """Export the EMA model to *path* in ONNX format.

        Uses the same opset=17 constraint as ADR-0249 / the ai/ export stack.
        Writes atomically (temp file + rename) to prevent nodes from loading
        a partial checkpoint.
        """
        import os
        import tempfile

        import torch

        with self._lock:
            self._ema_model.eval()
            dummy = torch.zeros(1, n_features)
            tmp_fd, tmp_path = tempfile.mkstemp(
                dir=os.path.dirname(os.path.abspath(path)),
                suffix=".tmp.onnx",
            )
            try:
                os.close(tmp_fd)
                torch.onnx.export(
                    self._ema_model,
                    dummy,
                    tmp_path,
                    opset_version=opset,
                    input_names=["features"],
                    output_names=["score"],
                    dynamic_axes={"features": {0: "batch"}},
                    do_constant_folding=True,
                )
                os.replace(tmp_path, path)
            except Exception:
                if os.path.exists(tmp_path):
                    os.unlink(tmp_path)
                raise

        self._last_ckpt_time = time.monotonic()
        self._samples_since_ckpt = 0

    # ------------------------------------------------------------------
    # Properties / metrics
    # ------------------------------------------------------------------

    @property
    def step_count(self) -> int:
        return self._step_count

    @property
    def model(self) -> "nn.Module":
        """The live (non-EMA) model being trained."""
        return self._model

    @property
    def ema_model(self) -> "nn.Module":
        """The EMA shadow model (used for ONNX export)."""
        return self._ema_model

    # ------------------------------------------------------------------
    # Internals
    # ------------------------------------------------------------------

    def _build_optimizer(self) -> "torch.optim.Optimizer":
        import torch.optim as optim

        if self._cfg.optimizer == "adam":
            return optim.Adam(
                self._model.parameters(),
                lr=self._cfg.lr,
                weight_decay=self._cfg.weight_decay,
            )
        # Default: SGD with momentum.
        return optim.SGD(
            self._model.parameters(),
            lr=self._cfg.lr,
            momentum=self._cfg.momentum,
            weight_decay=self._cfg.weight_decay,
        )

    def _apply_warmup_lr(self) -> None:
        """Scale LR linearly from 0 → cfg.lr over warmup_steps."""
        if self._step_count >= self._cfg.warmup_steps:
            return  # warmup finished — normal LR
        scale = (self._step_count + 1) / max(self._cfg.warmup_steps, 1)
        for pg in self._opt.param_groups:
            pg["lr"] = self._cfg.lr * scale

    def _warmup_fraction(self) -> float:
        return min(1.0, (self._step_count + 1) / max(self._cfg.warmup_steps, 1))

    def state_dict(self) -> dict:
        """Serialisable trainer state for pause/resume (not full checkpoint)."""
        return {
            "step_count": self._step_count,
            "samples_since_ckpt": self._samples_since_ckpt,
            "last_ckpt_time": self._last_ckpt_time,
            "model": self._model.state_dict(),
            "ema_model": self._ema_model.state_dict(),
            "optimizer": self._opt.state_dict(),
        }

    def load_state_dict(self, state: dict) -> None:
        with self._lock:
            self._step_count = state["step_count"]
            self._samples_since_ckpt = state["samples_since_ckpt"]
            self._last_ckpt_time = state["last_ckpt_time"]
            self._model.load_state_dict(state["model"])
            self._ema_model.load_state_dict(state["ema_model"])
            self._opt.load_state_dict(state["optimizer"])

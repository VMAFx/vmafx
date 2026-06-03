# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""C1 — feature-vector → MOS regressor (replaces / augments SVM)."""

from __future__ import annotations

from typing import cast

import pytorch_lightning as L
import torch
from torch import nn
from typing_extensions import TypedDict

from ..confidence import gaussian_nll


class _FRRegressorHParams(TypedDict):
    """Typed view of FRRegressor hyperparameters (Lightning stores them as MutableMapping)."""

    in_features: int
    hidden: int
    depth: int
    dropout: float
    lr: float
    weight_decay: float
    emit_variance: bool
    num_codecs: int


class FRRegressor(L.LightningModule):
    """Tiny MLP over precomputed libvmaf feature vectors (adm, vif, motion, ...).

    When @c emit_variance is True, the model outputs ``(N, 2)`` — column
    0 is the score (μ), column 1 is log-variance. Training switches
    from MSE to Gaussian NLL so inference-time callers can surface a
    confidence interval per sample. Single-output legacy mode stays the
    default so existing checkpoints continue to load unchanged.

    When @c num_codecs > 0, the model accepts a second input — a
    ``(N, num_codecs)`` one-hot codec vector — concatenated to the
    feature vector before the first MLP layer. See ``ai/src/vmaf_train/codec.py``,
    [ADR-0235](../../../docs/adr/0235-codec-aware-fr-regressor.md), and
    [Research-0040](../../../docs/research/0040-codec-aware-fr-conditioning.md).
    The default ``num_codecs=0`` keeps the v1 single-input contract so
    existing checkpoints (``model/tiny/fr_regressor_v1.onnx``) load
    unchanged.

    Forward signature:

      * ``num_codecs == 0``: ``forward(features) -> score``
      * ``num_codecs >  0``: ``forward(features, codec_onehot) -> score``

    The codec input is positional + required when ``num_codecs > 0`` so
    the ONNX graph stays a fixed-arity multi-input session compatible
    with libvmaf's ``vmaf_dnn_session_run`` two-input pattern (matches
    the LPIPS-Sq exporter precedent in ADR-0040 / ADR-0041).
    """

    @property
    def _hp(self) -> _FRRegressorHParams:
        """Typed view of ``self.hparams`` (Lightning's MutableMapping is untyped)."""
        return cast(_FRRegressorHParams, self.hparams)

    def __init__(
        self,
        in_features: int = 7,
        hidden: int = 64,
        depth: int = 2,
        dropout: float = 0.1,
        lr: float = 1e-3,
        weight_decay: float = 1e-5,
        emit_variance: bool = False,
        num_codecs: int = 0,
    ) -> None:
        super().__init__()
        self.save_hyperparameters()

        if num_codecs < 0:
            raise ValueError(f"num_codecs must be >= 0, got {num_codecs}")

        out_features = 2 if emit_variance else 1
        layers: list[nn.Module] = []
        prev = in_features + num_codecs
        for _ in range(depth):
            layers += [nn.Linear(prev, hidden), nn.GELU(), nn.Dropout(dropout)]
            prev = hidden
        layers.append(nn.Linear(prev, out_features))
        self.net = nn.Sequential(*layers)

    def forward(
        self,
        x: torch.Tensor,
        codec_onehot: torch.Tensor | None = None,
    ) -> torch.Tensor:
        if self._hp["num_codecs"] > 0:
            if codec_onehot is None:
                raise ValueError(
                    "codec_onehot is required when num_codecs > 0; "
                    "callers without codec metadata should pass an all-zero "
                    "or 'unknown'-bucket one-hot vector"
                )
            if codec_onehot.shape[-1] != self._hp["num_codecs"]:
                raise ValueError(
                    f"codec_onehot last-dim {codec_onehot.shape[-1]} != "
                    f"num_codecs {self._hp['num_codecs']}"
                )
            x = torch.cat([x, codec_onehot.to(x.dtype)], dim=-1)
        out = self.net(x)
        if self._hp["emit_variance"]:
            return out  # (N, 2): [:, 0] = score, [:, 1] = logvar
        return out.squeeze(-1)

    def _unpack_batch(
        self, batch: object
    ) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor | None]:
        """Accept (x, y) or (x, codec, y) tuples. Codec-blind callers
        keep the v1 2-tuple shape; codec-aware datamodules emit 3-tuples."""
        if len(batch) == 3:  # type: ignore[arg-type]
            x, codec, y = batch  # type: ignore[misc]
            return x, y, codec
        x, y = batch  # type: ignore[misc]
        return x, y, None

    def _step(self, batch: object, tag: str) -> torch.Tensor:
        x, y, codec = self._unpack_batch(batch)
        out = self(x, codec)
        if self._hp["emit_variance"]:
            pred = out[..., 0]
            logvar = out[..., 1]
            loss = gaussian_nll(pred, y, logvar).mean()
            self.log(f"{tag}/nll", loss, prog_bar=True, on_epoch=True)
            # Log the plain MSE too so runs with/without variance stay comparable.
            with torch.no_grad():
                self.log(
                    f"{tag}/mse",
                    nn.functional.mse_loss(pred, y),
                    on_epoch=True,
                )
            return loss
        loss = nn.functional.mse_loss(out, y)
        self.log(f"{tag}/mse", loss, prog_bar=True, on_epoch=True)
        return loss

    def training_step(self, batch: object, _idx: int) -> torch.Tensor:
        return self._step(batch, "train")

    def validation_step(self, batch: object, _idx: int) -> None:
        self._step(batch, "val")

    def test_step(self, batch: object, _idx: int) -> None:
        self._step(batch, "test")

    def configure_optimizers(self) -> torch.optim.Optimizer:
        return torch.optim.AdamW(
            self.parameters(),
            lr=self._hp["lr"],
            weight_decay=self._hp["weight_decay"],
        )

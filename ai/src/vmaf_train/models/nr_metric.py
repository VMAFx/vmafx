# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""C2 — no-reference metric: distorted frame → MOS (small CNN)."""

from __future__ import annotations

from typing import cast

import pytorch_lightning as L
import torch
from torch import nn
from typing_extensions import TypedDict

from ..confidence import gaussian_nll


class _NRMetricHParams(TypedDict):
    """Typed view of NRMetric hyperparameters (Lightning stores them as MutableMapping)."""

    in_channels: int
    width: int
    lr: float
    weight_decay: float
    emit_variance: bool


def _dw_sep(in_c: int, out_c: int, stride: int = 1) -> nn.Sequential:
    return nn.Sequential(
        nn.Conv2d(in_c, in_c, 3, stride=stride, padding=1, groups=in_c, bias=False),
        nn.BatchNorm2d(in_c),
        nn.ReLU6(inplace=True),
        nn.Conv2d(in_c, out_c, 1, bias=False),
        nn.BatchNorm2d(out_c),
        nn.ReLU6(inplace=True),
    )


class NRMetric(L.LightningModule):
    """MobileNet-tiny-ish backbone → global pool → scalar MOS.

    Supports the same @c emit_variance mode as the FR regressor — when
    on, the head emits ``(N, 2)`` (μ, logvar) and training switches to
    Gaussian NLL.
    """

    @property
    def _hp(self) -> _NRMetricHParams:
        """Typed view of ``self.hparams`` (Lightning's MutableMapping is untyped)."""
        return cast(_NRMetricHParams, self.hparams)

    def __init__(
        self,
        in_channels: int = 1,
        width: int = 16,
        lr: float = 1e-3,
        weight_decay: float = 1e-4,
        emit_variance: bool = False,
    ) -> None:
        super().__init__()
        self.save_hyperparameters()
        w = width
        out_features = 2 if emit_variance else 1
        self.stem = nn.Sequential(
            nn.Conv2d(in_channels, w, 3, stride=2, padding=1, bias=False),
            nn.BatchNorm2d(w),
            nn.ReLU6(inplace=True),
        )
        self.body = nn.Sequential(
            _dw_sep(w, w * 2, stride=2),
            _dw_sep(w * 2, w * 2),
            _dw_sep(w * 2, w * 4, stride=2),
            _dw_sep(w * 4, w * 4),
            _dw_sep(w * 4, w * 8, stride=2),
        )
        self.head = nn.Sequential(
            nn.AdaptiveAvgPool2d(1),
            nn.Flatten(),
            nn.Linear(w * 8, out_features),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        out = self.head(self.body(self.stem(x)))
        if self._hp["emit_variance"]:
            return out  # (N, 2): [:, 0] = score, [:, 1] = logvar
        return out.squeeze(-1)

    def _step(self, batch: object, tag: str) -> torch.Tensor:
        x, y = batch  # type: ignore[misc]
        out = self(x)
        if self._hp["emit_variance"]:
            pred = out[..., 0]
            logvar = out[..., 1]
            loss = gaussian_nll(pred, y, logvar).mean()
            self.log(f"{tag}/nll", loss, prog_bar=True, on_epoch=True)
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

    def configure_optimizers(self) -> torch.optim.Optimizer:
        return torch.optim.AdamW(
            self.parameters(),
            lr=self._hp["lr"],
            weight_decay=self._hp["weight_decay"],
        )

# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""C3 — learned residual filter for encoder pre-processing."""

from __future__ import annotations

from typing import cast

import pytorch_lightning as L
import torch
from torch import nn
from typing_extensions import TypedDict


class _LearnedFilterHParams(TypedDict):
    """Typed view of LearnedFilter hyperparameters (Lightning stores them as MutableMapping)."""

    channels: int
    width: int
    num_blocks: int
    lr: float


class _ResBlock(nn.Module):
    def __init__(self, channels: int) -> None:
        super().__init__()
        self.block = nn.Sequential(
            nn.Conv2d(channels, channels, 3, padding=1),
            nn.ReLU(inplace=True),
            nn.Conv2d(channels, channels, 3, padding=1),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return x + self.block(x)


class LearnedFilter(L.LightningModule):
    """Frame → frame residual CNN (denoise/deblock/sharpen) for ffmpeg vmaf_pre filter."""

    @property
    def _hp(self) -> _LearnedFilterHParams:
        """Typed view of ``self.hparams`` (Lightning's MutableMapping is untyped)."""
        return cast(_LearnedFilterHParams, self.hparams)

    def __init__(
        self,
        channels: int = 1,
        width: int = 16,
        num_blocks: int = 4,
        lr: float = 1e-4,
    ) -> None:
        super().__init__()
        self.save_hyperparameters()
        self.entry = nn.Conv2d(channels, width, 3, padding=1)
        self.body = nn.Sequential(*[_ResBlock(width) for _ in range(num_blocks)])
        self.exit = nn.Conv2d(width, channels, 3, padding=1)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        residual = self.exit(self.body(self.entry(x)))
        return torch.clamp(x + residual, 0.0, 1.0)

    def training_step(self, batch: object, _idx: int) -> torch.Tensor:
        deg, clean = batch  # type: ignore[misc]
        out = self(deg)
        loss = nn.functional.l1_loss(out, clean)
        self.log("train/l1", loss, prog_bar=True, on_epoch=True)
        return loss

    def validation_step(self, batch: object, _idx: int) -> None:
        deg, clean = batch  # type: ignore[misc]
        out = self(deg)
        self.log("val/l1", nn.functional.l1_loss(out, clean), prog_bar=True, on_epoch=True)

    def configure_optimizers(self) -> torch.optim.Optimizer:
        return torch.optim.AdamW(self.parameters(), lr=self._hp["lr"])

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
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
        residual: torch.Tensor = self.block(x)
        return x + residual


def _tensor_pair(batch: object) -> tuple[torch.Tensor, torch.Tensor]:
    """Validate Lightning's dynamically supplied two-tensor batch."""
    if not isinstance(batch, (tuple, list)) or len(batch) != 2:
        raise TypeError("LearnedFilter expects a two-item (degraded, clean) batch")
    degraded, clean = batch
    if not isinstance(degraded, torch.Tensor) or not isinstance(clean, torch.Tensor):
        raise TypeError("LearnedFilter batch items must both be torch.Tensor instances")
    return degraded, clean


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
        entry: torch.Tensor = self.entry(x)
        body: torch.Tensor = self.body(entry)
        residual: torch.Tensor = self.exit(body)
        return torch.clamp(x + residual, 0.0, 1.0)

    def training_step(self, batch: object, _idx: int) -> torch.Tensor:
        deg, clean = _tensor_pair(batch)
        out = self.forward(deg)
        loss = nn.functional.l1_loss(out, clean)
        if self._trainer is not None:
            self.log("train/l1", loss, prog_bar=True, on_epoch=True)
        return loss

    def validation_step(self, batch: object, _idx: int) -> None:
        deg, clean = _tensor_pair(batch)
        out = self.forward(deg)
        if self._trainer is not None:
            self.log("val/l1", nn.functional.l1_loss(out, clean), prog_bar=True, on_epoch=True)

    def configure_optimizers(self) -> torch.optim.Optimizer:
        return torch.optim.AdamW(self.parameters(), lr=self._hp["lr"])

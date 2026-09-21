# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Typed LPIPS 0.1 surface used by the deterministic exporter."""

import torch
from torch import nn

class LPIPS(nn.Module):
    def __init__(
        self,
        pretrained: bool = ...,
        net: str = ...,
        version: str = ...,
        lpips: bool = ...,
        spatial: bool = ...,
        pnet_rand: bool = ...,
        pnet_tune: bool = ...,
        use_dropout: bool = ...,
        model_path: str | None = ...,
        eval_mode: bool = ...,
        verbose: bool = ...,
    ) -> None: ...
    def forward(
        self,
        in0: torch.Tensor,
        in1: torch.Tensor,
        retPerLayer: bool = ...,
        normalize: bool = ...,
    ) -> torch.Tensor: ...

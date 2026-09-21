# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Typed subset of ``torchao.quantization.pt2e.quantize_pt2e`` used by VMAFx."""

from torch.fx import GraphModule
from torchao.quantization.pt2e.quantizer import Quantizer

def prepare_qat_pt2e(model: GraphModule, quantizer: Quantizer) -> GraphModule: ...

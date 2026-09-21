# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Typed subset of ``torchao.quantization.pt2e`` used by VMAFx."""

from torch.fx import GraphModule

def move_exported_model_to_train(model: GraphModule) -> GraphModule: ...

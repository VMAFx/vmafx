# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Typed subset of ``torchao.quantization.pt2e.quantizer`` used by VMAFx."""

class QuantizationConfig: ...

class Quantizer:
    def set_global(self, quantization_config: QuantizationConfig) -> Quantizer: ...

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Typed subset of torchao's X86Inductor quantizer used by VMAFx QAT."""

from torchao.quantization.pt2e.quantizer import QuantizationConfig, Quantizer

class X86InductorQuantizer(Quantizer):
    def set_global(self, quantization_config: QuantizationConfig) -> X86InductorQuantizer: ...

def get_default_x86_inductor_quantization_config(
    is_qat: bool = False,
    is_dynamic: bool = False,
    reduce_range: bool = False,
) -> QuantizationConfig: ...

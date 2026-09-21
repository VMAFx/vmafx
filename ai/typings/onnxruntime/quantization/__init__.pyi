# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Typed subset of onnxruntime.quantization 1.30 used by VMAFx."""

from collections.abc import Mapping
from enum import Enum
from os import PathLike

class CalibrationDataReader:
    def get_next(self) -> Mapping[str, object] | None: ...
    def rewind(self) -> None: ...

class QuantFormat(Enum):
    QOperator = ...
    QDQ = ...

class QuantType(Enum):
    QInt8 = ...
    QUInt8 = ...

def quantize_static(
    model_input: str | PathLike[str],
    model_output: str | PathLike[str],
    calibration_data_reader: CalibrationDataReader,
    **kwargs: object,
) -> None: ...
def quantize_dynamic(
    model_input: object,
    model_output: str | PathLike[str],
    **kwargs: object,
) -> None: ...

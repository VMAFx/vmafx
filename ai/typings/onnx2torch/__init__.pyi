# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Typed subset of onnx2torch used by the deterministic model exporter."""

from os import PathLike

from torch.nn import Module

def convert(model: str | PathLike[str] | object) -> Module: ...

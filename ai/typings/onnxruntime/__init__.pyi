# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Typed subset of ONNX Runtime 1.30 used by VMAFx."""

from collections.abc import Mapping, Sequence
from os import PathLike
from typing import Any

class NodeArg:
    name: str
    shape: list[int | str | None]
    type: str

class SessionOptions:
    log_severity_level: int

class InferenceSession:
    def __init__(
        self,
        path_or_bytes: str | PathLike[str] | bytes,
        sess_options: object | None = ...,
        providers: Sequence[str] | None = ...,
        provider_options: Sequence[Mapping[str, str]] | None = ...,
        **kwargs: object,
    ) -> None: ...
    def run(
        self,
        output_names: Sequence[str] | None,
        input_feed: Mapping[str, object],
        run_options: object | None = ...,
    ) -> list[Any]: ...
    def get_inputs(self) -> list[NodeArg]: ...
    def get_outputs(self) -> list[NodeArg]: ...
    def get_providers(self) -> list[str]: ...

def get_available_providers() -> list[str]: ...

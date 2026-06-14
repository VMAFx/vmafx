# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Pre-encode filter adapter registry (control-plane workstream D1).

Sibling family to :mod:`vmaftune.codec_adapters`. A *filter adapter*
describes a pre-encode FFmpeg filter whose strength knobs vmafx can
sweep in a VMAF-in-the-loop search, then drives the filter purely by
emitting ``-vf "<filter>=k=v:..."`` fragment strings — vmafx never runs
the filter itself.

The first member is the Pelorus deband filter
(``vf_pelorus_deband_vulkan``). Its 10 tunable knobs are frozen by the
two-repo control-plane contract (Pelorus ADR-0110); the vmafx side
hard-codes its search space against that table. See ADR-1116 for the
adapter-family decision and ADR-0106 for the coupling design.

The :class:`FilterAdapter` Protocol mirrors the
:class:`vmaftune.codec_adapters.CodecAdapter` shape: the search loop
consumes the declared knobs + the ``vf_fragment`` emitter and never
branches on the adapter's ``name``, so a future pre-filter (sharpen,
denoise, …) drops in as one file without touching the optimizer.
"""

from __future__ import annotations

from collections.abc import Mapping
from typing import Protocol, runtime_checkable

from .pelorus_deband import (
    DITHER_MODES,
    FILTER_NAME,
    PELORUS_DEBAND_KNOBS,
    Knob,
    KnobType,
    PelorusDebandAdapter,
)


@runtime_checkable
class FilterAdapter(Protocol):
    """Pre-encode filter-adapter contract (ADR-1116).

    Concrete adapters declare their tunable ``knobs`` (each a frozen
    ``(name, kind, lo, hi, default, semantics)`` record), validate a
    parameter dict against those bounds, and emit the ffmpeg ``-vf``
    fragment for a parameter dict. The prefilter search loop
    (:mod:`vmaftune.prefilter`) consumes this surface and never branches
    on ``name``.
    """

    name: str
    filter_name: str
    adapter_version: str

    @property
    def knobs(self) -> tuple[Knob, ...]:
        """The frozen tunable-knob table in contract order."""
        ...

    def knob(self, name: str) -> Knob:
        """Return the named :class:`Knob` or raise ``KeyError``."""
        ...

    def defaults(self) -> dict[str, float]:
        """Return the filter's default value for every knob."""
        ...

    def validate(self, params: Mapping[str, float]) -> None:
        """Raise ``ValueError`` if ``params`` violates the contract."""
        ...

    def clamp(self, params: Mapping[str, float]) -> dict[str, float]:
        """Clamp numeric values into range; raise on unknown knobs."""
        ...

    def vf_fragment(
        self,
        params: Mapping[str, float] | None = None,
        *,
        include_defaults: bool = False,
    ) -> str:
        """Emit the ``<filter>=k=v:...`` fragment for ``params``."""
        ...

    def vf_args(
        self,
        params: Mapping[str, float] | None = None,
        *,
        include_defaults: bool = False,
    ) -> list[str]:
        """Return the ``["-vf", "<fragment>"]`` argv slice."""
        ...


_REGISTRY: dict[str, FilterAdapter] = {
    "pelorus_deband": PelorusDebandAdapter(),
}


def get_filter_adapter(name: str) -> FilterAdapter:
    """Return the registered filter adapter named ``name``."""
    if name not in _REGISTRY:
        raise KeyError(f"unknown filter {name!r}; known pre-encode filters: {sorted(_REGISTRY)}")
    return _REGISTRY[name]


def known_filters() -> tuple[str, ...]:
    """Return the sorted tuple of registered filter-adapter names."""
    return tuple(sorted(_REGISTRY))


__all__ = [
    "DITHER_MODES",
    "FILTER_NAME",
    "PELORUS_DEBAND_KNOBS",
    "FilterAdapter",
    "Knob",
    "KnobType",
    "PelorusDebandAdapter",
    "get_filter_adapter",
    "known_filters",
]

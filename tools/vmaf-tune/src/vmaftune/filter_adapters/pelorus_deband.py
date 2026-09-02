# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Pelorus deband pre-filter adapter (control-plane workstream D1).

The Pelorus ``vf_pelorus_deband_vulkan`` filter is a *pre-encode*
debanding stage, not a codec. vmafx never runs the Vulkan filter — it
only emits the ``-vf "pelorus_deband_vulkan=range=..:thry=..:..."``
fragment string for a parameter dict and scores the encoded output.

The 10 tunable knobs declared here are a verbatim copy of the **frozen
control-plane contract** (Pelorus ADR-0110,
``docs/api/control-plane.md``). The names, FFmpeg ``AVOption`` types,
``[min, max]`` ranges, and defaults must mirror that table exactly.
Renaming / narrowing / retyping any knob is a coordinated two-repo
break (Pelorus + this adapter land together); see ADR-1116.

Out-of-contract AVOptions (``sample`` / ``blur`` / ``planes`` /
``meta``) are pipeline-topology or reporting switches and are
**intentionally absent** — they are set once per run by the harness,
never swept by the optimizer.
"""

from __future__ import annotations

import dataclasses
from collections.abc import Mapping
from typing import Literal

#: The ffmpeg filter name emitted in the ``-vf`` fragment. The vmafx
#: side hard-codes the Vulkan variant because the contract (ADR-0110)
#: freezes ``vf_pelorus_deband_vulkan`` specifically.
FILTER_NAME: str = "pelorus_deband_vulkan"

#: Knob type tags mirrored from the AVOption table. ``bool`` is a
#: 0/1 integer in FFmpeg; we keep it distinct so the search-space
#: builder can treat it as a binary categorical rather than a wide
#: ordinal range.
KnobType = Literal["int", "float", "bool", "enum"]


@dataclasses.dataclass(frozen=True)
class Knob:
    """One tunable AVOption from the frozen control-plane contract.

    ``lo`` / ``hi`` are inclusive bounds; ``default`` is the filter's
    own default (used when the knob is omitted from a parameter dict).
    ``kind`` drives both validation and how the search-space builder
    proposes values (``int`` / ``enum`` / ``bool`` are ordinal-integer
    or categorical; ``float`` is continuous).
    """

    name: str
    kind: KnobType
    lo: float
    hi: float
    default: float
    semantics: str

    @property
    def is_integral(self) -> bool:
        """True when the AVOption only accepts integer values."""
        return self.kind in ("int", "bool", "enum")


# ---------------------------------------------------------------------------
# The frozen contract (Pelorus ADR-0110 / docs/api/control-plane.md).
# DO NOT widen, narrow, rename, or retype without a coordinated two-repo
# PR. Order matches the contract table top-to-bottom; the emitted
# ``-vf`` fragment follows this order for deterministic, diff-friendly
# strings.
# ---------------------------------------------------------------------------
PELORUS_DEBAND_KNOBS: tuple[Knob, ...] = (
    Knob("range", "int", 1.0, 31.0, 15.0, "reference-sampling radius in pixels"),
    Knob("thry", "float", 0.0, 0.25, 0.012, "luma flat-test threshold (normalized full-range)"),
    Knob("thrc", "float", 0.0, 0.25, 0.012, "chroma flat-test threshold (normalized)"),
    Knob("grainy", "float", 0.0, 0.4, 0.006, "luma grain amplitude (normalized)"),
    Knob("grainc", "float", 0.0, 0.4, 0.0, "chroma grain amplitude (normalized)"),
    Knob("softness", "float", 0.0, 1.0, 0.5, "soft-blend transition width (0 = hard switch)"),
    Knob("detail", "float", 0.0, 0.25, 0.06, "detail-mask activity threshold (normalized)"),
    Knob("dither", "enum", 0.0, 2.0, 2.0, "0=none, 1=bayer8, 2=bluenoise"),
    Knob("dynamic", "bool", 0.0, 1.0, 1.0, "re-seed grain each frame"),
    Knob("protect", "bool", 0.0, 1.0, 1.0, "gate debanding off textured regions"),
)

#: Name → :class:`Knob` lookup for O(1) validation.
_KNOB_BY_NAME: dict[str, Knob] = {k.name: k for k in PELORUS_DEBAND_KNOBS}

#: ``dither`` enum integer → canonical string name. The AVOption table
#: accepts both integer and string forms; we emit integers for a stable,
#: locale-independent fragment, but expose the mapping for callers /
#: reports that want the human-readable name.
DITHER_MODES: dict[int, str] = {0: "none", 1: "bayer8", 2: "bluenoise"}


def _format_value(knob: Knob, value: float) -> str:
    """Render a validated value into its ffmpeg AVOption token.

    Integral knobs (``int`` / ``bool`` / ``enum``) emit a bare integer;
    floats emit a ``%g``-formatted token so ``0.012`` stays ``0.012``
    and ``0.0`` collapses to ``0`` (both accepted by the AVOption
    parser) without trailing-zero noise.
    """
    if knob.is_integral:
        return str(round(value))
    # ``%g`` drops trailing zeros and avoids scientific notation for the
    # contract's small magnitudes (0..0.4). The contract is bit-depth
    # independent, so no locale-sensitive formatting is involved.
    return f"{float(value):g}"


@dataclasses.dataclass(frozen=True)
class PelorusDebandAdapter:
    """Filter adapter for ``vf_pelorus_deband_vulkan`` (ADR-1116 / ADR-0110).

    Mirrors the :class:`vmaftune.filter_adapters.FilterAdapter` Protocol.
    Stateless and frozen — safe to share across threads / TPE trials.
    """

    name: str = "pelorus_deband"
    filter_name: str = FILTER_NAME
    # Bumps when the knob table / emission shape changes. Folded into
    # any cache key the prefilter loop derives so a contract update
    # invalidates stale recommendations (mirrors codec_adapters'
    # adapter_version convention, ADR-0298).
    adapter_version: str = "1"

    @property
    def knobs(self) -> tuple[Knob, ...]:
        """The frozen tunable-knob table (contract order)."""
        return PELORUS_DEBAND_KNOBS

    def knob(self, name: str) -> Knob:
        """Return the :class:`Knob` named ``name`` or raise ``KeyError``."""
        if name not in _KNOB_BY_NAME:
            raise KeyError(
                f"unknown pelorus deband knob {name!r}; the frozen contract "
                f"(ADR-0110) exposes: {sorted(_KNOB_BY_NAME)}"
            )
        return _KNOB_BY_NAME[name]

    def defaults(self) -> dict[str, float]:
        """Return the contract default for every knob."""
        return {k.name: k.default for k in PELORUS_DEBAND_KNOBS}

    def validate(self, params: Mapping[str, float]) -> None:
        """Raise ``ValueError`` if ``params`` violates the frozen contract.

        Rejects unknown knob names (including the deliberately
        out-of-contract ``sample`` / ``blur`` / ``planes`` / ``meta``)
        and out-of-range values. Integral knobs additionally reject
        non-integer inputs so a fractional ``range`` or ``dither`` is a
        hard error rather than a silently-rounded surprise.
        """
        for key, value in params.items():
            if key not in _KNOB_BY_NAME:
                raise ValueError(
                    f"{key!r} is not an autotune knob in the pelorus deband "
                    f"control-plane contract (ADR-0110). Tunable knobs: "
                    f"{sorted(_KNOB_BY_NAME)}. Note sample/blur/planes/meta "
                    f"are intentionally out-of-contract and must not be swept."
                )
            knob = _KNOB_BY_NAME[key]
            try:
                fval = float(value)
            except (TypeError, ValueError) as exc:
                raise ValueError(f"{key}: non-numeric value {value!r}") from exc
            if fval != fval:  # NaN
                raise ValueError(f"{key}: value is NaN")
            if not (knob.lo <= fval <= knob.hi):
                raise ValueError(
                    f"{key}={value} is outside the frozen contract range "
                    f"[{knob.lo:g}, {knob.hi:g}]"
                )
            if knob.is_integral and fval != int(fval):
                raise ValueError(
                    f"{key}={value} must be an integer ({knob.kind} knob); "
                    f"fractional values are rejected (the AVOption is integral)"
                )

    def clamp(self, params: Mapping[str, float]) -> dict[str, float]:
        """Clamp every supplied knob into its contract range.

        Unknown knob names still raise — clamping is for numeric
        out-of-range values, not for typos or out-of-contract options.
        Integral knobs are rounded to the nearest integer after
        clamping. Returns a new dict; the input is not mutated.
        """
        out: dict[str, float] = {}
        for key, value in params.items():
            if key not in _KNOB_BY_NAME:
                raise ValueError(
                    f"cannot clamp unknown knob {key!r}; not in the frozen " f"contract (ADR-0110)"
                )
            knob = _KNOB_BY_NAME[key]
            fval = float(value)
            fval = max(knob.lo, min(knob.hi, fval))
            if knob.is_integral:
                fval = float(round(fval))
            out[key] = fval
        return out

    def vf_fragment(
        self,
        params: Mapping[str, float] | None = None,
        *,
        include_defaults: bool = False,
    ) -> str:
        """Emit the ``pelorus_deband_vulkan=key=val:...`` fragment.

        ``params`` is a partial or full knob dict. Values are validated
        against the frozen contract before emission (raises
        ``ValueError`` on any violation). Omitted knobs fall back to the
        filter's own default — when ``include_defaults`` is False
        (the default) they are simply left off the fragment so the
        filter applies its built-in default; when True every contract
        knob is emitted explicitly (useful for reproducible / fully
        pinned encode commands and for diffing two parameter sets).

        Knobs are emitted in contract order regardless of ``params``
        iteration order, so the fragment is deterministic and
        diff-friendly.
        """
        resolved: dict[str, float] = dict(params or {})
        self.validate(resolved)
        if include_defaults:
            merged = self.defaults()
            merged.update(resolved)
            resolved = merged
        tokens: list[str] = []
        for knob in PELORUS_DEBAND_KNOBS:
            if knob.name not in resolved:
                continue
            tokens.append(f"{knob.name}={_format_value(knob, resolved[knob.name])}")
        if not tokens:
            # No knobs requested and defaults not forced — emit the bare
            # filter name so the chain is still a valid -vf entry.
            return self.filter_name
        return f"{self.filter_name}=" + ":".join(tokens)

    def vf_args(
        self,
        params: Mapping[str, float] | None = None,
        *,
        include_defaults: bool = False,
    ) -> list[str]:
        """Return the ``["-vf", "<fragment>"]`` argv slice.

        Convenience wrapper over :meth:`vf_fragment` for callers that
        splice directly into an ffmpeg argv list (e.g. the prefilter
        encode driver). The filter is emitted as a single ``-vf`` entry;
        a caller chaining it with other filters should compose the
        fragments and pass one ``-vf`` itself.
        """
        return ["-vf", self.vf_fragment(params, include_defaults=include_defaults)]


__all__ = [
    "DITHER_MODES",
    "FILTER_NAME",
    "PELORUS_DEBAND_KNOBS",
    "Knob",
    "KnobType",
    "PelorusDebandAdapter",
]

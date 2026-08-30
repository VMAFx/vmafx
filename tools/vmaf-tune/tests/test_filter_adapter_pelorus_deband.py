# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Pelorus deband filter-adapter tests (ADR-1116 / pelorus ADR-0110).

Unit-level only — no live encode. Validates:

1. The frozen 10-knob contract (names / types / ranges / defaults)
   matches the pelorus ADR-0110 control-plane table exactly.
2. ``-vf`` fragment emission (knob -> string) is deterministic and
   contract-ordered.
3. Range clamping + validation reject out-of-range and out-of-contract
   inputs (including the deliberately-excluded sample/blur/planes/meta).
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.filter_adapters import (  # noqa: E402
    PELORUS_DEBAND_KNOBS,
    FilterAdapter,
    PelorusDebandAdapter,
    get_filter_adapter,
    known_filters,
)
from vmaftune.filter_adapters.pelorus_deband import (  # noqa: E402
    DITHER_MODES,
    FILTER_NAME,
)

# The frozen contract, transcribed straight from
# pelorus docs/api/control-plane.md (ADR-0110). The test asserts the
# adapter mirrors THIS table — a drift in either direction fails the
# gate, which is exactly the two-repo break detector we want.
_CONTRACT = {
    # name:      (kind,     lo,    hi,    default)
    "range": ("int", 1.0, 31.0, 15.0),
    "thry": ("float", 0.0, 0.25, 0.012),
    "thrc": ("float", 0.0, 0.25, 0.012),
    "grainy": ("float", 0.0, 0.4, 0.006),
    "grainc": ("float", 0.0, 0.4, 0.0),
    "softness": ("float", 0.0, 1.0, 0.5),
    "detail": ("float", 0.0, 0.25, 0.06),
    "dither": ("enum", 0.0, 2.0, 2.0),
    "dynamic": ("bool", 0.0, 1.0, 1.0),
    "protect": ("bool", 0.0, 1.0, 1.0),
}

_OUT_OF_CONTRACT = ("sample", "blur", "planes", "meta")


@pytest.fixture()
def adapter() -> PelorusDebandAdapter:
    return PelorusDebandAdapter()


# ---------------------------------------------------------------------------
# Contract conformance.
# ---------------------------------------------------------------------------


def test_registry_exposes_pelorus_deband() -> None:
    assert "pelorus_deband" in known_filters()
    a = get_filter_adapter("pelorus_deband")
    assert isinstance(a, PelorusDebandAdapter)
    assert isinstance(a, FilterAdapter)  # runtime_checkable Protocol


def test_unknown_filter_raises() -> None:
    with pytest.raises(KeyError, match="unknown filter"):
        get_filter_adapter("sharpen")


def test_exactly_ten_contract_knobs(adapter: PelorusDebandAdapter) -> None:
    assert len(adapter.knobs) == 10
    assert len(PELORUS_DEBAND_KNOBS) == 10
    assert {k.name for k in adapter.knobs} == set(_CONTRACT)


@pytest.mark.parametrize("name", list(_CONTRACT))
def test_knob_matches_frozen_contract(adapter: PelorusDebandAdapter, name: str) -> None:
    kind, lo, hi, default = _CONTRACT[name]
    knob = adapter.knob(name)
    assert knob.kind == kind, f"{name} type drifted from ADR-0110 contract"
    assert knob.lo == lo, f"{name} min drifted from ADR-0110 contract"
    assert knob.hi == hi, f"{name} max drifted from ADR-0110 contract"
    assert knob.default == default, f"{name} default drifted from ADR-0110 contract"


def test_defaults_returns_every_contract_default(adapter: PelorusDebandAdapter) -> None:
    defaults = adapter.defaults()
    assert defaults == {n: _CONTRACT[n][3] for n in _CONTRACT}


def test_filter_name_is_vulkan_variant(adapter: PelorusDebandAdapter) -> None:
    assert adapter.filter_name == FILTER_NAME == "pelorus_deband_vulkan"


def test_dither_enum_mapping_is_frozen() -> None:
    assert DITHER_MODES == {0: "none", 1: "bayer8", 2: "bluenoise"}


def test_unknown_knob_lookup_raises(adapter: PelorusDebandAdapter) -> None:
    with pytest.raises(KeyError, match="unknown pelorus deband knob"):
        adapter.knob("sharpness")


# ---------------------------------------------------------------------------
# -vf fragment emission.
# ---------------------------------------------------------------------------


def test_partial_fragment_emits_only_requested_knobs(adapter: PelorusDebandAdapter) -> None:
    frag = adapter.vf_fragment({"range": 15, "thry": 0.012, "grainy": 0.006})
    assert frag == "pelorus_deband_vulkan=range=15:thry=0.012:grainy=0.006"


def test_fragment_is_contract_ordered_regardless_of_input_order(
    adapter: PelorusDebandAdapter,
) -> None:
    # Pass knobs out of contract order; emission must still be ordered.
    frag = adapter.vf_fragment({"grainy": 0.006, "range": 15, "thry": 0.012})
    assert frag == "pelorus_deband_vulkan=range=15:thry=0.012:grainy=0.006"


def test_include_defaults_emits_all_ten(adapter: PelorusDebandAdapter) -> None:
    frag = adapter.vf_fragment(include_defaults=True)
    # All 10 knobs, contract order, integral knobs as bare ints.
    assert frag == (
        "pelorus_deband_vulkan="
        "range=15:thry=0.012:thrc=0.012:grainy=0.006:grainc=0:"
        "softness=0.5:detail=0.06:dither=2:dynamic=1:protect=1"
    )


def test_integral_knobs_emit_bare_integers(adapter: PelorusDebandAdapter) -> None:
    frag = adapter.vf_fragment({"range": 12, "dither": 1, "dynamic": 0, "protect": 1})
    assert frag == "pelorus_deband_vulkan=range=12:dither=1:dynamic=0:protect=1"


def test_empty_params_emit_bare_filter_name(adapter: PelorusDebandAdapter) -> None:
    assert adapter.vf_fragment() == "pelorus_deband_vulkan"
    assert adapter.vf_fragment({}) == "pelorus_deband_vulkan"


def test_vf_args_returns_argv_slice(adapter: PelorusDebandAdapter) -> None:
    args = adapter.vf_args({"range": 20})
    assert args == ["-vf", "pelorus_deband_vulkan=range=20"]


def test_float_formatting_drops_trailing_zeros(adapter: PelorusDebandAdapter) -> None:
    # 0.0 collapses to "0"; 0.25 stays "0.25".
    frag = adapter.vf_fragment({"grainc": 0.0, "thry": 0.25})
    assert frag == "pelorus_deband_vulkan=thry=0.25:grainc=0"


# ---------------------------------------------------------------------------
# Validation + clamping against the frozen ranges.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("name", "value"),
    [
        ("range", 0),  # below min 1
        ("range", 32),  # above max 31
        ("thry", -0.01),
        ("thry", 0.26),  # above 0.25
        ("grainy", 0.41),  # above 0.4
        ("softness", 1.1),
        ("detail", 0.3),  # above 0.25
        ("dither", 3),  # above enum max 2
        ("dynamic", 2),  # above bool max 1
        ("protect", -1),
    ],
)
def test_out_of_range_value_rejected(
    adapter: PelorusDebandAdapter, name: str, value: float
) -> None:
    with pytest.raises(ValueError, match="outside the frozen contract range"):
        adapter.validate({name: value})


@pytest.mark.parametrize("name", _OUT_OF_CONTRACT)
def test_out_of_contract_knob_rejected(adapter: PelorusDebandAdapter, name: str) -> None:
    with pytest.raises(ValueError, match="not an autotune knob"):
        adapter.validate({name: 1})


@pytest.mark.parametrize(
    ("name", "value"),
    [
        ("range", 1.5),  # int knob, in-range fractional
        ("dither", 1.5),  # enum knob, in-range fractional
        ("dynamic", 0.5),  # bool knob — 0.5 is in [0,1] but not integral
        ("protect", 0.5),  # bool knob — same
    ],
)
def test_fractional_integral_knob_rejected(
    adapter: PelorusDebandAdapter, name: str, value: float
) -> None:
    with pytest.raises(ValueError, match="must be an integer"):
        adapter.validate({name: value})


def test_nan_value_rejected(adapter: PelorusDebandAdapter) -> None:
    with pytest.raises(ValueError, match="NaN"):
        adapter.validate({"thry": float("nan")})


def test_boundary_values_accepted(adapter: PelorusDebandAdapter) -> None:
    # Inclusive bounds must validate cleanly.
    adapter.validate(
        {
            "range": 1,
            "thry": 0.0,
            "thrc": 0.25,
            "grainy": 0.4,
            "softness": 1.0,
            "detail": 0.25,
            "dither": 2,
            "dynamic": 1,
            "protect": 0,
        }
    )


def test_clamp_pulls_values_into_range(adapter: PelorusDebandAdapter) -> None:
    clamped = adapter.clamp({"range": 99, "thry": 0.5, "grainy": -0.2, "dither": 5})
    assert clamped == {"range": 31.0, "thry": 0.25, "grainy": 0.0, "dither": 2.0}


def test_clamp_rounds_integral_knobs(adapter: PelorusDebandAdapter) -> None:
    clamped = adapter.clamp({"range": 12.6, "dither": 0.4})
    assert clamped["range"] == 13.0
    assert clamped["dither"] == 0.0


def test_clamp_rejects_unknown_knob(adapter: PelorusDebandAdapter) -> None:
    with pytest.raises(ValueError, match="cannot clamp unknown knob"):
        adapter.clamp({"sample": 4})


def test_vf_fragment_validates_before_emitting(adapter: PelorusDebandAdapter) -> None:
    with pytest.raises(ValueError, match="outside the frozen contract range"):
        adapter.vf_fragment({"range": 999})

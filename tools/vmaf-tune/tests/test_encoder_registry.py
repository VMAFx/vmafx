# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The Python adapters agree with the encoder registry (ADR-3388).

``codec_adapters/registry.gen.json`` is generated from
``api/vmafx-encoders.toml`` and embedded byte-identical by the Go
``pkg/encoderregistry``; this test holds every adapter in ``_REGISTRY``
equal to its implemented row, so a fact changed on one side fails here.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from vmaftune.bisect import _absolute_crf_range
from vmaftune.codec_adapters import _REGISTRY, known_codecs
from vmaftune.codec_adapters import registry as encoder_registry

FIELDS = (
    ("encoder", "ffmpeg_encoder", None),
    ("quality_knob", None, "knob"),
    ("quality_range", None, "quality_range"),
    ("quality_default", None, "quality_default"),
    ("invert_quality", None, "invert_quality"),
    ("presets", None, "presets"),
    ("adapter_version", None, "version"),
    ("probe_preset", None, "probe_preset"),
    ("probe_quality", None, "probe_quality"),
    ("supports_qpfile", None, "supports_qpfile"),
    ("supports_encoder_stats", None, "supports_encoder_stats"),
    ("supports_two_pass", None, "supports_two_pass"),
)


def _expected(row: dict, row_key: str | None, adapter_key: str | None) -> object:
    value = row[row_key] if row_key else row["adapter"][adapter_key]
    return tuple(value) if isinstance(value, list) else value


def test_implemented_ids_equal_known_codecs() -> None:
    assert sorted(r["id"] for r in encoder_registry.implemented()) == list(known_codecs())


@pytest.mark.parametrize("name", sorted(_REGISTRY))
def test_adapter_matches_registry_row(name: str) -> None:
    adapter = _REGISTRY[name]
    row = encoder_registry.row(name)
    assert "python" in row["surfaces"]
    for attr, row_key, adapter_key in FIELDS:
        assert getattr(adapter, attr) == _expected(row, row_key, adapter_key), (
            name,
            attr,
        )


@pytest.mark.parametrize("name", sorted(_REGISTRY))
def test_bisect_window_is_registry_absolute_range(name: str) -> None:
    low, high = encoder_registry.row(name)["adapter"]["absolute_range"]
    assert _absolute_crf_range(_REGISTRY[name]) == (low, high)


def test_unknown_row_raises_and_has_no_range() -> None:
    with pytest.raises(KeyError, match="no encoder registry row"):
        encoder_registry.row("libnot-an-encoder")
    assert encoder_registry.absolute_range("libnot-an-encoder") is None


def test_gap_rows_have_no_range(monkeypatch: pytest.MonkeyPatch) -> None:
    gap = {"id": "libgap", "adapter": None}
    doc = {"encoders": [gap], "variants": []}
    monkeypatch.setattr(encoder_registry, "document", lambda: doc)
    assert encoder_registry.absolute_range("libgap") is None
    assert encoder_registry.implemented() == ()


def test_boundary_ranges_are_inclusive_pairs() -> None:
    for entry in encoder_registry.implemented():
        low, high = entry["adapter"]["absolute_range"]
        q_low, q_high = entry["adapter"]["quality_range"]
        assert low <= q_low <= q_high <= high, entry["id"]


def test_python_copy_matches_go_copy() -> None:
    go_copy = Path(__file__).resolve().parents[3] / "pkg/encoderregistry/registry.gen.json"
    if not go_copy.is_file():
        pytest.skip(f"Go copy not in this tree ({go_copy}); codegen --check covers it")
    assert go_copy.read_bytes() == encoder_registry.GENERATED.read_bytes()

# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Branch-coverage tests for :mod:`vmaf_train.registry`.

These complement the torch-dependent end-to-end test in ``test_registry.py``
by exercising the pure-Python paths (``_hash_file``, ``compute_config_hash``,
``dumps_registry_json``, ``write_registry_json``, ``_sanitize_nonfinite``,
``ModelMetadata.to_json``, and the ``load`` round-trip) that don't need an
actual ONNX export. Round 2 of the ai/src coverage push.
"""

from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path

from vmaf_train.registry import (
    SCHEMA_VERSION,
    VALID_KINDS,
    ModelMetadata,
    _hash_file,
    _sanitize_nonfinite,
    compute_config_hash,
    dumps_registry_json,
    load,
    write_registry_json,
)


def test_hash_file_matches_hashlib_over_multi_chunk_input(tmp_path: Path) -> None:
    """Round-trip a >64 KiB payload to exercise the chunked-read loop."""
    payload = b"abc123" * 20_000  # 120 KB → multiple 64 KiB reads
    p = tmp_path / "blob.bin"
    p.write_bytes(payload)
    expected = hashlib.sha256(payload).hexdigest()
    assert _hash_file(p) == expected


def test_hash_file_handles_empty_file(tmp_path: Path) -> None:
    p = tmp_path / "empty.bin"
    p.write_bytes(b"")
    assert _hash_file(p) == hashlib.sha256(b"").hexdigest()


def test_compute_config_hash_without_manifest_is_just_config(tmp_path: Path) -> None:
    cfg = tmp_path / "cfg.yaml"
    cfg.write_text("model: fr\n")
    expected = hashlib.sha256(b"model: fr\n").hexdigest()
    assert compute_config_hash(cfg) == expected


def test_compute_config_hash_with_existing_manifest_includes_it(tmp_path: Path) -> None:
    cfg = tmp_path / "cfg.yaml"
    cfg.write_text("model: fr\n")
    manifest = tmp_path / "m.yaml"
    manifest.write_text("entries: []\n")

    h = hashlib.sha256()
    h.update(b"model: fr\n")
    h.update(b"\x00")
    h.update(b"entries: []\n")
    assert compute_config_hash(cfg, manifest) == h.hexdigest()


def test_compute_config_hash_with_missing_manifest_skips_it(tmp_path: Path) -> None:
    """Branch: ``manifest_path.exists()`` is False — should fall back to config only."""
    cfg = tmp_path / "cfg.yaml"
    cfg.write_text("model: fr\n")
    missing = tmp_path / "does_not_exist.yaml"
    assert not missing.exists()
    # Falls back to just hashing the config.
    expected = hashlib.sha256(b"model: fr\n").hexdigest()
    assert compute_config_hash(cfg, missing) == expected


def test_sanitize_nonfinite_replaces_nan_and_inf() -> None:
    assert _sanitize_nonfinite(math.nan) is None
    assert _sanitize_nonfinite(math.inf) is None
    assert _sanitize_nonfinite(-math.inf) is None


def test_sanitize_nonfinite_preserves_finite_floats_and_ints() -> None:
    assert _sanitize_nonfinite(1.5) == 1.5
    assert _sanitize_nonfinite(0.0) == 0.0
    assert _sanitize_nonfinite(42) == 42
    assert _sanitize_nonfinite("string") == "string"
    assert _sanitize_nonfinite(None) is None


def test_sanitize_nonfinite_recurses_into_dict_and_list() -> None:
    payload = {"good": 1.5, "bad": math.nan, "nested": [1.0, math.inf, {"deep": -math.inf}]}
    sanitised = _sanitize_nonfinite(payload)
    assert sanitised == {"good": 1.5, "bad": None, "nested": [1.0, None, {"deep": None}]}


def test_dumps_registry_json_produces_valid_json_with_no_nan() -> None:
    payload = {"models": [{"name": "a", "score": math.nan, "thresh": math.inf}]}
    raw = dumps_registry_json(payload)
    # Must be RFC 8259 compliant — no NaN/Infinity literals.
    assert "NaN" not in raw
    assert "Infinity" not in raw
    parsed = json.loads(raw)
    assert parsed == {"models": [{"name": "a", "score": None, "thresh": None}]}


def test_dumps_registry_json_no_trailing_newline() -> None:
    raw = dumps_registry_json({"a": 1})
    assert not raw.endswith("\n")


def test_dumps_registry_json_is_sorted_and_indented_by_default() -> None:
    raw = dumps_registry_json({"z": 1, "a": 2})
    # sort_keys default → "a" before "z"
    assert raw.index('"a"') < raw.index('"z"')
    # indent default → contains newline between keys
    assert "\n" in raw


def test_dumps_registry_json_respects_caller_kwargs() -> None:
    raw = dumps_registry_json({"a": 1, "b": 2}, indent=None, sort_keys=False)
    # indent=None → no newlines
    assert "\n" not in raw


def test_write_registry_json_writes_trailing_newline(tmp_path: Path) -> None:
    dst = tmp_path / "registry.json"
    write_registry_json(dst, {"models": []})
    content = dst.read_text(encoding="utf-8")
    assert content.endswith("\n")
    assert json.loads(content) == {"models": []}


def test_write_registry_json_sanitises_nonfinite_inplace(tmp_path: Path) -> None:
    dst = tmp_path / "registry.json"
    write_registry_json(dst, {"bad": math.nan})
    content = dst.read_text(encoding="utf-8")
    assert "NaN" not in content
    assert json.loads(content) == {"bad": None}


def test_model_metadata_to_json_serialises_defaults() -> None:
    meta = ModelMetadata(
        schema_version=SCHEMA_VERSION,
        name="m",
        kind="fr",
        onnx_opset=17,
        input_names=["x"],
        output_names=["y"],
    )
    raw = meta.to_json()
    assert raw.endswith("\n")
    doc = json.loads(raw)
    assert doc["schema_version"] == SCHEMA_VERSION
    assert doc["normalization"] == {}
    assert doc["dataset"] is None
    assert doc["cosign_signature"] is None


def test_load_roundtrips_metadata_without_onnx(tmp_path: Path) -> None:
    """Exercise ``registry.load`` without going through ``register`` (no ONNX)."""
    meta = ModelMetadata(
        schema_version=SCHEMA_VERSION,
        name="example",
        kind="nr",
        onnx_opset=17,
        input_names=["features"],
        output_names=["score"],
        normalization={"mean": [0.0], "std": [1.0]},
        dataset="synthetic",
        expected_output_range=[0.0, 100.0],
        license="BSD-3-Clause-Plus-Patent",
        notes="round-trip test",
    )
    sidecar = tmp_path / "example.json"
    sidecar.write_text(meta.to_json())

    loaded = load(sidecar)
    assert loaded == meta


def test_valid_kinds_includes_all_three_documented_kinds() -> None:
    assert {"fr", "nr", "filter"} == VALID_KINDS


def test_schema_version_is_positive_int() -> None:
    assert isinstance(SCHEMA_VERSION, int)
    assert SCHEMA_VERSION >= 1

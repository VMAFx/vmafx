# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for ``model/tiny/registry.json`` and its JSON Schema (T6-9 / ADR-0209).

Validates two invariants:

1. The shipped ``registry.json`` is *valid* against ``registry.schema.json``
   and is internally consistent (sha256 matches on-disk ONNX, sidecars
   exist, ``int8_sha256`` present iff ``quant_mode != fp32``,
   ``sigstore_bundle`` paths well-formed).
2. The validator *rejects* representative malformed entries — wrong
   hex length, bad ``kind`` enum, missing required fields, malformed
   ``sigstore_bundle`` extension.

These are fork-local additions; the Netflix golden assertions are
untouched (CLAUDE.md §8).
"""

from __future__ import annotations

import copy
import json
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "scripts"))

from validate_model_registry import (  # noqa: E402  pylint: disable=wrong-import-position
    _consistency_check,
    _structural_fallback_validate,
    validate,
)

REGISTRY_PATH = REPO_ROOT / "model" / "tiny" / "registry.json"
SCHEMA_PATH = REPO_ROOT / "model" / "tiny" / "registry.schema.json"


def _load_registry() -> dict:
    return json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))


def test_shipped_registry_passes_full_validation() -> None:
    rc, errors = validate(REGISTRY_PATH, SCHEMA_PATH)
    assert rc == 0, "registry validation failed:\n  " + "\n  ".join(errors)


def test_registry_schema_version_is_one() -> None:
    """Locks the post-T6-9 layout. Bump this assertion when schema_version moves."""
    reg = _load_registry()
    assert reg["schema_version"] == 1


def test_every_entry_has_license_metadata() -> None:
    """T6-9 added license + license_url + sigstore_bundle. Lock the rule."""
    reg = _load_registry()
    for m in reg["models"]:
        assert "license" in m, f"{m['id']}: missing license"
        assert "sigstore_bundle" in m, f"{m['id']}: missing sigstore_bundle"
        assert m["sigstore_bundle"].endswith(".sigstore.json")


@pytest.mark.xfail(
    strict=True,
    reason=(
        "ADR-1105: ensemble production flip is deferred to the one-shot post-RC "
        "retrain. The ADR-0321 production weights were LOSO-validated against "
        "codec_vocab=14; the vocab was later trimmed to 6, so #865 regenerated "
        "the on-disk ONNX in smoke mode (smoke=true) to keep the load path "
        "correct. Real production weights at codec_vocab=6 require re-running "
        "export_ensemble_v2_seeds.py, which is part of the locked one-shot "
        "retrain. strict=True flips this back to a hard failure the moment the "
        "retrain lands real weights (smoke=false + matching sidecar sha), "
        "forcing removal of this marker. See ADR-1105 and ADR-0321 follow-ups."
    ),
)
def test_fr_regressor_v2_ensemble_seed_rows_are_production() -> None:
    """ADR-0321 production flip; deferred to one-shot retrain per ADR-1105.

    The target contract is asserted verbatim below; the xfail marker tracks
    the deferral. When the one-shot retrain re-exports real production weights
    at codec_vocab=6, this test will xpass and strict=True will fail the suite
    until the marker is removed.
    """
    reg = _load_registry()
    rows = {m["id"]: m for m in reg["models"]}
    for seed in range(5):
        mid = f"fr_regressor_v2_ensemble_v1_seed{seed}"
        row = rows[mid]
        sidecar = json.loads((REGISTRY_PATH.parent / f"{mid}.json").read_text(encoding="utf-8"))
        assert row["smoke"] is False
        assert sidecar["sha256"] == row["sha256"]
        assert sidecar["gate"]["passed"] is True
        assert sidecar["gate"]["verdict"] == "PROMOTE"


def test_structural_fallback_rejects_bad_hex_sha256() -> None:
    reg = _load_registry()
    bad = copy.deepcopy(reg)
    bad["models"][0]["sha256"] = "deadbeef"  # too short
    errors = _structural_fallback_validate(bad)
    assert any("sha256" in e for e in errors)


def test_structural_fallback_rejects_unknown_kind() -> None:
    reg = _load_registry()
    bad = copy.deepcopy(reg)
    bad["models"][0]["kind"] = "magic"
    errors = _structural_fallback_validate(bad)
    assert any("kind" in e for e in errors)


def test_structural_fallback_rejects_missing_required() -> None:
    bad = {"schema_version": 1, "models": [{"id": "x"}]}
    errors = _structural_fallback_validate(bad)
    assert any("missing required" in e for e in errors)


def test_structural_fallback_rejects_bad_schema_version() -> None:
    bad = {"schema_version": 99, "models": []}
    errors = _structural_fallback_validate(bad)
    assert any("schema_version" in e for e in errors)


def test_consistency_rejects_unknown_model_file(tmp_path: Path) -> None:
    """Cross-file consistency catches an entry that points at a missing file."""
    bad = {
        "schema_version": 1,
        "models": [
            {
                "id": "ghost",
                "kind": "fr",
                "onnx": "ghost.onnx",
                "sha256": "0" * 64,
            }
        ],
    }
    errors = _consistency_check(bad, tmp_path)  # tmp_path has no ghost.onnx
    assert any("missing ONNX" in e for e in errors)


def test_consistency_rejects_malformed_bundle_extension(tmp_path: Path) -> None:
    onnx_path = tmp_path / "x.onnx"
    onnx_path.write_bytes(b"")  # empty file is enough; sha mismatch caught separately
    import hashlib

    sha = hashlib.sha256(b"").hexdigest()
    # Sidecar must exist for non-smoke entry.
    (tmp_path / "x.json").write_text("{}", encoding="utf-8")
    bad = {
        "schema_version": 1,
        "models": [
            {
                "id": "x",
                "kind": "fr",
                "onnx": "x.onnx",
                "sha256": sha,
                "sigstore_bundle": "x.bundle",  # wrong extension
            }
        ],
    }
    errors = _consistency_check(bad, tmp_path)
    assert any("sigstore_bundle" in e and ".sigstore.json" in e for e in errors)


@pytest.mark.skipif(
    "jsonschema" not in sys.modules
    and pytest.importorskip("jsonschema", reason="optional") is None,
    reason="jsonschema not installed; covered by structural fallback",
)
def test_jsonschema_rejects_bad_id_pattern() -> None:
    """When jsonschema is installed, the full Draft 2020-12 validator runs."""
    import jsonschema

    schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    bad = {
        "schema_version": 1,
        "models": [
            {
                "id": "Has Spaces",  # violates pattern
                "kind": "fr",
                "onnx": "x.onnx",
                "sha256": "0" * 64,
            }
        ],
    }
    validator = jsonschema.Draft202012Validator(schema)
    errors = list(validator.iter_errors(bad))
    assert any("does not match" in e.message or "pattern" in e.message for e in errors)


def _graph_bakes_scaler(onnx_path: Path) -> bool:
    """True when the ONNX graph applies the StandardScaler itself.

    Prefers the ``onnx`` protobuf parser (exact ``op_type`` match); on legs
    without ``onnx`` installed, falls back to a length-prefixed protobuf byte
    scan (``0x22`` = field 4 ``NodeProto.op_type``, ``0x03`` = string length).
    """
    try:
        import onnx  # type: ignore[import-not-found]
    except ImportError:
        raw = onnx_path.read_bytes()
        return (b"\x22\x03Sub" in raw) and (b"\x22\x03Div" in raw)
    ops = {n.op_type for n in onnx.load(str(onnx_path), load_external_data=False).graph.node}
    return "Sub" in ops and "Div" in ops


def test_int8_models_with_scaler_ops_declare_onnx_has_scaler() -> None:
    """Regression gate for T-TINY-V3-INT8-SIDECAR-MISSING-ONNX-HAS-SCALER-2026-09-04.

    ``model/tiny/vmaf_tiny_v3.int8.json`` shipped without
    ``"onnx_has_scaler": true`` even though ``vmaf_tiny_v3.int8.onnx`` bakes
    the feature scaler as Constant nodes. ``core/src/libvmaf.c`` therefore
    normalised the feature vector a second time and the pooled
    ``vmaf_tiny_model`` score on the Netflix src01 pair read 16.02 instead of
    71.95 (fp32 baseline 72.36).
    """
    tiny_dir = REGISTRY_PATH.parent
    checked = 0
    for int8_onnx in sorted(tiny_dir.glob("*.int8.onnx")):
        if not _graph_bakes_scaler(int8_onnx):
            continue
        checked += 1
        sidecar = int8_onnx.with_suffix(".json")
        if not sidecar.is_file():
            base_stem = int8_onnx.name[: -len(".int8.onnx")]
            sidecar = int8_onnx.with_name(f"{base_stem}.json")
        assert sidecar.is_file(), f"{int8_onnx.name}: companion sidecar missing"
        sdata = json.loads(sidecar.read_text(encoding="utf-8"))
        assert sdata.get("onnx_has_scaler") is True, (
            f"{int8_onnx.name} bakes scaler ops but {sidecar.name} does not "
            f"declare onnx_has_scaler: true"
        )
    assert checked > 0, "no scaler-baking int8 model found — fixture drift"

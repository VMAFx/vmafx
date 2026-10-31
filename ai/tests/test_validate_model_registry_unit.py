# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Unit tests for :mod:`ai.scripts.validate_model_registry` (T6-9 / ADR-0209).

Covers schema validation (jsonschema; fails closed when it is absent),
cross-file consistency invariants (sha256 match, sidecar presence,
quant_mode → int8_sha256 pairing, sigstore_bundle path shape), and the
``main`` CLI surface (exit codes 0/1/2, ``--out-json`` reporting).
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
from typing import Any
from unittest.mock import patch

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "ai" / "scripts" / "validate_model_registry.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("vmr_under_test", _SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


VMR = _load_module()


# ---------------------------------------------------------------------------
# _jsonschema_errors (the only schema validator; no structural stand-in)
# ---------------------------------------------------------------------------

_SCHEMA = json.loads((_REPO_ROOT / "model" / "tiny" / "registry.schema.json").read_text())


def _entry(**over: Any) -> dict[str, Any]:
    row: dict[str, Any] = {
        "id": "x",
        "kind": "fr",
        "onnx": "x.onnx",
        "sha256": "0" * 64,
        "teacher": "none",
    }
    row.update(over)
    return row


def test_no_structural_fallback_validator_remains() -> None:
    assert not hasattr(VMR, "_structural_fallback_validate")
    assert not hasattr(VMR, "_try_jsonschema_validate")


def test_schema_rejects_non_dict() -> None:
    errs = VMR._jsonschema_errors(["not-a-dict"], _SCHEMA)
    assert errs


def test_schema_rejects_bad_schema_version() -> None:
    errs = VMR._jsonschema_errors({"schema_version": 99, "models": []}, _SCHEMA)
    assert any("schema_version" in e for e in errs)


def test_schema_rejects_non_list_models() -> None:
    errs = VMR._jsonschema_errors({"schema_version": 1, "models": "oops"}, _SCHEMA)
    assert any("models" in e for e in errs)


def test_schema_flags_missing_required_fields() -> None:
    errs = VMR._jsonschema_errors({"schema_version": 1, "models": [{"id": "x"}]}, _SCHEMA)
    assert any("required" in e for e in errs)


def test_schema_flags_invalid_kind() -> None:
    errs = VMR._jsonschema_errors(
        {"schema_version": 1, "models": [_entry(kind="nonsense")]}, _SCHEMA
    )
    assert any("kind" in e for e in errs)


def test_schema_flags_bad_sha_length() -> None:
    errs = VMR._jsonschema_errors({"schema_version": 1, "models": [_entry(sha256="abc")]}, _SCHEMA)
    assert any("sha256" in e for e in errs)


def test_schema_flags_missing_teacher() -> None:
    entry = {"id": "x", "kind": "fr", "onnx": "x.onnx", "sha256": "0" * 64}
    errs = VMR._jsonschema_errors({"schema_version": 1, "models": [entry]}, _SCHEMA)
    assert any("teacher" in e for e in errs)


def test_schema_flags_bad_teacher_pattern() -> None:
    bad_teachers = ["VMAF_V0.6.1", "vmaf 0.6.1", "", "vmaf@0.6.1"]
    for bad in bad_teachers:
        row = {"id": "x", "kind": "fr", "onnx": "x.onnx", "sha256": "0" * 64, "teacher": bad}
        errs = VMR._jsonschema_errors({"schema_version": 1, "models": [row]}, _SCHEMA)
        assert any("teacher" in e for e in errs), f"expected pattern error for {bad!r}"


def test_schema_accepts_teacher_boundary_and_valid() -> None:
    for valid in ["none", "unknown", "vmaf_v0.6.1", "vmaf_v1.0.16_3d0h"]:
        row = {"id": "x", "kind": "fr", "onnx": "x.onnx", "sha256": "0" * 64, "teacher": valid}
        assert VMR._jsonschema_errors({"schema_version": 1, "models": [row]}, _SCHEMA) == []


def test_consistency_check_flags_missing_teacher(tmp_path: Path) -> None:
    blob = b"payload"
    (tmp_path / "x.onnx").write_bytes(blob)
    sha = hashlib.sha256(blob).hexdigest()
    reg = {"models": [{"id": "x", "kind": "fr", "onnx": "x.onnx", "sha256": sha, "smoke": True}]}
    errs = VMR._consistency_check(reg, tmp_path)
    assert any("missing required field 'teacher'" in e for e in errs)


def test_consistency_check_accepts_boundary_teachers(tmp_path: Path) -> None:
    blob = b"payload"
    (tmp_path / "x.onnx").write_bytes(blob)
    sha = hashlib.sha256(blob).hexdigest()
    for t in ["none", "unknown"]:
        reg = {
            "models": [
                {
                    "id": "x",
                    "kind": "fr",
                    "onnx": "x.onnx",
                    "sha256": sha,
                    "smoke": True,
                    "teacher": t,
                }
            ]
        }
        errs = VMR._consistency_check(reg, tmp_path)
        assert not any("teacher" in e for e in errs)


def test_consistency_check_flags_nonexistent_teacher(tmp_path: Path) -> None:
    blob = b"payload"
    (tmp_path / "x.onnx").write_bytes(blob)
    sha = hashlib.sha256(blob).hexdigest()
    reg = {
        "models": [
            {
                "id": "x",
                "kind": "fr",
                "onnx": "x.onnx",
                "sha256": sha,
                "smoke": True,
                "teacher": "nonexistent_teacher_model_v99",
            }
        ]
    }
    errs = VMR._consistency_check(reg, tmp_path)
    assert any(
        "teacher model 'nonexistent_teacher_model_v99' not found under model/" in e for e in errs
    )


def test_consistency_check_accepts_real_teacher(tmp_path: Path) -> None:
    blob = b"payload"
    (tmp_path / "x.onnx").write_bytes(blob)
    sha = hashlib.sha256(blob).hexdigest()
    reg = {
        "models": [
            {
                "id": "x",
                "kind": "fr",
                "onnx": "x.onnx",
                "sha256": sha,
                "smoke": True,
                "teacher": "vmaf_v0.6.1",
            }
        ]
    }
    errs = VMR._consistency_check(reg, tmp_path)
    assert not any("teacher" in e for e in errs)


def test_shipped_registry_every_entry_has_valid_teacher() -> None:
    reg_path = _REPO_ROOT / "model" / "tiny" / "registry.json"
    registry = json.loads(reg_path.read_text(encoding="utf-8"))
    models = registry.get("models", [])
    assert len(models) == 26
    for m in models:
        assert "teacher" in m, f"model {m.get('id')} missing teacher"
        teacher = m["teacher"]
        assert isinstance(teacher, str) and teacher != ""
        if teacher not in ("none", "unknown"):
            # Check model exists under model/
            matches = list((_REPO_ROOT / "model").glob(f"**/{teacher}.json"))
            assert len(matches) > 0, f"model {m['id']} teacher {teacher} not found under model/"


def test_schema_accepts_a_valid_entry() -> None:
    assert VMR._jsonschema_errors({"schema_version": 1, "models": [_entry()]}, _SCHEMA) == []


def test_validate_fails_closed_when_jsonschema_is_missing(tmp_path: Path) -> None:
    reg = tmp_path / "registry.json"
    reg.write_text(json.dumps({"schema_version": 1, "models": []}))
    schema = tmp_path / "schema.json"
    schema.write_text("{}")
    real_import = __import__

    def _no_jsonschema(name: str, *a: Any, **k: Any) -> Any:
        if name == "jsonschema":
            raise ImportError("blocked for the test")
        return real_import(name, *a, **k)

    with patch("builtins.__import__", side_effect=_no_jsonschema):
        rc, errs = VMR.validate(reg, schema)
    assert rc == 2
    assert (
        len(errs) == 1
        and "jsonschema" in errs[0]
        and "requirements/locks/jsonschema.txt" in errs[0]
    )


# ---------------------------------------------------------------------------
# _consistency_check
# ---------------------------------------------------------------------------


def _hash_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def test_consistency_check_flags_missing_onnx(tmp_path: Path) -> None:
    reg = {
        "models": [
            {
                "id": "x",
                "kind": "fr",
                "onnx": "absent.onnx",
                "sha256": "0" * 64,
            }
        ]
    }
    errs = VMR._consistency_check(reg, tmp_path)
    assert any("missing ONNX file" in e for e in errs)


def test_consistency_check_flags_sha_mismatch(tmp_path: Path) -> None:
    blob = b"payload"
    onnx = tmp_path / "x.onnx"
    onnx.write_bytes(blob)
    reg = {
        "models": [
            {
                "id": "x",
                "kind": "fr",
                "onnx": "x.onnx",
                "sha256": "f" * 64,
                "smoke": True,
            }
        ]
    }
    errs = VMR._consistency_check(reg, tmp_path)
    assert any("sha256 mismatch" in e for e in errs)


def test_consistency_check_requires_sidecar_for_non_smoke(tmp_path: Path) -> None:
    blob = b"payload"
    onnx = tmp_path / "x.onnx"
    onnx.write_bytes(blob)
    reg = {
        "models": [
            {
                "id": "x",
                "kind": "fr",
                "onnx": "x.onnx",
                "sha256": _hash_bytes(blob),
            }
        ]
    }
    errs = VMR._consistency_check(reg, tmp_path)
    assert any("missing sidecar" in e for e in errs)


def test_consistency_check_quant_mode_requires_int8_sha(tmp_path: Path) -> None:
    blob = b"payload"
    onnx = tmp_path / "x.onnx"
    onnx.write_bytes(blob)
    (tmp_path / "x.json").write_text("{}")  # sidecar to focus on the int8 rule
    reg = {
        "models": [
            {
                "id": "x",
                "kind": "fr",
                "onnx": "x.onnx",
                "sha256": _hash_bytes(blob),
                "quant_mode": "int8_dynamic",
            }
        ]
    }
    errs = VMR._consistency_check(reg, tmp_path)
    assert any("requires int8_sha256" in e for e in errs)


def test_consistency_check_int8_sha_mismatch(tmp_path: Path) -> None:
    blob = b"payload"
    int8_blob = b"int8payload"
    onnx = tmp_path / "x.onnx"
    int8 = tmp_path / "x.int8.onnx"
    onnx.write_bytes(blob)
    int8.write_bytes(int8_blob)
    (tmp_path / "x.json").write_text("{}")
    reg = {
        "models": [
            {
                "id": "x",
                "kind": "fr",
                "onnx": "x.onnx",
                "sha256": _hash_bytes(blob),
                "quant_mode": "int8_static",
                "int8_sha256": "0" * 64,
            }
        ]
    }
    errs = VMR._consistency_check(reg, tmp_path)
    assert any("int8_sha256 mismatch" in e for e in errs)


def test_consistency_check_flags_duplicate_ids(tmp_path: Path) -> None:
    reg = {
        "models": [
            {"id": "dup", "kind": "fr", "onnx": "", "sha256": "0" * 64},
            {"id": "dup", "kind": "fr", "onnx": "", "sha256": "0" * 64},
        ]
    }
    errs = VMR._consistency_check(reg, tmp_path)
    assert any("duplicate model id" in e for e in errs)


def test_consistency_check_sigstore_bundle_path_shape(tmp_path: Path) -> None:
    blob = b"payload"
    onnx = tmp_path / "x.onnx"
    onnx.write_bytes(blob)
    (tmp_path / "x.json").write_text("{}")
    reg = {
        "models": [
            {
                "id": "x",
                "kind": "fr",
                "onnx": "x.onnx",
                "sha256": _hash_bytes(blob),
                "sigstore_bundle": "x.bundle.txt",
            }
        ]
    }
    errs = VMR._consistency_check(reg, tmp_path)
    assert any("sigstore_bundle must end with .sigstore.json" in e for e in errs)


def test_consistency_check_accepts_well_formed_entry(tmp_path: Path) -> None:
    blob = b"payload"
    onnx = tmp_path / "x.onnx"
    onnx.write_bytes(blob)
    (tmp_path / "x.json").write_text("{}")
    reg = {
        "models": [
            {
                "id": "x",
                "kind": "fr",
                "onnx": "x.onnx",
                "sha256": _hash_bytes(blob),
                "sigstore_bundle": "x.sigstore.json",
                "teacher": "none",
            }
        ]
    }
    errs = VMR._consistency_check(reg, tmp_path)
    assert errs == []


# ---------------------------------------------------------------------------
# validate() — orchestrator
# ---------------------------------------------------------------------------


def test_validate_returns_2_when_files_missing(tmp_path: Path) -> None:
    rc, errs = VMR.validate(tmp_path / "absent.json", tmp_path / "absent.schema.json")
    assert rc == 2
    assert any("not found" in e for e in errs)


def test_validate_returns_1_on_bad_json(tmp_path: Path) -> None:
    reg = tmp_path / "reg.json"
    schema = tmp_path / "schema.json"
    reg.write_text("not-json-at-all")
    schema.write_text("{}")
    rc, errs = VMR.validate(reg, schema)
    assert rc == 1
    assert any("parse error" in e for e in errs)


def test_validate_returns_0_on_clean_minimal_registry(tmp_path: Path) -> None:
    # An empty schema accepts anything: only the cross-file checks decide.
    blob = b"payload"
    onnx = tmp_path / "x.onnx"
    onnx.write_bytes(blob)
    (tmp_path / "x.json").write_text("{}")
    reg_path = tmp_path / "registry.json"
    reg_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "models": [
                    {
                        "id": "x",
                        "kind": "fr",
                        "onnx": "x.onnx",
                        "sha256": _hash_bytes(blob),
                        "teacher": "none",
                    }
                ],
            }
        )
    )
    schema = tmp_path / "schema.json"
    schema.write_text("{}")
    rc, errs = VMR.validate(reg_path, schema)
    assert (rc, errs) == (0, [])


# ---------------------------------------------------------------------------
# main()
# ---------------------------------------------------------------------------


def test_main_returns_2_on_missing_files(tmp_path: Path) -> None:
    rc = VMR.main(
        [
            str(tmp_path / "absent.json"),
            "--schema",
            str(tmp_path / "absent.schema.json"),
        ]
    )
    assert rc == 2


def test_main_returns_0_with_clean_registry(tmp_path: Path) -> None:
    blob = b"payload"
    onnx = tmp_path / "x.onnx"
    onnx.write_bytes(blob)
    (tmp_path / "x.json").write_text("{}")
    reg_path = tmp_path / "registry.json"
    reg_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "models": [
                    {
                        "id": "x",
                        "kind": "fr",
                        "onnx": "x.onnx",
                        "sha256": _hash_bytes(blob),
                        "teacher": "none",
                    }
                ],
            }
        )
    )
    schema = tmp_path / "schema.json"
    schema.write_text("{}")

    rc = VMR.main([str(reg_path), "--schema", str(schema)])
    assert rc == 0


def test_main_writes_out_json_on_pass(tmp_path: Path) -> None:
    blob = b"payload"
    onnx = tmp_path / "x.onnx"
    onnx.write_bytes(blob)
    (tmp_path / "x.json").write_text("{}")
    reg_path = tmp_path / "registry.json"
    reg_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "models": [
                    {
                        "id": "x",
                        "kind": "fr",
                        "onnx": "x.onnx",
                        "sha256": _hash_bytes(blob),
                        "teacher": "none",
                    }
                ],
            }
        )
    )
    schema = tmp_path / "schema.json"
    schema.write_text("{}")
    out = tmp_path / "report.json"
    rc = VMR.main([str(reg_path), "--schema", str(schema), "--out-json", str(out)])
    assert rc == 0
    payload = json.loads(out.read_text())
    assert payload["ok"] is True
    assert payload["error_count"] == 0
    assert payload["model_count"] == 1


def test_main_writes_out_json_on_fail(tmp_path: Path) -> None:
    reg_path = tmp_path / "registry.json"
    # missing onnx file → consistency check fails
    reg_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "models": [
                    {
                        "id": "x",
                        "kind": "fr",
                        "onnx": "ghost.onnx",
                        "sha256": "0" * 64,
                        "teacher": "none",
                    }
                ],
            }
        )
    )
    schema = tmp_path / "schema.json"
    schema.write_text("{}")
    out = tmp_path / "report.json"
    rc = VMR.main([str(reg_path), "--schema", str(schema), "--out-json", str(out)])
    assert rc == 1
    payload = json.loads(out.read_text())
    assert payload["ok"] is False
    assert payload["error_count"] >= 1


# ---------------------------------------------------------------------------
# Graph metadata: registry opset and sidecars against the shipped graphs
# ---------------------------------------------------------------------------

_TINY = _REPO_ROOT / "model" / "tiny"


def _stage_graph(
    tmp_path: Path, src: str, name: str, sidecar: dict[str, Any] | None, **entry: Any
) -> dict[str, Any]:
    """Copy a shipped graph into @p tmp_path as <name>.onnx with an optional
    sidecar, and return a registry entry for it (sha256 filled in)."""
    data = (_TINY / src).read_bytes()
    (tmp_path / f"{name}.onnx").write_bytes(data)
    if sidecar is not None:
        (tmp_path / f"{name}.json").write_text(json.dumps(sidecar), encoding="utf-8")
    row: dict[str, Any] = {
        "id": name,
        "kind": "fr",
        "onnx": f"{name}.onnx",
        "smoke": True,
        "teacher": "none",
    }
    row["sha256"] = hashlib.sha256(data).hexdigest()
    row.update(entry)
    return row


def _graph_errors(tmp_path: Path, *rows: dict[str, Any]) -> list[str]:
    errors: list[str] = VMR._consistency_check(
        {"schema_version": 1, "models": list(rows)}, tmp_path
    )
    return errors


def test_graph_metadata_accepts_matching_sidecar(tmp_path: Path) -> None:
    sidecar = {
        "opset": 17,
        "features": ["adm2", "vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3", "motion2"],
        "input_names": ["features"],
        "output_names": ["vmaf"],
    }
    row = _stage_graph(tmp_path, "vmaf_tiny_v2.onnx", "fv", sidecar, opset=17)
    assert _graph_errors(tmp_path, row) == []


def test_graph_metadata_flags_registry_opset(tmp_path: Path) -> None:
    row = _stage_graph(tmp_path, "vmaf_tiny_v2.onnx", "fv", None, opset=18)
    errs = _graph_errors(tmp_path, row)
    assert any("registry opset 18 but fv.onnx imports opset 17" in e for e in errs)


def test_graph_metadata_flags_sidecar_opset_and_names(tmp_path: Path) -> None:
    sidecar = {"opset": 18, "output_name": "boundary_logits", "input_names": ["frames", "x"]}
    row = _stage_graph(tmp_path, "transnet_v2.onnx", "tn", sidecar)
    errs = _graph_errors(tmp_path, row)
    assert any("tn.json: opset 18" in e for e in errs)
    assert any("output_name 'boundary_logits' is not a graph output" in e for e in errs)
    assert any("input_names ['frames', 'x']" in e for e in errs)


def test_graph_metadata_flags_feature_count(tmp_path: Path) -> None:
    sidecar = {"feature_order": ["adm2", "vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3"]}
    row = _stage_graph(tmp_path, "vmaf_tiny_v2.onnx", "fv", sidecar)
    errs = _graph_errors(tmp_path, row)
    assert any("5 feature names for a 6-wide input" in e for e in errs)


def test_graph_metadata_flags_codec_block_width(tmp_path: Path) -> None:
    """fr_regressor_v2's codec input is 14 wide: 12 encoders + preset + CRF."""
    sidecar = {
        "encoder_vocab": [f"enc{i}" for i in range(11)],
        "codec_block_dim": 13,
        "codec_block_layout": [f"slot{i}" for i in range(14)],
    }
    row = _stage_graph(tmp_path, "fr_regressor_v2.onnx", "frv2", sidecar)
    errs = _graph_errors(tmp_path, row)
    assert any("encoder_vocab of 11 entries describes a codec block of 13" in e for e in errs)
    assert any("codec_block_dim 13, graph 14" in e for e in errs)
    assert not any("codec_block_layout" in e for e in errs)


def test_graph_metadata_flags_stale_sidecar_sha(tmp_path: Path) -> None:
    sidecar = {"sha256": "0" * 64}
    row = _stage_graph(tmp_path, "vmaf_tiny_v2.onnx", "fv", sidecar)
    errs = _graph_errors(tmp_path, row)
    assert any("fv.json: sha256 " + "0" * 64 in e for e in errs)


def test_graph_metadata_checks_the_int8_sibling(tmp_path: Path) -> None:
    """Boundary: the fp32 graph matches, its int8 sibling imports another opset."""
    int8 = (_TINY / "nr_metric_v1.int8.onnx").read_bytes()
    (tmp_path / "fv.int8.onnx").write_bytes(int8)
    row = _stage_graph(
        tmp_path,
        "vmaf_tiny_v2.onnx",
        "fv",
        None,
        opset=17,
        quant_mode="dynamic",
        int8_sha256=hashlib.sha256(int8).hexdigest(),
    )
    errs = _graph_errors(tmp_path, row)
    assert errs == ["fv: registry opset 17 but fv.int8.onnx imports opset 18"]


def test_graph_metadata_reports_unreadable_graph(tmp_path: Path) -> None:
    data = b"fixture-onnx"
    (tmp_path / "bad.onnx").write_bytes(data)
    row = {"id": "bad", "kind": "fr", "onnx": "bad.onnx", "smoke": True}
    row["sha256"] = hashlib.sha256(data).hexdigest()
    errs = _graph_errors(tmp_path, row)
    assert any("cannot read bad.onnx as ONNX" in e for e in errs)

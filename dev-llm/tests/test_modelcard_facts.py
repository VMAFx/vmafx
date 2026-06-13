"""Fact-collection for the model-card generator.

These tests stay in-process: no Ollama, no network. The fact collector
is the trust boundary — the LLM is told to only use facts from this
block, so its correctness is the whole game.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest


def _require(*mods: str) -> None:
    for m in mods:
        pytest.importorskip(m)


def _tiny_onnx(path: Path, in_features: int = 6) -> None:
    _require("onnx")
    import onnx
    from onnx import TensorProto, helper

    x = helper.make_tensor_value_info("features", TensorProto.FLOAT, ["N", in_features])
    y = helper.make_tensor_value_info("score", TensorProto.FLOAT, ["N", 1])
    w = helper.make_tensor("W", TensorProto.FLOAT, [in_features, 1], [0.1] * in_features)
    b = helper.make_tensor("B", TensorProto.FLOAT, [1], [0.0])
    node = helper.make_node("Gemm", ["features", "W", "B"], ["score"])
    graph = helper.make_graph([node], "mlp", [x], [y], [w, b])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    onnx.save(model, str(path))


def _sidecar(path: Path, **overrides) -> None:
    base = {
        "schema_version": 1,
        "name": path.stem,
        "kind": "fr",
        "dataset": "netflix-public-v1",
        "license": "BSD-3-Clause-Plus-Patent",
        "train_commit": "deadbeef",
        "input_names": ["features"],
        "output_names": ["score"],
        "normalization": {
            "mean": [0.5] * 6,
            "std": [0.1] * 6,
        },
    }
    base.update(overrides)
    path.write_text(json.dumps(base))


def test_collect_facts_captures_onnx_and_sidecar(tmp_path: Path) -> None:
    _require("onnx")
    from vmaf_dev_llm.modelcard_facts import collect_facts

    onnx_path = tmp_path / "fr.onnx"
    _tiny_onnx(onnx_path)
    _sidecar(onnx_path.with_suffix(".json"))
    facts = collect_facts(onnx_path)
    assert facts.name == "fr"
    assert facts.kind == "fr"
    assert facts.opset == 17
    assert facts.input_names == ["features"]
    assert facts.output_names == ["score"]
    assert facts.feature_contract_ok is True
    assert facts.sha256 and len(facts.sha256) == 64
    assert facts.byte_size > 0
    assert facts.normalization and len(facts.normalization["mean"]) == 6


def test_missing_sidecar_does_not_crash(tmp_path: Path) -> None:
    _require("onnx")
    from vmaf_dev_llm.modelcard_facts import collect_facts

    onnx_path = tmp_path / "naked.onnx"
    _tiny_onnx(onnx_path)
    facts = collect_facts(onnx_path)
    assert facts.sidecar_path is None
    assert facts.kind is None
    # ONNX-derived fields are still populated.
    assert facts.opset == 17


def test_feature_contract_mismatch_flagged(tmp_path: Path) -> None:
    _require("onnx")
    from vmaf_dev_llm.modelcard_facts import collect_facts

    onnx_path = tmp_path / "fr_wrong.onnx"
    _tiny_onnx(onnx_path, in_features=4)  # wrong column count
    _sidecar(onnx_path.with_suffix(".json"), kind="fr")
    facts = collect_facts(onnx_path)
    assert facts.feature_contract_ok is False


def test_op_allowlist_parsed_from_c_source(tmp_path: Path) -> None:
    _require("onnx")
    from vmaf_dev_llm.modelcard_facts import collect_facts

    onnx_path = tmp_path / "m.onnx"
    _tiny_onnx(onnx_path)
    _sidecar(onnx_path.with_suffix(".json"))
    # Build a minimal fake libvmaf tree with an allowlist containing Gemm
    # so our model's single op is allowed.
    (tmp_path / "libvmaf" / "src" / "dnn").mkdir(parents=True)
    (tmp_path / "libvmaf" / "src" / "dnn" / "op_allowlist.c").write_text(
        'static const char *allowed_ops[] = {"Gemm", "Relu"};\n'
    )
    facts = collect_facts(onnx_path, repo_root=tmp_path)
    assert facts.op_allowlist_status == "ok"
    assert facts.forbidden_ops == []


def test_op_allowlist_flags_forbidden_op(tmp_path: Path) -> None:
    _require("onnx")
    from vmaf_dev_llm.modelcard_facts import collect_facts

    onnx_path = tmp_path / "m.onnx"
    _tiny_onnx(onnx_path)
    (tmp_path / "libvmaf" / "src" / "dnn").mkdir(parents=True)
    (tmp_path / "libvmaf" / "src" / "dnn" / "op_allowlist.c").write_text(
        'static const char *allowed_ops[] = {"Conv"};\n'
    )
    facts = collect_facts(onnx_path, repo_root=tmp_path)
    assert facts.op_allowlist_status == "forbidden ops present"
    assert "Gemm" in facts.forbidden_ops


def test_to_markdown_skips_empty_and_escapes_lists(tmp_path: Path) -> None:
    _require("onnx")
    from vmaf_dev_llm.modelcard_facts import collect_facts

    onnx_path = tmp_path / "m.onnx"
    _tiny_onnx(onnx_path)
    _sidecar(onnx_path.with_suffix(".json"))
    md = collect_facts(onnx_path).to_markdown()
    assert "name: m" in md
    assert "kind: fr" in md
    # JSON-encoded list so the LLM sees it unambiguously.
    assert '"features"' in md


def test_eval_block_populated_when_parquet_given(tmp_path: Path) -> None:
    for m in ("onnx", "onnxruntime", "pandas", "scipy"):
        pytest.importorskip(m)
    import numpy as np
    import pandas as pd

    from vmaf_dev_llm.modelcard_facts import FEATURE_COLUMNS, collect_facts

    onnx_path = tmp_path / "m.onnx"
    _tiny_onnx(onnx_path, in_features=len(FEATURE_COLUMNS))
    _sidecar(onnx_path.with_suffix(".json"))

    rng = np.random.default_rng(0)
    data = {c: rng.standard_normal(80).astype(np.float32) for c in FEATURE_COLUMNS}
    data["mos"] = sum(data[c] for c in FEATURE_COLUMNS) * 0.1 + 50
    data["key"] = [f"k_{i:03d}" for i in range(80)]
    parquet = tmp_path / "f.parquet"
    pd.DataFrame(data).to_parquet(parquet)

    facts = collect_facts(onnx_path, features=parquet, split="all")
    assert facts.eval_report is not None
    assert facts.eval_report["n"] == 80
    assert facts.eval_report["split"] == "all"
    assert -1.0 <= facts.eval_report["plcc"] <= 1.0

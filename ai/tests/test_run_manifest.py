# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for shared AI run-manifest provenance helpers."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

from aiutils.run_manifest import (
    build_run_manifest_payload,
    build_run_provenance,
    describe_path,
    dumps_manifest_json,
    normalise_namespace,
    write_manifest_json,
    write_run_manifest,
)


def test_describe_path_hashes_existing_files_relative_to_repo(tmp_path: Path) -> None:
    root = tmp_path / "repo"
    root.mkdir()
    payload = root / "data.jsonl"
    payload.write_text("row\n", encoding="utf-8")

    described = describe_path(payload, repo_root=root)

    assert described["path"] == "data.jsonl"
    assert described["kind"] == "file"
    assert described["exists"] is True
    assert isinstance(described["sha256"], str)
    assert len(described["sha256"]) == 64


def test_normalise_namespace_serializes_paths_and_sorts_keys(tmp_path: Path) -> None:
    args = argparse.Namespace(
        beta=[tmp_path / "b", None],
        alpha=tmp_path / "a",
        hidden="skip",
    )

    normalised = normalise_namespace(args, exclude={"hidden"})

    assert list(normalised) == ["alpha", "beta"]
    assert normalised["alpha"] == str(tmp_path / "a")
    assert normalised["beta"] == [str(tmp_path / "b"), None]


def test_normalise_namespace_replaces_nonfinite_floats(tmp_path: Path) -> None:
    args = argparse.Namespace(alpha=math.nan, beta=math.inf, gamma=-math.inf, delta=1.25)

    normalised = normalise_namespace(args)

    assert normalised == {"alpha": None, "beta": None, "delta": 1.25, "gamma": None}


def test_build_run_provenance_records_inputs_outputs_and_args(tmp_path: Path) -> None:
    root = tmp_path / "repo"
    root.mkdir()
    script = root / "ai" / "scripts" / "train.py"
    script.parent.mkdir(parents=True)
    script.write_text("print('train')\n", encoding="utf-8")
    input_path = root / "features.jsonl"
    input_path.write_text("{}\n", encoding="utf-8")
    output_path = root / "model.onnx"

    provenance = build_run_provenance(
        entrypoint=script,
        repo_root=root,
        argv=["--features", str(input_path), "--out", str(output_path)],
        args=argparse.Namespace(features=input_path, out=output_path, run_argv_json="[]"),
        inputs={"features": input_path},
        outputs={"model": output_path},
        exclude_args={"run_argv_json"},
    )

    assert provenance["schema"] == "ai-run-provenance-v1"
    assert provenance["entrypoint"]["path"] == "ai/scripts/train.py"
    assert provenance["entrypoint"]["sha256"]
    assert provenance["args"] == {"features": str(input_path), "out": str(output_path)}
    assert provenance["inputs"]["features"]["kind"] == "file"
    assert provenance["outputs"]["model"]["kind"] == "missing"


def test_write_manifest_json_is_sorted_and_newline_terminated(tmp_path: Path) -> None:
    manifest = tmp_path / "manifest.json"

    write_manifest_json(manifest, {"z": 1, "a": {"b": 2}})

    raw = manifest.read_text(encoding="utf-8")
    assert raw.endswith("\n")
    assert raw.splitlines()[1].strip().startswith('"a"')
    assert json.loads(raw) == {"a": {"b": 2}, "z": 1}


def test_write_manifest_json_is_strict_for_nonfinite_values(tmp_path: Path) -> None:
    manifest = tmp_path / "manifest.json"

    write_manifest_json(
        manifest,
        {
            "metrics": {
                "plcc": math.nan,
                "srocc": math.inf,
                "rmse": -math.inf,
                "ok": 0.75,
            }
        },
    )

    raw = manifest.read_text(encoding="utf-8")
    assert "NaN" not in raw
    assert "Infinity" not in raw
    assert json.loads(raw)["metrics"] == {
        "ok": 0.75,
        "plcc": None,
        "rmse": None,
        "srocc": None,
    }


def test_write_manifest_json_accepts_json_lists(tmp_path: Path) -> None:
    cache = tmp_path / "cache.json"

    write_manifest_json(cache, [{"score": math.nan}, {"score": 1.0}])

    raw = cache.read_text(encoding="utf-8")
    assert raw.endswith("\n")
    assert "NaN" not in raw
    assert json.loads(raw) == [{"score": None}, {"score": 1.0}]


def test_dumps_manifest_json_matches_write_boundary() -> None:
    raw = dumps_manifest_json({"z": math.inf, "a": {"score": math.nan}})

    assert raw.endswith("\n")
    assert "Infinity" not in raw
    assert "NaN" not in raw
    assert json.loads(raw) == {"a": {"score": None}, "z": None}


def test_build_run_manifest_payload_deduplicates_common_envelope(tmp_path: Path) -> None:
    root = tmp_path / "repo"
    root.mkdir()
    script = root / "ai" / "scripts" / "extract.py"
    script.parent.mkdir(parents=True)
    script.write_text("print('extract')\n", encoding="utf-8")
    source = root / "features.parquet"
    source.write_text("fake\n", encoding="utf-8")
    output = root / "report.json"

    payload = build_run_manifest_payload(
        schema="example-manifest-v1",
        entrypoint=script,
        repo_root=root,
        argv=["--source", str(source), "--output", str(output)],
        args={"source": source, "output": output},
        inputs={"source": source},
        outputs={"report": output},
        sections={"row_count": 7, "config": {"features": ("adm2", "motion2")}},
    )

    assert payload["schema"] == "example-manifest-v1"
    assert payload["row_count"] == 7
    assert payload["config"] == {"features": ["adm2", "motion2"]}
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"
    assert payload["run_provenance"]["inputs"]["source"]["kind"] == "file"
    assert payload["run_provenance"]["outputs"]["report"]["kind"] == "missing"


def test_write_run_manifest_writes_payload(tmp_path: Path) -> None:
    root = tmp_path / "repo"
    root.mkdir()
    script = root / "script.py"
    script.write_text("print('x')\n", encoding="utf-8")
    manifest = tmp_path / "manifest.json"

    write_run_manifest(
        manifest,
        schema="example-run-v1",
        entrypoint=script,
        repo_root=root,
        argv=["--ok"],
        args=argparse.Namespace(ok=True),
        sections={"status": "pass"},
    )

    payload = json.loads(manifest.read_text(encoding="utf-8"))
    assert payload["schema"] == "example-run-v1"
    assert payload["status"] == "pass"
    assert payload["run_provenance"]["entrypoint"]["path"] == "script.py"

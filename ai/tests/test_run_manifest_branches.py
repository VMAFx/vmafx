# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Branch-coverage top-up for ``aiutils.run_manifest``.

The existing ``test_run_manifest.py`` covers the happy paths for shared
provenance. This module closes the residual branches around path
normalisation: relative-path fallback when paths live outside the repo
root, directory describes, None-valued path entries, and sequence-typed
path collections.
"""

from __future__ import annotations

import json
from pathlib import Path

from aiutils.run_manifest import (
    build_run_provenance,
    describe_path,
    describe_paths,
    normalise_manifest_value,
    repo_relative_path,
    write_manifest_json,
    write_run_manifest,
)

# ---------------------------------------------- normalise_manifest_value


class _Opaque:
    """A value that's neither a JSON scalar nor a Mapping/Sequence/Path."""

    def __str__(self) -> str:
        return "opaque-token"


def test_normalise_manifest_value_falls_back_to_str_for_opaque() -> None:
    """Unrecognised types fall through to ``str(value)`` (line 32)."""
    assert normalise_manifest_value(_Opaque()) == "opaque-token"


def test_normalise_manifest_value_path_becomes_str(tmp_path: Path) -> None:
    """A Path value renders as its string form (line 22)."""
    p = tmp_path / "foo.txt"
    assert normalise_manifest_value(p) == str(p)


def test_normalise_manifest_value_mapping_sorts_keys() -> None:
    """A Mapping is recursively normalised and key-sorted (lines 23-27)."""
    result = normalise_manifest_value({"b": 1, "a": [2, 3]})
    assert list(result.keys()) == ["a", "b"]
    assert result == {"a": [2, 3], "b": 1}


def test_normalise_manifest_value_list_recurses() -> None:
    """List/tuple branch recurses element-wise (line 29)."""
    assert normalise_manifest_value((1, "x", _Opaque())) == [1, "x", "opaque-token"]


# -------------------------------------------------- repo_relative_path


def test_repo_relative_path_falls_back_to_str_when_outside_repo(tmp_path: Path) -> None:
    """Paths outside the repo root return their raw string form."""
    repo = tmp_path / "repo"
    repo.mkdir()
    outside = tmp_path / "outside" / "file.txt"
    outside.parent.mkdir()
    outside.write_text("x", encoding="utf-8")

    result = repo_relative_path(outside, repo_root=repo)

    assert result == str(outside)


# ----------------------------------------------------- describe_path


def test_describe_path_handles_directory(tmp_path: Path) -> None:
    """Directory targets report ``kind == "directory"`` without a sha256."""
    repo = tmp_path
    sub = tmp_path / "subdir"
    sub.mkdir()

    info = describe_path(sub, repo_root=repo)

    assert info["kind"] == "directory"
    assert info["exists"] is True
    assert "sha256" not in info


def test_describe_path_handles_missing(tmp_path: Path) -> None:
    """Non-existent targets report ``kind == "missing"``."""
    info = describe_path(tmp_path / "nope", repo_root=tmp_path)
    assert info["kind"] == "missing"
    assert info["exists"] is False


# ---------------------------------------------------- describe_paths


def test_describe_paths_passes_through_none(tmp_path: Path) -> None:
    """``None`` entries in a path map are preserved as JSON null."""
    described = describe_paths({"optional_output": None}, repo_root=tmp_path)
    assert described == {"optional_output": None}


def test_describe_paths_recurses_into_path_sequence(tmp_path: Path) -> None:
    """A list/tuple of Paths is described element-wise as a JSON list."""
    a = tmp_path / "a.txt"
    a.write_text("a", encoding="utf-8")
    b = tmp_path / "b.txt"
    b.write_text("b", encoding="utf-8")

    described = describe_paths({"shards": [a, b]}, repo_root=tmp_path)

    assert isinstance(described["shards"], list)
    assert len(described["shards"]) == 2
    for entry in described["shards"]:
        assert entry["kind"] == "file"
        assert "sha256" in entry


def test_describe_paths_passes_through_scalar(tmp_path: Path) -> None:
    """A plain scalar (non-Path, non-sequence) is normalised, not described."""
    described = describe_paths({"row_count": 12345}, repo_root=tmp_path)
    assert described == {"row_count": 12345}


# ----------------------- build_run_provenance integration coverage


def test_build_run_provenance_emits_inputs_with_optional_none(tmp_path: Path) -> None:
    """Inputs with a None entry round-trip through build_run_provenance."""
    entry = tmp_path / "entrypoint.py"
    entry.write_text("# stub\n", encoding="utf-8")
    in_path = tmp_path / "data.parquet"
    in_path.write_text("x", encoding="utf-8")

    payload = build_run_provenance(
        entrypoint=entry,
        repo_root=tmp_path,
        argv=["--flag"],
        args={"flag": True},
        inputs={"features": in_path, "optional": None},
    )

    assert payload["schema"] == "ai-run-provenance-v1"
    inputs = payload["inputs"]
    assert isinstance(inputs, dict)
    assert inputs["optional"] is None
    assert inputs["features"]["kind"] == "file"


# ----------------------- write_run_manifest + write_manifest_json


def test_write_manifest_json_creates_parent_and_trailing_newline(tmp_path: Path) -> None:
    """``write_manifest_json`` mkdirs the parent and ends with ``\\n``."""
    out = tmp_path / "nested" / "deep" / "manifest.json"
    payload = {"b": 2, "a": 1}

    write_manifest_json(out, payload)

    assert out.is_file()
    raw = out.read_text(encoding="utf-8")
    assert raw.endswith("\n")
    parsed = json.loads(raw)
    assert parsed == payload


def test_write_run_manifest_round_trips_full_payload(tmp_path: Path) -> None:
    """``write_run_manifest`` writes a schema + sections + run_provenance blob."""
    entry = tmp_path / "trainer.py"
    entry.write_text("# stub\n", encoding="utf-8")
    in_path = tmp_path / "features.parquet"
    in_path.write_text("x", encoding="utf-8")
    out = tmp_path / "manifest.json"

    write_run_manifest(
        out,
        schema="ai-run-manifest-v1",
        entrypoint=entry,
        repo_root=tmp_path,
        argv=["--epochs", "1"],
        args={"epochs": 1},
        inputs={"features": in_path},
        sections={"rows": 42},
    )

    payload = json.loads(out.read_text(encoding="utf-8"))
    assert payload["schema"] == "ai-run-manifest-v1"
    assert payload["rows"] == 42
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"
    assert payload["run_provenance"]["inputs"]["features"]["kind"] == "file"

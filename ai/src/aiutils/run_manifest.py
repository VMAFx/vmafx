# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Shared helpers for AI training/export run provenance manifests."""

from __future__ import annotations

import argparse
import json
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

from aiutils.file_utils import sha256, write_text_atomic

JsonScalar = str | int | float | bool | None
JsonValue = JsonScalar | list["JsonValue"] | dict[str, "JsonValue"]


def normalise_manifest_value(value: Any) -> JsonValue:
    """Convert common Python CLI values to deterministic JSON values."""
    if isinstance(value, Path):
        return str(value)
    if isinstance(value, Mapping):
        return {
            str(key): normalise_manifest_value(item)
            for key, item in sorted(value.items(), key=lambda pair: str(pair[0]))
        }
    if isinstance(value, tuple | list):
        return [normalise_manifest_value(item) for item in value]
    if isinstance(value, JsonScalar):
        return value
    return str(value)


def normalise_namespace(
    args: argparse.Namespace | Mapping[str, Any], *, exclude: set[str] | None = None
) -> dict[str, JsonValue]:
    """Return a sorted, JSON-safe copy of parsed CLI arguments."""
    raw = dict(vars(args)) if isinstance(args, argparse.Namespace) else dict(args)
    excluded = exclude or set()
    return {key: normalise_manifest_value(raw[key]) for key in sorted(raw) if key not in excluded}


def repo_relative_path(path: Path, *, repo_root: Path) -> str:
    """Render ``path`` relative to ``repo_root`` when possible."""
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return str(path)


def describe_path(path: Path, *, repo_root: Path) -> dict[str, JsonValue]:
    """Describe a filesystem path without requiring it to exist."""
    resolved = Path(path)
    info: dict[str, JsonValue] = {
        "path": repo_relative_path(resolved, repo_root=repo_root),
        "exists": resolved.exists(),
    }
    if resolved.is_file():
        info["kind"] = "file"
        info["sha256"] = sha256(resolved)
    elif resolved.is_dir():
        info["kind"] = "directory"
    else:
        info["kind"] = "missing"
    return info


def _describe_path_value(value: Any, *, repo_root: Path) -> JsonValue:
    if value is None:
        return None
    if isinstance(value, Path):
        return describe_path(value, repo_root=repo_root)
    if isinstance(value, Sequence) and not isinstance(value, str | bytes | bytearray):
        return [_describe_path_value(item, repo_root=repo_root) for item in value]
    return normalise_manifest_value(value)


def describe_paths(paths: Mapping[str, Any], *, repo_root: Path) -> dict[str, JsonValue]:
    """Describe a named input/output path map for a manifest."""
    return {
        str(key): _describe_path_value(value, repo_root=repo_root)
        for key, value in sorted(paths.items(), key=lambda pair: str(pair[0]))
    }


def build_run_provenance(
    *,
    entrypoint: Path,
    repo_root: Path,
    argv: Sequence[str],
    args: argparse.Namespace | Mapping[str, Any],
    inputs: Mapping[str, Any] | None = None,
    outputs: Mapping[str, Any] | None = None,
    exclude_args: set[str] | None = None,
) -> dict[str, JsonValue]:
    """Build the stable ``run_provenance`` sidecar block."""
    provenance: dict[str, JsonValue] = {
        "schema": "ai-run-provenance-v1",
        "entrypoint": describe_path(entrypoint, repo_root=repo_root),
        "argv": [str(item) for item in argv],
        "args": normalise_namespace(args, exclude=exclude_args),
    }
    if inputs:
        provenance["inputs"] = describe_paths(inputs, repo_root=repo_root)
    if outputs:
        provenance["outputs"] = describe_paths(outputs, repo_root=repo_root)
    return provenance


def build_run_manifest_payload(
    *,
    schema: str,
    entrypoint: Path,
    repo_root: Path,
    argv: Sequence[str],
    args: argparse.Namespace | Mapping[str, Any],
    inputs: Mapping[str, Any] | None = None,
    outputs: Mapping[str, Any] | None = None,
    exclude_args: set[str] | None = None,
    sections: Mapping[str, Any] | None = None,
) -> dict[str, JsonValue]:
    """Build a deterministic script-specific manifest with shared provenance.

    ``sections`` carries adapter-style per-script fields such as row counts,
    selected features, gate status, or calibration metadata. The envelope and
    ``run_provenance`` block remain shared so scripts do not hand-roll the
    same JSON structure.
    """
    payload: dict[str, JsonValue] = {"schema": schema}
    if sections:
        for key, value in sorted(sections.items(), key=lambda pair: str(pair[0])):
            payload[str(key)] = normalise_manifest_value(value)
    payload["run_provenance"] = build_run_provenance(
        entrypoint=entrypoint,
        repo_root=repo_root,
        argv=argv,
        args=args,
        inputs=inputs,
        outputs=outputs,
        exclude_args=exclude_args,
    )
    return payload


def write_run_manifest(
    path: Path,
    *,
    schema: str,
    entrypoint: Path,
    repo_root: Path,
    argv: Sequence[str],
    args: argparse.Namespace | Mapping[str, Any],
    inputs: Mapping[str, Any] | None = None,
    outputs: Mapping[str, Any] | None = None,
    exclude_args: set[str] | None = None,
    sections: Mapping[str, Any] | None = None,
) -> None:
    """Write a deterministic script manifest plus shared run provenance."""
    write_manifest_json(
        path,
        build_run_manifest_payload(
            schema=schema,
            entrypoint=entrypoint,
            repo_root=repo_root,
            argv=argv,
            args=args,
            inputs=inputs,
            outputs=outputs,
            exclude_args=exclude_args,
            sections=sections,
        ),
    )


def write_manifest_json(path: Path, payload: Mapping[str, Any]) -> None:
    """Write a deterministic JSON manifest atomically with a trailing newline.

    Uses :func:`aiutils.file_utils.write_text_atomic` so a crash during
    the write never leaves a partially-truncated manifest on disk.
    """
    write_text_atomic(path, json.dumps(payload, indent=2, sort_keys=True) + "\n")

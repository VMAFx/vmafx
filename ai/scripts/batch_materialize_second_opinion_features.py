#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
"""Run second-opinion feature materialization for multiple tables."""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from _script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_repo_root=True, include_ai_scripts=True)
from materialize_second_opinion_features import REPO_ROOT, materialize  # noqa: E402

from aiutils.cli_helpers import (  # noqa: E402
    add_batch_manifest_arguments,
    collect_cli_argv,
    make_argument_parser,
)
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

SCRIPT_PATH = _SCRIPT_PATHS.script_path
_CONFIG_FIELDS = {
    "key_column",
    "score_key_field",
    "score_field",
    "key_normalize",
    "missing_policy",
    "prefix",
    "overwrite",
}
_TABLE_FIELDS = {"id", "features", "scores", "out", "audit_json"}


@dataclass(frozen=True)
class SecondOpinionBatchSpec:
    """Resolved table entry from a second-opinion batch manifest."""

    table_id: str
    features: Path
    scores: list[str]
    out: Path
    audit_json: Path | None
    config: dict[str, Any]


@dataclass(frozen=True)
class SecondOpinionBatchOptions:
    """Runtime policy for the batch run."""

    fail_fast: bool = False


def load_batch_manifest(
    path: Path, *, base_dir: Path | None = None
) -> list[SecondOpinionBatchSpec]:
    """Load and validate a second-opinion materialization batch manifest."""
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError(f"invalid JSON manifest {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise ValueError("manifest root must be a JSON object")
    root_dir = base_dir or path.parent
    defaults = payload.get("defaults", {})
    if defaults is None:
        defaults = {}
    if not isinstance(defaults, dict):
        raise ValueError("manifest defaults must be an object")
    tables = payload.get("tables")
    if not isinstance(tables, list) or not tables:
        raise ValueError("manifest tables must be a non-empty array")
    specs: list[SecondOpinionBatchSpec] = []
    for index, raw_table in enumerate(tables):
        if not isinstance(raw_table, dict):
            raise ValueError(f"table entry {index} must be an object")
        specs.append(_table_spec(raw_table, defaults=defaults, base_dir=root_dir, index=index))
    return specs


def run_batch(
    specs: list[SecondOpinionBatchSpec],
    *,
    manifest_path: Path,
    raw_argv: list[str],
    args: argparse.Namespace,
    options: SecondOpinionBatchOptions,
) -> dict[str, Any]:
    """Run each manifest table and return the batch report."""
    table_reports: list[dict[str, Any]] = []
    failed_tables = 0
    input_rows = 0
    output_rows = 0
    for spec in specs:
        report: dict[str, Any] = {
            "id": spec.table_id,
            "features": str(spec.features),
            "scores": list(spec.scores),
            "out": str(spec.out),
            "audit_json": str(spec.audit_json) if spec.audit_json else None,
            "config": dict(spec.config),
        }
        try:
            audit = materialize(
                spec.features,
                list(spec.scores),
                spec.out,
                audit_json=spec.audit_json,
                run_provenance=build_run_provenance(
                    entrypoint=SCRIPT_PATH,
                    repo_root=REPO_ROOT,
                    argv=raw_argv,
                    args=args,
                    inputs={
                        "manifest": manifest_path,
                        "features": spec.features,
                        "scores": spec.scores,
                    },
                    outputs={"out": str(spec.out), "audit_json": str(spec.audit_json)},
                ),
                **spec.config,
            )
            report["status"] = "ok"
            report["summary"] = {
                "input_rows": audit["input_rows"],
                "output_rows": audit["output_rows"],
                "competitors": audit["competitors"],
            }
            input_rows += int(audit["input_rows"])
            output_rows += int(audit["output_rows"])
        except Exception as exc:  # pragma: no cover - exercised through main
            report["status"] = "error"
            report["error"] = str(exc)
            failed_tables += 1
            table_reports.append(report)
            if options.fail_fast:
                break
            continue
        table_reports.append(report)
    return {
        "schema": "second-opinion-materializer-batch-v1",
        "status": "failed" if failed_tables else "ok",
        "manifest": str(manifest_path),
        "summary": {
            "tables": len(table_reports),
            "failed_tables": failed_tables,
            "input_rows": input_rows,
            "output_rows": output_rows,
        },
        "tables": table_reports,
        "run_provenance": build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=args,
            inputs={"manifest": manifest_path},
            outputs={"report_json": args.report_json, "report_md": args.report_md},
        ),
    }


def write_markdown_report(path: Path, payload: dict[str, Any]) -> None:
    """Write a compact human-readable batch report."""
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Second-Opinion Materializer Batch Report",
        "",
        f"Status: **{payload['status']}**",
        "",
        "| Table | Status | Input rows | Output rows | Output |",
        "| --- | --- | ---: | ---: | --- |",
    ]
    for table in payload["tables"]:
        summary = table.get("summary", {})
        lines.append(
            "| {id} | {status} | {input_rows} | {output_rows} | `{out}` |".format(
                id=table["id"],
                status=table["status"],
                input_rows=summary.get("input_rows", "-"),
                output_rows=summary.get("output_rows", "-"),
                out=table["out"],
            )
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _table_spec(
    raw_table: dict[str, Any],
    *,
    defaults: dict[str, Any],
    base_dir: Path,
    index: int,
) -> SecondOpinionBatchSpec:
    unknown_defaults = set(defaults) - _CONFIG_FIELDS
    if unknown_defaults:
        raise ValueError(f"unknown default keys: {sorted(unknown_defaults)}")
    unknown_table = set(raw_table) - _CONFIG_FIELDS - _TABLE_FIELDS
    if unknown_table:
        raise ValueError(f"unknown keys in table entry {index}: {sorted(unknown_table)}")
    table_id = str(raw_table.get("id") or "").strip()
    if not table_id:
        raise ValueError(f"table entry {index} is missing non-empty id")
    scores = raw_table.get("scores")
    if not isinstance(scores, list) or not scores:
        raise ValueError(f"table entry {index} needs a non-empty scores array")
    config = {**defaults, **{key: raw_table[key] for key in raw_table if key in _CONFIG_FIELDS}}
    return SecondOpinionBatchSpec(
        table_id=table_id,
        features=_required_path(raw_table, "features", base_dir, index=index),
        scores=[_resolve_score_spec(value, base_dir) for value in scores],
        out=_required_path(raw_table, "out", base_dir, index=index),
        audit_json=_optional_path(raw_table.get("audit_json"), base_dir),
        config=config,
    )


def _required_path(raw_table: dict[str, Any], key: str, base_dir: Path, *, index: int) -> Path:
    if key not in raw_table or raw_table[key] in (None, ""):
        raise ValueError(f"table entry {index} is missing {key}")
    return _resolve_path(raw_table[key], base_dir)


def _optional_path(value: Any, base_dir: Path) -> Path | None:
    if value in (None, ""):
        return None
    return _resolve_path(value, base_dir)


def _resolve_path(value: Any, base_dir: Path) -> Path:
    path = Path(str(value))
    return path if path.is_absolute() else base_dir / path


def _resolve_score_spec(value: Any, base_dir: Path) -> str:
    text = str(value)
    if "=" in text:
        label, raw_path = text.split("=", 1)
        return f"{label}={_resolve_path(raw_path, base_dir)}"
    return str(_resolve_path(text, base_dir))


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = make_argument_parser(description=__doc__)
    add_batch_manifest_arguments(parser)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    args = _parse_args(raw_argv)
    try:
        specs = load_batch_manifest(args.manifest, base_dir=args.base_dir)
        payload = run_batch(
            specs,
            manifest_path=args.manifest,
            raw_argv=raw_argv,
            args=args,
            options=SecondOpinionBatchOptions(fail_fast=args.fail_fast),
        )
    except ValueError as exc:
        print(f"second-opinion batch manifest error: {exc}", file=sys.stderr)
        return 2
    if args.report_json is not None:
        write_manifest_json(args.report_json, payload)
    if args.report_md is not None:
        write_markdown_report(args.report_md, payload)
    summary = payload["summary"]
    print(
        "second-opinion batch materialize: "
        f"tables={summary['tables']} input_rows={summary['input_rows']} "
        f"output_rows={summary['output_rows']} failed_tables={summary['failed_tables']}",
        file=sys.stderr,
    )
    return 0 if payload["status"] == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())

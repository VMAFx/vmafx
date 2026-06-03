#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
"""Run explicit MOS-label materialization for multiple feature tables."""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from _script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_repo_root=True, include_ai_scripts=True)
from materialize_mos_labels import REPO_ROOT, materialize  # noqa: E402

from aiutils.cli_helpers import (  # noqa: E402
    add_batch_manifest_arguments,
    collect_cli_argv,
    make_argument_parser,
)
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

SCRIPT_PATH = _SCRIPT_PATHS.script_path
_CONFIG_FIELDS = {
    "feature_key_column",
    "label_key_column",
    "label_mos_column",
    "key_normalize",
    "feature_key_regex",
    "label_key_regex",
    "min_match_rate",
    "status_column",
    "overwrite",
}
_TABLE_FIELDS = {"id", "features", "labels", "out", "audit_json"}


@dataclass(frozen=True)
class MosBatchSpec:
    """Resolved table entry from a MOS-label batch manifest."""

    table_id: str
    features: Path
    labels: list[Path]
    out: Path
    audit_json: Path | None
    config: dict[str, Any]


@dataclass(frozen=True)
class MosBatchOptions:
    """Runtime policy for the batch run."""

    fail_fast: bool = False


def load_batch_manifest(path: Path, *, base_dir: Path | None = None) -> list[MosBatchSpec]:
    """Load and validate a MOS-label materialization batch manifest."""
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
    specs: list[MosBatchSpec] = []
    for index, raw_table in enumerate(tables):
        if not isinstance(raw_table, dict):
            raise ValueError(f"table entry {index} must be an object")
        specs.append(_table_spec(raw_table, defaults=defaults, base_dir=root_dir, index=index))
    return specs


def run_batch(
    specs: list[MosBatchSpec],
    *,
    manifest_path: Path,
    raw_argv: list[str],
    args: argparse.Namespace,
    options: MosBatchOptions,
) -> dict[str, Any]:
    """Run each manifest table and return the batch report."""
    table_reports: list[dict[str, Any]] = []
    failed_tables = 0
    input_rows = 0
    output_rows = 0
    matched_rows = 0
    missing_rows = 0
    for spec in specs:
        report: dict[str, Any] = {
            "id": spec.table_id,
            "features": str(spec.features),
            "labels": [str(path) for path in spec.labels],
            "out": str(spec.out),
            "audit_json": str(spec.audit_json) if spec.audit_json else None,
            "config": dict(spec.config),
        }
        try:
            audit = materialize(
                spec.features,
                list(spec.labels),
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
                        "labels": spec.labels,
                    },
                    outputs={
                        "out": str(spec.out),
                        "audit_json": str(spec.audit_json),
                    },
                ),
                **spec.config,
            )
            report["status"] = "ok"
            report["summary"] = {
                "input_rows": audit["input_rows"],
                "output_rows": audit["output_rows"],
                "matched_rows": audit["matched_rows"],
                "missing_rows": audit["missing_rows"],
                "match_rate": audit["match_rate"],
                "labels_loaded": audit["labels_loaded"],
            }
            input_rows += int(audit["input_rows"])
            output_rows += int(audit["output_rows"])
            matched_rows += int(audit["matched_rows"])
            missing_rows += int(audit["missing_rows"])
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
        "schema": "mos-label-materializer-batch-v1",
        "status": "failed" if failed_tables else "ok",
        "manifest": str(manifest_path),
        "summary": {
            "tables": len(table_reports),
            "failed_tables": failed_tables,
            "input_rows": input_rows,
            "output_rows": output_rows,
            "matched_rows": matched_rows,
            "missing_rows": missing_rows,
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
        "# MOS Label Materializer Batch Report",
        "",
        f"Status: **{payload['status']}**",
        "",
        "| Table | Status | Match rate | Matched rows | Missing rows | Output |",
        "| --- | --- | ---: | ---: | ---: | --- |",
    ]
    for table in payload["tables"]:
        summary = table.get("summary", {})
        match_rate = summary.get("match_rate")
        match_text = "-" if match_rate is None else f"{float(match_rate):.3f}"
        lines.append(
            "| {id} | {status} | {match_rate} | {matched_rows} | {missing_rows} | `{out}` |".format(
                id=table["id"],
                status=table["status"],
                match_rate=match_text,
                matched_rows=summary.get("matched_rows", "-"),
                missing_rows=summary.get("missing_rows", "-"),
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
) -> MosBatchSpec:
    unknown_defaults = set(defaults) - _CONFIG_FIELDS
    if unknown_defaults:
        raise ValueError(f"unknown default keys: {sorted(unknown_defaults)}")
    unknown_table = set(raw_table) - _CONFIG_FIELDS - _TABLE_FIELDS
    if unknown_table:
        raise ValueError(f"unknown keys in table entry {index}: {sorted(unknown_table)}")
    table_id = str(raw_table.get("id") or "").strip()
    if not table_id:
        raise ValueError(f"table entry {index} is missing non-empty id")
    labels = raw_table.get("labels")
    if not isinstance(labels, list) or not labels:
        raise ValueError(f"table entry {index} needs a non-empty labels array")
    config = {**defaults, **{key: raw_table[key] for key in raw_table if key in _CONFIG_FIELDS}}
    return MosBatchSpec(
        table_id=table_id,
        features=_required_path(raw_table, "features", base_dir, index=index),
        labels=[_resolve_path(value, base_dir) for value in labels],
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
            options=MosBatchOptions(fail_fast=args.fail_fast),
        )
    except ValueError as exc:
        print(f"MOS-label batch manifest error: {exc}", file=sys.stderr)
        return 2
    if args.report_json is not None:
        write_manifest_json(args.report_json, payload)
    if args.report_md is not None:
        write_markdown_report(args.report_md, payload)
    summary = payload["summary"]
    print(
        "MOS-label batch materialize: "
        f"tables={summary['tables']} input_rows={summary['input_rows']} "
        f"matched_rows={summary['matched_rows']} failed_tables={summary['failed_tables']}",
        file=sys.stderr,
    )
    return 0 if payload["status"] == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())

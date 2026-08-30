#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Run saliency feature materialization for multiple tables from a manifest."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import asdict, dataclass, fields
from pathlib import Path
from typing import Any

from _script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(
    __file__,
    include_repo_root=True,
    include_ai_scripts=True,
    include_vmaf_tune_src=True,
)
from materialize_saliency_features import (  # noqa: E402
    REPO_ROOT,
    SaliencyFn,
    SaliencyMaterializeConfig,
    SubprocessRunner,
    materialize_rows,
    read_table,
    write_table,
)

from aiutils.cli_helpers import (  # noqa: E402
    add_batch_manifest_arguments,
    collect_cli_argv,
    make_argument_parser,
)
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

SCRIPT_PATH = _SCRIPT_PATHS.script_path
_CONFIG_FIELDS = {field.name for field in fields(SaliencyMaterializeConfig)}
_TABLE_FIELDS = {"id", "input", "output", "audit_json"}
_PATH_CONFIG_FIELDS = {"root", "model_path"}


@dataclass(frozen=True)
class BatchTableSpec:
    """Resolved table entry from a saliency batch manifest."""

    table_id: str
    input: Path
    output: Path
    audit_json: Path | None
    config: SaliencyMaterializeConfig


@dataclass(frozen=True)
class BatchRunOptions:
    """Runtime policy for a batch materialization invocation."""

    allow_row_failures: bool = False
    fail_fast: bool = False


def load_batch_manifest(path: Path, *, base_dir: Path | None = None) -> list[BatchTableSpec]:
    """Load and validate a saliency materialization batch manifest."""
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
    specs: list[BatchTableSpec] = []
    for index, raw_table in enumerate(tables):
        if not isinstance(raw_table, dict):
            raise ValueError(f"table entry {index} must be an object")
        specs.append(_table_spec(raw_table, defaults=defaults, base_dir=root_dir, index=index))
    return specs


def run_batch(
    specs: list[BatchTableSpec],
    *,
    manifest_path: Path,
    raw_argv: list[str],
    args: argparse.Namespace,
    options: BatchRunOptions,
    runner: SubprocessRunner = subprocess.run,
    saliency_fn: SaliencyFn | None = None,
) -> dict[str, Any]:
    """Run every table spec and return the batch report payload."""
    table_reports: list[dict[str, Any]] = []
    total_rows = 0
    ok_rows = 0
    skipped_rows = 0
    failed_rows = 0
    failed_tables = 0
    for spec in specs:
        report: dict[str, Any] = {
            "id": spec.table_id,
            "input": str(spec.input),
            "output": str(spec.output),
            "audit_json": str(spec.audit_json) if spec.audit_json else None,
            "config": _config_payload(spec.config),
        }
        try:
            rows = read_table(spec.input)
            enriched, summary = materialize_rows(
                rows,
                spec.config,
                runner=runner,
                saliency_fn=saliency_fn,
            )
            write_table(spec.output, enriched)
            _write_table_audit(spec, summary=asdict(summary), manifest_path=manifest_path)
            report["summary"] = asdict(summary)
            if summary.failed:
                report["status"] = "row-failures"
                failed_tables += 1
            else:
                report["status"] = "ok"
            total_rows += summary.total
            ok_rows += summary.ok
            skipped_rows += summary.skipped_existing
            failed_rows += summary.failed
        except Exception as exc:  # pragma: no cover - exercised through main
            report["status"] = "error"
            report["error"] = str(exc)
            failed_tables += 1
            table_reports.append(report)
            if options.fail_fast:
                break
            continue
        table_reports.append(report)
        if options.fail_fast and report["status"] != "ok":
            break
    batch_status = "ok"
    if failed_tables:
        batch_status = "row-failures" if failed_rows and options.allow_row_failures else "failed"
    payload = {
        "schema": "saliency-materializer-batch-v1",
        "status": batch_status,
        "manifest": str(manifest_path),
        "summary": {
            "tables": len(table_reports),
            "failed_tables": failed_tables,
            "total": total_rows,
            "ok": ok_rows,
            "skipped_existing": skipped_rows,
            "failed": failed_rows,
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
    return payload


def write_markdown_report(path: Path, payload: dict[str, Any]) -> None:
    """Write a compact human-readable batch report."""
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Saliency Materializer Batch Report",
        "",
        f"Status: **{payload['status']}**",
        "",
        "| Table | Status | Total | OK | Skipped | Failed | Output |",
        "| --- | --- | ---: | ---: | ---: | ---: | --- |",
    ]
    for table in payload["tables"]:
        summary = table.get("summary", {})
        lines.append(
            "| {id} | {status} | {total} | {ok} | {skipped} | {failed} | `{output}` |".format(
                id=table["id"],
                status=table["status"],
                total=summary.get("total", "-"),
                ok=summary.get("ok", "-"),
                skipped=summary.get("skipped_existing", "-"),
                failed=summary.get("failed", "-"),
                output=table["output"],
            )
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _table_spec(
    raw_table: dict[str, Any],
    *,
    defaults: dict[str, Any],
    base_dir: Path,
    index: int,
) -> BatchTableSpec:
    unknown_defaults = set(defaults) - _CONFIG_FIELDS
    if unknown_defaults:
        raise ValueError(f"unknown default keys: {sorted(unknown_defaults)}")
    unknown_table = set(raw_table) - _CONFIG_FIELDS - _TABLE_FIELDS
    if unknown_table:
        raise ValueError(f"unknown keys in table entry {index}: {sorted(unknown_table)}")
    table_id = str(raw_table.get("id") or "").strip()
    if not table_id:
        raise ValueError(f"table entry {index} is missing non-empty id")
    input_path = _required_path(raw_table, "input", base_dir, index=index)
    output_path = _required_path(raw_table, "output", base_dir, index=index)
    audit_json = _optional_path(raw_table.get("audit_json"), base_dir)
    config_values = {
        **defaults,
        **{key: raw_table[key] for key in raw_table if key in _CONFIG_FIELDS},
    }
    for key in _PATH_CONFIG_FIELDS:
        if key in config_values:
            config_values[key] = _optional_path(config_values[key], base_dir)
    return BatchTableSpec(
        table_id=table_id,
        input=input_path,
        output=output_path,
        audit_json=audit_json,
        config=SaliencyMaterializeConfig(**config_values),
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


def _config_payload(config: SaliencyMaterializeConfig) -> dict[str, Any]:
    payload = asdict(config)
    for key in _PATH_CONFIG_FIELDS:
        if payload[key] is not None:
            payload[key] = str(payload[key])
    return payload


def _write_table_audit(
    spec: BatchTableSpec,
    *,
    summary: dict[str, Any],
    manifest_path: Path,
) -> None:
    if spec.audit_json is None:
        return
    write_manifest_json(
        spec.audit_json,
        {
            "schema": "saliency-materializer-audit-v1",
            "batch_manifest": str(manifest_path),
            "table_id": spec.table_id,
            "input": str(spec.input),
            "output": str(spec.output),
            "config": _config_payload(spec.config),
            "summary": summary,
        },
    )


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = make_argument_parser(description=__doc__)
    add_batch_manifest_arguments(parser, allow_row_failures=True)
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
            options=BatchRunOptions(
                allow_row_failures=args.allow_row_failures,
                fail_fast=args.fail_fast,
            ),
        )
    except ValueError as exc:
        print(f"saliency batch manifest error: {exc}", file=sys.stderr)
        return 2
    if args.report_json is not None:
        write_manifest_json(args.report_json, payload)
    if args.report_md is not None:
        write_markdown_report(args.report_md, payload)
    summary = payload["summary"]
    print(
        "saliency batch materialize: "
        f"tables={summary['tables']} total={summary['total']} ok={summary['ok']} "
        f"skipped={summary['skipped_existing']} failed={summary['failed']}",
        file=sys.stderr,
    )
    if payload["status"] == "ok" or (
        payload["status"] == "row-failures" and args.allow_row_failures
    ):
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(main())

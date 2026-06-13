#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Merge two or more vmaf-tune corpus JSONL files into one.

The vmaf-tune corpus schema is the row contract emitted by
``tools/vmaf-tune/src/vmaftune/corpus.py`` (Phase A, ADR-0237). It is
also the input contract consumed by
``ai/scripts/train_fr_regressor_v2.py``. When mixing the Netflix Public
drop (``.workingdir2/netflix/``) with BVI-DVC (ADR-0310), this utility
concatenates row streams, validates each row carries the canonical
:data:`vmaftune.CORPUS_ROW_KEYS`, and de-duplicates by ``src_sha256`` —
the corpus uses a content hash of the source YUV as the natural key
across mirrors and re-encodes.

Usage::

    python ai/scripts/merge_corpora.py \\
        --inputs runs/netflix_corpus.jsonl runs/bvi_dvc_corpus.jsonl \\
        --output runs/fr_v2_train_corpus.jsonl

Exit codes:
  0 — merged successfully (summary on stderr)
  1 — at least one row failed schema validation
  2 — input file missing or unreadable
"""

from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Iterable
from pathlib import Path
from typing import Any

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_vmaf_tune_src=True)
_REPO_ROOT = _SCRIPT_PATHS.repo_root

from vmaftune import CORPUS_ROW_KEYS  # noqa: E402

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.jsonl_utils import iter_jsonl  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

_REQUIRED_KEYS: frozenset[str] = frozenset(CORPUS_ROW_KEYS)


def _validate_row(path: Path, line_no: int, row: dict) -> None:
    """Assert ``row`` carries every key in :data:`CORPUS_ROW_KEYS`.

    Hard-fails the run on first violation. The training pipeline cannot
    silently drop schema-bad rows: a missing ``src_sha256`` or
    ``vmaf_score`` would corrupt the dedup pass or the regression
    target.
    """
    if not isinstance(row, dict):
        print(
            f"error: {path}:{line_no}: expected JSON object, got {type(row).__name__}",
            file=sys.stderr,
        )
        raise SystemExit(1)
    missing = _REQUIRED_KEYS - row.keys()
    if missing:
        print(
            f"error: {path}:{line_no}: missing required keys: " f"{sorted(missing)}",
            file=sys.stderr,
        )
        raise SystemExit(1)


def _dedup_key(row: dict) -> tuple[str, str, str | int, str | int]:
    """Compose a per-encode identity tuple.

    A ``src_sha256`` collision alone is not a duplicate — the same
    source can legitimately appear under multiple ``(encoder, preset,
    crf)`` triples. Treat the four-tuple as the natural key.
    """
    return (
        str(row.get("src_sha256", "")),
        str(row.get("encoder", "")),
        row.get("preset", ""),
        row.get("crf", ""),
    )


def merge(inputs: Iterable[Path], output: Path) -> tuple[int, int, int, int]:
    """Stream-merge ``inputs`` into ``output``; return summary counters.

    Returns ``(rows_in, rows_out, duplicates, unique_sources)``.
    """
    seen: set[tuple[str, str, str | int, str | int]] = set()
    unique_sources: set[str] = set()
    rows_in = 0
    rows_out = 0
    duplicates = 0

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8") as out_fp:
        for path in inputs:
            if not path.is_file():
                print(f"error: input not found: {path}", file=sys.stderr)
                raise SystemExit(2)
            for line_no, row in iter_jsonl(path):
                rows_in += 1
                _validate_row(path, line_no, row)
                key = _dedup_key(row)
                if key in seen:
                    duplicates += 1
                    continue
                seen.add(key)
                src = row.get("src_sha256", "")
                if isinstance(src, str) and src:
                    unique_sources.add(src)
                out_fp.write(json.dumps(row, sort_keys=True) + "\n")
                rows_out += 1
    return rows_in, rows_out, duplicates, len(unique_sources)


def _write_manifest(
    *,
    manifest_out: Path,
    inputs: list[Path],
    output: Path,
    args: argparse.Namespace,
    argv: list[str],
    summary: dict[str, Any],
) -> None:
    """Write a replay manifest for the merged corpus JSONL."""
    write_manifest_json(
        manifest_out,
        {
            "schema": "vmaf-tune-corpus-merge-manifest-v1",
            "required_keys": sorted(_REQUIRED_KEYS),
            "dedup_key": ["src_sha256", "encoder", "preset", "crf"],
            "summary": summary,
            "run_provenance": build_run_provenance(
                entrypoint=Path(__file__),
                repo_root=_REPO_ROOT,
                argv=argv,
                args=args,
                inputs={"corpus_jsonl": inputs},
                outputs={"jsonl": output, "manifest": manifest_out},
            ),
        },
    )


def main(argv: list[str] | None = None) -> int:
    parsed_argv = collect_cli_argv(argv)
    ap = make_argument_parser(prog="merge_corpora.py", description=__doc__)
    ap.add_argument(
        "--inputs",
        nargs="+",
        required=True,
        type=Path,
        help="Two or more vmaf-tune corpus JSONL files.",
    )
    ap.add_argument(
        "--output",
        required=True,
        type=Path,
        help="Destination JSONL.",
    )
    ap.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help=(
            "Run-provenance JSON sidecar. Defaults to <output>.manifest.json and "
            "records the input shards, schema keys, dedup counters, and exact "
            "CLI args used to build the merged corpus JSONL."
        ),
    )
    args = ap.parse_args(parsed_argv)

    if len(args.inputs) < 2:
        print(
            "error: --inputs requires at least 2 paths to be a meaningful merge",
            file=sys.stderr,
        )
        return 2

    if args.manifest_out is None:
        args.manifest_out = args.output.with_suffix(".manifest.json")

    rows_in, rows_out, dupes, sources = merge(args.inputs, args.output)
    summary = {
        "rows_in": rows_in,
        "rows_out": rows_out,
        "duplicates": dupes,
        "unique_sources": sources,
    }
    _write_manifest(
        manifest_out=args.manifest_out,
        inputs=args.inputs,
        output=args.output,
        args=args,
        argv=parsed_argv,
        summary=summary,
    )
    print(
        f"[merge_corpora] rows_in={rows_in} rows_out={rows_out} "
        f"duplicates={dupes} unique_sources={sources} manifest={args.manifest_out} "
        f"-> {args.output}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())

#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Add CHUG/K150K side metadata to an existing FULL_FEATURES parquet.

This salvages long-running feature-extraction jobs that were started
without ``extract_k150k_features.py --metadata-jsonl``. The feature rows
are matched by ``clip_name`` to the basename of each JSONL row's ``src``
or ``filename`` field, then CHUG content identity, raw MOS, ladder, and
content-level split columns are filled into the parquet.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pandas as pd

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_ai_scripts=True)
REPO_ROOT = _SCRIPT_PATHS.repo_root
from extract_k150k_features import DEFAULT_CHUG_SPLIT_SEED, _load_jsonl_metadata  # noqa: E402

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.parquet_utils import write_parquet_atomic  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402


def _is_missing(value: Any) -> bool:
    return bool(pd.isna(value))


def enrich_frame(
    frame: pd.DataFrame,
    metadata: dict[str, dict[str, Any]],
    *,
    overwrite: bool = False,
) -> tuple[pd.DataFrame, dict[str, int]]:
    """Return ``frame`` with metadata columns filled by ``clip_name``."""
    if "clip_name" not in frame.columns:
        raise ValueError("input parquet must contain a clip_name column")

    out = frame.copy()
    metadata_keys = sorted({key for meta in metadata.values() for key in meta})
    for key in metadata_keys:
        if key not in out.columns:
            out[key] = pd.NA

    matched = 0
    updated = 0
    missing = 0
    for idx, clip_name in out["clip_name"].items():
        meta = metadata.get(str(clip_name))
        if not meta:
            missing += 1
            continue
        matched += 1
        for key, value in meta.items():
            old = out.at[idx, key]
            if overwrite or _is_missing(old):
                out.at[idx, key] = value
                updated += 1

    return out, {
        "rows": len(out),
        "metadata_rows": len(metadata),
        "matched_rows": matched,
        "missing_rows": missing,
        "updated_cells": updated,
    }


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    ap = make_argument_parser(
        prog="enrich_k150k_parquet_metadata.py",
        description=__doc__,
    )
    ap.add_argument(
        "--features-parquet",
        type=Path,
        required=True,
        help="Existing FULL_FEATURES parquet to enrich.",
    )
    ap.add_argument(
        "--metadata-jsonl",
        type=Path,
        required=True,
        help="Corpus JSONL sidecar, e.g. .workingdir2/chug/chug.jsonl.",
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Output parquet path. Defaults to in-place rewrite of --features-parquet.",
    )
    ap.add_argument(
        "--split-seed",
        default=DEFAULT_CHUG_SPLIT_SEED,
        help="Seed for deterministic CHUG content-level splits.",
    )
    ap.add_argument(
        "--overwrite-metadata",
        action="store_true",
        help="Overwrite existing metadata cells instead of filling only missing values.",
    )
    ap.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help=(
            "Run-provenance JSON sidecar. Defaults to <out>.manifest.json and "
            "records the input parquet, sidecar, enrichment counters, and exact "
            "CLI args used to build the derived parquet."
        ),
    )
    args = ap.parse_args(raw_argv)

    if not args.features_parquet.is_file():
        raise SystemExit(f"error: features parquet not found: {args.features_parquet}")
    if not args.metadata_jsonl.is_file():
        raise SystemExit(f"error: metadata JSONL not found: {args.metadata_jsonl}")

    metadata = _load_jsonl_metadata(args.metadata_jsonl, split_seed=args.split_seed)
    frame = pd.read_parquet(args.features_parquet)
    enriched, stats = enrich_frame(
        frame,
        metadata,
        overwrite=args.overwrite_metadata,
    )
    out_path = args.out or args.features_parquet
    if args.manifest_out is None:
        args.manifest_out = out_path.with_suffix(".manifest.json")
    write_parquet_atomic(enriched, out_path, index=False)
    write_manifest_json(
        args.manifest_out,
        {
            "schema": "k150k-metadata-enrichment-manifest-v1",
            "stats": stats,
            "metadata_keys": sorted({key for meta in metadata.values() for key in meta}),
            "overwrite_metadata": bool(args.overwrite_metadata),
            "run_provenance": build_run_provenance(
                entrypoint=Path(__file__),
                repo_root=REPO_ROOT,
                argv=raw_argv,
                args=args,
                inputs={
                    "features_parquet": args.features_parquet,
                    "metadata_jsonl": args.metadata_jsonl,
                },
                outputs={"parquet": out_path, "manifest": args.manifest_out},
            ),
        },
    )
    print(
        json.dumps(
            {"out": str(out_path), "manifest": str(args.manifest_out), **stats}, sort_keys=True
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

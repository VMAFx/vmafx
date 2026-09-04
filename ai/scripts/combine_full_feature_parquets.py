#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Combine refreshed per-corpus FULL_FEATURES parquet shards.

The tiny-model training chain uses aggregate parquet files such as
``runs/full_features_4corpus.parquet`` and
``runs/full_features_5corpus_with_hw.parquet``. Those aggregates are
derived data, so this script makes the rebuild explicit and repeatable:

    python ai/scripts/combine_full_feature_parquets.py \\
        --input netflix=runs/full_features_netflix_refresh_20260520.parquet \\
        --input konvid=runs/full_features_konvid_refresh_20260520.parquet \\
        --input bvi=runs/full_features_bvi_dvc_D_refresh_20260520.parquet \\
        --out runs/full_features_refresh_20260520.parquet

Each input is normalized to:

    corpus, source, frame_index, codec, <FULL_FEATURES>, vmaf
"""

from __future__ import annotations

import argparse
from pathlib import Path

import pandas as pd

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_repo_root=True)
REPO_ROOT = _SCRIPT_PATHS.repo_root
from ai.data.feature_extractor import FULL_FEATURES  # noqa: E402

# isort: split
from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

OUTPUT_COLUMNS: tuple[str, ...] = (
    "corpus",
    "source",
    "frame_index",
    "codec",
    "teacher_model",
    *FULL_FEATURES,
    "vmaf",
)


def _parse_input(spec: str) -> tuple[str, Path]:
    if "=" not in spec:
        raise argparse.ArgumentTypeError("expected LABEL=PATH")
    label, path_text = spec.split("=", 1)
    label = label.strip()
    if not label:
        raise argparse.ArgumentTypeError("input label must not be empty")
    path = Path(path_text)
    return label, path


def _normalise_frame_index(series: pd.Series, n_rows: int) -> pd.Series:
    if series.empty:
        return pd.Series(range(n_rows), dtype="int64")
    return series.fillna(0).astype("int64")


def _normalise_shard(
    label: str,
    path: Path,
    max_rows: int | None = None,
    assume_teacher: str | None = None,
) -> pd.DataFrame:
    if not path.is_file():
        raise FileNotFoundError(path)
    df = pd.read_parquet(path)
    if max_rows is not None:
        df = df.head(max_rows)

    out = pd.DataFrame(index=df.index)
    out["corpus"] = label
    if "source" in df.columns:
        out["source"] = df["source"].astype(str)
    elif "key" in df.columns:
        out["source"] = df["key"].astype(str)
    elif "dis_basename" in df.columns:
        out["source"] = df["dis_basename"].astype(str)
    else:
        out["source"] = label

    if "frame_index" in df.columns:
        out["frame_index"] = _normalise_frame_index(df["frame_index"], len(df))
    else:
        out["frame_index"] = pd.Series(range(len(df)), index=df.index, dtype="int64")

    out["codec"] = df["codec"].astype(str) if "codec" in df.columns else "unknown"

    if "teacher_model" in df.columns:
        distinct = [str(x) for x in df["teacher_model"].dropna().unique()]
        if len(distinct) > 1:
            raise ValueError(f"{path}: mixed teacher_model values in shard: {distinct}")
        if len(distinct) == 0:
            if not assume_teacher:
                raise ValueError(
                    f"{path}: empty 'teacher_model' column; pass --assume-teacher <name>"
                )
            shard_teacher = assume_teacher
        else:
            shard_teacher = distinct[0]
            if assume_teacher is not None and assume_teacher != shard_teacher:
                raise ValueError(
                    f"{path}: shard teacher_model '{shard_teacher}' conflicts with --assume-teacher '{assume_teacher}'"
                )
    else:
        if assume_teacher is None:
            raise ValueError(
                f"{path}: missing required 'teacher_model' column; pass --assume-teacher <name> to ingest legacy tables"
            )
        shard_teacher = assume_teacher
    out["teacher_model"] = shard_teacher

    missing = [feature for feature in FULL_FEATURES if feature not in df.columns]
    for feature in FULL_FEATURES:
        out[feature] = df[feature] if feature in df.columns else float("nan")
    if "vmaf" not in df.columns:
        raise ValueError(f"{path}: missing required 'vmaf' column")
    out["vmaf"] = df["vmaf"]
    out = out[list(OUTPUT_COLUMNS)]
    out.attrs["missing_features"] = missing
    out.attrs["teacher_model"] = shard_teacher
    return out


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    parser = make_argument_parser(
        prog="combine_full_feature_parquets.py",
        description=__doc__,
    )
    parser.add_argument(
        "--input",
        dest="inputs",
        action="append",
        type=_parse_input,
        required=True,
        help="Input shard as LABEL=PATH. Repeat for each corpus shard.",
    )
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--max-rows-per-input",
        type=int,
        default=None,
        help="Smoke-test cap applied independently to each shard.",
    )
    parser.add_argument(
        "--assume-teacher",
        default=None,
        help="Teacher model name to assume when ingesting legacy tables lacking a 'teacher_model' column.",
    )
    parser.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help=(
            "Run-provenance JSON sidecar. Defaults to <out>.manifest.json and "
            "records the input shards, normalized columns, filled feature gaps, "
            "and exact CLI args used to build the derived parquet."
        ),
    )
    args = parser.parse_args(raw_argv)
    if args.manifest_out is None:
        args.manifest_out = args.out.with_suffix(".manifest.json")

    shards: list[pd.DataFrame] = []
    input_stats: list[dict[str, object]] = []
    for label, path in args.inputs:
        shard = _normalise_shard(
            label,
            path,
            args.max_rows_per_input,
            assume_teacher=args.assume_teacher,
        )
        missing = list(shard.attrs.get("missing_features", []))
        print(
            f"[combine-full] {label}: {len(shard)} rows from {path}"
            + (f" (missing features filled NaN: {missing})" if missing else ""),
            flush=True,
        )
        input_stats.append(
            {
                "label": label,
                "path": str(path),
                "rows": len(shard),
                "teacher_model": shard.attrs.get("teacher_model"),
                "missing_features": missing,
            }
        )
        shards.append(shard)

    teachers = {shard.attrs["teacher_model"] for shard in shards if not shard.empty}
    if len(teachers) > 1:
        raise ValueError(
            f"Cannot combine shards with conflicting teacher models: {sorted(teachers)}"
        )
    combined_teacher = next(iter(teachers)) if teachers else (args.assume_teacher or "unknown")

    combined = pd.concat(shards, ignore_index=True, sort=False)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    combined.to_parquet(args.out, index=False)
    corpora = {str(key): int(value) for key, value in combined["corpus"].value_counts().items()}
    write_manifest_json(
        args.manifest_out,
        {
            "schema": "full-feature-parquet-combine-manifest-v1",
            "teacher_model": combined_teacher,
            "stats": {
                "input_count": len(args.inputs),
                "output_rows": len(combined),
                "corpora": corpora,
                "teacher_model": combined_teacher,
                "max_rows_per_input": args.max_rows_per_input,
            },
            "inputs": input_stats,
            "output_columns": list(OUTPUT_COLUMNS),
            "run_provenance": build_run_provenance(
                entrypoint=Path(__file__),
                repo_root=REPO_ROOT,
                argv=raw_argv,
                args=args,
                inputs={"shards": [path for _, path in args.inputs]},
                outputs={"parquet": args.out, "manifest": args.manifest_out},
            ),
        },
    )
    print(
        f"[combine-full] wrote {args.out}: rows={len(combined)} "
        f"corpora={corpora} manifest={args.manifest_out}",
        flush=True,
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())

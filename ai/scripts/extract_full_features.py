#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Extract the FULL_FEATURES (Research-0026) over the Netflix corpus.

Phase 1 (PR #185) added ``FULL_FEATURES`` to
``ai/data/feature_extractor.py``. This script is the Phase 2
re-extraction driver that produces the parquet feeding the
correlation / mutual-information / feature-importance analysis.

The output schema is one row per (pair, frame):

  source           : str   (e.g. "BigBuckBunny")
  dis_basename     : str   (e.g. "BigBuckBunny_30_384_550.yuv")
  frame_index      : int   (0-based per-pair frame number)
  codec            : str   (encoder family; ``"unknown"`` for this corpus)
  vmaf             : float (vmaf_v0.6.1 teacher score)
  <26 feature columns from FULL_FEATURES>

(ADR-0559 extended FULL_FEATURES from 22 to 26 columns by appending
speed_temporal and speed_chroma_u/v/uv.  The speed_* extractors are CPU-only;
GPU twins are tracked in ADR-0557 and ADR-0558.)

The Netflix Public corpus ships pre-encoded distorted YUVs with no
in-band codec metadata, so the ``codec`` column defaults to ``"unknown"``
— this is the documented limitation cited in
[ADR-0235](../../docs/adr/0235-codec-aware-fr-regressor.md). Override
via ``--codec`` when re-extracting against a manifest that does carry
encoder labels.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

import numpy as np

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_repo_root=True)
REPO_ROOT = _SCRIPT_PATHS.repo_root

from ai.data.feature_extractor import (  # noqa: E402
    DEFAULT_VMAF_BINARY,
    FULL_FEATURES,
    extract_features,
)
from ai.data.netflix_loader import iter_pairs  # noqa: E402
from ai.data.scores import teacher_scores  # noqa: E402

# isort: split
from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.file_utils import write_text_atomic  # noqa: E402
from aiutils.parquet_utils import write_parquet_atomic  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402


def _per_clip_cache_path(root: Path, source: str, dis_stem: str) -> Path:
    return root / source / f"{dis_stem}.json"


def _load_or_compute(
    pair,
    cache_dir: Path,
    vmaf_binary: Path,
) -> dict:
    src = pair.source
    stem = pair.dis_path.stem
    cache_path = _per_clip_cache_path(cache_dir, src, stem)
    if cache_path.is_file():
        return json.loads(cache_path.read_text())

    feats = extract_features(
        pair.ref_path,
        pair.dis_path,
        pair.width,
        pair.height,
        features=FULL_FEATURES,
        vmaf_binary=vmaf_binary,
    )
    teacher = teacher_scores(
        pair.ref_path,
        pair.dis_path,
        pair.width,
        pair.height,
        vmaf_binary=vmaf_binary,
    )
    payload = {
        "feature_names": list(feats.feature_names),
        "per_frame": feats.per_frame.tolist(),
        "teacher_per_frame": teacher.per_frame.tolist(),
    }
    write_text_atomic(cache_path, json.dumps(payload))
    return payload


def _write_manifest(
    path: Path,
    *,
    args: argparse.Namespace,
    argv: list[str] | None,
    pairs_count: int,
    rows_count: int,
    elapsed_s: float,
) -> None:
    write_manifest_json(
        path,
        {
            "schema": "netflix-full-features-manifest-v1",
            "features": list(FULL_FEATURES),
            "stats": {
                "pairs": pairs_count,
                "rows": rows_count,
                "elapsed_s": round(elapsed_s, 6),
            },
            "run_provenance": build_run_provenance(
                entrypoint=Path(__file__),
                repo_root=REPO_ROOT,
                argv=sys.argv[1:] if argv is None else argv,
                args=args,
                inputs={
                    "data_root": args.data_root,
                    "cache_dir": args.cache_dir,
                    "vmaf_bin": args.vmaf_bin,
                },
                outputs={"parquet": args.out, "manifest": args.manifest_out},
            ),
        },
    )


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    ap = make_argument_parser(
        prog="extract_full_features.py",
        description=__doc__,
    )
    ap.add_argument(
        "--data-root",
        type=Path,
        default=Path(
            os.environ.get(
                "VMAF_NETFLIX_CORPUS_DIR",
                str(Path(__file__).resolve().parents[2] / ".corpus" / "netflix"),
            )
        ),
        help=(
            "Netflix corpus root. Override with the ``VMAF_NETFLIX_CORPUS_DIR`` "
            "env var when running inside the dev-mcp container or other layouts."
        ),
    )
    ap.add_argument(
        "--cache-dir",
        type=Path,
        default=Path.home() / ".cache" / "vmaf-tiny-ai-full",
    )
    ap.add_argument(
        "--vmaf-bin",
        type=Path,
        default=DEFAULT_VMAF_BINARY,
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=Path("runs/full_features_netflix.parquet"),
    )
    ap.add_argument("--max-pairs", type=int, default=None)
    ap.add_argument(
        "--codec",
        type=str,
        default="unknown",
        help="Codec label baked into the parquet's `codec` column. The "
        "Netflix Public corpus ships pre-encoded distortions without "
        "in-band codec metadata, so the safe default is 'unknown' "
        "(bucketed via ai/src/vmaf_train/codec.py). Override when "
        "re-extracting against a manifest that does carry labels.",
    )
    ap.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help=(
            "Run-provenance JSON sidecar. Defaults to <out>.manifest.json and "
            "records feature names, row counts, corpus/cache inputs, and exact "
            "CLI args used to build the parquet."
        ),
    )
    args = ap.parse_args(raw_argv)
    if args.manifest_out is None:
        args.manifest_out = args.out.with_suffix(".manifest.json")

    if not args.vmaf_bin.is_file():
        print(f"error: vmaf binary not found at {args.vmaf_bin}", file=sys.stderr)
        return 2

    rows: list[dict] = []
    pairs = list(iter_pairs(args.data_root, max_pairs=args.max_pairs))
    print(f"[extract] {len(pairs)} pairs; FULL_FEATURES = {len(FULL_FEATURES)} features")
    t0 = time.time()
    for i, pair in enumerate(pairs):
        wt = time.time() - t0
        print(
            f"[extract] {i + 1}/{len(pairs)} {pair.source}/{pair.dis_path.name} (elapsed {wt:.0f}s)"
        )
        payload = _load_or_compute(pair, args.cache_dir, args.vmaf_bin)
        per_frame = np.asarray(payload["per_frame"], dtype=np.float32)
        teacher_per_frame = np.asarray(payload["teacher_per_frame"], dtype=np.float32)
        n = min(per_frame.shape[0], teacher_per_frame.shape[0])
        for fi in range(n):
            row = {
                "source": pair.source,
                "dis_basename": pair.dis_path.name,
                "frame_index": fi,
                "codec": args.codec,
                "vmaf": float(teacher_per_frame[fi]),
            }
            for col, val in zip(FULL_FEATURES, per_frame[fi], strict=False):
                row[col] = float(val)
            rows.append(row)

    import pandas as pd  # local import — pandas optional for non-Phase-2 paths

    df = pd.DataFrame(rows)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    write_parquet_atomic(df, args.out)
    elapsed_s = time.time() - t0
    _write_manifest(
        args.manifest_out,
        args=args,
        argv=raw_argv,
        pairs_count=len(pairs),
        rows_count=len(df),
        elapsed_s=elapsed_s,
    )
    print(
        f"[extract] wrote {args.out}: {len(df)} rows × {len(df.columns)} cols "
        f"in {elapsed_s:.0f}s wall; manifest {args.manifest_out}"
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())

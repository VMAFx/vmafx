#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""3-arch LOSO evaluation harness — extends ``eval_loso_mlp_small.py``.

Scores each architecture's 9 leave-one-source-out fold ONNX
checkpoints against their held-out clip, then aggregates per-arch
PLCC / SROCC / RMSE means. Architectures covered:

* ``mlp_small``  (257 params,  default tiny model)
* ``mlp_medium`` (2 561 params, larger absolute-fit variant)
* ``linear``     (7 params,    sanity-floor baseline)

Outputs:
  runs/loso_eval/loso_3arch_eval.json
  runs/loso_eval/loso_3arch_eval.md

Companion to PR #165 (mlp_small alone) — the 3-arch sweep is the
canonical Netflix-corpus comparison documented in ADR-0203.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

import numpy as np

SCRIPT_PATH = Path(__file__).resolve()
REPO_ROOT = SCRIPT_PATH.parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))
sys.path.insert(0, str(REPO_ROOT))

# Reuse the existing helpers — same constants, same load_session
# external-data workaround, same per-clip cache loader.
from ai.scripts.eval_loso_mlp_small import CLIPS, _eval, _load_clip, _load_session  # noqa: E402

ARCHS = ("mlp_small", "mlp_medium", "linear")


def _eval_arch(arch: str, loso_dir: Path, clip_xy: dict) -> dict:
    """Score @p arch's 9 fold ONNXs on their respective held-out clips."""
    per_fold: dict[str, dict[str, float]] = {}
    plccs, sroccs, rmses = [], [], []
    print(f"[eval] === arch={arch} ===", flush=True)
    for clip in CLIPS:
        path = loso_dir / f"fold_{clip}" / f"{arch}_final.onnx"
        if not path.is_file():
            print(f"[eval]   fold={clip:14s} MISSING ({path})", flush=True)
            continue
        sess = _load_session(path)
        x, y = clip_xy[clip]
        m = _eval(sess, x, y)
        per_fold[clip] = m
        plccs.append(m["plcc"])
        sroccs.append(m["srocc"])
        rmses.append(m["rmse"])
        print(
            f"[eval]   fold={clip:14s} n={m['n']:4d} "
            f"PLCC={m['plcc']:.4f} SROCC={m['srocc']:.4f} RMSE={m['rmse']:.3f}",
            flush=True,
        )
    if not plccs:
        return {"per_fold": per_fold, "aggregate": None}
    aggregate = {
        "mean_plcc": float(np.mean(plccs)),
        "mean_srocc": float(np.mean(sroccs)),
        "mean_rmse": float(np.mean(rmses)),
        "std_plcc": float(np.std(plccs, ddof=1)) if len(plccs) > 1 else 0.0,
        "std_srocc": float(np.std(sroccs, ddof=1)) if len(sroccs) > 1 else 0.0,
        "std_rmse": float(np.std(rmses, ddof=1)) if len(rmses) > 1 else 0.0,
    }
    print(
        f"[eval]   {arch:14s} mean PLCC={aggregate['mean_plcc']:.4f} "
        f"SROCC={aggregate['mean_srocc']:.4f} RMSE={aggregate['mean_rmse']:.3f} "
        f"(±{aggregate['std_plcc']:.4f} / ±{aggregate['std_srocc']:.4f} / ±{aggregate['std_rmse']:.3f})",
        flush=True,
    )
    return {"per_fold": per_fold, "aggregate": aggregate}


def _markdown(report: dict) -> str:
    lines = ["# 3-arch LOSO evaluation — Netflix corpus\n"]
    lines.append(f"Generated: {report['generated']}\n")
    lines.append(f"Corpus: `{report['corpus']}`\n")
    lines.append("\n## Aggregate (mean ± std across 9 folds)\n")
    lines.append("| arch | mean PLCC | mean SROCC | mean RMSE |")
    lines.append("|---|---:|---:|---:|")
    for arch in ARCHS:
        agg = report["archs"][arch].get("aggregate")
        if agg is None:
            lines.append(f"| {arch} | — | — | — |")
            continue
        lines.append(
            f"| {arch} | "
            f"{agg['mean_plcc']:.4f} ± {agg['std_plcc']:.4f} | "
            f"{agg['mean_srocc']:.4f} ± {agg['std_srocc']:.4f} | "
            f"{agg['mean_rmse']:.3f} ± {agg['std_rmse']:.3f} |"
        )
    lines.append("")
    for arch in ARCHS:
        per_fold = report["archs"][arch]["per_fold"]
        if not per_fold:
            continue
        lines.append(f"\n## {arch} per-fold\n")
        lines.append("| fold | n | PLCC | SROCC | RMSE |")
        lines.append("|---|---:|---:|---:|---:|")
        for clip in CLIPS:
            m = per_fold.get(clip)
            if m is None:
                lines.append(f"| {clip} | — | — | — | — |")
                continue
            lines.append(
                f"| {clip} | {m['n']} | " f"{m['plcc']:.4f} | {m['srocc']:.4f} | {m['rmse']:.3f} |"
            )
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--data-root",
        type=Path,
        default=Path(
            os.environ.get(
                "VMAF_NETFLIX_CORPUS_DIR",
                str(REPO_ROOT / ".corpus" / "netflix"),
            )
        ),
        help=(
            "Netflix corpus root (ref/ + dis/). Override with the "
            "``VMAF_NETFLIX_CORPUS_DIR`` env var for container or "
            "non-maintainer layouts."
        ),
    )
    ap.add_argument(
        "--training-runs-dir",
        type=Path,
        default=REPO_ROOT / "model" / "tiny" / "training_runs",
        help="Parent directory containing loso_<arch>/fold_<clip>/ trees.",
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=REPO_ROOT / "runs" / "loso_eval",
    )
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    if not args.data_root.is_dir():
        print(f"error: data-root not found: {args.data_root}", file=sys.stderr)
        return 2

    clip_xy: dict[str, tuple[np.ndarray, np.ndarray]] = {}
    for clip in CLIPS:
        clip_xy[clip] = _load_clip(args.data_root, clip)

    json_out = args.out / "loso_3arch_eval.json"
    md_out = args.out / "loso_3arch_eval.md"

    from aiutils.run_manifest import build_run_provenance, write_manifest_json

    report: dict = {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "corpus": str(args.data_root),
        "archs": {},
        "run_provenance": build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=sys.argv,
            args=args,
            inputs={
                "data_root": args.data_root,
                "training_runs_dir": args.training_runs_dir,
            },
            outputs={"json_report": json_out, "markdown_report": md_out},
        ),
    }
    for arch in ARCHS:
        loso_dir = args.training_runs_dir / f"loso_{arch}"
        report["archs"][arch] = _eval_arch(arch, loso_dir, clip_xy)

    write_manifest_json(json_out, report)
    print(f"[eval] wrote {json_out}", flush=True)

    with md_out.open("w", encoding="utf-8") as f:
        f.write(_markdown(report))
    print(f"[eval] wrote {md_out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

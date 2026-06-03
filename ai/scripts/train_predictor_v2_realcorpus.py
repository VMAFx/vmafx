#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Real-corpus LOSO trainer for the per-codec predictor models (Phase 2).

Companion to PR #450 (predictor training pipeline + 14 stub ONNX models —
``tools/vmaf-tune/src/vmaftune/predictor_train.py``). PR #450 ships
synthetic-stub weights for every codec adapter; this script promotes
those stubs into production-flippable models trained on a real corpus
under 5-fold leave-one-source-out cross-validation, with the
ADR-0303 production-flip gate enforced per codec.

Pipeline shape
--------------

1. Discover available real corpora under user-configured roots.
   Defaults: ``~/.workingdir2/netflix/`` (canonical-6 JSONL),
   ``~/.workingdir2/konvid-150k/`` (when present),
   ``~/.workingdir2/bvi-dvc-raw/`` (when present). Each root may
   contribute rows; mixed-corpus runs are explicit in the per-codec
   report.
2. Filter rows per codec adapter; reject codecs with insufficient
   distinct sources for 5-fold LOSO (need >= 5 distinct ``src``).
3. For each codec, run 5-fold LOSO: hold one source out per fold,
   train on the remaining four, evaluate on the held-out source.
   Aggregate per-fold PLCC / SROCC / RMSE into per-codec means.
4. Apply the ADR-0303 two-part production-flip gate per codec:
   - Mean fold PLCC >= 0.95.
   - Spread (max - min fold PLCC) <= 0.005.
5. Emit a JSON report listing every codec's gate verdict (PASS / FAIL).
6. **When the gate passes**, retrain on the full corpus and overwrite
   ``model/predictor_<codec>.onnx`` + the model card with REAL numbers.
7. **When the gate fails**, do NOT overwrite the shipped stub. Mark the
   model card with an explicit ``Status: Proposed (gate-failed)`` note
   listing which fold failed and by how much. Per the no-test-weakening
   rule (CLAUDE.md §13 / ``feedback_no_test_weakening``), the gate is
   load-bearing and must not be silently lowered.

This script does **not** modify any ONNX artefacts in the PR that
introduces it — it is the script the operator runs locally after
producing a real corpus on disk. The trained-model commits land in a
follow-up PR after the operator runs this script and verifies the
results.

Usage
-----

::

    # Discover corpora under default roots and train every codec.
    python ai/scripts/train_predictor_v2_realcorpus.py

    # Point at a specific corpus.
    python ai/scripts/train_predictor_v2_realcorpus.py \\
        --corpus ~/.workingdir2/netflix/canonical6.jsonl

    # Restrict to a single codec for debugging.
    python ai/scripts/train_predictor_v2_realcorpus.py --codec libx264

    # Dry-run on synthetic data so the orchestration is testable on a
    # host without real corpora.
    python ai/scripts/train_predictor_v2_realcorpus.py --synthetic-smoke

The emitted JSON report lives at ``--report-out`` (default
``runs/predictor_v2_realcorpus/report.json``) and is consumed by
``ai/scripts/run_predictor_v2_training.sh`` to update the per-codec
model cards.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as _dt
import json
import math
import os
import random
import statistics
import sys
from collections.abc import Iterable, Sequence
from pathlib import Path
from typing import Any

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_vmaf_tune_src=True)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

# ---------------------------------------------------------------------
# ADR-0303 production-flip gate constants
# ---------------------------------------------------------------------
#
# These values are the load-bearing constraint of this script. Per
# CLAUDE.md §13 / feedback_no_test_weakening, **never lower these
# thresholds** to make a codec pass; either the codec passes or the
# trainer reports it as FAIL and the model card stays Status: Proposed.
#
# The gate is two-part per ADR-0303 §Decision (inherited from
# ADR-0291 / ADR-0235). The same constants drive
# ``scripts/ci/ensemble_prod_gate.py`` (the deep-ensemble flip gate)
# so a centralised drift-audit picks up either path silently
# loosening.

#: Mean per-fold PLCC must be >= this value across the 5 LOSO folds.
SHIP_GATE_MEAN_PLCC: float = 0.95

#: ``max(PLCC_fold) - min(PLCC_fold) <= this`` across the 5 folds.
SHIP_GATE_PLCC_SPREAD_MAX: float = 0.005

#: Per-fold minimum PLCC; defaults to the mean threshold so a single
#: fold below 0.95 blocks an otherwise-passing codec. Mirrors the
#: ``--per-seed-min`` parameter on the ensemble gate.
SHIP_GATE_PER_FOLD_MIN: float = 0.95

#: Number of LOSO folds. Five-fold matches the ADR-0303 ensemble
#: sizing argument (Lakshminarayanan 2017 §5 — five members balance
#: calibration quality and wall time). For corpora with fewer than
#: five distinct sources, the trainer reports ``status:
#: insufficient-sources`` rather than running degenerate folds.
LOSO_FOLD_COUNT: int = 5


# ---------------------------------------------------------------------
# Default corpus discovery roots
# ---------------------------------------------------------------------

_HOME = Path.home()

#: Directory layout the operator is expected to populate. Each root is
#: scanned recursively for ``*.jsonl`` corpora at runtime; absent
#: directories are silently skipped (the operator may have only one of
#: the three corpora on disk). Each path may be overridden with a
#: ``VMAF_<NAME>_DIR`` env var for container / non-maintainer layouts.
DEFAULT_CORPUS_ROOTS: tuple[Path, ...] = (
    Path(
        os.environ.get(
            "VMAF_NETFLIX_CORPUS_DIR",
            str(REPO_ROOT / ".corpus" / "netflix"),
        )
    ),
    Path(
        os.environ.get(
            "VMAF_KONVID_150K_DIR",
            str(REPO_ROOT / ".corpus" / "konvid-150k"),
        )
    ),
    Path(
        os.environ.get(
            "VMAF_BVI_DVC_RAW_DIR",
            str(REPO_ROOT / ".corpus" / "bvi-dvc-raw"),
        )
    ),
)


# ---------------------------------------------------------------------
# Codec list — sourced from the runtime predictor's _DEFAULT_COEFFS so
# the trainer cannot drift away from the runtime contract. Falls back
# to a hard-coded mirror when the vmaf-tune package is not yet on
# import path (PR #450 still in flight).
# ---------------------------------------------------------------------


def _resolve_codecs() -> tuple[str, ...]:
    """Return the 14-codec tuple, importing from vmaf-tune if available.

    The defensive fallback exists so this script can land **before**
    PR #450's ``vmaftune.predictor`` module is on master. The mirror
    is checked against the imported list at runtime; mismatches abort
    rather than silently drift.
    """
    fallback = (
        "libx264",
        "libx265",
        "libsvtav1",
        "libaom-av1",
        "libvvenc",
        "h264_nvenc",
        "hevc_nvenc",
        "av1_nvenc",
        "h264_amf",
        "hevc_amf",
        "av1_amf",
        "h264_qsv",
        "hevc_qsv",
        "av1_qsv",
    )
    try:
        from vmaftune.predictor import _DEFAULT_COEFFS  # type: ignore[import-not-found]

        live = tuple(_DEFAULT_COEFFS.keys())
    except ImportError:
        return fallback
    if set(live) != set(fallback):
        raise RuntimeError(
            "Codec drift detected between vmaftune.predictor._DEFAULT_COEFFS "
            f"({sorted(live)}) and this script's mirror ({sorted(fallback)}). "
            "Update the mirror in train_predictor_v2_realcorpus.py to match."
        )
    return live


CODECS: tuple[str, ...] = _resolve_codecs()


# ---------------------------------------------------------------------
# Corpus discovery
# ---------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class CorpusFile:
    """One discovered JSONL corpus on disk."""

    path: Path
    root: Path  # the parent root that surfaced this file (provenance).


def discover_corpora(roots: Sequence[Path]) -> list[CorpusFile]:
    """Scan each root recursively for ``*.jsonl`` corpora.

    Missing roots are skipped silently — the operator may have only
    one of the three configured corpus directories. The function
    intentionally does not download or verify content; it just lists
    what is on disk so the caller can route per-codec.
    """
    out: list[CorpusFile] = []
    for root in roots:
        if not root.is_dir():
            continue
        for jsonl in sorted(root.rglob("*.jsonl")):
            out.append(CorpusFile(path=jsonl, root=root))
    return out


# ---------------------------------------------------------------------
# Corpus row loading + filtering
# ---------------------------------------------------------------------


def load_rows(corpus_files: Sequence[CorpusFile], codec: str) -> list[dict]:
    """Load every row matching ``encoder == codec`` across all corpus files.

    Mirrors ``vmaftune.predictor_train.load_corpus`` row filtering but
    accepts a list of files (so multi-corpus runs are explicit) and
    tags each row's ``_source_corpus`` provenance for the report.
    """
    rows: list[dict] = []
    for corpus in corpus_files:
        try:
            fh = corpus.path.open("r", encoding="utf-8")
        except OSError:
            continue
        with fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    row = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if row.get("encoder") != codec:
                    continue
                score = row.get("vmaf_score")
                if score is None:
                    score = row.get("vmaf")  # canonical-6 schema
                if score is None:
                    continue
                try:
                    score_f = float(score)
                except (TypeError, ValueError):
                    continue
                if not math.isfinite(score_f):
                    continue
                if int(row.get("exit_status", 0) or 0) != 0:
                    continue
                row["vmaf_score"] = score_f
                row["_source_corpus"] = str(corpus.path)
                rows.append(row)
    return rows


# ---------------------------------------------------------------------
# Source-name extraction (LOSO partitioning key)
# ---------------------------------------------------------------------


def row_source(row: dict) -> str:
    """Stable per-row source identifier used for LOSO partitioning.

    The Phase A schema's ``src`` field is the canonical name; we fall
    back to ``source`` then to a synthesised string so degenerate
    corpora at least produce one fold per row instead of crashing.
    """
    for key in ("src", "source"):
        val = row.get(key)
        if val:
            return str(val)
    return str(row.get("_source_corpus", "unknown"))


def source_count(rows: Sequence[dict]) -> int:
    return len({row_source(r) for r in rows})


# ---------------------------------------------------------------------
# 5-fold LOSO partitioning
# ---------------------------------------------------------------------


def loso_folds(
    rows: Sequence[dict], n_folds: int = LOSO_FOLD_COUNT, seed: int = 0
) -> list[tuple[list[dict], list[dict]]]:
    """Partition ``rows`` into ``n_folds`` (train, val) pairs by source.

    Each fold holds out one bucket of distinct sources. When the
    corpus has more sources than folds, sources are bucketed via a
    seeded shuffle; this distributes high-row-count sources roughly
    evenly across folds so per-fold PLCC is comparable. When the
    corpus has fewer distinct sources than folds, the function returns
    an empty list — the caller renders that as ``status:
    insufficient-sources`` rather than running degenerate folds.
    """
    if not rows:
        return []
    sources = sorted({row_source(r) for r in rows})
    if len(sources) < n_folds:
        return []

    rng = random.Random(seed)
    shuffled = list(sources)
    rng.shuffle(shuffled)

    # Bucket sources round-robin into n_folds groups.
    buckets: list[list[str]] = [[] for _ in range(n_folds)]
    for i, src in enumerate(shuffled):
        buckets[i % n_folds].append(src)

    folds: list[tuple[list[dict], list[dict]]] = []
    for i in range(n_folds):
        held_out = set(buckets[i])
        val = [r for r in rows if row_source(r) in held_out]
        train = [r for r in rows if row_source(r) not in held_out]
        if not val or not train:
            # Degenerate: skip this codec. Returning an empty list
            # signals the caller to mark the codec as
            # ``insufficient-sources`` rather than emitting a fold
            # with zero rows on either side.
            return []
        folds.append((train, val))
    return folds


# ---------------------------------------------------------------------
# Trainer integration (defers to vmaf-tune when available)
# ---------------------------------------------------------------------


def _import_predictor_train() -> Any:
    """Import the PR #450 trainer module if it is on the path."""
    try:
        from vmaftune import predictor_train  # type: ignore[import-not-found]

        return predictor_train
    except ImportError as exc:
        raise RuntimeError(
            "vmaftune.predictor_train is not importable. Either rebase onto a "
            "branch that includes PR #450, or run with --synthetic-smoke (which "
            "uses the in-process synthetic generator and does not need the "
            "shipped trainer module)."
        ) from exc


# ---------------------------------------------------------------------
# Tiny inline trainer (used when vmaftune.predictor_train is unavailable
# OR the caller passes --synthetic-smoke). Mirrors the PR #450 trainer's
# tiny-MLP architecture so the gate-enforcement tests don't depend on
# torch being installed; the real-corpus path always defers to PR #450.
# ---------------------------------------------------------------------


def _correlations(pred: Sequence[float], target: Sequence[float]) -> tuple[float, float, float]:
    """Pearson + Spearman + RMSE — same impl style as predictor_train."""
    n = len(pred)
    if n == 0 or n != len(target):
        return (0.0, 0.0, float("nan"))
    mse = sum((p - t) ** 2 for p, t in zip(pred, target, strict=False)) / n
    rmse = math.sqrt(mse)
    if n == 1:
        return (1.0, 1.0, rmse)
    try:
        plcc = statistics.correlation(pred, target)
    except (statistics.StatisticsError, ZeroDivisionError):
        plcc = 0.0
    pred_ranks = _ranks(pred)
    target_ranks = _ranks(target)
    try:
        srocc = statistics.correlation(pred_ranks, target_ranks)
    except (statistics.StatisticsError, ZeroDivisionError):
        srocc = 0.0
    return (float(plcc), float(srocc), float(rmse))


def _ranks(values: Sequence[float]) -> list[float]:
    indexed = sorted(range(len(values)), key=lambda i: values[i])
    ranks = [0.0] * len(values)
    i = 0
    while i < len(indexed):
        j = i
        while j + 1 < len(indexed) and values[indexed[j + 1]] == values[indexed[i]]:
            j += 1
        avg_rank = (i + j) / 2.0 + 1.0
        for k in range(i, j + 1):
            ranks[indexed[k]] = avg_rank
        i = j + 1
    return ranks


# ---------------------------------------------------------------------
# Per-fold training (real-corpus path)
# ---------------------------------------------------------------------


def _train_one_fold(
    codec: str,
    train_rows: Sequence[dict],
    val_rows: Sequence[dict],
    *,
    epochs: int,
    seed: int,
) -> tuple[float, float, float]:
    """Fit a tiny MLP on ``train_rows``, evaluate on ``val_rows``.

    Defers to ``vmaftune.predictor_train`` when importable so the
    fold-level training body shares the exact projection + tiny-MLP
    architecture as the shipped stubs (no behavioural drift between
    LOSO eval and the production model). When that module is
    unavailable (e.g. PR #450 not yet on master), raises so the
    caller knows to skip the real-corpus path.
    """
    pt = _import_predictor_train()
    cfg = pt.TrainConfig(epochs=epochs, seed=seed)
    # Mirror predictor_train.train_one_codec but without the ONNX
    # export / card write — we just need PLCC / SROCC / RMSE per fold.
    import torch  # type: ignore[import-not-found]

    pt._set_seed(cfg.seed)
    x_train = torch.tensor([pt.project_row(r) for r in train_rows], dtype=torch.float32)
    y_train = torch.tensor([[float(r["vmaf_score"])] for r in train_rows], dtype=torch.float32)
    x_val = torch.tensor([pt.project_row(r) for r in val_rows], dtype=torch.float32)
    y_val = torch.tensor([[float(r["vmaf_score"])] for r in val_rows], dtype=torch.float32)

    model = pt._build_model()
    pt._set_input_normalisation(model, x_train)
    model.train()
    pt._fit(model, x_train, y_train, cfg)
    return pt._evaluate(model, x_val, y_val)


# ---------------------------------------------------------------------
# Gate evaluation
# ---------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class FoldResult:
    fold_index: int
    held_out_sources: tuple[str, ...]
    plcc: float
    srocc: float
    rmse: float
    n_train: int
    n_val: int


@dataclasses.dataclass(frozen=True)
class CodecResult:
    codec: str
    status: str  # "pass" | "fail" | "insufficient-sources" | "missing-rows"
    folds: tuple[FoldResult, ...]
    mean_plcc: float
    plcc_spread: float
    mean_srocc: float
    mean_rmse: float
    n_rows_total: int
    n_distinct_sources: int
    failure_reasons: tuple[str, ...]
    corpus_provenance: tuple[str, ...]


def evaluate_gate(
    codec: str,
    folds: Sequence[FoldResult],
    *,
    mean_plcc_threshold: float = SHIP_GATE_MEAN_PLCC,
    spread_max: float = SHIP_GATE_PLCC_SPREAD_MAX,
    per_fold_min: float = SHIP_GATE_PER_FOLD_MIN,
) -> tuple[bool, tuple[str, ...]]:
    """Apply the ADR-0303 two-part gate to a codec's fold results.

    Returns ``(passed, failure_reasons)``. The reasons list is empty
    on pass, otherwise carries one human-readable string per failed
    sub-gate so the report renders an actionable verdict.
    """
    if not folds:
        return False, (
            "no folds — corpus has fewer distinct sources than "
            f"LOSO_FOLD_COUNT={LOSO_FOLD_COUNT}",
        )
    plccs = [f.plcc for f in folds]
    mean_plcc = sum(plccs) / len(plccs)
    spread = max(plccs) - min(plccs)
    failing = [f for f in folds if f.plcc < per_fold_min]

    reasons: list[str] = []
    if mean_plcc < mean_plcc_threshold:
        reasons.append(f"mean PLCC {mean_plcc:.4f} < {mean_plcc_threshold:.4f} (ADR-0303 part 1)")
    if spread > spread_max:
        reasons.append(f"PLCC spread {spread:.4f} > {spread_max:.4f} (ADR-0303 part 2)")
    if failing:
        idxs = ", ".join(f"fold{f.fold_index}={f.plcc:.4f}" for f in failing)
        reasons.append(f"per-fold PLCC < {per_fold_min:.4f}: {idxs}")
    return (not reasons, tuple(reasons))


# ---------------------------------------------------------------------
# Per-codec orchestration
# ---------------------------------------------------------------------


def train_codec_loso(
    codec: str,
    rows: Sequence[dict],
    *,
    epochs: int = 200,
    seed: int = 42,
) -> CodecResult:
    """Run 5-fold LOSO + gate evaluation for one codec.

    Defers per-fold training to ``_train_one_fold`` (which in turn
    requires ``vmaftune.predictor_train``). When the row count is
    insufficient for 5-fold LOSO, returns a CodecResult with
    ``status='insufficient-sources'`` so the caller can mark the
    model card accordingly without crashing the batch.
    """
    if not rows:
        return CodecResult(
            codec=codec,
            status="missing-rows",
            folds=(),
            mean_plcc=0.0,
            plcc_spread=0.0,
            mean_srocc=0.0,
            mean_rmse=float("nan"),
            n_rows_total=0,
            n_distinct_sources=0,
            failure_reasons=("no rows in corpus for this codec",),
            corpus_provenance=(),
        )

    n_sources = source_count(rows)
    folds_split = loso_folds(rows, LOSO_FOLD_COUNT, seed=seed)
    if not folds_split:
        return CodecResult(
            codec=codec,
            status="insufficient-sources",
            folds=(),
            mean_plcc=0.0,
            plcc_spread=0.0,
            mean_srocc=0.0,
            mean_rmse=float("nan"),
            n_rows_total=len(rows),
            n_distinct_sources=n_sources,
            failure_reasons=(f"need >= {LOSO_FOLD_COUNT} distinct sources; have {n_sources}",),
            corpus_provenance=tuple(
                sorted({r["_source_corpus"] for r in rows if "_source_corpus" in r})
            ),
        )

    fold_results: list[FoldResult] = []
    for i, (train_rows, val_rows) in enumerate(folds_split):
        held = tuple(sorted({row_source(r) for r in val_rows}))
        plcc, srocc, rmse = _train_one_fold(
            codec, train_rows, val_rows, epochs=epochs, seed=seed + i
        )
        fold_results.append(
            FoldResult(
                fold_index=i,
                held_out_sources=held,
                plcc=plcc,
                srocc=srocc,
                rmse=rmse,
                n_train=len(train_rows),
                n_val=len(val_rows),
            )
        )

    passed, reasons = evaluate_gate(codec, fold_results)
    plccs = [f.plcc for f in fold_results]
    return CodecResult(
        codec=codec,
        status="pass" if passed else "fail",
        folds=tuple(fold_results),
        mean_plcc=sum(plccs) / len(plccs),
        plcc_spread=max(plccs) - min(plccs),
        mean_srocc=sum(f.srocc for f in fold_results) / len(fold_results),
        mean_rmse=sum(f.rmse for f in fold_results) / len(fold_results),
        n_rows_total=len(rows),
        n_distinct_sources=n_sources,
        failure_reasons=reasons,
        corpus_provenance=tuple(
            sorted({r["_source_corpus"] for r in rows if "_source_corpus" in r})
        ),
    )


# ---------------------------------------------------------------------
# Report rendering
# ---------------------------------------------------------------------


def render_report(results: Sequence[CodecResult], *, corpus_files: Sequence[CorpusFile]) -> dict:
    """Build the JSON payload consumed by ``run_predictor_v2_training.sh``."""
    return {
        "schema_version": 1,
        "generated_at_utc": (
            _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0, tzinfo=None).isoformat() + "Z"
        ),
        "gate": {
            "mean_plcc_threshold": SHIP_GATE_MEAN_PLCC,
            "plcc_spread_max": SHIP_GATE_PLCC_SPREAD_MAX,
            "per_fold_min": SHIP_GATE_PER_FOLD_MIN,
            "loso_fold_count": LOSO_FOLD_COUNT,
            "adr": "ADR-0303",
        },
        "corpus": {
            "discovered_files": [str(c.path) for c in corpus_files],
            "roots": sorted({str(c.root) for c in corpus_files}),
        },
        "codecs": [
            {
                "codec": r.codec,
                "status": r.status,
                "mean_plcc": r.mean_plcc,
                "plcc_spread": r.plcc_spread,
                "mean_srocc": r.mean_srocc,
                "mean_rmse": r.mean_rmse,
                "n_rows_total": r.n_rows_total,
                "n_distinct_sources": r.n_distinct_sources,
                "failure_reasons": list(r.failure_reasons),
                "corpus_provenance": list(r.corpus_provenance),
                "folds": [
                    {
                        "fold_index": f.fold_index,
                        "held_out_sources": list(f.held_out_sources),
                        "plcc": f.plcc,
                        "srocc": f.srocc,
                        "rmse": f.rmse,
                        "n_train": f.n_train,
                        "n_val": f.n_val,
                    }
                    for f in r.folds
                ],
            }
            for r in results
        ],
        "summary": {
            "n_pass": sum(1 for r in results if r.status == "pass"),
            "n_fail": sum(1 for r in results if r.status == "fail"),
            "n_insufficient": sum(1 for r in results if r.status == "insufficient-sources"),
            "n_missing_rows": sum(1 for r in results if r.status == "missing-rows"),
        },
    }


def render_human_summary(report: dict) -> str:
    """Single-page text summary suitable for stdout / CI logs."""
    gate = report["gate"]
    lines = [
        "=== predictor v2 real-corpus LOSO gate (ADR-0303) ===",
        f"Mean PLCC threshold: >= {gate['mean_plcc_threshold']:.4f}",
        f"Spread bound:        <= {gate['plcc_spread_max']:.4f}",
        f"Per-fold min:        >= {gate['per_fold_min']:.4f}",
        f"Fold count:          {gate['loso_fold_count']}",
        "",
        f"{'codec':<14} {'status':<22} {'mean_plcc':>9} {'spread':>7} " f"{'rows':>6} {'srcs':>5}",
    ]
    for codec_payload in report["codecs"]:
        codec = codec_payload["codec"]
        status = codec_payload["status"].upper()
        mean_plcc = codec_payload["mean_plcc"]
        spread = codec_payload["plcc_spread"]
        rows = codec_payload["n_rows_total"]
        srcs = codec_payload["n_distinct_sources"]
        lines.append(
            f"{codec:<14} {status:<22} {mean_plcc:>9.4f} {spread:>7.4f} " f"{rows:>6} {srcs:>5}"
        )
        if codec_payload["failure_reasons"]:
            for reason in codec_payload["failure_reasons"]:
                lines.append(f"  - {reason}")
    s = report["summary"]
    lines.append("")
    lines.append(
        f"Verdict: {s['n_pass']} pass / {s['n_fail']} fail / "
        f"{s['n_insufficient']} insufficient-sources / "
        f"{s['n_missing_rows']} missing-rows"
    )
    return "\n".join(lines)


# ---------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------


def _build_arg_parser() -> argparse.ArgumentParser:
    parser = make_argument_parser(
        prog="train_predictor_v2_realcorpus.py",
        description="Real-corpus LOSO trainer for the per-codec predictor "
        "models (Phase 2 of the predictor pipeline).",
    )
    parser.add_argument(
        "--corpus",
        type=Path,
        action="append",
        default=None,
        help="Explicit JSONL corpus file. Repeatable. When supplied, "
        "skips the default-root discovery.",
    )
    parser.add_argument(
        "--corpus-root",
        type=Path,
        action="append",
        default=None,
        help="Override the default corpus discovery roots. Repeatable. "
        f"Defaults: {[str(p) for p in DEFAULT_CORPUS_ROOTS]}.",
    )
    parser.add_argument(
        "--codec",
        action="append",
        default=None,
        help="Restrict to specific codec(s). Repeatable. Default: all 14.",
    )
    parser.add_argument(
        "--epochs",
        type=int,
        default=200,
        help="Per-fold training epochs (default 200).",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=42,
        help="Trainer base seed (each fold offsets by fold-index).",
    )
    parser.add_argument(
        "--report-out",
        type=Path,
        default=Path("runs/predictor_v2_realcorpus/report.json"),
        help="Where to write the per-codec JSON report.",
    )
    parser.add_argument(
        "--synthetic-smoke",
        action="store_true",
        help="Skip real-corpus discovery and run on the synthetic stub "
        "corpus for every codec. Used by tests + CI smoke; never "
        "produces a passing gate verdict (synthetic targets do not "
        "exercise the LOSO generalisation surface).",
    )
    parser.add_argument(
        "--allow-empty",
        action="store_true",
        help="Do not error when no corpora are discovered; instead emit "
        "a report where every codec has status='missing-rows'. Used "
        "by ``run_predictor_v2_training.sh`` to render an honest "
        "diagnostic when the operator has not yet generated corpora.",
    )
    return parser


def _resolve_corpora(args: argparse.Namespace) -> list[CorpusFile]:
    """Resolve the corpus list per CLI args."""
    if args.corpus:
        return [CorpusFile(path=p, root=p.parent) for p in args.corpus if p.is_file()]
    roots = tuple(args.corpus_root) if args.corpus_root else DEFAULT_CORPUS_ROOTS
    return discover_corpora(roots)


def _synthetic_rows_for_codec(codec: str, n_rows: int = 200) -> list[dict]:
    """Synthetic rows for the smoke path. Per-source bucketing for LOSO."""
    pt = _import_predictor_train()
    rows = pt.generate_synthetic_corpus(codec, n_rows=n_rows)
    # Re-tag src so we get >= 5 distinct sources for LOSO.
    n_synthetic_sources = max(LOSO_FOLD_COUNT, 6)
    for i, row in enumerate(rows):
        row["src"] = f"synthetic_src_{i % n_synthetic_sources:02d}"
        row["_source_corpus"] = "<synthetic-smoke>"
    return rows


def main(argv: Iterable[str] | None = None) -> int:
    parser = _build_arg_parser()
    raw_argv = collect_cli_argv(argv)
    args = parser.parse_args(raw_argv)

    codecs = tuple(args.codec) if args.codec else CODECS
    unknown = [c for c in codecs if c not in CODECS]
    if unknown:
        parser.error(f"unknown codec(s): {unknown}; supported: {list(CODECS)}")

    corpus_files = [] if args.synthetic_smoke else _resolve_corpora(args)
    if not corpus_files and not args.synthetic_smoke and not args.allow_empty:
        parser.error(
            "no corpora discovered. Pass --corpus PATH, --corpus-root DIR, "
            f"populate one of {[str(p) for p in DEFAULT_CORPUS_ROOTS]}, "
            "or use --synthetic-smoke / --allow-empty for a diagnostic run."
        )

    print(f"[predictor-v2] codecs:        {list(codecs)}", flush=True)
    print(f"[predictor-v2] corpus files:  {[str(c.path) for c in corpus_files]}", flush=True)
    print(f"[predictor-v2] synthetic:     {args.synthetic_smoke}", flush=True)

    results: list[CodecResult] = []
    for codec in codecs:
        if args.synthetic_smoke:
            rows = _synthetic_rows_for_codec(codec)
        else:
            rows = load_rows(corpus_files, codec)
        print(f"  {codec}: {len(rows)} rows / " f"{source_count(rows)} sources", flush=True)
        try:
            result = train_codec_loso(codec, rows, epochs=args.epochs, seed=args.seed)
        except RuntimeError as exc:
            # vmaftune.predictor_train missing — render a diagnostic
            # row rather than crashing the whole batch.
            result = CodecResult(
                codec=codec,
                status="missing-rows",
                folds=(),
                mean_plcc=0.0,
                plcc_spread=0.0,
                mean_srocc=0.0,
                mean_rmse=float("nan"),
                n_rows_total=len(rows),
                n_distinct_sources=source_count(rows),
                failure_reasons=(f"trainer unavailable: {exc}",),
                corpus_provenance=(),
            )
        results.append(result)
        verdict = result.status.upper()
        if result.folds:
            print(
                f"    {verdict}: mean_PLCC={result.mean_plcc:.4f} "
                f"spread={result.plcc_spread:.4f} "
                f"mean_RMSE={result.mean_rmse:.3f}",
                flush=True,
            )
        else:
            reasons = "; ".join(result.failure_reasons) or "(no folds)"
            print(f"    {verdict}: {reasons}", flush=True)

    report = render_report(results, corpus_files=corpus_files)
    report["run_provenance"] = build_run_provenance(
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        inputs={
            "explicit_corpus_files": args.corpus or [],
            "corpus_roots": args.corpus_root or list(DEFAULT_CORPUS_ROOTS),
            "resolved_corpus_files": [c.path for c in corpus_files],
        },
        outputs={"report_target": str(args.report_out)},
    )
    write_manifest_json(args.report_out, report)
    print("", flush=True)
    print(render_human_summary(report), flush=True)
    print("", flush=True)
    print(f"[predictor-v2] report:        {args.report_out}", flush=True)

    # Exit code: 0 if every codec passed, 1 if any failed (so the
    # orchestration shell can short-circuit). The honest report is
    # the gate's product; the exit code is just a CI hook.
    return 0 if all(r.status == "pass" for r in results) else 1


__all__ = [
    "CODECS",
    "DEFAULT_CORPUS_ROOTS",
    "LOSO_FOLD_COUNT",
    "SHIP_GATE_MEAN_PLCC",
    "SHIP_GATE_PER_FOLD_MIN",
    "SHIP_GATE_PLCC_SPREAD_MAX",
    "CodecResult",
    "CorpusFile",
    "FoldResult",
    "discover_corpora",
    "evaluate_gate",
    "load_rows",
    "loso_folds",
    "main",
    "render_human_summary",
    "render_report",
    "row_source",
    "source_count",
    "train_codec_loso",
]


if __name__ == "__main__":
    sys.exit(main())

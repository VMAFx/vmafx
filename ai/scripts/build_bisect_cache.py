#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Generate the deterministic bisect-model-quality fixture cache.

The default fixture is a small synthetic cache, but the same generator
can now materialise a real DMOS/MOS-aligned feature parquet into the
nightly-bisect layout. What lives in `ai/testdata/bisect/` is fully
reproducible from this script with fixed seeds; CI re-runs and asserts
byte-equality before running the bisect, which is how we catch silent
drift in pandas / pyarrow / onnx serialisation.

Layout:

    ai/testdata/bisect/
        features.parquet      256 rows x (6 DEFAULT_FEATURES + mos)
        models/model_NN.onnx  8 linear FR models, all "good"

The committed timeline is intentionally regression-free: the head and
tail both pass the gate, so a green nightly means the bisect end-to-end
wiring still works. The synthetic-regression case lives in
`ai/tests/test_bisect_model_quality.py::test_bisect_localises_first_bad`,
which builds a bad-from-index-5 timeline at runtime.

Usage:

    python ai/scripts/build_bisect_cache.py             # regenerate in place
    python ai/scripts/build_bisect_cache.py --check     # diff against committed
    python ai/scripts/build_bisect_cache.py \
        --source-features runs/dmos_features.parquet \
        --target-column dmos
"""

from __future__ import annotations

import argparse
import filecmp
import shutil
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

import numpy as np
import onnx
import pandas as pd
import pyarrow.parquet as pq
from onnx import TensorProto, helper

SCRIPT_PATH = Path(__file__).resolve()
REPO_ROOT = SCRIPT_PATH.parents[2]
if str(REPO_ROOT / "ai" / "src") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))

from aiutils.run_manifest import write_run_manifest  # noqa: E402

DEFAULT_FEATURES = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)
N_FEATURES = len(DEFAULT_FEATURES)
N_ROWS = 256
N_MODELS = 8
FEATURE_SEED = 20260418
MODEL_SEED = 20260419
TARGET_COLUMN_CANDIDATES = ("mos", "dmos", "target", "score")


def _save_linear_fr(path: Path, weights: np.ndarray, bias: float = 0.0) -> None:
    x = helper.make_tensor_value_info("input", TensorProto.FLOAT, ["N", N_FEATURES])
    y = helper.make_tensor_value_info("score", TensorProto.FLOAT, ["N"])
    w = helper.make_tensor(
        "W",
        TensorProto.FLOAT,
        [N_FEATURES, 1],
        weights.astype(np.float32).flatten().tolist(),
    )
    b = helper.make_tensor("b", TensorProto.FLOAT, [1], [float(bias)])
    axes = helper.make_tensor("axes", TensorProto.INT64, [1], [1])
    mm = helper.make_node("MatMul", ["input", "W"], ["mm"])
    add = helper.make_node("Add", ["mm", "b"], ["wide"])
    sq = helper.make_node("Squeeze", ["wide", "axes"], ["score"])
    graph = helper.make_graph([mm, add, sq], "fr_linear", [x], [y], [w, b, axes])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    # Pin producer fields so the serialised bytes don't depend on host onnx version.
    model.producer_name = "vmaf-train.bisect-cache"
    model.producer_version = "1"
    model.ir_version = 9
    onnx.save(model, str(path))


def _write_features(out: Path, features: np.ndarray, target: np.ndarray) -> None:
    df = pd.DataFrame({name: features[:, i] for i, name in enumerate(DEFAULT_FEATURES)})
    df["mos"] = target.astype(np.float32)
    # Pin the row index so parquet metadata is reproducible.
    df.index = pd.RangeIndex(start=0, stop=len(df), name="row")
    df.to_parquet(out, engine="pyarrow", compression="zstd", index=True)


def build_features(out: Path) -> tuple[np.ndarray, np.ndarray]:
    rng = np.random.default_rng(FEATURE_SEED)
    feats = rng.uniform(0.2, 0.9, size=(N_ROWS, N_FEATURES)).astype(np.float32)
    # Targets on the [0, 1] band so a sum-of-features predictor correlates
    # ~ perfectly. Add tiny noise so PLCC is < 1 but well above any sane gate.
    target = feats.sum(axis=1) / N_FEATURES
    target += rng.normal(0.0, 1e-3, size=N_ROWS).astype(np.float32)
    _write_features(out, feats, target)
    return feats, target.astype(np.float32)


def _resolve_target_column(columns: Sequence[str], target_column: str | None) -> str:
    if target_column is not None:
        if target_column not in columns:
            raise ValueError(f"target column not found: {target_column}")
        return target_column
    for name in TARGET_COLUMN_CANDIDATES:
        if name in columns:
            return name
    candidates = ", ".join(TARGET_COLUMN_CANDIDATES)
    raise ValueError(f"no target column found; pass --target-column (tried: {candidates})")


def load_source_features(
    path: Path, target_column: str | None = None
) -> tuple[np.ndarray, np.ndarray]:
    """Load a real feature parquet into the bisect-cache input contract."""
    table = pq.read_table(str(path))
    columns = table.column_names
    missing = [name for name in DEFAULT_FEATURES if name not in columns]
    if missing:
        joined = ", ".join(missing)
        raise ValueError(f"source parquet missing required feature columns: {joined}")
    target_name = _resolve_target_column(columns, target_column)
    df = table.select([*DEFAULT_FEATURES, target_name]).to_pandas()
    numeric = df.apply(pd.to_numeric, errors="coerce")
    numeric = numeric.replace([np.inf, -np.inf], np.nan).dropna()
    if numeric.empty:
        raise ValueError("source parquet has no finite feature/target rows")
    feats = numeric.loc[:, DEFAULT_FEATURES].to_numpy(dtype=np.float32)
    target = numeric.loc[:, target_name].to_numpy(dtype=np.float32)
    return feats, target


def _fit_linear_weights(features: np.ndarray, target: np.ndarray) -> tuple[np.ndarray, float]:
    design = np.column_stack([features.astype(np.float64), np.ones(features.shape[0])])
    coeff, *_ = np.linalg.lstsq(design, target.astype(np.float64), rcond=None)
    weights = coeff[:N_FEATURES].astype(np.float32)
    bias = float(coeff[N_FEATURES])
    return weights, bias


def build_models(
    out_dir: Path,
    features: np.ndarray | None = None,
    target: np.ndarray | None = None,
) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(MODEL_SEED)
    if features is None or target is None:
        good_w = np.full(N_FEATURES, 1.0 / N_FEATURES, dtype=np.float32)
        bias = 0.0
    else:
        good_w, bias = _fit_linear_weights(features, target)
    noise_scale = np.maximum(np.abs(good_w), np.float32(1.0)) * np.float32(1e-4)
    for i in range(N_MODELS):
        # All models are "good" — tiny perturbations stay well above the
        # PLCC gate. Bisect verdict on this set is "no regression in range".
        w = good_w + rng.normal(0.0, noise_scale, size=N_FEATURES).astype(np.float32)
        _save_linear_fr(out_dir / f"model_{i:02d}.onnx", w, bias=bias)


def regenerate(
    out_dir: Path,
    source_features: Path | None = None,
    target_column: str | None = None,
) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    if source_features is None:
        build_features(out_dir / "features.parquet")
        build_models(out_dir / "models")
    else:
        feats, target = load_source_features(source_features, target_column=target_column)
        _write_features(out_dir / "features.parquet", feats, target)
        build_models(out_dir / "models", feats, target)


def _artifact_counts(out_dir: Path) -> dict[str, int]:
    model_dir = out_dir / "models"
    return {
        "feature_parquets": int((out_dir / "features.parquet").is_file()),
        "onnx_models": len(list(model_dir.glob("model_*.onnx"))) if model_dir.is_dir() else 0,
    }


def _write_manifest(
    *,
    path: Path,
    args: argparse.Namespace,
    raw_argv: Sequence[str],
    mode: str,
    status: str,
    exit_code: int,
) -> None:
    write_run_manifest(
        path,
        schema="bisect-cache-manifest-v1",
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        inputs={"source_features": args.source_features},
        outputs={"cache_dir": args.out, "manifest": path},
        sections={
            "mode": mode,
            "status": status,
            "exit_code": int(exit_code),
            "target_column_candidates": list(TARGET_COLUMN_CANDIDATES),
            "default_features": list(DEFAULT_FEATURES),
            "artifact_counts": _artifact_counts(args.out),
        },
    )


def _compare_parquet(committed: Path, fresh: Path) -> str | None:
    """Return None if logically equal, else a human-readable diff message.

    Compares the typed Arrow Table (schema + values) and the row-count.
    Deliberately ignores parquet writer metadata such as the
    ``created_by`` field, which embeds the host pyarrow version
    (``parquet-cpp-arrow version X.Y.Z``) and drifts every time a CI
    image bumps pyarrow without any actual content change. ADR-0262
    documents the rationale.
    """
    a = pq.read_table(str(committed))
    b = pq.read_table(str(fresh))
    if a.schema != b.schema:
        return f"schema drift: {committed.name}"
    if a.num_rows != b.num_rows:
        return f"row-count drift: {committed.name} ({a.num_rows} vs {b.num_rows})"
    if not a.equals(b):
        return f"row-content drift: {committed.name}"
    return None


def _compare_onnx(committed: Path, fresh: Path) -> str | None:
    """Return None if onnx models are byte-equal, else a diff message.

    ONNX serialisation is deterministic across the supported onnx
    versions because we pin ``producer_name``, ``producer_version``, and
    ``ir_version`` in ``_save_linear_fr``. Byte equality stays the gate
    here so weight, opset, or graph-topology drift trip immediately.
    """
    if filecmp.cmp(committed, fresh, shallow=False):
        return None
    return f"byte drift: {committed.relative_to(committed.parents[1])}"


def check(
    out_dir: Path,
    source_features: Path | None = None,
    target_column: str | None = None,
) -> int:
    """Return 0 if regenerated content matches the committed tree.

    Parquet files are compared by typed Arrow Table content; ONNX
    files are compared byte-for-byte. See ADR-0262 for the policy
    behind the parquet-only relaxation.
    """
    assert out_dir.is_dir(), f"expected committed cache at {out_dir}"
    with tempfile.TemporaryDirectory() as tmp_str:
        tmp = Path(tmp_str)
        regenerate(tmp, source_features=source_features, target_column=target_column)
        diffs: list[str] = []
        for committed in sorted(out_dir.rglob("*")):
            if committed.is_dir():
                continue
            if committed.name == "README.md":
                continue
            rel = committed.relative_to(out_dir)
            fresh = tmp / rel
            if not fresh.exists():
                diffs.append(f"missing in regen: {rel}")
                continue
            suffix = committed.suffix.lower()
            if suffix == ".parquet":
                msg = _compare_parquet(committed, fresh)
            elif suffix == ".onnx":
                msg = _compare_onnx(committed, fresh)
            else:
                # Unknown artefact: fall back to byte comparison so we don't
                # silently let a new file format slip through unguarded.
                msg = None if filecmp.cmp(committed, fresh, shallow=False) else f"byte drift: {rel}"
            if msg is not None:
                diffs.append(msg)
        for fresh in sorted(tmp.rglob("*")):
            if fresh.is_dir():
                continue
            rel = fresh.relative_to(tmp)
            if not (out_dir / rel).exists():
                diffs.append(f"new file in regen: {rel}")
        if diffs:
            for d in diffs:
                print(f"DRIFT  {d}", file=sys.stderr)
            print(
                "\nRegenerate the committed cache:\n" "  python ai/scripts/build_bisect_cache.py\n",
                file=sys.stderr,
            )
            return 1
    print("OK  bisect cache matches regenerated content")
    return 0


def main(argv: list[str] | None = None) -> int:
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument(
        "--out",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "testdata" / "bisect",
        help="Output directory (default: ai/testdata/bisect)",
    )
    p.add_argument(
        "--check",
        action="store_true",
        help="Diff regenerated bytes against committed; exit 1 on drift",
    )
    p.add_argument(
        "--source-features",
        type=Path,
        help=(
            "Optional real feature parquet with adm2/vif_scale0..3/motion2 plus a target "
            "column. When omitted, the deterministic synthetic cache is generated."
        ),
    )
    p.add_argument(
        "--target-column",
        help="Target column in --source-features (default: first of mos,dmos,target,score)",
    )
    p.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help="Optional replay manifest JSON sidecar with ADR-0661 run provenance.",
    )
    args = p.parse_args(raw_argv)
    if args.check:
        rc = check(args.out, source_features=args.source_features, target_column=args.target_column)
        if args.manifest_out is not None:
            _write_manifest(
                path=args.manifest_out,
                args=args,
                raw_argv=raw_argv,
                mode="check",
                status="pass" if rc == 0 else "fail",
                exit_code=rc,
            )
        return rc
    # Wipe only generated artifacts; preserve hand-written siblings such as
    # README.md that explain the cache to future readers.
    parquet = args.out / "features.parquet"
    models = args.out / "models"
    if parquet.exists():
        parquet.unlink()
    if models.exists():
        shutil.rmtree(models)
    regenerate(args.out, source_features=args.source_features, target_column=args.target_column)
    print(f"OK  wrote {args.out}")
    if args.manifest_out is not None:
        _write_manifest(
            path=args.manifest_out,
            args=args,
            raw_argv=raw_argv,
            mode="regenerate",
            status="written",
            exit_code=0,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())

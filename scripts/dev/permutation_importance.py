#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Permutation feature importance for vmaf_tiny_v2.onnx (canonical-6).

Loads the shipped ONNX model, samples a held-out slice from the 4-corpus
parquet, and measures the PLCC drop after shuffling each input column in
turn. Mean +/- std over multiple seeds.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import onnxruntime as ort
import pyarrow.parquet as pq
from scipy.stats import pearsonr

REPO = Path(__file__).resolve().parents[2]
MODEL = REPO / "model/tiny/vmaf_tiny_v2.onnx"
SIDECAR = REPO / "model/tiny/vmaf_tiny_v2.json"
PARQUET = REPO / "runs/full_features_4corpus.parquet"
SAMPLE_N = 5000
N_SEEDS = 5


def _load_sample(feats: list[str]) -> tuple[np.ndarray, np.ndarray, int]:
    rng = np.random.default_rng(20260503)
    columns = [*feats, "vmaf"]
    frame = pq.read_table(PARQUET, columns=columns).to_pandas()
    frame = frame.dropna(subset=columns).reset_index(drop=True)
    sample_size = min(SAMPLE_N, len(frame))
    indices = rng.choice(len(frame), size=sample_size, replace=False)
    sample = frame.iloc[indices].reset_index(drop=True)
    print(f"Sample: {sample_size} rows from {len(frame)} total", flush=True)
    features = sample[feats].to_numpy(dtype=np.float32)
    targets = sample["vmaf"].to_numpy(dtype=np.float32)
    return features, targets, sample_size


def main() -> int:
    sidecar = json.loads(SIDECAR.read_text())
    feats: list[str] = sidecar["features"]
    print(f"Model: {MODEL.name}  features: {feats}", flush=True)

    X, y, n = _load_sample(feats)

    sess = ort.InferenceSession(str(MODEL), providers=["CPUExecutionProvider"])
    in_name = sess.get_inputs()[0].name
    out_name = sess.get_outputs()[0].name

    def predict(arr: np.ndarray) -> np.ndarray:
        return np.asarray(sess.run([out_name], {in_name: arr})[0]).reshape(-1)

    base_pred = predict(X)
    base_plcc, _ = pearsonr(base_pred, y)
    print(f"Baseline PLCC: {base_plcc:.6f}", flush=True)

    rows = []
    for j, fname in enumerate(feats):
        plccs: list[float] = []
        for seed in range(N_SEEDS):
            rng_s = np.random.default_rng(1000 + seed)
            Xp = X.copy()
            perm = rng_s.permutation(n)
            Xp[:, j] = Xp[perm, j]
            pred = predict(Xp)
            plcc, _ = pearsonr(pred, y)
            plccs.append(float(plcc))
        arr = np.array(plccs)
        mean_plcc = float(arr.mean())
        std_plcc = float(arr.std())
        drop_mean = float(base_plcc - mean_plcc)
        drop_std = std_plcc
        rows.append((fname, mean_plcc, std_plcc, drop_mean, drop_std))
        print(
            f"  {fname:12s}  PLCC={mean_plcc:.6f} ± {std_plcc:.6f}  " f"drop={drop_mean:+.6f}",
            flush=True,
        )

    rows.sort(key=lambda r: r[3], reverse=True)
    print("\nRanked by importance (drop):", flush=True)
    print(
        "| Rank | Feature | Baseline PLCC | After permutation | Drop ± std |",
        flush=True,
    )
    print("|---|---|---|---|---|", flush=True)
    for rank, (fname, mean_plcc, std_plcc, drop, _drop_std) in enumerate(rows, 1):
        print(
            f"| {rank} | `{fname}` | {base_plcc:.4f} | "
            f"{mean_plcc:.4f} | {drop:+.4f} ± {std_plcc:.4f} |",
            flush=True,
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())

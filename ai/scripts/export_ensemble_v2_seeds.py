#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Export production-quality ``fr_regressor_v2_ensemble_v1_seed{0..4}`` ONNX members.

Companion to ``ai/scripts/train_fr_regressor_v2_ensemble_loso.py`` (ADR-0319,
the LOSO trainer that produces the ship-gate verdict in
``runs/ensemble_v2_real/PROMOTE.json``). After the LOSO gate has passed
(mean PLCC >= 0.95, spread <= 0.005, per-seed >= 0.95 — recorded in
``PROMOTE.json``), this driver fits a **final** FRRegressor per seed on
the FULL Phase A corpus (no held-out fold) and exports each one as an
ONNX file at ``model/tiny/fr_regressor_v2_ensemble_v1_seed{N}.onnx``.

The full-corpus fit is what ships at inference time — the LOSO PLCC
was the gate for whether we ship at all; the production checkpoint is
trained on every available source so the ensemble has maximum signal.

Each seed also gets a sidecar JSON ``..._seed{N}.json`` with the
canonical sidecar shape (mirrors ``model/tiny/fr_regressor_v2.json``):
encoder vocab v2, codec block layout, scaler params, training recipe,
gate-pass evidence from PROMOTE.json. The sidecar is required by
``core/test/dnn/test_registry.sh`` for every non-smoke registry
entry.

Usage::

    python ai/scripts/export_ensemble_v2_seeds.py \\
        --corpus runs/phase_a/full_grid/per_frame_canonical6.jsonl \\
        --promote-json runs/ensemble_v2_real/PROMOTE.json \\
        --seeds 0,1,2,3,4

The script then prints (per seed) the new ONNX sha256, which the
operator copies into ``model/tiny/registry.json`` — or pass
``--update-registry`` to do that in-place.

See ADR-0321 + docs/ai/models/fr_regressor_v2_probabilistic.md.
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path
from typing import Any

import numpy as np

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_ai_scripts=True)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

# Reuse the LOSO trainer's corpus loader + canonical constants so the
# codec block layout is identical to what was gate-validated.
from train_fr_regressor_v2_ensemble_loso import (  # noqa: E402  # type: ignore[import-not-found]
    CANONICAL_6,
    CODEC_BLOCK_DIM,
    ENCODER_VOCAB,
    ENCODER_VOCAB_VERSION,
    _load_corpus,
    _set_seed_all,
)

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.file_utils import sha256  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

CODEC_BLOCK_LAYOUT: list[str] = [f"encoder_onehot[{e}]" for e in ENCODER_VOCAB] + [
    "preset_norm",
    "crf_norm",
]


def _train_full_corpus(
    seed: int,
    feat_norm: np.ndarray,
    codec_block: np.ndarray,
    target: np.ndarray,
    *,
    epochs: int,
    batch_size: int,
    lr: float,
    weight_decay: float,
):  # type: ignore[no-untyped-def]
    """Train one FRRegressor on the FULL corpus for the production checkpoint.

    Mirrors ``_train_one_fold`` from the LOSO trainer (ADR-0319) — same
    architecture (FRRegressor(in=6, hidden=64, depth=2, dropout=0.1,
    num_codecs=14)), same Adam optimiser, same MSE loss — but trained
    on every row instead of leave-one-out folds.
    """
    import torch
    from torch.utils.data import DataLoader, TensorDataset

    from vmaf_train.models import FRRegressor

    _set_seed_all(seed)
    model = FRRegressor(
        in_features=len(CANONICAL_6),
        hidden=64,
        depth=2,
        dropout=0.1,
        lr=lr,
        weight_decay=weight_decay,
        num_codecs=CODEC_BLOCK_DIM,
    )
    ds = TensorDataset(
        torch.from_numpy(feat_norm.astype(np.float32)),
        torch.from_numpy(codec_block.astype(np.float32)),
        torch.from_numpy(target.astype(np.float32)),
    )
    loader = DataLoader(ds, batch_size=batch_size, shuffle=True, drop_last=False)
    opt = torch.optim.Adam(model.parameters(), lr=lr, weight_decay=weight_decay)
    loss_fn = torch.nn.MSELoss()
    model.train()
    for _ep in range(epochs):
        for xb, cb, yb in loader:
            opt.zero_grad()
            pred = model(xb, cb)
            loss = loss_fn(pred, yb)
            loss.backward()
            opt.step()
    model.eval()
    return model


def _export_onnx(model, onnx_path: Path) -> str:  # type: ignore[no-untyped-def]
    """Export the trained FRRegressor as a two-input ONNX (features + codec).

    Input contract mirrors ``train_fr_regressor_v2_ensemble.py::_export_member``:
    ``features``: float32 [N, 6], ``codec_onehot``: float32 [N, 14] (12-slot
    encoder one-hot + preset_norm + crf_norm). Output: ``score`` float32 [N].
    Opset 17 — matches the registry's ``opset: 17`` entry.
    """
    import torch

    onnx_path.parent.mkdir(parents=True, exist_ok=True)
    model = model.eval()
    dummy_features = torch.zeros(1, len(CANONICAL_6), dtype=torch.float32)
    dummy_codec = torch.zeros(1, CODEC_BLOCK_DIM, dtype=torch.float32)
    torch.onnx.export(
        model,
        (dummy_features, dummy_codec),
        str(onnx_path),
        input_names=["features", "codec_onehot"],
        output_names=["score"],
        dynamic_axes={
            "features": {0: "batch"},
            "codec_onehot": {0: "batch"},
            "score": {0: "batch"},
        },
        opset_version=17,
    )
    return sha256(onnx_path)


def _build_sidecar(
    seed: int,
    *,
    onnx_name: str,
    onnx_sha256: str,
    feature_mean: list[float],
    feature_std: list[float],
    cq_min: float,
    cq_max: float,
    n_rows: int,
    epochs: int,
    batch_size: int,
    lr: float,
    weight_decay: float,
    corpus_path: str,
    corpus_sha256: str,
    promote: dict[str, Any],
    run_provenance: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Build the per-seed sidecar JSON.

    Mirrors the canonical ``model/tiny/fr_regressor_v2.json`` shape so
    downstream loaders that already understand v2 sidecars Just Work,
    but adds ensemble-specific fields (``seed``, ``ensemble_id``,
    ``loso_mean_plcc``, ``gate``) for traceability.
    """
    seed_str = str(seed)
    gate = promote.get("gate", {})
    per_seed_plccs = gate.get("per_seed_plccs", {}) or {}
    per_seed_plcc = per_seed_plccs.get(seed_str)
    sidecar = {
        "id": f"fr_regressor_v2_ensemble_v1_seed{seed}",
        "ensemble_id": "fr_regressor_v2_ensemble_v1",
        "seed": seed,
        "kind": "fr",
        "codec_aware": True,
        "codec_block_layout": list(CODEC_BLOCK_LAYOUT),
        "encoder_vocab": list(ENCODER_VOCAB),
        "encoder_vocab_version": ENCODER_VOCAB_VERSION,
        "feature_order": list(CANONICAL_6),
        "feature_mean": feature_mean,
        "feature_std": feature_std,
        "input_names": ["features", "codec_onehot"],
        "output_names": ["score"],
        "onnx": onnx_name,
        "opset": 17,
        "sha256": onnx_sha256,
        "corpus": {
            "path": corpus_path,
            "sha256": corpus_sha256,
            "n_rows": n_rows,
            "cq_min": cq_min,
            "cq_max": cq_max,
        },
        "training_recipe": {
            "in_features": len(CANONICAL_6),
            "hidden": 64,
            "depth": 2,
            "dropout": 0.1,
            "num_codecs": CODEC_BLOCK_DIM,
            "epochs": epochs,
            "batch_size": batch_size,
            "lr": lr,
            "weight_decay": weight_decay,
            "optimizer": "Adam",
            "loss": "MSE",
            "fit_set": "full_corpus",
            "scaler": "fit_on_full_corpus",
        },
        "gate": {
            "verdict": promote.get("verdict"),
            "mean_plcc": gate.get("mean_plcc"),
            "mean_plcc_threshold": gate.get("mean_plcc_threshold"),
            "plcc_spread": gate.get("plcc_spread"),
            "plcc_spread_max": gate.get("plcc_spread_max"),
            "per_seed_min_threshold": gate.get("per_seed_min"),
            "this_seed_loso_plcc": per_seed_plcc,
            "passed": gate.get("passed"),
        },
        "loso_mean_plcc": gate.get("mean_plcc"),
        "notes": (
            f"Production ensemble member of fr_regressor_v2_ensemble_v1 "
            f"(seed={seed}). Fit on full Phase A canonical-6 corpus "
            f"({n_rows} rows) after the 9-fold LOSO gate passed per "
            f"PROMOTE.json (verdict={promote.get('verdict')}, "
            f"mean_plcc={gate.get('mean_plcc'):.4f}). 6 canonical libvmaf "
            f"features (adm2, vif_scale0..3, motion2) + 14-D codec block "
            f"(12-slot encoder one-hot v2 + preset_norm + crf_norm) -> "
            f"VMAF teacher score. See docs/ai/models/"
            f"fr_regressor_v2_probabilistic.md, fr_regressor_v2_ensemble_v1.json "
            f"(ensemble manifest), ADR-0303, ADR-0309, ADR-0319, ADR-0321."
        ),
        "parent_adrs": ["ADR-0303", "ADR-0309", "ADR-0319", "ADR-0321"],
    }
    if run_provenance is not None:
        sidecar["run_provenance"] = run_provenance
    return sidecar


def main(argv: list[str] | None = None) -> int:
    ap = make_argument_parser(prog="export_ensemble_v2_seeds")
    ap.add_argument(
        "--corpus",
        type=Path,
        default=REPO_ROOT / "runs" / "phase_a" / "full_grid" / "per_frame_canonical6.jsonl",
    )
    ap.add_argument(
        "--promote-json",
        type=Path,
        default=REPO_ROOT / "runs" / "ensemble_v2_real" / "PROMOTE.json",
        help="Path to the LOSO gate verdict JSON (PROMOTE.json from ADR-0309).",
    )
    ap.add_argument(
        "--seeds",
        type=str,
        default="0,1,2,3,4",
        help="Comma-separated seeds (default: 0,1,2,3,4).",
    )
    ap.add_argument("--epochs", type=int, default=200)
    ap.add_argument("--batch-size", type=int, default=32)
    ap.add_argument("--lr", type=float, default=5e-4)
    ap.add_argument("--weight-decay", type=float, default=1e-5)
    ap.add_argument(
        "--out-dir",
        type=Path,
        default=REPO_ROOT / "model" / "tiny",
    )
    ap.add_argument(
        "--update-registry",
        action="store_true",
        help="Patch sha256 + smoke=false on the 5 seed rows in registry.json.",
    )
    raw_argv = collect_cli_argv(argv)
    args = ap.parse_args(raw_argv)

    seeds = [int(s) for s in args.seeds.split(",") if s.strip()]
    if not seeds:
        print("error: --seeds must be non-empty", file=sys.stderr)
        return 2
    if not args.corpus.is_file():
        print(f"error: corpus not found at {args.corpus}", file=sys.stderr)
        return 2
    if not args.promote_json.is_file():
        print(f"error: PROMOTE.json not found at {args.promote_json}", file=sys.stderr)
        return 2

    promote = json.loads(args.promote_json.read_text())
    if promote.get("verdict") != "PROMOTE":
        print(
            f"error: PROMOTE.json verdict is {promote.get('verdict')!r}, refusing "
            "to flip non-PROMOTE seeds to production.",
            file=sys.stderr,
        )
        return 3

    print(f"[export-ens] loading corpus from {args.corpus}", flush=True)
    corpus_sha = sha256(args.corpus)
    corpus = _load_corpus(args.corpus)
    df = corpus["df"]
    feat_full = df[list(CANONICAL_6)].to_numpy(dtype=np.float64)
    target = df["vmaf"].to_numpy(dtype=np.float64)
    feature_mean = corpus["feature_mean"]
    feature_std = corpus["feature_std"]
    feat_norm = ((feat_full - feature_mean) / feature_std).astype(np.float32)
    codec_block = corpus["codec_block"]

    print(
        f"[export-ens] n_rows={corpus['n_rows']} corpus_sha256={corpus_sha[:16]}... "
        f"cq=[{corpus['cq_min']}, {corpus['cq_max']}]",
        flush=True,
    )

    args.out_dir.mkdir(parents=True, exist_ok=True)
    new_shas: dict[int, str] = {}
    output_targets: dict[str, str | None] = {}
    for seed in seeds:
        output_targets[f"seed{seed}_onnx"] = str(
            args.out_dir / f"fr_regressor_v2_ensemble_v1_seed{seed}.onnx"
        )
        output_targets[f"seed{seed}_sidecar"] = str(
            args.out_dir / f"fr_regressor_v2_ensemble_v1_seed{seed}.json"
        )
    output_targets["registry"] = (
        str(args.out_dir / "registry.json") if args.update_registry else None
    )
    run_provenance = build_run_provenance(
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        inputs={"corpus": args.corpus, "promote_json": args.promote_json},
        outputs=output_targets,
    )
    for seed in seeds:
        t0 = time.time()
        print(f"[export-ens] seed={seed} training full-corpus model...", flush=True)
        model = _train_full_corpus(
            seed,
            feat_norm,
            codec_block,
            target,
            epochs=args.epochs,
            batch_size=args.batch_size,
            lr=args.lr,
            weight_decay=args.weight_decay,
        )

        onnx_name = f"fr_regressor_v2_ensemble_v1_seed{seed}.onnx"
        onnx_path = args.out_dir / onnx_name
        sha = _export_onnx(model, onnx_path)
        new_shas[seed] = sha
        sidecar = _build_sidecar(
            seed,
            onnx_name=onnx_name,
            onnx_sha256=sha,
            feature_mean=feature_mean.astype(float).tolist(),
            feature_std=feature_std.astype(float).tolist(),
            cq_min=corpus["cq_min"],
            cq_max=corpus["cq_max"],
            n_rows=corpus["n_rows"],
            epochs=args.epochs,
            batch_size=args.batch_size,
            lr=args.lr,
            weight_decay=args.weight_decay,
            corpus_path=str(args.corpus.relative_to(REPO_ROOT)),
            corpus_sha256=corpus_sha,
            promote=promote,
            run_provenance=run_provenance,
        )
        sidecar_path = args.out_dir / f"fr_regressor_v2_ensemble_v1_seed{seed}.json"
        write_manifest_json(sidecar_path, sidecar)
        elapsed = time.time() - t0
        print(
            f"[export-ens] seed={seed} wrote {onnx_name} sha={sha[:16]}... "
            f"+ sidecar ({elapsed:.1f}s)",
            flush=True,
        )

    if args.update_registry:
        reg_path = args.out_dir / "registry.json"
        reg = json.loads(reg_path.read_text())
        for entry in reg.get("models", []):
            mid = entry.get("id", "")
            if mid.startswith("fr_regressor_v2_ensemble_v1_seed"):
                seed = int(mid.rsplit("seed", 1)[-1])
                if seed in new_shas:
                    entry["sha256"] = new_shas[seed]
                    entry["smoke"] = False
        write_manifest_json(reg_path, reg)
        print("[export-ens] patched registry.json: 5 seeds smoke=false + new sha256s")

    print("[export-ens] done. New sha256 per seed:")
    for seed, sha in sorted(new_shas.items()):
        print(f"  seed{seed}: {sha}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
